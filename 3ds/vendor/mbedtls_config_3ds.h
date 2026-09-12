/*
 * mbedTLS configuration for the 3DS.
 *
 * WHY WE BUNDLE OUR OWN TLS AT ALL
 * --------------------------------
 * libctru's httpc delegates TLS to the console's system SSL module, whose
 * cipher list is fixed in firmware. pong.wardcrew.com is fronted by Cloudflare
 * serving an ECDSA P-256 certificate with NO RSA certificate available --
 * verified directly: RSA-auth cipher suites are answered with handshake_failure.
 * Whether a given 3DS firmware will negotiate ECDHE-ECDSA is not something we
 * can control or even reliably discover.
 *
 * Linking mbedTLS removes the question. We choose the cipher suites, so the
 * handshake either works everywhere or fails for a reason we can read.
 *
 * WHAT IS ENABLED AND WHY
 * -----------------------
 * Exactly enough to complete a TLS 1.2 handshake against Cloudflare's modern
 * profile, and nothing else. Every module left out is ROM the 3DS does not have
 * to load and code that cannot contain a bug that matters to us:
 *   - client only (no MBEDTLS_SSL_SRV_C)
 *   - no DTLS, no file I/O, no self-tests, no PSK, no DHE
 *   - ECDHE key exchange with P-256/P-384/X25519, matching what the edge offers
 *   - AES-GCM and ChaCha20-Poly1305 (the edge negotiated CHACHA20 by default;
 *     ChaCha is also markedly faster than AES on an ARM11 with no AES
 *     instructions, which matters on a 67MHz CPU)
 *   - ECDSA and RSA verification, so this keeps working if the cert changes
 */

#ifndef MBEDTLS_CONFIG_3DS_H
#define MBEDTLS_CONFIG_3DS_H

/* ---- platform ---------------------------------------------------------- */
/* newlib gives us calloc/free/snprintf; it does NOT give us /dev/urandom, so
 * platform entropy is off and we supply mbedtls_hardware_poll() ourselves
 * from the 3DS PS service. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_C

/* newlib on the 3DS has no CLOCK_MONOTONIC, so mbedTLS cannot build its own
 * millisecond clock. We supply mbedtls_ms_time() from osGetTime() in
 * source/net/tls_glue.c; it is resolved when the app links, which also keeps
 * libctru out of the library build entirely. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* ---- ciphers ----------------------------------------------------------- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C

/* ---- hashes ------------------------------------------------------------ */
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* ---- public key -------------------------------------------------------- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C

/* Curves the edge actually offers. Dropping the rest saves a lot of code. */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

/* ---- random ------------------------------------------------------------ */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* ---- x509 -------------------------------------------------------------- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* ---- TLS --------------------------------------------------------------- */
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_SERVER_NAME_INDICATION   /* Cloudflare REQUIRES SNI */
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* Session resumption. The handshake is by far the most expensive thing this
 * console does; being able to resume instead of redoing ECDHE is the
 * difference between a usable poll rate and a slideshow if keep-alive drops. */
#define MBEDTLS_SSL_SESSION_TICKETS

/* ---- footprint --------------------------------------------------------- */
/* 16KB is the TLS record ceiling; the 3DS has 32MB (64MB on New 3DS) of
 * userland RAM but we also hold a 1MB SOC buffer and the GPU command buffers,
 * so two 16KB TLS buffers are worth being deliberate about. Our largest
 * response is a batch of 8 snapshots -- 256 bytes -- so 4KB is generous. */
#define MBEDTLS_SSL_IN_CONTENT_LEN  4096
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096

#define MBEDTLS_ERROR_C   /* human-readable handshake failures; worth the space */

#endif /* MBEDTLS_CONFIG_3DS_H */
