#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "red.h"

enum { RECEPTOR, AUXILIAR, PUBLICADOR, HILOS };

_Static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "Se requiere un indicador atómico sin bloqueo");
static atomic_bool terminar = ATOMIC_VAR_INIT(false);

static void pedir_cierre(int senal)
{
    (void)senal;
    atomic_store_explicit(&terminar, true, memory_order_relaxed);
}

typedef struct {
    char origen[32];
    long long numero, monto; /* El monto va en centavos. Origen y número identifican la venta. */
} Venta;

typedef struct {
    char host[128], puerto[6], id[32];
    size_t enviado, consultado; /* Hasta dónde le enviamos y hasta dónde leímos. */
    unsigned long long orden; /* Cero si el par no responde. */
} Par;

typedef struct {
    char id[32];
    /* Propio suma las ventas de esta tienda. Total incluye las de su cadena. */
    long long meta, propio, total, numero;
    int archivo, cantidad;
    bool en_cadena, registro_danado;
    const char *lista;
    unsigned long long conexiones;
    Par *pares;
    Venta *ventas;
    size_t usadas, capacidad, publicado_hasta;
    bool recuperando, sincronizando;
    atomic_bool eligiendo[HILOS];
    atomic_ullong boleto[HILOS];
} Tienda;

typedef struct {
    Tienda *tienda;
    int escucha;
    int hilo;
} Receptor;

static int positivo(const char *s, long long limite, long long *valor)
{
    if (!isdigit((unsigned char)*s)) return 0;
    char *fin;
    errno = 0;
    long long n = strtoll(s, &fin, 10);
    if (errno || *fin || n < 1 || n > limite) return 0;
    *valor = n;
    return 1;
}

/* La interfaz usa dólares. El registro y el protocolo de ventas usan centavos. */
static int dolares(const char *s, long long *centavos)
{
    if (*s == '$') s++;
    if (!isdigit((unsigned char)*s)) return 0;
    char *fin;
    errno = 0;
    long long unidades = strtoll(s, &fin, 10);
    if (errno) return 0;
    s = fin;
    int fraccion = 0;
    if (*s == '.') {
        s++;
        if (!isdigit((unsigned char)*s)) return 0;
        fraccion = (*s++ - '0') * 10;
        if (isdigit((unsigned char)*s)) fraccion += *s++ - '0';
    }
    if (*s || unidades > (LLONG_MAX - fraccion) / 100) return 0;
    *centavos = unidades * 100 + fraccion;
    return *centavos > 0;
}

static int es_id(const char *s)
{
    if (!*s || strlen(s) > 31) return 0;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_') return 0;
    }
    return 1;
}

/* Bakery de Lamport da acceso al registro a un hilo a la vez. */
static void tomar_turno(Tienda *t, int hilo)
{
    /* Aviso que estoy eligiendo. El boleto vale uno más que el mayor que veo. */
    atomic_store(&t->eligiendo[hilo], true);
    unsigned long long maximo = 0;
    for (int i = 0; i < HILOS; i++) {
        unsigned long long n = atomic_load(&t->boleto[i]);
        if (n > maximo) maximo = n;
    }
    unsigned long long mio = maximo + 1;
    atomic_store(&t->boleto[hilo], mio);
    atomic_store(&t->eligiendo[hilo], false);

    for (int i = 0; i < HILOS; i++) {
        if (i == hilo) continue;
        /* Espero a que el otro termine de elegir. Si empatamos, va primero el ID menor. */
        while (atomic_load(&t->eligiendo[i])) sched_yield();
        for (;;) {
            unsigned long long otro = atomic_load(&t->boleto[i]);
            if (otro == 0 || otro > mio || (otro == mio && i > hilo)) break;
            sched_yield();
        }
    }
}

static void dejar_turno(Tienda *t, int hilo)
{
    /* Con cero, los demás saben que este hilo ya dejó el turno. */
    atomic_store(&t->boleto[hilo], 0);
}

static int escribir(int fd, const char *s, size_t n, bool por_red)
{
    while (n) {
        if (por_red) {
            int listo = esperar_socket(fd, POLLOUT, ESPERA);
            if (listo < 0 && errno == EINTR && !atomic_load(&terminar)) continue;
            if (listo <= 0) return 0;
        }
        ssize_t escritos = write(fd, s, n);
        if (escritos < 0 && errno == EINTR) continue;
        if (escritos <= 0) return 0;
        s += escritos;
        n -= (size_t)escritos;
    }
    return 1;
}

