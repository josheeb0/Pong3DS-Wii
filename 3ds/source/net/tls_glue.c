/*
 * Platform callbacks mbedTLS needs from the 3DS.
 *
 * mbedTLS is built with no knowledge of libctru (see 3ds/vendor/build-mbedtls.sh),
 * which keeps the library reusable and its build independent of the console SDK.
 * The two holes that leaves are filled here, resolved when the app links.
 */

#include <3ds.h>
#include <string.h>
#include <mbedtls/build_info.h>
#include <mbedtls/platform_time.h>

/*
 * Entropy.
 *
 * newlib has no /dev/urandom, so MBEDTLS_NO_PLATFORM_ENTROPY is set and this
 * becomes the ONLY seed material for the CTR_DRBG that generates our ECDHE
 * private keys. Getting it wrong is not a subtle bug -- a predictable seed
 * means a predictable session key.
 *
 * PS_GenerateRandomBytes is the console's own CSPRNG, which is the right
 * source. It can fail (the PS service may not be available), and in that case
 * we report zero bytes gathered rather than silently substituting something
 * weak: mbedTLS will then refuse to seed and the connection fails loudly.
 */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    (void)data;
    *olen = 0;
    if (output == NULL || len == 0) return 0;

    if (R_FAILED(PS_GenerateRandomBytes(output, len))) {
        return -1; /* MBEDTLS_ERR_ENTROPY_SOURCE_FAILED, without the header */
    }
    *olen = len;
    return 0;
}

/*
 * Millisecond clock.
 *
 * mbedTLS 3.6 wants a monotonic millisecond source and newlib on the 3DS has
 * no CLOCK_MONOTONIC, so MBEDTLS_PLATFORM_MS_TIME_ALT is set and this supplies
 * it. osGetTime() is milliseconds since epoch and is what the rest of the game
 * uses as its timebase too.
 */
mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)osGetTime();
}
