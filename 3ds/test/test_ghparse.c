/*
 * Tests for the GitHub release parsing.
 *
 * It is a string search rather than a JSON parser, which is the right call for
 * a console with no JSON library -- but string searches fail in quiet ways, so
 * the edge cases are pinned here: whitespace around the colon, the key
 * appearing inside a value, absent or malformed input, and tags that carry no
 * number.
 *
 * Includes a fragment of a REAL API response, because the shape of the document
 * is the thing most likely to change underneath us.
 */

#include <stdio.h>
#include <string.h>
#include "../source/util/ghparse.h"
#include "../source/util/update.h"

static int fails = 0;

static void tag_is(const char *json, const char *want, const char *what)
{
    char got[64] = {0};
    bool ok = pong_gh_first_tag(json, got, sizeof got);
    if (want == NULL) {
        if (ok) { printf("  FAIL %-34s accepted, expected rejection (got '%s')\n", what, got); fails++; }
        else printf("  ok   %-34s rejected\n", what);
        return;
    }
    if (!ok || strcmp(got, want) != 0) {
        printf("  FAIL %-34s got '%s', want '%s'\n", what, ok ? got : "(none)", want);
        fails++;
        return;
    }
    printf("  ok   %-34s -> %s\n", what, got);
}

static void build_is(const char *tag, uint32_t want)
{
    uint32_t got = pong_gh_build_from_tag(tag);
    if (got != want) {
        printf("  FAIL build_from_tag(\"%s\") = %u, want %u\n", tag ? tag : "(null)", got, want);
        fails++;
    } else {
        printf("  ok   build_from_tag(%-12s) -> %u\n", tag ? tag : "(null)", got);
    }
}

int main(void)
{
    printf("=== tag extraction ===\n");

    tag_is("[{\"tag_name\":\"build-31\"}]", "build-31", "compact");
    tag_is("[{\"tag_name\": \"build-31\"}]", "build-31", "space after colon");
    tag_is("[{\n  \"tag_name\" : \"build-7\"\n}]", "build-7", "newlines and spaces");

    /* The API returns newest first, so the FIRST tag is the one we want --
     * this is the property the whole approach rests on. */
    tag_is("[{\"tag_name\":\"build-31\"},{\"tag_name\":\"build-30\"}]",
           "build-31", "first of several");

    /* Fields that appear BEFORE tag_name must not be mistaken for it. */
    tag_is("[{\"url\":\"https://x/tag_name\",\"tag_name\":\"build-9\"}]",
           "build-9", "key-like text in an earlier value");

    tag_is("[]", NULL, "empty array");
    tag_is("{\"message\":\"Not Found\"}", NULL, "API error body");
    tag_is("[{\"tag_name\":}]", NULL, "malformed, no value");
    tag_is("[{\"tag_name\":\"\"}]", NULL, "empty tag");
    tag_is("", NULL, "empty input");

    {   /* Longer than the caller's buffer must be refused, not truncated: a
         * truncated tag would build a download URL for a release that does not
         * exist. */
        char small[8] = {0};
        bool ok = pong_gh_first_tag("[{\"tag_name\":\"build-1234567890\"}]", small, sizeof small);
        if (ok) { printf("  FAIL overlong tag was accepted into a small buffer\n"); fails++; }
        else printf("  ok   overlong tag rejected rather than truncated\n");
    }

    printf("\n=== a real API response fragment ===\n");
    {
        static const char real[] =
            "[{\"url\":\"https://api.github.com/repos/johndoe6345789/Pong3DS-Wii/releases/12345\","
            "\"assets_url\":\"https://api.github.com/repos/x/y/releases/12345/assets\","
            "\"html_url\":\"https://github.com/johndoe6345789/Pong3DS-Wii/releases/tag/build-31\","
            "\"id\":12345,\"tag_name\":\"build-31\",\"target_commitish\":\"main\","
            "\"name\":\"Build 31\",\"draft\":false,\"prerelease\":true}]";
        tag_is(real, "build-31", "real response shape");
    }

    printf("\n=== build numbers ===\n");
    build_is("build-31", 31);
    build_is("build-1", 1);
    build_is("build-9999", 9999);
    build_is("v1.2.3", 1);        /* first number wins; tags are build-N here */
    build_is("nightly", 0);       /* no number at all */
    build_is("", 0);
    build_is(NULL, 0);

    printf("\n=== update target presets ===\n");
    {
        /* The upstream must be first: it is the shipped default, and a default
         * pointing at a personal fork would have every console tracking one
         * contributor. */
        const PongUpdateTarget *first = &PONG_UPDATE_TARGETS[0];
        if (first->source != PONG_UPDATE_SRC_GITHUB ||
            strcmp(first->owner, "josheeb0") != 0) {
            printf("  FAIL first preset is %s, expected GitHub josheeb0\n", first->label);
            fails++;
        } else {
            printf("  ok   first preset is the upstream (%s)\n", first->label);
        }

        /* Every preset must be findable, or cycling would skip entries. */
        for (int i = 0; i < PONG_UPDATE_TARGET_COUNT; i++) {
            const PongUpdateTarget *t = &PONG_UPDATE_TARGETS[i];
            int idx = pong_update_target_index(t->source, t->owner, t->repo);
            if (idx != i) {
                printf("  FAIL preset %d (%s) resolved to %d\n", i, t->label, idx);
                fails++;
            }
        }
        printf("  ok   all %d presets resolve to themselves\n", PONG_UPDATE_TARGET_COUNT);

        /* A hand-edited config matching no preset must report -1, so the menu
         * starts the cycle from the top rather than rewriting it to a neighbour. */
        int custom = pong_update_target_index(PONG_UPDATE_SRC_GITHUB, "someone", "else");
        if (custom != -1) { printf("  FAIL custom repo resolved to %d\n", custom); fails++; }
        else printf("  ok   a custom owner/repo is not mistaken for a preset\n");

        /* Cycling must visit every preset exactly once before repeating. */
        int seen[8] = {0};
        int idx = 0;
        for (int n = 0; n < PONG_UPDATE_TARGET_COUNT; n++) {
            seen[idx]++;
            idx = (idx + 1) % PONG_UPDATE_TARGET_COUNT;
        }
        int bad = 0;
        for (int i = 0; i < PONG_UPDATE_TARGET_COUNT; i++) if (seen[i] != 1) bad++;
        if (bad) { printf("  FAIL cycling visited %d preset(s) wrongly\n", bad); fails++; }
        else printf("  ok   cycling visits each preset exactly once\n");

        char buf[80];
        pong_update_target_label(PONG_UPDATE_SRC_GITHUB, "someone", "else", buf, sizeof buf);
        if (strstr(buf, "someone/else") == NULL) {
            printf("  FAIL custom label was '%s', expected the owner/repo\n", buf);
            fails++;
        } else {
            printf("  ok   a custom target names itself: %s\n", buf);
        }
    }

    printf("\n");
    if (fails) { printf("FAILED: %d check(s)\n", fails); return 1; }
    printf("PASSED: GitHub release parsing and update targets\n");
    return 0;
}