static int confirmar_archivo(int fd)
{
    int resultado;
    do {
        resultado = fsync(fd);
    } while (resultado < 0 && errno == EINTR);
    return resultado == 0;
}

static int pedir(const char *host, const char *puerto, const char *orden, char r[LINEA])
{
    int fd = abrir_socket(host, puerto);
    if (fd < 0) return 0;
    int ok = escribir(fd, orden, strlen(orden), true) && mensaje(fd, r);
    close(fd);
    return ok;
}

static int leer_venta(const char *linea, Venta *v)
{
    char numero[32], monto[32], sobra[2];
    return sscanf(linea, "%31s %31s %31s %1s", v->origen, numero, monto, sobra) == 3 &&
           es_id(v->origen) && positivo(numero, LLONG_MAX, &v->numero) &&
           positivo(monto, LLONG_MAX, &v->monto);
}

static const char *guardar(Tienda *t, Venta v, bool disco)
{
    if (disco && t->registro_danado) return "ERROR REGISTRO\n";
    /* Si se perdió una confirmación, el par puede mandar la misma venta de nuevo. */
    for (size_t i = 0; i < t->usadas; i++) {
        const Venta *anterior = &t->ventas[i];
        if (anterior->numero == v.numero && strcmp(anterior->origen, v.origen) == 0) {
            return anterior->monto == v.monto ? "OK DUPLICADA\n" : "ERROR CONFLICTO\n";
        }
    }
    if (v.monto > LLONG_MAX - t->total) return "ERROR TOTAL\n";
    if (t->usadas == t->capacidad) {
        if (t->capacidad > (size_t)-1 / sizeof(Venta) / 2) return "ERROR MEMORIA\n";
        size_t capacidad = t->capacidad ? 2 * t->capacidad : 32;
        Venta *ventas = realloc(t->ventas, capacidad * sizeof *ventas);
        if (!ventas) return "ERROR MEMORIA\n";
        t->ventas = ventas;
        t->capacidad = capacidad;
    }
    if (disco) {
        char fila[LINEA];
        int n = snprintf(fila, sizeof fila, "%s %lld %lld\n", v.origen, v.numero, v.monto);
        if (n <= 0 || (size_t)n >= sizeof fila) return "ERROR REGISTRO\n";
        off_t previo = lseek(t->archivo, 0, SEEK_END);
        if (previo < 0) return "ERROR REGISTRO\n";
        /* Primero guardo y sincronizo la línea. Solo después cambio los totales. */
        if (!escribir(t->archivo, fila, (size_t)n, false) || !confirmar_archivo(t->archivo)) {
            /* Si quedó una escritura a medias, intento volver al final anterior. */
            if (ftruncate(t->archivo, previo) != 0 || !confirmar_archivo(t->archivo)) {
                t->registro_danado = true;
                fprintf(stderr, "%s: no se pudo restaurar el registro de ventas\n", t->id);
            }
            return "ERROR REGISTRO\n";
        }
    }
    t->ventas[t->usadas++] = v;
    t->total += v.monto;
    if (strcmp(v.origen, t->id) == 0) {
        t->propio += v.monto;
        if (v.numero > t->numero) t->numero = v.numero;
    }
    if (disco) printf("%s registra %s-%lld monto=$%lld.%02lld total=$%lld.%02lld\n",
                      t->id, v.origen, v.numero, v.monto / 100, v.monto % 100,
                      t->total / 100, t->total % 100);
    return "OK REGISTRADA\n";
}

static int abrir_registro(Tienda *t, const char *ruta)
{
    FILE *f = fopen(ruta, "a+");
    if (!f) return 0;
    rewind(f);
    char linea[LINEA];
    int ok = 1;
    for (;;) {
        off_t inicio = ftello(f);
        if (!fgets(linea, sizeof linea, f)) break;
        Venta v;
        if (!strchr(linea, '\n')) {
            /* Tras un corte puede quedar incompleta solo la última línea. */
            ok = feof(f) && inicio >= 0 &&
                 ftruncate(fileno(f), inicio) == 0 && confirmar_archivo(fileno(f));
            break;
        }
        /* Al arrancar reconstruyo los totales con las líneas que ya están guardadas. */
        if (!leer_venta(linea, &v) || strncmp(guardar(t, v, false), "OK", 2)) {
            ok = 0;
            break;
        }
    }
    if (ok && !ferror(f)) t->archivo = dup(fileno(f));
    fclose(f);
    return t->archivo >= 0;
}

