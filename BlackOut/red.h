#ifndef BLACKOUT_RED_H
#define BLACKOUT_RED_H

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LINEA 256
#define ESPERA 800

static int esperar_socket(int fd, short evento, int milisegundos)
{
    struct pollfd p = {fd, evento, 0};
    return poll(&p, 1, milisegundos);
}

/* TCP entrega un flujo de bytes. El salto de línea marca el final del mensaje. */
static int mensaje(int fd, char linea[LINEA])
{
    struct timespec inicio, ahora;
    if (clock_gettime(CLOCK_MONOTONIC, &inicio) != 0) return 0;
    for (size_t i = 0; i + 1 < LINEA; i++) {
        if (clock_gettime(CLOCK_MONOTONIC, &ahora) != 0) return 0;
        long usado = (ahora.tv_sec - inicio.tv_sec) * 1000 +
                     (ahora.tv_nsec - inicio.tv_nsec) / 1000000;
        if (usado >= ESPERA || esperar_socket(fd, POLLIN, ESPERA - (int)usado) <= 0) return 0;
        if (read(fd, &linea[i], 1) != 1 || !linea[i]) return 0;
        if (linea[i] == '\n') {
            linea[i] = '\0';
            return 1;
        }
    }
    return 0;
}

/* Con host hago connect. Sin host, abro el puerto que escucha la tienda. */
static int abrir_socket(const char *host, const char *puerto)
{
    struct addrinfo info = {0}, *a;
    info.ai_family = AF_INET;
    info.ai_socktype = SOCK_STREAM;
    info.ai_flags = host ? 0 : AI_PASSIVE;
    if (getaddrinfo(host, puerto, &info, &a) != 0) return -1;
    int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    bool ok = fd >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) == 0;
    if (ok && !host) {
        int si = 1;
        ok = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &si, sizeof si) == 0 &&
             bind(fd, a->ai_addr, a->ai_addrlen) == 0 && listen(fd, 20) == 0;
    } else if (ok && connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
        int error;
        socklen_t tam = sizeof error;
        ok = errno == EINPROGRESS && esperar_socket(fd, POLLOUT, ESPERA) > 0 &&
             getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &tam) == 0 && !error;
    }
    freeaddrinfo(a);
    if (ok) return fd;
    if (fd >= 0) close(fd);
    return -1;
}

#endif
