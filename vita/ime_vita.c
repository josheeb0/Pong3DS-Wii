#include "ime_vita.h"

#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <string.h>

/* The IME speaks UTF-16. Everything above it speaks ASCII, and a player name
 * or an IP address is ASCII by nature, so the conversion is a widening rather
 * than an encoder. Anything outside ASCII is dropped instead of mangled. */
static void to_utf16(const char *in, uint16_t *out, size_t cap)
{
    size_t i = 0;
    if (in) {
        for (; in[i] && i + 1 < cap; i++) {
            out[i] = (uint16_t)(unsigned char)in[i];
        }
    }
    out[i] = 0;
}

static void from_utf16(const uint16_t *in, char *out, size_t cap)
{
    size_t i = 0;
    for (; in[i] && i + 1 < cap; i++) {
        out[i] = (in[i] < 0x80) ? (char)in[i] : '?';
    }
    out[i] = '\0';
}

#define IME_MAX 64

static uint16_t s_title[IME_MAX];
static uint16_t s_initial[IME_MAX];
static uint16_t s_buffer[IME_MAX + 1];
static bool     s_active = false;

bool pong_ime_open(const char *title, const char *initial, unsigned max_len)
{
    if (s_active) return false;
    if (max_len == 0 || max_len > IME_MAX) max_len = IME_MAX;

    to_utf16(title, s_title, IME_MAX);
    to_utf16(initial, s_initial, IME_MAX);
    memset(s_buffer, 0, sizeof s_buffer);

    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0;                 /* whatever the system offers */
    p.languagesForced    = SCE_TRUE;
    p.type               = SCE_IME_TYPE_BASIC_LATIN;   /* names and addresses */
    p.title              = s_title;
    p.maxTextLength      = max_len;
    p.initialText        = s_initial;
    p.inputTextBuffer    = s_buffer;

    if (sceImeDialogInit(&p) < 0) return false;
    s_active = true;
    return true;
}

bool pong_ime_active(void) { return s_active; }

PongImeResult pong_ime_poll(char *out, size_t cap)
{
    if (!s_active) return PONG_IME_CANCELLED;

    SceCommonDialogStatus st = sceImeDialogGetStatus();
    if (st != SCE_COMMON_DIALOG_STATUS_FINISHED) return PONG_IME_PENDING;

    SceImeDialogResult r;
    memset(&r, 0, sizeof r);
    sceImeDialogGetResult(&r);

    bool accepted = (r.button == SCE_IME_DIALOG_BUTTON_ENTER);
    if (accepted && out && cap > 0) from_utf16(s_buffer, out, cap);

    sceImeDialogTerm();
    s_active = false;
    return accepted ? PONG_IME_ACCEPTED : PONG_IME_CANCELLED;
}