static int enviar_venta(const Par *p, const Venta *v)
{
    char orden[LINEA], r[LINEA];
    snprintf(orden, sizeof orden, "DIFUNDE %s %lld %lld\n", v->origen, v->numero, v->monto);
    return pedir(p->host, p->puerto, orden, r) &&
           (strcmp(r, "OK REGISTRADA") == 0 || strcmp(r, "OK DUPLICADA") == 0);
}

/* FIN indica que el par ya no tiene más ventas en ese punto del registro. */
static int consultar_venta(const Par *p, size_t indice, Venta *v)
{
    char orden[LINEA], respuesta[LINEA];
    snprintf(orden, sizeof orden, "VENTA %zu\n", indice + 1);
    if (!pedir(p->host, p->puerto, orden, respuesta)) return -1;
    if (strcmp(respuesta, "FIN") == 0) return 0;
    if (strncmp(respuesta, "VENTA ", 6) != 0 || !leer_venta(respuesta + 6, v)) return -1;
    return 1;
}

static int origen_caido(const Tienda *t, const char *origen)
{
    if (strcmp(t->id, origen) == 0) return 0;
    int desconocido = 0;
    for (int i = 0; i < t->cantidad; i++) {
        const Par *p = &t->pares[i];
        if (strcmp(p->id, origen) == 0) return !p->orden;
        if (!p->orden && !*p->id) desconocido = 1;
    }
    return desconocido;
}

/* La dirección identifica al par aunque cambie el archivo de configuración. */
static const Par *buscar_par(const Par *pares, int cantidad, const Par *buscado)
{
    for (int i = 0; i < cantidad; i++) {
        if (strcmp(pares[i].host, buscado->host) == 0 &&
            strcmp(pares[i].puerto, buscado->puerto) == 0) return &pares[i];
    }
    return NULL;
}

static int agregar_par(Par **pares, int *cantidad, const char *texto)
{
    long long puerto;
    const char *dos = strrchr(texto, ':');
    Par p = {0};
    if (!dos || dos == texto || dos - texto >= (long)sizeof p.host ||
        strlen(dos + 1) >= sizeof p.puerto || !positivo(dos + 1, 65535, &puerto))
        return 0;
    memcpy(p.host, texto, (size_t)(dos - texto));
    snprintf(p.puerto, sizeof p.puerto, "%lld", puerto);
    if (buscar_par(*pares, *cantidad, &p)) return 1;
    if (*cantidad == INT_MAX || (size_t)*cantidad >= SIZE_MAX / sizeof p) return 0;
    Par *nuevos = realloc(*pares, ((size_t)*cantidad + 1) * sizeof p);
    if (!nuevos) return 0;
    *pares = nuevos;
    nuevos[(*cantidad)++] = p;
    return 1;
}

static int cargar_pares(Tienda *t)
{
    FILE *f = fopen(t->lista, "r");
    if (!f) return 0;
    Par *nuevos = NULL;
    int cantidad = 0;
    char *linea = NULL;
    size_t espacio = 0;
    int ok = 1;
    while (getline(&linea, &espacio, f) >= 0) {
        char *comentario = strchr(linea, '#');
        if (comentario) *comentario = '\0';
        char texto[LINEA], extra[2];
        int campos = sscanf(linea, "%255s %1s", texto, extra);
        if (campos < 1) continue;
        if (campos != 1 || !agregar_par(&nuevos, &cantidad, texto)) {
            ok = 0;
            break;
        }
    }
    ok = ok && feof(f) && !ferror(f);
    free(linea);
    fclose(f);
    if (!ok) {
        free(nuevos);
        return 0;
    }
    /* Cambiar la lista no reinicia las entregas de los pares conservados. */
    for (int i = 0; i < cantidad; i++) {
        const Par *anterior = buscar_par(t->pares, t->cantidad, &nuevos[i]);
        if (anterior) nuevos[i] = *anterior;
    }
    free(t->pares);
    t->pares = nuevos;
    t->cantidad = cantidad;
    return 1;
}

static int comparar_orden(const void *a, const void *b)
{
    const Par *primero = a, *segundo = b;
    return (primero->orden > segundo->orden) - (primero->orden < segundo->orden);
}

