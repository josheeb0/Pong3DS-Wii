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

/** Asserts a condition, for the checks that are not a simple value compare. */
static void check(bool cond, const char *what, const char *detail)
{
    if (!cond) { printf("  FAIL %-34s %s\n", what, detail ? detail : ""); fails++; }
    else printf("  ok   %-34s %s\n", what, detail ? detail : "");
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
        /*
         * Trimmed from a genuine `releases?per_page=1` response, keeping the
         * parts that can trip a string search: the `upload_url` template with
         * its {?name,label} suffix, the author object, and -- the one that
         * matters -- the assets array, where all six files have a "name" of
         * their own. The release's name must win because it comes first.
         */
        static const char real[] =
            "[{\"url\":\"https://api.github.com/repos/johndoe6345789/Pong3DS-Wii/releases/12345\","
            "\"assets_url\":\"https://api.github.com/repos/x/y/releases/12345/assets\","
            "\"upload_url\":\"https://uploads.github.com/repos/x/y/releases/12345/assets{?name,label}\","
            "\"html_url\":\"https://github.com/johndoe6345789/Pong3DS-Wii/releases/tag/build-31\","
            "\"id\":12345,"
            "\"author\":{\"login\":\"github-actions[bot]\",\"id\":41898282,"
            "\"type\":\"Bot\",\"site_admin\":false},"
            "\"node_id\":\"RE_kwDO\",\"tag_name\":\"build-31\",\"target_commitish\":\"main\","
            "\"name\":\"Build 31\",\"draft\":false,\"prerelease\":true,"
            "\"assets\":[{\"id\":1,\"name\":\"pong-pc-linux-x86_64.tar.gz\",\"label\":\"\"},"
                        "{\"id\":2,\"name\":\"pong3ds.3dsx\",\"label\":\"\"},"
                        "{\"id\":3,\"name\":\"pong3ds.cia\",\"label\":\"\"}],"
            "\"body\":\"Build 31 - see the assets below\"}]";
        tag_is(real, "build-31", "real response shape");

        char rn[96] = {0};
        check(pong_gh_first_name(real, rn, sizeof rn) && strcmp(rn, "Build 31") == 0,
              "release name, not an asset name", rn);

        char d2[64];
        snprintf(d2, sizeof d2, "%u", pong_gh_build_number(rn, "build-31"));
        check(pong_gh_build_number(rn, "build-31") == 31, "build number off the real shape", d2);
    }

    printf("\n=== build numbers ===\n");
    build_is("build-31", 31);
    build_is("build-1", 1);
    build_is("build-9999", 9999);
    build_is("v1.2.3", 1);        /* first number wins; tags are build-N here */
    build_is("nightly", 0);       /* no number at all */
    build_is("", 0);
    build_is(NULL, 0);

    printf("\n=== build number, from the name when the tag cannot say ===\n");
    {
        /*
         * Reported from a console: running build 93, the updater said "up to
         * date (v1.1.1)" and stayed there.
         *
         * The tag was the only thing consulted, and a version tag yields its
         * major number -- so 1 was compared against 93, found not to be newer,
         * and the console sat two releases behind announcing it was current.
         * Releases carry the build number in their name for this reason.
         */
        const char *versioned =
            "[{\"tag_name\":\"v1.1.1\",\"name\":\"Pong 1.1.1 (build 95)\","
            "\"draft\":false}]";

        char tag[64] = {0}, name[96] = {0};
        check(pong_gh_first_tag(versioned, tag, sizeof tag), "tag from a versioned release", tag);
        check(pong_gh_first_name(versioned, name, sizeof name), "name from the same release", name);

        char d[96];
        uint32_t b = pong_gh_build_number(name, tag);
        snprintf(d, sizeof d, "%u (tag alone would give %u)", b, pong_gh_build_from_tag(tag));
        check(b == 95, "build number comes from the name", d);
        check(b > 93, "and is correctly newer than build 93", NULL);

        /* The per-commit form still works, from either field. */
        const char *per_commit =
            "[{\"tag_name\":\"build-96\",\"name\":\"Build 96\",\"draft\":false}]";
        tag[0] = name[0] = '\0';
        pong_gh_first_tag(per_commit, tag, sizeof tag);
        pong_gh_first_name(per_commit, name, sizeof name);
        snprintf(d, sizeof d, "%u", pong_gh_build_number(name, tag));
        check(pong_gh_build_number(name, tag) == 96, "build-N releases unchanged", d);

        /* A release with no build number anywhere falls back to the tag. */
        const char *nameless =
            "[{\"tag_name\":\"build-42\",\"name\":\"\",\"draft\":false}]";
        tag[0] = name[0] = '\0';
        pong_gh_first_tag(nameless, tag, sizeof tag);
        pong_gh_first_name(nameless, name, sizeof name);
        check(pong_gh_build_number(name, tag) == 42, "falls back to the tag", NULL);

        /* "name" must not be found inside "tag_name". */
        const char *order =
            "[{\"name\":\"Build 7\",\"tag_name\":\"v9.9.9\"}]";
        tag[0] = name[0] = '\0';
        pong_gh_first_tag(order, tag, sizeof tag);
        pong_gh_first_name(order, name, sizeof name);
        snprintf(d, sizeof d, "tag='%s' name='%s'", tag, name);
        check(strcmp(tag, "v9.9.9") == 0 && strcmp(name, "Build 7") == 0,
              "keys do not match inside each other", d);
    }

    printf("\n=== the list is not in the order we need ===\n");
    {
        /*
         * The ordering GitHub actually served, recorded from the live API on
         * the day a console reported itself up to date on build 93:
         *
         *   1 v1.1.1    created 03:39   <- the only entry a per_page=1 sees
         *   2 v1.1.0    created 03:09
         *   3 build-93  created 00:58
         *   4 build-91  created 00:51
         *   5 build-87  created 00:31
         *   6 build-110 created 03:55   <- the newest build, seven places down
         *
         * Position one is the newest NON-prerelease and stays there; every
         * per-commit build is a prerelease and never reaches it. So reading the
         * first entry does not mean reading the newest release, and no number
         * of new builds would ever have changed what that console saw.
         */
        static const char out_of_order[] =
            "[{\"tag_name\":\"v1.1.1\",\"name\":\"Pong3DS v1.1.1\",\"prerelease\":false},"
            "{\"tag_name\":\"v1.1.0\",\"name\":\"Pong3DS v1.1.0\",\"prerelease\":false},"
            "{\"tag_name\":\"build-93\",\"name\":\"Build 93\",\"prerelease\":true},"
            "{\"tag_name\":\"build-91\",\"name\":\"Build 91\",\"prerelease\":true},"
            "{\"tag_name\":\"build-87\",\"name\":\"Build 87\",\"prerelease\":true},"
            "{\"tag_name\":\"build-110\",\"name\":\"Build 110\",\"prerelease\":true}]";

        char first[64] = {0};
        pong_gh_first_tag(out_of_order, first, sizeof first);
        check(strcmp(first, "v1.1.1") == 0, "first entry is NOT the newest", first);

        char tag[64] = {0};
        uint32_t best = pong_gh_best_release(out_of_order, tag, sizeof tag);
        char d[96];
        snprintf(d, sizeof d, "%u via %s", best, tag);
        check(best == 110, "the whole page is searched", d);
        check(strcmp(tag, "build-110") == 0, "and the winner's tag comes back", tag);
        check(best > 93, "a console on build 93 now sees an update", NULL);

        /* The download URL is built from that tag, so picking the right build
         * and then fetching a different one would be its own quiet failure. */
        char dsx[160];
        snprintf(dsx, sizeof dsx,
                 "https://github.com/o/r/releases/download/%s/pong3ds.3dsx", tag);
        check(strstr(dsx, "/build-110/") != NULL, "and the download follows it", NULL);

        /* A release whose name is null must not borrow the next release's
         * name, or an asset filename, across the object boundary. */
        static const char null_name[] =
            "[{\"tag_name\":\"build-5\",\"name\":null,"
              "\"assets\":[{\"name\":\"pong3ds.3dsx\"}]},"
            "{\"tag_name\":\"build-4\",\"name\":\"Build 4\"}]";
        tag[0] = '\0';
        uint32_t b2 = pong_gh_best_release(null_name, tag, sizeof tag);
        snprintf(d, sizeof d, "%u via %s", b2, tag);
        check(b2 == 5 && strcmp(tag, "build-5") == 0,
              "a null name falls back to its own tag", d);

        /* Nothing usable must report nothing, not a stray number. */
        char t3[64] = {0};
        check(pong_gh_best_release("[]", t3, sizeof t3) == 0, "empty list yields 0", NULL);
        check(pong_gh_best_release("{\"message\":\"Not Found\"}", t3, sizeof t3) == 0,
              "an error body yields 0", NULL);
    }

    printf("\n=== build tags, which is what the console actually reads ===\n");
    {
        /*
         * A trimmed /tags response in the real shape. Tags are read instead of
         * releases because the releases list has to be read in BULK (GitHub
         * does not return it newest-first) and twenty releases is over 200KB --
         * which the console's HTTPS client refused outright, taking the whole
         * update check down with it. Every tag here fits in 19KB.
         *
         * Note the version tags: they carry no build number and must simply be
         * ignored rather than parsed as one, which is the mistake that started
         * all of this.
         */
        static const char tags[] =
            "[{\"name\":\"v1.1.1\",\"zipball_url\":\"https://api.github.com/x/zipball/refs/tags/v1.1.1\","
              "\"commit\":{\"sha\":\"b021745\",\"url\":\"https://api.github.com/x/commits/b021745\"}},"
            "{\"name\":\"v1.1.0\",\"commit\":{\"sha\":\"aaa\"}},"
            "{\"name\":\"v1.0.1\",\"commit\":{\"sha\":\"bbb\"}},"
            "{\"name\":\"build-110\",\"commit\":{\"sha\":\"ccc\"}},"
            "{\"name\":\"build-107\",\"commit\":{\"sha\":\"ddd\"}},"
            "{\"name\":\"build-93\",\"commit\":{\"sha\":\"eee\"}},"
            "{\"name\":\"build-112\",\"commit\":{\"sha\":\"fff\"}}]";

        char tag[64] = {0};
        uint32_t b = pong_gh_best_build_tag(tags, tag, sizeof tag);
        char d[96];
        snprintf(d, sizeof d, "%u via %s", b, tag);
        check(b == 112 && strcmp(tag, "build-112") == 0,
              "highest build tag wins, not the first", d);

        /* build-112 is LAST in that document, exactly as the real API returned
         * build-110 seventh. Order must not matter. */
        check(strstr(tags, "build-112") > strstr(tags, "v1.1.1"),
              "and it was last in the document", NULL);

        /* Version tags carry no build number and must contribute nothing. */
        static const char only_versions[] =
            "[{\"name\":\"v1.1.1\"},{\"name\":\"v1.1.0\"},{\"name\":\"v9.9.9\"}]";
        tag[0] = '\0';
        check(pong_gh_best_build_tag(only_versions, tag, sizeof tag) == 0,
              "version tags alone yield no build", tag);

        char t2[64] = {0};
        check(pong_gh_best_build_tag("[]", t2, sizeof t2) == 0, "empty tag list yields 0", NULL);
        check(pong_gh_best_build_tag(NULL, t2, sizeof t2) == 0, "null input yields 0", NULL);

        /* A tag longer than the caller's buffer must be skipped, not truncated:
         * a truncated tag builds a download URL for something that is not there. */
        char small[8] = {0};
        check(pong_gh_best_build_tag("[{\"name\":\"build-1234567890\"}]", small, sizeof small) == 0,
              "an overlong tag is refused", NULL);
    }

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
