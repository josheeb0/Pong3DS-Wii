/*
 * Diagnostic log on the SD card. See log.c for why it is deliberately verbose.
 *
 * Safe to call before pong_log_open() -- everything is a no-op until the file
 * is open, so logging can be added anywhere without ordering worries.
 */

#ifndef PONG_LOG_H
#define PONG_LOG_H

#include <stdint.h>

#define PONG_LOG_PATH     "sdmc:/3ds/pong3ds.log"
#define PONG_LOG_PATH_OLD "sdmc:/3ds/pong3ds.log.old"

void pong_log_open(void);
void pong_log_close(void);

/** printf into the log, timestamped with ms since session start. */
void pong_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/** A visual break, so a long file stays scannable. */
void pong_log_section(const char *title);

/** Console model, firmware, wifi strength, free memory. */
void pong_log_system(uint32_t build_id, int protocol_version);

/** IP, netmask, broadcast -- answers "is it even on the network I think". */
void pong_log_network_identity(void);

#endif /* PONG_LOG_H */