/* Busco los pares que responden; los que vuelven se anotan al final. */
static bool actualizar_pares(Tienda *t)
{
    bool hay_corte = false, corte_nuevo = false;
    for (int i = 0; i < t->cantidad && !atomic_load(&terminar); i++) {
        Par *p = &t->pares[i];
        char r[LINEA], id[32];
        if (!pedir(p->host, p->puerto, "SALUDO\n", r) ||
            sscanf(r, "SALUDO %31s", id) != 1 || !es_id(id) || strcmp(id, t->id) == 0) {
            if (p->orden) {
                printf("%s pierde conexión con %s\n", t->id, p->id);
                corte_nuevo = true;
            }
            p->orden = 0;
            hay_corte = true;
            continue;
        }
        if (*p->id && strcmp(p->id, id) != 0) {
            p->enviado = 0;
            p->consultado = 0;
            p->orden = 0;
        }
        if (!p->orden) {
            p->orden = ++t->conexiones;
            printf("%s conecta con %s (orden %llu)\n", t->id, id, p->orden);
        }
        strcpy(p->id, id);
    }
    if (corte_nuevo) {
        /* El par que cayó puede haber dejado copias de sus ventas en otros. */
        for (int i = 0; i < t->cantidad; i++) t->pares[i].consultado = 0;
    }
    if (t->cantidad > 1) qsort(t->pares, (size_t)t->cantidad, sizeof *t->pares, comparar_orden);
    return hay_corte;
}

/* Al reiniciar leo todas las ventas. Durante el corte filtro las del origen caído.
 * Devuelvo true cuando el par confirma que llegué al final de su registro. */
static bool recuperar_ventas(Tienda *t, const Par *p, size_t *indice, size_t cantidad,
                             bool reinicio)
{
    for (size_t i = 0; i < cantidad && !atomic_load(&terminar); i++) {
        Venta v;
        int resultado = consultar_venta(p, *indice, &v);
        if (resultado <= 0) return resultado == 0;
        if (reinicio || origen_caido(t, v.origen)) {
            tomar_turno(t, PUBLICADOR);
            const char *guardada = guardar(t, v, true);
            dejar_turno(t, PUBLICADOR);
            if (strncmp(guardada, "OK ", 3) != 0) return false;
        }
        (*indice)++;
    }
    return false;
}

/* Al reiniciar, consulto a los pares que responden antes de aceptar compras nuevas. */
static void sincronizar_reinicio(Tienda *t)
{
    for (int i = 0; i < t->cantidad && !atomic_load(&terminar); i++) {
        const Par *p = &t->pares[i];
        char respuesta[LINEA], sobra[2];
        size_t cantidad;
        if (!pedir(p->host, p->puerto, "REGISTROS\n", respuesta) ||
            strncmp(respuesta, "REGISTROS ", 10) != 0 ||
            !isdigit((unsigned char)respuesta[10]) ||
            sscanf(respuesta + 10, "%zu %1s", &cantidad, sobra) != 1) continue;
        /* Pido una cantidad fija para terminar aunque el par siga vendiendo. */
        size_t indice = 0;
        recuperar_ventas(t, p, &indice, cantidad, true);
    }
    tomar_turno(t, PUBLICADOR);
    t->sincronizando = false;
    dejar_turno(t, PUBLICADOR);
}

static void *publicar(void *arg)
{
    Tienda *t = arg;
    const struct timespec espera = {0, 100000000};
    time_t ultima_lista = (time_t)-1;
    if (t->sincronizando) sincronizar_reinicio(t);
    while (!atomic_load(&terminar)) {
        struct timespec reloj;
        if (t->lista && clock_gettime(CLOCK_MONOTONIC, &reloj) == 0 &&
            reloj.tv_sec != ultima_lista) {
            ultima_lista = reloj.tv_sec;
            cargar_pares(t);
        }
        bool hay_corte = actualizar_pares(t);


        tomar_turno(t, PUBLICADOR);
        size_t limite = t->usadas;
        dejar_turno(t, PUBLICADOR);
        size_t confirmado = limite;
        bool recuperando = false;
        for (int i = 0; i < t->cantidad && !atomic_load(&terminar); i++) {
            Par *p = &t->pares[i];
            if (!p->orden) continue;
            for (int paso = 0; paso < 16 && p->enviado < limite && !atomic_load(&terminar); paso++) {
                tomar_turno(t, PUBLICADOR);
                Venta v = t->ventas[p->enviado];
                /* Con la venta copiada ya puedo soltar el turno antes de usar la red. */
                dejar_turno(t, PUBLICADOR);
                /* Difundo las ventas propias. Si el par no da el OK, reintento más tarde. */
                if (strcmp(v.origen, t->id) == 0 && !enviar_venta(p, &v)) break;
                p->enviado++;
            }
            if (p->enviado < confirmado) confirmado = p->enviado;
            if (hay_corte && !recuperar_ventas(t, p, &p->consultado, 16, false))
                recuperando = true;
        }
        tomar_turno(t, PUBLICADOR);
        t->publicado_hasta = confirmado;
        t->recuperando = recuperando;
        dejar_turno(t, PUBLICADOR);
        nanosleep(&espera, NULL);
    }
    return NULL;
}

