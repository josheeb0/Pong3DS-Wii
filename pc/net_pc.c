/*
 * getaddrinfo and struct addrinfo are POSIX, not ISO C, and glibc hides them
 * under -std=c99 unless asked. macOS's headers expose them anyway, so this
 * built there and failed on Linux with "storage size of 'hints' isn't known".
 * Declared here rather than as a compiler flag, so the requirement travels
 * with the file that has it.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
  #define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__)
  #define _DARWIN_C_SOURCE          /* macOS hides some of it the other way */
#endif

#include "net_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Windows does not have BSD sockets.
 *
 * The shape is close enough that one set of calls covers both once the names
 * and the error reporting are reconciled: Winsock needs an explicit startup,
 * closes with closesocket(), sets non-blocking with ioctlsocket() rather than
 * fcntl(), and reports through WSAGetLastError() instead of errno. The macros
 * below are the whole difference.
 */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALID    INVALID_SOCKET
  #define sock_close      closesocket
  #define sock_errno()    WSAGetLastError()
  #define SOCK_WOULDBLOCK WSAEWOULDBLOCK
  typedef int socklen_like;
#else
  #include <errno.h>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  typedef int sock_t;
  #define SOCK_INVALID    (-1)
  #define sock_close      close
  #define sock_errno()    errno
  #define SOCK_WOULDBLOCK EAGAIN
  typedef socklen_t socklen_like;
#endif

/* Winsock's strerror equivalent is awkward; a number is honest and greppable. */
static void sock_err_str(int e, char *out, size_t cap)
{
#ifdef _WIN32
    snprintf(out, cap, "winsock error %d", e);
#else
    snprintf(out, cap, "%s", strerror(e));
#endif
}

struct PongNetPC {
    sock_t fd;
    bool   closed;
};

PongNetPC *pong_pc_connect(const char *host, uint16_t port, uint32_t timeout_ms,
                           char *err, size_t errcap)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);

#ifdef _WIN32
    /* Once per process; Winsock refuses every call before this. Idempotent
     * enough that doing it on each connect is simpler than tracking it. */
    static bool wsa_started = false;
    if (!wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            snprintf(err, errcap, "WSAStartup failed");
            return NULL;
        }
        wsa_started = true;
    }
#endif

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;        /* the server's raw path is v4 */
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || !res) {
        snprintf(err, errcap, "cannot resolve %s (%s)", host, gai_strerror(gai));
        return NULL;
    }

    sock_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == SOCK_INVALID) {
        char e[96];
        sock_err_str(sock_errno(), e, sizeof e);
        snprintf(err, errcap, "socket: %s", e);
        freeaddrinfo(res);
        return NULL;
    }

    /* Same reasoning as the console: every frame here is tiny and latency
     * sensitive, so Nagle would batch inputs into a stutter. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);

#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;             /* Winsock takes plain ms */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
#else
    struct timeval tv;
    tv.tv_sec = (time_t)(timeout_ms / 1000);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000) * 1000);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif

    if (connect(fd, res->ai_addr, (socklen_like)res->ai_addrlen) != 0) {
        char e[96];
        sock_err_str(sock_errno(), e, sizeof e);
        snprintf(err, errcap, "connect to %s:%u: %s", host, (unsigned)port, e);
        sock_close(fd);
        freeaddrinfo(res);
        return NULL;
    }
    freeaddrinfo(res);

    /* Non-blocking from here: the render loop must never wait on the network. */
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif

    PongNetPC *n = (PongNetPC *)calloc(1, sizeof *n);
    if (!n) { sock_close(fd); snprintf(err, errcap, "out of memory"); return NULL; }
    n->fd = fd;
    return n;
}

void pong_pc_close(PongNetPC *n)
{
    if (!n) return;
    if (n->fd != SOCK_INVALID) sock_close(n->fd);
    free(n);
}

int pong_pc_recv(PongNetPC *n, uint8_t *buf, size_t cap)
{
    if (!n || n->closed) return -1;
    int r = (int)recv(n->fd, (char *)buf, (int)cap, 0);
    if (r > 0) return r;
    if (r == 0) { n->closed = true; return -1; }      /* the server hung up */
    {
        int e = sock_errno();
        if (e == SOCK_WOULDBLOCK
#ifndef _WIN32
            || e == EWOULDBLOCK
#endif
           ) return 0;
    }
    n->closed = true;
    return -1;
}

bool pong_pc_send(PongNetPC *n, const uint8_t *buf, size_t len)
{
    if (!n || n->closed) return false;
    size_t sent = 0;
    while (sent < len) {
        int w = (int)send(n->fd, (const char *)(buf + sent), (int)(len - sent), 0);
        if (w > 0) { sent += (size_t)w; continue; }
        int e = sock_errno();
        if (w < 0 && (e == SOCK_WOULDBLOCK
#ifndef _WIN32
                      || e == EWOULDBLOCK
#endif
                     )) {
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
