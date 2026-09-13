#ifndef PONG_VERSION_H
#define PONG_VERSION_H

/*
 * The human-facing version, and the build number behind it.
 *
 * Two numbers because they answer different questions. The VERSION is what a
 * release is called and what a player recognises -- "1.1.1". The BUILD is what
 * the updater compares, and it has to be monotonic, which a version number is
 * not: v1.1.1 sorts below v1.2 but its first digit run is 1, and reading a
 * version as a build is precisely how a console on build 93 spent two releases
 * announcing it was up to date.
 *
 * Both are stamped by the build system from one source -- the VERSION file at
 * the repository root, and CI's run number -- so no platform can disagree.
 * The fallbacks below apply only to a workstation build.
 */
#ifndef PONG_VERSION
#define PONG_VERSION "0.0.0"
#endif

#ifndef PONG_BUILD_ID
#define PONG_BUILD_ID 0
#endif

#endif /* PONG_VERSION_H */
