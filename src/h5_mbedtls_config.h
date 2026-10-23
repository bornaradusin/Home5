/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Home5's changes to mbedTLS 3.6's default configuration (its
 * MBEDTLS_USER_CONFIG_FILE). Home5 brings its own sockets, files, random bytes
 * and calendar (h5_net.c, h5_ps5.c). Taken from PC5; the rest is mbedTLS as it comes. */
#ifndef H5_MBEDTLS_CONFIG_H
#define H5_MBEDTLS_CONFIG_H

#undef MBEDTLS_NET_C                /* BSD sockets: Home5 has its own */
#undef MBEDTLS_TIMING_C             /* alarm() and signals */
#undef MBEDTLS_FS_IO                /* Home5 reads the certificates itself */
#undef MBEDTLS_PSA_ITS_FILE_C       /* needs MBEDTLS_FS_IO */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C /* needs the above */
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_DEBUG_C
#define MBEDTLS_NO_PLATFORM_ENTROPY    /* no getrandom or /dev/urandom on the console: */
#define MBEDTLS_ENTROPY_HARDWARE_ALT   /* mbedtls_hardware_poll, from the system's generator */
#define MBEDTLS_PLATFORM_GMTIME_R_ALT  /* mbedtls_platform_gmtime_r, Home5's own */
#define MBEDTLS_PLATFORM_MEMORY        /* its memory from h5_alloc, set by h5_net.c */
#define MBEDTLS_TEST_SW_INET_PTON      /* its own address parser, not the system's inet_pton */
#define MBEDTLS_NO_UDBL_DIVISION       /* its own long division, not the compiler's __udivti3 */

#endif
