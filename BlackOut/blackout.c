#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LINEA 256
#define ESPERA 800

typedef struct {
    char origen[32];
    long long numero, monto;
} Venta;

typedef struct {
    char host[128], puerto[6], id[32];
    int disponible;
    size_t enviado, consultado;
} Par;

typedef struct {
    char id[32];
    long long meta, propio, total, numero;
    int archivo, cantidad;
    Par *pares;
    Venta *ventas;
    size_t usadas, capacidad;
    atomic_ulong entrada, salida;
} Tienda;

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

static int es_id(const char *s)
{
    if (!*s || strlen(s) > 31) return 0;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_') return 0;
    }
    return 1;
}

static void tomar_turno(Tienda *t)
{
    unsigned long n = atomic_fetch_add(&t->entrada, 1);
    while (atomic_load(&t->salida) != n) sched_yield();
}

static void dejar_turno(Tienda *t)
{
    atomic_fetch_add(&t->salida, 1);
}

static int mensaje(int fd, char linea[LINEA])
{
    struct timespec inicio, ahora;
    if (clock_gettime(CLOCK_MONOTONIC, &inicio) != 0) return 0;
    for (size_t i = 0; i + 1 < LINEA; i++) {
        if (clock_gettime(CLOCK_MONOTONIC, &ahora) != 0) return 0;
        long usado = (ahora.tv_sec - inicio.tv_sec) * 1000 +
                     (ahora.tv_nsec - inicio.tv_nsec) / 1000000;
        struct pollfd p = {fd, POLLIN, 0};
        if (usado >= ESPERA || poll(&p, 1, ESPERA - (int)usado) <= 0) return 0;
        if (read(fd, &linea[i], 1) != 1 || !linea[i]) return 0;
        if (linea[i] == '\n') {
            linea[i] = '\0';
            return 1;
        }
    }
    return 0;
}

static int escribir(int fd, const char *s, size_t n)
{
    while (n) {
        struct pollfd p = {fd, POLLOUT, 0};
        if (poll(&p, 1, ESPERA) <= 0) return 0;
        ssize_t escritos = write(fd, s, n);
        if (escritos <= 0) return 0;
        s += escritos;
        n -= (size_t)escritos;
    }
    return 1;
}

static int abrir_socket(const char *host, const char *puerto)
{
    struct addrinfo info = {0}, *a;
    info.ai_family = AF_INET;
    info.ai_socktype = SOCK_STREAM;
    info.ai_flags = host ? 0 : AI_PASSIVE;
    if (getaddrinfo(host, puerto, &info, &a) != 0) return -1;
    int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    int ok = 0;
    if (fd >= 0 && !host) {
        int si = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &si, sizeof si);
        ok = bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, 20) == 0;
    } else if (fd >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) == 0) {
        ok = connect(fd, a->ai_addr, a->ai_addrlen) == 0;
        if (!ok && errno == EINPROGRESS) {
            struct pollfd p = {fd, POLLOUT, 0};
            int error;
            socklen_t tam = sizeof error;
            ok = poll(&p, 1, ESPERA) > 0 &&
                 getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &tam) == 0 && !error;
        }
    }
    freeaddrinfo(a);
    if (ok) return fd;
    if (fd >= 0) close(fd);
    return -1;
}