static void atender(Tienda *t, int fd, int hilo)
{
    char linea[LINEA], r[LINEA];
    const char *respuesta = "ERROR SOLICITUD\n";
    if (!mensaje(fd, linea)) return;
    if (t->en_cadena && strcmp(linea, "SALUDO") == 0) {
        snprintf(r, sizeof r, "SALUDO %s\n", t->id);
        escribir(fd, r, strlen(r), true);
        return;
    }
    Venta v;
    long long n;
    tomar_turno(t, hilo);
    if (strcmp(linea, "ESTADO") == 0) {
        const char *estado = "ABIERTA";
        if (t->total >= t->meta) estado = "CERRADA";
        else if (t->sincronizando) estado = "RECUPERANDO";
        snprintf(r, sizeof r, "ESTADO %s %s $%lld.%02lld $%lld.%02lld\n", t->id, estado,
                 t->propio / 100, t->propio % 100, t->total / 100, t->total % 100);
        respuesta = r;
    } else if (strcmp(linea, "PENDIENTES") == 0) {
        size_t pendientes = t->en_cadena ? t->usadas - t->publicado_hasta : 0;
        if (!pendientes && (t->recuperando || t->sincronizando)) pendientes = 1;
        snprintf(r, sizeof r, "PENDIENTES %zu\n", pendientes);
        respuesta = r;
    } else if (strncmp(linea, "COMPRA ", 7) == 0 && positivo(linea + 7, LLONG_MAX, &n)) {
        /* Cada tienda decide el cierre con las ventas que ya tiene registradas. */
        if (t->total >= t->meta) respuesta = "ERROR CERRADA\n";
        else if (t->sincronizando) respuesta = "ERROR RECUPERANDO\n";
        else if (t->numero == LLONG_MAX) respuesta = "ERROR SECUENCIA\n";
        else {
            strcpy(v.origen, t->id);
            v.numero = t->numero + 1;
            v.monto = n;
            respuesta = guardar(t, v, true);
        }
    } else if (t->en_cadena && strncmp(linea, "DIFUNDE ", 8) == 0 &&
               leer_venta(linea + 8, &v) && strcmp(v.origen, t->id)) {
        /* Una tienda cerrada todavía recibe avisos de ventas que ya se aceptaron. */
        respuesta = guardar(t, v, true);
    } else if (t->en_cadena && strcmp(linea, "REGISTROS") == 0) {
        snprintf(r, sizeof r, "REGISTROS %zu\n", t->usadas);
        respuesta = r;
    } else if (t->en_cadena && strncmp(linea, "VENTA ", 6) == 0 &&
               positivo(linea + 6, LLONG_MAX, &n)) {
        respuesta = "FIN\n";
        if ((unsigned long long)n <= t->usadas) {
            v = t->ventas[(size_t)n - 1];
            snprintf(r, sizeof r, "VENTA %s %lld %lld\n", v.origen, v.numero, v.monto);
            respuesta = r;
        }
    }
    dejar_turno(t, hilo);
    escribir(fd, respuesta, strlen(respuesta), true);
}

/* Dos hilos atienden sockets; Bakery protege los cambios al registro. */
static void *recibir(void *arg)
{
    Receptor *r = arg;
    while (!atomic_load(&terminar)) {
        if (esperar_socket(r->escucha, POLLIN, 250) <= 0) continue;
        int otro = accept(r->escucha, NULL, NULL);
        if (otro >= 0) {
            atender(r->tienda, otro, r->hilo);
            close(otro);
        }
    }
    return NULL;
}

