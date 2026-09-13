#ifndef PONG_LANSHAPE_H
#define PONG_LANSHAPE_H

#include <stdbool.h>

/**
 * Whether an address looks like something on this network.
 *
 * Used to decide whether a bare address may assume the raw TCP port. It is
 * deliberately SHAPE-based rather than a lookup: the question is what the user
 * meant, and it has to be answered while they are typing, without a DNS round
 * trip that might hang.
 *
 * Getting it wrong costs a typed port, never a wrong connection -- a
 * LAN-shaped address that is not actually on the LAN simply fails to connect
 * the way it would have anyway.
 *
 * In its own file so the host test links THIS function rather than a copy of
 * it. A copied helper and its test agree with each other forever, including
 * after the original changes.
 */
bool pong_lan_shaped(const char *host);

#endif /* PONG_LANSHAPE_H */
