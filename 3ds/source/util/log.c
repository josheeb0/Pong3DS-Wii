/*
 * Diagnostic log, written to the SD card.
 *
 * Exists because the feedback loop on a console is brutal: the screen is 320px
 * wide, there is no debugger attached, and every question costs a round trip
 * through a person carrying a handheld to a computer. So when something fails,
 * the log should already contain everything needed to diagnose it -- system
 * state, configuration, and a timestamped trace of every network stage with its
 * result and duration -- rather than the one line whoever wrote the code
 * happened to think of.
 *
 * Deliberately verbose. A few KB of text is free; another round trip is not.
 */

#include "log.h"

#include <3ds.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static FILE *g_log = NULL;
static uint64_t g_t0 = 0;

/* Past this the file is rotated, so a console left running does not slowly fill
 * the SD card while still keeping the previous session for comparison. */
#define LOG_MAX_BYTES (256 * 1024)

static void rotate_if_large(void)
{
    struct stat st;
    if (stat(PONG_LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        remove(PONG_LOG_PATH_OLD);
        rename(PONG_LOG_PATH, PONG_LOG_PATH_OLD);
    }
}

void pong_log_open(void)
{
    if (g_log) return;
    rotate_if_large();
    g_log = fopen(PONG_LOG_PATH, "a");
    g_t0 = osGetTime();
}

void pong_log_close(void)
{
    if (!g_log) return;
    fclose(g_log);
    g_log = NULL;
}

void pong_log(const char *fmt, ...)
{
    if (!g_log) return;
    /* Milliseconds since this session started: relative time is what makes a
     * slow stage obvious, and the console's wall clock is often wrong anyway. */
    fprintf(g_log, "[%6llu] ", (unsigned long long)(osGetTime() - g_t0));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    /* Flushed every line on purpose: if the next thing that happens is a crash
     * or a hang, an unflushed buffer means the log stops exactly where it
     * became interesting. */
    fflush(g_log);
}

void pong_log_section(const char *title)
{
    if (!g_log) return;
    fprintf(g_log, "\n=== %s ===\n", title);
    fflush(g_log);
}

void pong_log_system(uint32_t build_id, int protocol_version)
{
    pong_log_section("session start");

    pong_log("build          : %lu (protocol v%d)", (unsigned long)build_id, protocol_version);

    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    pong_log("model          : %s", isNew ? "New 3DS" : "Old 3DS");

    u32 fw = osGetFirmVersion();
    u32 kern = osGetKernelVersion();
    pong_log("firm           : %lu.%lu.%lu   kernel %lu.%lu.%lu",
             (unsigned long)GET_VERSION_MAJOR(fw), (unsigned long)GET_VERSION_MINOR(fw),
             (unsigned long)GET_VERSION_REVISION(fw),
             (unsigned long)GET_VERSION_MAJOR(kern), (unsigned long)GET_VERSION_MINOR(kern),
             (unsigned long)GET_VERSION_REVISION(kern));

    pong_log("wifi strength  : %u/3", (unsigned)osGetWifiStrength());
    pong_log("app mem free   : %lu KB", (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024));
    pong_log("linear free    : %lu KB", (unsigned long)(linearSpaceFree() / 1024));
}

void pong_log_network_identity(void)
{
    struct in_addr me;
    me.s_addr = (in_addr_t)gethostid();
    const char *ip = inet_ntoa(me);
    pong_log("console IP     : %s", ip ? ip : "(none)");

    struct in_addr host, mask, bcast;
    if (SOCU_GetIPInfo(&host, &mask, &bcast) == 0) {
        /* inet_ntoa returns a shared static buffer, so each value has to be
         * copied out before the next call overwrites it. */
        char h[20], m[20], b[20];
        snprintf(h, sizeof h, "%s", inet_ntoa(host));
        snprintf(m, sizeof m, "%s", inet_ntoa(mask));
        snprintf(b, sizeof b, "%s", inet_ntoa(bcast));
        pong_log("netmask        : %s   broadcast %s   (host %s)", m, b, h);
    }
}
