/*
 * The two pieces of GitHub-response parsing, kept free of libctru so they can
 * be tested on the host like the codec and the address parser.
 *
 * Deliberately not a JSON parser. The console has no JSON library and writing
 * one to extract a single field from a document we do not control would be
 * several hundred lines of parsing and allocation for no benefit. The API
 * returns releases newest-first, so the first "tag_name" is the answer.
 */

#ifndef PONG_GHPARSE_H
#define PONG_GHPARSE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** Extracts the first "tag_name" value. False if absent or too long for `cap`. */
bool pong_gh_first_tag(const char *json, char *out, size_t cap);

/** "build-31" -> 31. Returns 0 when the tag carries no number. */
uint32_t pong_gh_build_from_tag(const char *tag);

/** The first release's `name` field, alongside its tag. */
bool pong_gh_first_name(const char *json, char *out, size_t cap);

/**
 * The build number of a release, from its name if it says one and its tag
 * otherwise.
 *
 * The tag alone is not enough. Per-commit releases are tagged `build-93` and
 * parse cleanly, but a version tag like `v1.1.1` yields 1 -- so a console on
 * build 93 compared 1 against 93, decided it was newer, and reported itself up
 * to date while sitting two releases behind. Releases carry "build N" in their
 * name for exactly this reason.
 */
uint32_t pong_gh_build_number(const char *name, const char *tag);

/**
 * The highest build number across every release in the document, and the tag
 * that carries it. Returns 0 when the document holds no usable release.
 *
 * Use this rather than reading the first release: GitHub does not return the
 * list newest-first. A newly published build-110 was served at position seven,
 * behind a build-87 four hours older, while position one stayed on the newest
 * non-prerelease -- so a console reading only the first entry never sees a new
 * build at all.
 */
uint32_t pong_gh_best_release(const char *json, char *out_tag, size_t cap);

#endif /* PONG_GHPARSE_H */
