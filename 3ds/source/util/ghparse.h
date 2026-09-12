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

#endif /* PONG_GHPARSE_H */
