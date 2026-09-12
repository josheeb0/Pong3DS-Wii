#include "net_pc.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct PongNetPC {
    int  fd;
    bool closed;
};

PongNetPC *pong_pc_connect(const char *host, uint16_t port, uint32_t timeout_ms,
                           char *err, size_t errcap)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;        /* the server's raw path is v4 */
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || !res) {
        snprintf(err, errcap, "cannot resolve %s (%s)", host, gai_strerror(gai));
        return NULL;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        snprintf(err, errcap, "socket: %s", strerror(errno));
        freeaddrinfo(res);
        return NULL;
    }

    /* Same reasoning as the console: every frame here is tiny and latency
     * sensitive, so Nagle would batch inputs into a stutter. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    struct timeval tv;
    tv.tv_sec = (time_t)(timeout_ms / 1000);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        snprintf(err, errcap, "connect to %s:%u: %s", host, (unsigned)port, strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return NULL;
    }
    freeaddrinfo(res);

    /* Non-blocking from here: the render loop must never wait on the network. */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    PongNetPC *n = (PongNetPC *)calloc(1, sizeof *n);
    if (!n) { close(fd); snprintf(err, errcap, "out of memory"); return NULL; }
    n->fd = fd;
    return n;
}

void pong_pc_close(PongNetPC *n)
{
    if (!n) return;
    if (n->fd >= 0) close(n->fd);
    free(n);
}

int pong_pc_recv(PongNetPC *n, uint8_t *buf, size_t cap)
{
    if (!n || n->closed) return -1;
    ssize_t r = recv(n->fd, buf, cap, 0);
    if (r > 0) return (int)r;
    if (r == 0) { n->closed = true; return -1; }      /* the server hung up */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    n->closed = true;
    return -1;
}

bool pong_pc_send(PongNetPC *n, const uint8_t *buf, size_t len)
{
    if (!n || n->closed) return false;
    size_t sent = 0;
    while (sent < len) {
        ssize_t w = send(n->fd, buf + sent, len - sent, 0);
        if (w > 0) { sent += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* The socket buffer is full, which for frames this small means the
             * link is genuinely wedged. Dropping an input is correct -- they
             * are absolute positions, so the next one repairs it. */
            return false;
        }
        n->closed = true;
        return false;
    }
    return true;
}