static int tienda(int argc, char **argv)
{
    Tienda t = {.archivo = -1};
    long long puerto;
    if (argc < 6 || !es_id(argv[2]) || !positivo(argv[3], 65535, &puerto) ||
        !dolares(argv[4], &t.meta)) return 2;
    strcpy(t.id, argv[2]);
    t.en_cadena = argc > 6;
    for (int i = 0; i < HILOS; i++) {
        atomic_init(&t.eligiendo[i], false);
        atomic_init(&t.boleto[i], 0);
    }
    if (t.en_cadena && argv[6][0] == '@') {
        if (argc != 7 || !argv[6][1]) return 2;
        t.lista = argv[6] + 1;
        if (!cargar_pares(&t)) {
            fprintf(stderr, "%s: no se pudo leer la lista de pares\n", t.id);
            return 1;
        }
    } else {
        for (int i = 6; i < argc; i++) {
            if (!agregar_par(&t.pares, &t.cantidad, argv[i])) {
                free(t.pares);
                return 2;
            }
        }
    }
    t.sincronizando = t.en_cadena && access(argv[5], F_OK) == 0;
    int fd = abrir_socket(NULL, argv[3]);
    pthread_t auxiliar, publicador;
    Receptor principal = {&t, fd, RECEPTOR};
    Receptor segundo = {&t, fd, AUXILIAR};
    struct sigaction accion = {0};
    accion.sa_handler = pedir_cierre;
    sigemptyset(&accion.sa_mask);
    bool ok = fd >= 0 && abrir_registro(&t, argv[5]) &&
              sigaction(SIGINT, &accion, NULL) == 0 && sigaction(SIGTERM, &accion, NULL) == 0;
    bool auxiliar_activo = ok && pthread_create(&auxiliar, NULL, recibir, &segundo) == 0;
    bool publicador_activo = auxiliar_activo && t.en_cadena &&
                              pthread_create(&publicador, NULL, publicar, &t) == 0;
    ok = auxiliar_activo && (!t.en_cadena || publicador_activo);
    if (ok) recibir(&principal);
    else {
        fprintf(stderr, "%s: error al iniciar puerto, registro o hilo\n", t.id);
        atomic_store(&terminar, true);
    }
    if (auxiliar_activo) pthread_join(auxiliar, NULL);
    if (publicador_activo) pthread_join(publicador, NULL);
    if (fd >= 0) close(fd);
    if (t.archivo >= 0) close(t.archivo);
    free(t.pares);
    free(t.ventas);
    return ok ? 0 : 1;
}

static int cliente(int argc, char **argv)
{
    char orden[LINEA], r[LINEA];
    long long puerto, monto, numero;
    if (argc < 5 || !positivo(argv[3], 65535, &puerto)) return 2;
    if (argc == 5 && (strcmp(argv[4], "ESTADO") == 0 || strcmp(argv[4], "PENDIENTES") == 0)) {
        snprintf(orden, sizeof orden, "%s\n", argv[4]);
    } else if (argc == 6 && strcmp(argv[4], "COMPRA") == 0 &&
             dolares(argv[5], &monto)) {
        snprintf(orden, sizeof orden, "COMPRA %lld\n", monto);
    } else if (argc == 8 && strcmp(argv[4], "DIFUNDE") == 0 && es_id(argv[5]) &&
               positivo(argv[6], LLONG_MAX, &numero) && positivo(argv[7], LLONG_MAX, &monto)) {
        snprintf(orden, sizeof orden, "DIFUNDE %s %lld %lld\n", argv[5], numero, monto);
    } else return 2;
    if (!pedir(argv[2], argv[3], orden, r)) return 1;
    puts(r);
    return strncmp(r, "ERROR", 5) == 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    int resultado = 2;
    if (argc > 1 && strcmp(argv[1], "tienda") == 0) resultado = tienda(argc, argv);
    else if (argc > 1 && strcmp(argv[1], "cliente") == 0) resultado = cliente(argc, argv);
    if (resultado == 2) {
        fputs("blackout tienda ID PUERTO META_USD REGISTRO [HOST:PUERTO ... | @ARCHIVO]\n"
              "blackout cliente HOST PUERTO ESTADO | PENDIENTES | COMPRA MONTO_USD\n", stderr);
    }
    return resultado;
}