static int pedir(const char *host, const char *puerto, const char *orden, char r[LINEA])
{
    int fd = abrir_socket(host, puerto);
    if (fd < 0) return 0;
    int ok = escribir(fd, orden, strlen(orden)) && mensaje(fd, r);
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

static const char *guardar(Tienda *t, Venta v, int disco)
{
    for (size_t i = 0; i < t->usadas; i++) {
        Venta anterior = t->ventas[i];
        if (anterior.numero == v.numero && strcmp(anterior.origen, v.origen) == 0) {
            return anterior.monto == v.monto ? "OK DUPLICADA\n" : "ERROR CONFLICTO\n";
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
        off_t previo = lseek(t->archivo, 0, SEEK_END);
        if (previo < 0 || write(t->archivo, fila, (size_t)n) != n || fsync(t->archivo) != 0) {
            if (previo >= 0 && ftruncate(t->archivo, previo) == 0) fsync(t->archivo);
            return "ERROR REGISTRO\n";
        }
    }
    t->ventas[t->usadas++] = v;
    t->total += v.monto;
    if (strcmp(v.origen, t->id) == 0) {
        t->propio += v.monto;
        if (v.numero > t->numero) t->numero = v.numero;
    }
    if (disco) printf("%s registra %s-%lld monto=%lld total=%lld\n",
                      t->id, v.origen, v.numero, v.monto, t->total);
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
            ok = feof(f) && inicio >= 0 && ftruncate(fileno(f), inicio) == 0;
            break;
        }
        if (!leer_venta(linea, &v) || strncmp(guardar(t, v, 0), "OK", 2)) {
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
    return pedir(p->host, p->puerto, orden, r) && strncmp(r, "OK ", 3) == 0;
}

static int origen_caido(const Tienda *t, const char *origen)
{
    if (strcmp(t->id, origen) == 0) return 0;
    int desconocido = 0;
    for (int i = 0; i < t->cantidad; i++) {
        const Par *p = &t->pares[i];
        if (strcmp(p->id, origen) == 0) return !p->disponible;
        if (!p->disponible && !*p->id) desconocido = 1;
    }
    return desconocido;
}

/* Cada par conserva su avance. Confirmar dos veces la misma venta es seguro. */
static void *publicar(void *arg)
{
    Tienda *t = arg;
    const struct timespec espera = {0, 50000000};
    for (;;) {
        int hay_corte = 0;
        for (int i = 0; i < t->cantidad; i++) {
            Par *p = &t->pares[i];
            char r[LINEA], id[32];
            p->disponible = pedir(p->host, p->puerto, "ESTADO\n", r) &&
                            sscanf(r, "ESTADO %31s", id) == 1 && es_id(id);
            if (p->disponible) strcpy(p->id, id);
            else hay_corte = 1;
        }
        tomar_turno(t);
        size_t limite = t->usadas;
        dejar_turno(t);
        for (int i = 0; i < t->cantidad; i++) {
            Par *p = &t->pares[i];
            if (!p->disponible) continue;
            for (int paso = 0; paso < 16 && p->enviado < limite; paso++) {
                tomar_turno(t);
                Venta v = t->ventas[p->enviado];
                dejar_turno(t);
                if (strcmp(v.origen, t->id) == 0 && !enviar_venta(p, &v)) break;
                p->enviado++;
            }
            if (!hay_corte) continue;
            for (int paso = 0; paso < 16; paso++) {
                char orden[LINEA], r[LINEA];
                snprintf(orden, sizeof orden, "VENTA %zu\n", p->consultado + 1);
                if (!pedir(p->host, p->puerto, orden, r)) break;
                if (strcmp(r, "FIN") == 0) {
                    p->consultado = 0;
                    break;
                }
                Venta v;
                if (strncmp(r, "VENTA ", 6) || !leer_venta(r + 6, &v)) break;
                if (origen_caido(t, v.origen)) {
                    tomar_turno(t);
                    guardar(t, v, 1);
                    dejar_turno(t);
                }
                p->consultado++;
            }
        }
        nanosleep(&espera, NULL);
    }
    return NULL;
}

static void atender(Tienda *t, int fd)
{
    char linea[LINEA], r[LINEA];
    const char *respuesta = "ERROR SOLICITUD\n";
    if (!mensaje(fd, linea)) return;
    Venta v;
    long long n;
    tomar_turno(t);
    if (strcmp(linea, "ESTADO") == 0) {
        snprintf(r, sizeof r, "ESTADO %s %s %lld %lld\n", t->id,
                 t->total < t->meta ? "ABIERTA" : "CERRADA", t->propio, t->total);
        respuesta = r;
    } else if (strncmp(linea, "COMPRA ", 7) == 0 && positivo(linea + 7, LLONG_MAX, &n)) {
        if (t->total >= t->meta) respuesta = "ERROR CERRADA\n";
        else if (t->numero == LLONG_MAX) respuesta = "ERROR SECUENCIA\n";
        else {
            strcpy(v.origen, t->id);
            v.numero = t->numero + 1;
            v.monto = n;
            respuesta = guardar(t, v, 1);
        }
    } else if (t->cantidad && strncmp(linea, "DIFUNDE ", 8) == 0 &&
               leer_venta(linea + 8, &v) && strcmp(v.origen, t->id)) {
        respuesta = guardar(t, v, 1);
    } else if (t->cantidad && strncmp(linea, "VENTA ", 6) == 0 &&
               positivo(linea + 6, LLONG_MAX, &n)) {
        respuesta = "FIN\n";
        if ((unsigned long long)n <= t->usadas) {
            v = t->ventas[(size_t)n - 1];
            snprintf(r, sizeof r, "VENTA %s %lld %lld\n", v.origen, v.numero, v.monto);
            respuesta = r;
        }
    }
    dejar_turno(t);
    escribir(fd, respuesta, strlen(respuesta));
}

static int tienda(int argc, char **argv)
{
    Tienda t = {.archivo = -1};
    long long puerto;
    if (argc < 6 || !es_id(argv[2]) || !positivo(argv[3], 65535, &puerto) ||
        !positivo(argv[4], LLONG_MAX, &t.meta)) return 2;
    strcpy(t.id, argv[2]);
    t.cantidad = argc - 6;
    atomic_init(&t.entrada, 0);
    atomic_init(&t.salida, 0);
    if (t.cantidad) {
        t.pares = calloc((size_t)t.cantidad, sizeof *t.pares);
        if (!t.pares) return 1;
        for (int i = 0; i < t.cantidad; i++) {
            const char *texto = argv[i + 6];
            const char *dos = strrchr(texto, ':');
            if (!dos || dos == texto || dos - texto > 127 || strlen(dos + 1) > 5 ||
                !positivo(dos + 1, 65535, &puerto)) {
                free(t.pares);
                return 2;
            }
            memcpy(t.pares[i].host, texto, (size_t)(dos - texto));
            strcpy(t.pares[i].puerto, dos + 1);
        }
    }
    int fd = abrir_socket(NULL, argv[3]);
    pthread_t hilo;
    if (fd < 0 || !abrir_registro(&t, argv[5]) ||
        (t.cantidad && pthread_create(&hilo, NULL, publicar, &t))) {
        fprintf(stderr, "%s: error al iniciar puerto, registro o hilo\n", t.id);
        if (fd >= 0) close(fd);
        if (t.archivo >= 0) close(t.archivo);
        free(t.pares);
        free(t.ventas);
        return 1;
    }
    for (;;) {
        int otro = accept(fd, NULL, NULL);
        if (otro >= 0) {
            atender(&t, otro);
            close(otro);
        }
    }
}

static int cliente(int argc, char **argv)
{
    char orden[LINEA], r[LINEA];
    long long puerto, monto, numero;
    if (argc < 5 || !positivo(argv[3], 65535, &puerto)) return 2;
    if (argc == 5 && strcmp(argv[4], "ESTADO") == 0) strcpy(orden, "ESTADO\n");
    else if (argc == 6 && strcmp(argv[4], "COMPRA") == 0 &&
             positivo(argv[5], LLONG_MAX, &monto)) {
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
        fputs("blackout tienda ID PUERTO META REGISTRO [HOST:PUERTO ...]\n"
              "blackout cliente HOST PUERTO ESTADO | COMPRA MONTO\n", stderr);
    }
    return resultado;
}
