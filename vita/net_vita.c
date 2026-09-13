/*
 * TCP for the Vita, over SceNet.
 *
 * Same interface as the desktop's net_pc.h so the game loop above it does not
 * care which it is talking to. The Vita does not have BSD sockets: SceNet is
 * the same shape with its own names, its own constants and an explicit memory
 * pool, so this is a translation rather than a port.
 *
 * Like the desktop, this speaks raw TCP only. The HTTPS path exists for the
 * 3DS, which needs to reach a server through a tunnel from someone's living
 * room; a Vita on the same network as the server does not.
 */

#include "net_pc.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct PongNetPC {
    int  fd;
    bool closed;
};

/* SceNet wants a pool it owns for the life of the process. 64KB is the size
 * the samples use and is ample for one socket carrying 24-byte frames. */
#define NET_POOL_BYTES (64 * 1024)
static char s_net_pool[NET_POOL_BYTES];
static bool s_net_ready = false;

/** Brings the networking stack up once. Safe to call repeatedly. */
static bool net_start(char *err, size_t errcap)
{
    if (s_net_ready) return true;

    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) {
        snprintf(err, errcap, "could not load the net module");
        return false;
    }

    SceNetInitParam param;
    param.memory = s_net_pool;
    param.size   = NET_POOL_BYTES;
    param.flags  = 0;

    int rc = sceNetInit(&param);
    /* Already initialised is success, not failure: something else on the
     * system may have got there first. */
    if (rc < 0 && rc != (int)SCE_NET_ERROR_EBUSY) {
        snprintf(err, errcap, "sceNetInit failed (0x%08x)", (unsigned)rc);
        return false;
    }

    sceNetCtlInit();
    s_net_ready = true;
    return true;
}

PongNetPC *pong_pc_connect(const char *host, uint16_t port, uint32_t timeout_ms,
                           char *err, size_t errcap)
{
    (void)timeout_ms;   /* SceNet connect blocks; the socket goes non-blocking after */

    if (!net_start(err, errcap)) return NULL;

    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port   = sceNetHtons(port);

    /*
     * An address only, no name resolution.
     *
     * The Vita can resolve names through sceNetResolver, but the thing this
     * connects to is a server on the same network and is configured by IP --
     * and a .local name would not resolve here either, for the same reason it
     * does not on the 3DS.
     */
    if (sceNetInetPton(SCE_NET_AF_INET, host, &addr.sin_addr) <= 0) {
        snprintf(err, errcap, "'%s' is not an IP address", host);
        return NULL;
    }

    int fd = sceNetSocket("pong", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errcap, "socket failed (0x%08x)", (unsigned)fd);
        return NULL;
    }

    /* Every frame here is tiny and latency sensitive; Nagle would batch inputs
     * into a stutter. Same reasoning as the console and the desktop. */
    int one = 1;
    sceNetSetsockopt(fd, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof one);

    int rc = sceNetConnect(fd, (SceNetSockaddr *)&addr, sizeof addr);
    if (rc < 0) {
        snprintf(err, errcap, "connect to %s:%u failed (0x%08x)",
                 host, (unsigned)port, (unsigned)rc);
        sceNetSocketClose(fd);
        return NULL;
    }

    /* Non-blocking from here: the render loop must never wait on the network. */
    int nb = 1;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nb, sizeof nb);

    PongNetPC *n = (PongNetPC *)calloc(1, sizeof *n);
    if (!n) {
        sceNetSocketClose(fd);
        snprintf(err, errcap, "out of memory");
        return NULL;
    }
    n->fd = fd;
    return n;
}

void pong_pc_close(PongNetPC *n)
{
    if (!n) return;
    if (n->fd >= 0) sceNetSocketClose(n->fd);
    free(n);
}

int pong_pc_recv(PongNetPC *n, uint8_t *buf, size_t cap)
{
    if (!n || n->closed) return -1;
    int r = sceNetRecv(n->fd, buf, cap, 0);
    if (r > 0) return r;
    if (r == 0) { n->closed = true; return -1; }        /* the server hung up */
    if (r == (int)SCE_NET_ERROR_EWOULDBLOCK) return 0;  /* nothing waiting */
    n->closed = true;
    return -1;
}

bool pong_pc_send(PongNetPC *n, const uint8_t *buf, size_t len)
{
    if (!n || n->closed) return false;
    size_t sent = 0;
    while (sent < len) {
        int w = sceNetSend(n->fd, buf + sent, len - sent, 0);
        if (w > 0) { sent += (size_t)w; continue; }
        if (w == (int)SCE_NET_ERROR_EWOULDBLOCK) {
            /* The socket buffer is full, which for frames this small means the
             * link is wedged. Dropping an input is correct: they are absolute
             * positions, so the next one repairs it. */
            return false;
        }
        n->closed = true;
        return false;
    }
    return true;
}
