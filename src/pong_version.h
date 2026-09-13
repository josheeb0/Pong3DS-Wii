#ifndef PONG_VERSION_H
#define PONG_VERSION_H

/*
 * The build number, which is the only version this project has.
 *
 * Named versions were tried and removed. A version string cannot be compared
 * the way an update check needs: "v1.1.1" reduces to 1, which is not newer than
 * build 93, so a console sat two releases behind reporting itself up to date.
 * A build number is monotonic by construction and every change gets a higher
 * one, so "newest" and "largest" are the same question.
 *
 * Stamped by the build system from CI's run number. The fallback of 0 applies
 * only to a workstation build, and is deliberately a number no release can ever
 * have, so a local build is always visibly not a release.
 */
#ifndef PONG_BUILD_ID
#define PONG_BUILD_ID 0
#endif

#endif /* PONG_VERSION_H */
