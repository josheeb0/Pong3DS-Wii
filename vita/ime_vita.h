/*
 * Text entry on the Vita, through the system IME.
 *
 * The equivalent of the 3DS's swkbd and the desktop's SDL text input: the
 * platform owns the keyboard, so the platform layer owns this. The IME is
 * modal and spans frames -- it is opened, then polled until the player accepts
 * or cancels -- so it cannot be a blocking call the way swkbd is.
 */

#ifndef PONG_IME_VITA_H
#define PONG_IME_VITA_H

#include <stdbool.h>
#include <stddef.h>

/** Opens the keyboard. `initial` may be NULL. Returns false if it would not open. */
bool pong_ime_open(const char *title, const char *initial, unsigned max_len);

/** True while the keyboard is on screen and needs pong_ime_poll(). */
bool pong_ime_active(void);

typedef enum {
    PONG_IME_PENDING = 0,   /* still open */
    PONG_IME_ACCEPTED,      /* the player confirmed; `out` holds the text */
    PONG_IME_CANCELLED,
} PongImeResult;

/** Call once a frame while active. Fills `out` on PONG_IME_ACCEPTED. */
PongImeResult pong_ime_poll(char *out, size_t cap);

#endif /* PONG_IME_VITA_H */
