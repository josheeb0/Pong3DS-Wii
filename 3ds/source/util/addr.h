/*
 * Server address entry, in the style of Minecraft's Direct Connect: one field,
 * sensible defaults for anything omitted. See addr.c for accepted forms.
 */

#ifndef PONG_ADDR_H
#define PONG_ADDR_H

#include <stdbool.h>
#include <stddef.h>
#include "net.h"

/** Longest address we will accept, including scheme and port. */
#define PONG_ADDR_MAX 128

/**
 * Parses an address into `net`, filling in defaults.
 *
 * Returns false with a human-readable reason in `err` -- which is shown on the
 * bottom screen, so it needs to be short and specific.
 */
bool pong_addr_parse(const char *input, PongNetConfig *net, char *err, size_t errcap);

/** Renders the current config back to the shortest form that round-trips. */
void pong_addr_format(const PongNetConfig *net, char *out, size_t cap);

/** Opens the system software keyboard. Returns false if cancelled. */
bool pong_addr_prompt(char *buf, size_t cap, const char *initial);

#endif /* PONG_ADDR_H */
