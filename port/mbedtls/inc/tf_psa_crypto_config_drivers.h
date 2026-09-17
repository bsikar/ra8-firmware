
/**
 * @file port/mbedtls/inc/tf_psa_crypto_config_drivers.h
 * @brief PSA crypto configuration -- builtin-driver tuning options.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Split-out sub-header of @ref tf_psa_crypto_config.h carrying the
 * builtin-driver tuning knobs (`MBEDTLS_AESNI_C`, `MBEDTLS_AESCE_C`,
 * ECP / SHA / RSA size-vs-speed levers, the PQCP MLDSA options) plus
 * the trailing removed-mechanisms testing switch. The thin umbrella
 * `tf_psa_crypto_config.h` includes this file along with its sibling
 * sub-headers so existing consumers that
 * `#include "tf_psa_crypto_config.h"` are unchanged. The original
 * Mbed TLS license header for the moved content is preserved verbatim
 * below.
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

/**
 * \name SECTION: Builtin drivers
 *
 * This section sets driver specific settings.
 * \{
 */

/**
 * @brief MBEDTLS AESNI c.
 *
 * \def MBEDTLS_AESNI_C
 *
 * Enable AES-NI support on x86-64 or x86-32.
 *
 * \note AESNI is only supported with certain compilers and target options:
 * - Visual Studio: supported
 * - GCC, x86-64, target not explicitly supporting AESNI:
 *   requires MBEDTLS_HAVE_ASM.
 * - GCC, x86-32, target not explicitly supporting AESNI:
 *   not supported.
 * - GCC, x86-64 or x86-32, target supporting AESNI: supported.
 *   For this assembly-less implementation, you must currently compile
 *   `drivers/builtin/src/aesni.c` and `drivers/builtin/src/aes.c` with machine
 *   options to enable SSE2 and AESNI instructions: `gcc -msse2 -maes -mpclmul`
 *   or `clang -maes -mpclmul`.
 * - Non-x86 targets: this option is silently ignored.
 * - Other compilers: this option is silently ignored.
 *
 * \note
 * Above, "GCC" includes compatible compilers such as Clang.
 * The limitations on target support are likely to be relaxed in the future.
 *
 * Module:  drivers/builtin/src/aesni.c
 * Caller:  drivers/builtin/src/aes.c
 *
 * Requires: MBEDTLS_HAVE_ASM (on some platforms, see note)
 *
 * This modules adds support for the AES-NI instructions on x86.
 */
#define MBEDTLS_AESNI_C

/**
 * @brief MBEDTLS AESCE c.
 *
 * \def MBEDTLS_AESCE_C
 *
 * Enable AES cryptographic extension support on Armv8.
 *
 * Module:  drivers/builtin/src/aesce.c
 * Caller:  drivers/builtin/src/aes.c
 *
 * Requires: The AES built-in implementation
 *
 * \warning Runtime detection only works on Linux. For non-Linux operating
 *          system, Armv8-A Cryptographic Extensions must be supported by
 *          the CPU when this option is enabled.
 *
 * \note    Minimum compiler versions for this feature when targeting aarch64
 *          are Clang 4.0; armclang 6.6; GCC 6.0; or MSVC 2019 version 16.11.2.
 *          Minimum compiler versions for this feature when targeting 32-bit
 *          Arm or Thumb are Clang 11.0; armclang 6.20; or GCC 6.0.
 *
 * \note \c CFLAGS must be set to a minimum of \c -march=armv8-a+crypto for
 * armclang <= 6.9
 *
 * This module adds support for the AES Armv8-A Cryptographic Extensions on Armv8 systems.
 */
#define MBEDTLS_AESCE_C

/**
 * \def MBEDTLS_AES_ROM_TABLES
 *
 * Use precomputed AES tables stored in ROM.
 *
 * Uncomment this macro to use precomputed AES tables stored in ROM.
 * Comment this macro to generate AES tables in RAM at runtime.
 *
 * Tradeoff: Using precomputed ROM tables reduces RAM usage by ~8kb
 * (or ~2kb if \c MBEDTLS_AES_FEWER_TABLES is used) and reduces the
 * initialization time before the first AES operation can be performed.
 * It comes at the cost of additional ~8kb ROM use (resp. ~2kb if \c
 * MBEDTLS_AES_FEWER_TABLES below is used), and potentially degraded
 * performance if ROM access is slower than RAM access.
 *
 * This option is independent of \c MBEDTLS_AES_FEWER_TABLES.
 */
//#define MBEDTLS_AES_ROM_TABLES

/**
 * \def MBEDTLS_AES_FEWER_TABLES
 *
 * Use less ROM/RAM for AES tables.
 *
 * Uncommenting this macro omits 75% of the AES tables from
 * ROM / RAM (depending on the value of \c MBEDTLS_AES_ROM_TABLES)
 * by computing their values on the fly during operations
 * (the tables are entry-wise rotations of one another).
 *
 * Tradeoff: Uncommenting this reduces the RAM / ROM footprint
 * by ~6kb but at the cost of more arithmetic operations during
 * runtime. Specifically, one has to compare 4 accesses within
 * different tables to 4 accesses with additional arithmetic
 * operations within the same table. The performance gain/loss
 * depends on the system and memory details.
 *
 * This option is independent of \c MBEDTLS_AES_ROM_TABLES.
 */
//#define MBEDTLS_AES_FEWER_TABLES

/**
 * \def MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH
 *
 * Use only 128-bit keys in AES operations to save ROM.
 *
 * Uncomment this macro to remove support for AES operations that use 192-
 * or 256-bit keys.
 *
 * Uncommenting this macro reduces the size of AES code by ~300 bytes
 * on v8-M/Thumb2.
 *
 * Module:  drivers/builtin/src/aes.c
 *
 * Requires: The AES built-in implementation
 */
//#define MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH

/*
 * Disable plain C implementation for AES.
 *
 * When the plain C implementation is enabled, and an implementation using a
 * special CPU feature (such as MBEDTLS_AESCE_C) is also enabled, runtime
 * detection will be used to select between them.
 *
 * If only one implementation is present, runtime detection will not be used.
 * This configuration will crash at runtime if running on a CPU without the
 * necessary features. It will not build unless at least one of MBEDTLS_AESCE_C
 * and/or MBEDTLS_AESNI_C is enabled & present in the build.
 */
//#define MBEDTLS_AES_USE_HARDWARE_ONLY

/**
 * \def MBEDTLS_BLOCK_CIPHER_NO_DECRYPT
 *
 * Remove decryption operation for AES, ARIA and Camellia block cipher.
 *
 * \note  This feature is incompatible with PSA_WANT_ALG_ECB_NO_PADDING,
 *        PSA_WANT_ALG_CBC_NO_PADDING, PSA_WANT_ALG_CBC_PKCS7 and
 *        MBEDTLS_NIST_KW_C.
 *
 * Module:  drivers/builtin/src/aes.c
 *          drivers/builtin/src/aesce.c
 *          drivers/builtin/src/aesni.c
 *          drivers/builtin/src/aria.c
 *          drivers/builtin/src/camellia.c
 *          drivers/builtin/src/cipher.c
 */
//#define MBEDTLS_BLOCK_CIPHER_NO_DECRYPT

/**
 * \def MBEDTLS_CAMELLIA_SMALL_MEMORY
 *
 * Use less ROM for the Camellia implementation (saves about 768 bytes).
 *
 * Uncomment this macro to use less memory for Camellia.
 */
//#define MBEDTLS_CAMELLIA_SMALL_MEMORY

/**
 * Enable the verified implementations of ECDH primitives from Project Everest
 * (currently only Curve25519).
 *
 * The Everest code is provided under the Apache 2.0 license only; therefore enabling this
 * option is not compatible with taking the library under the GPL v2.0-or-later license.
 */
//#define MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED

/**
 * @brief MBEDTLS ECP NIST OPTIM.
 *
 * \def MBEDTLS_ECP_NIST_OPTIM
 *
 * Enable specific 'modulo p' routines for each NIST prime.
 * Depending on the prime and architecture, makes operations 4 to 8 times
 * faster on the corresponding curve.
 *
 * Comment this macro to disable NIST curves optimisation.
 */
#define MBEDTLS_ECP_NIST_OPTIM

/**
 * \def MBEDTLS_ECP_RESTARTABLE
 *
 * Enable "non-blocking" ECC operations that can return early and be resumed.
 *
 * This allows various functions to pause by returning
 * #PSA_OPERATION_INCOMPLETE and then be called later again in
 * order to further progress and eventually complete their operation. This is
 * controlled through psa_interruptible_set_max_ops() which limits the maximum
 * number of ECC operations a function may perform before pausing; see
 * psa_interruptible_set_max_ops() for more information.
 *
 * This is useful in non-threaded environments if you want to avoid blocking
 * for too long on ECC (and, hence, X.509 or SSL/TLS) operations.
 *
 * This option:
 * - Adds xxx_restartable() variants of existing operations in the
 *   following modules, with corresponding restart context types:
 *   - ECP (for Short Weierstrass curves only): scalar multiplication (mul),
 *     linear combination (muladd);
 *   - ECDSA: signature generation & verification;
 *   - PK: signature generation & verification;
 *   - X509: certificate chain verification.
 * - Adds mbedtls_ecdh_enable_restart() in the ECDH module.
 * - Changes the behaviour of TLS 1.2 clients (not servers) when using the
 *   ECDHE-ECDSA key exchange (not other key exchanges) to make all ECC
 *   computations restartable:
 *   - verification of the server's key exchange signature;
 *   - verification of the server's certificate chain;
 *   - generation of the client's signature if client authentication is used,
 *     with an ECC key/certificate.
 *
 * \note  When this option is enabled, restartable operations in PK, X.509
 *        and TLS (see above) are not using PSA. On the other hand, ECDH
 *        computations in TLS are using PSA, and are not restartable. These
 *        are temporary limitations that should be lifted in the future. (See
 *        https://github.com/Mbed-TLS/mbedtls/issues/9784 and
 *        https://github.com/Mbed-TLS/mbedtls/issues/9817)
 *
 * Requires: Builtin support of Elliptic Curves.
 *
 * Uncomment this macro to enable restartable ECC computations.
 */
//#define MBEDTLS_ECP_RESTARTABLE

/**
 * Uncomment to enable using new bignum code in the ECC modules.
 *
 * \warning This is currently experimental, incomplete and therefore should not
 * be used in production.
 */
//#define MBEDTLS_ECP_WITH_MPI_UINT

/**
 * \def MBEDTLS_GCM_LARGE_TABLE
 *
 * Enable large pre-computed tables for  Galois/Counter Mode (GCM).
 * Can significantly increase throughput on systems without GCM hardware
 * acceleration (e.g., AESNI, AESCE).
 *
 * The mbedtls_gcm_context size will increase by 3840 bytes.
 * The code size will increase by roughly 344 bytes.
 *
 * Module:  drivers/builtin/src/gcm.c
 *
 * Requires: The GCM built-in implementation
 */
//#define MBEDTLS_GCM_LARGE_TABLE

/**
 * \def MBEDTLS_HAVE_ASM
 *
 * The compiler has support for asm().
 *
 * Requires support for asm() in compiler.
 *
 * Used in:
 *      drivers/builtin/src/aesni.h
 *      drivers/builtin/src/aria.c
 *      drivers/builtin/src/bn_mul.h
 *      utilities/constant_time.c
 *
 * Required by:
 *      MBEDTLS_AESCE_C
 *      MBEDTLS_AESNI_C (on some platforms)
 *
 * Comment to disable the use of assembly code.
 *
 * Disabled: the hand-tuned MULADDC inline asm in bn_mul.h targets
 * Cortex-M4/M7 and asks for more GPRs than the compiler can spare
 * at -O0 on Cortex-M85, where it fails with "asm operand has
 * impossible constraints or there are not enough registers". The
 * portable C fallback works fine.
 */
//#define MBEDTLS_HAVE_ASM

/**
 * \def MBEDTLS_HAVE_SSE2
 *
 * CPU supports SSE2 instruction set.
 *
 * Uncomment if the CPU supports SSE2 (IA-32 specific).
 */
//#define MBEDTLS_HAVE_SSE2

/**
 * \def MBEDTLS_NO_UDBL_DIVISION
 *
 * The platform lacks support for double-width integer division (64-bit
 * division on a 32-bit platform, 128-bit division on a 64-bit platform).
 *
 * Used in:
 *      include/mbedtls/bignum.h
 *      drivers/builtin/src/bignum.c
 *
 * The bignum code uses double-width division to speed up some operations.
 * Double-width division is often implemented in software that needs to
 * be linked with the program. The presence of a double-width integer
 * type is usually detected automatically through preprocessor macros,
 * but the automatic detection cannot know whether the code needs to
 * and can be linked with an implementation of division for that type.
 * By default division is assumed to be usable if the type is present.
 * Uncomment this option to prevent the use of double-width division.
 *
 * Note that division for the native integer type is always required.
 * Furthermore, a 64-bit type is always required even on a 32-bit
 * platform, but it need not support multiplication or division. In some
 * cases it is also desirable to disable some double-width operations. For
 * example, if double-width division is implemented in software, disabling
 * it can reduce code size in some embedded targets.
 */
//#define MBEDTLS_NO_UDBL_DIVISION

/**
 * \def MBEDTLS_NO_64BIT_MULTIPLICATION
 *
 * The platform lacks support for 32x32 -> 64-bit multiplication.
 *
 * Used in:
 *      drivers/builtin/src/poly1305.c
 *
 * Some parts of the library may use multiplication of two unsigned 32-bit
 * operands with a 64-bit result in order to speed up computations. On some
 * platforms, this is not available in hardware and has to be implemented in
 * software, usually in a library provided by the toolchain.
 *
 * Sometimes it is not desirable to have to link to that library. This option
 * removes the dependency of that library on platforms that lack a hardware
 * 64-bit multiplier by embedding a software implementation in Mbed TLS.
 *
 * Note that depending on the compiler, this may decrease performance compared
 * to using the library function provided by the toolchain.
 */
//#define MBEDTLS_NO_64BIT_MULTIPLICATION

/**
 * Uncomment to enable p256-m. This is an alternative implementation of
 * key generation, ECDH and (randomized) ECDSA on the curve SECP256R1.
 * Compared to the default implementation:
 *
 * - p256-m has a much smaller code size and RAM footprint.
 * - p256-m is only available via the PSA API. This includes the pk module.
 * - p256-m does not support deterministic ECDSA, EC-JPAKE, custom protocols
 *   over the core arithmetic, or deterministic derivation of keys.
 *
 * We recommend enabling this option if your application uses the PSA API
 * and the only elliptic curve support it needs is ECDH and ECDSA over
 * SECP256R1.
 *
 * If you enable this option, you do not need to enable any ECC-related
 * MBEDTLS_xxx option. You do need to separately request support for the
 * cryptographic mechanisms through the PSA API:
 * - #MBEDTLS_PSA_CRYPTO_C for PSA-based configuration;
 * - #PSA_WANT_ECC_SECP_R1_256;
 * - #PSA_WANT_ALG_ECDH and/or #PSA_WANT_ALG_ECDSA as needed;
 * - #PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY, #PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC,
 *   #PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT,
 *   #PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT and/or
 *   #PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE as needed.
 *
 * \note To benefit from the smaller code size of p256-m, make sure that you
 *       do not enable any ECC-related option not supported by p256-m: this
 *       would cause the built-in ECC implementation to be built as well, in
 *       order to provide the required option.
 *       Make sure #PSA_WANT_ALG_DETERMINISTIC_ECDSA, #PSA_WANT_ALG_JPAKE and
 *       #PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_DERIVE, and curves other than
 *       SECP256R1 are disabled as they are not supported by this driver.
 *       Also, avoid defining #MBEDTLS_PK_PARSE_EC_COMPRESSED or
 *       #MBEDTLS_PK_PARSE_EC_EXTENDED as those currently require a subset of
 *       the built-in ECC implementation, see docs/driver-only-builds.md.
 */
//#define MBEDTLS_PSA_P256M_DRIVER_ENABLED

/**
 * \def MBEDTLS_RSA_NO_CRT
 *
 * Do not use the Chinese Remainder Theorem
 * for the RSA private operation.
 *
 * Uncomment this macro to disable the use of CRT in RSA.
 *
 */
//#define MBEDTLS_RSA_NO_CRT

/**
 * \def MBEDTLS_SHA256_SMALLER
 *
 * Enable an implementation of SHA-256 that has lower ROM footprint but also
 * lower performance.
 *
 * The default implementation is meant to be a reasonable compromise between
 * performance and size. This version optimizes more aggressively for size at
 * the expense of performance. Eg on Cortex-M4 it reduces the size of
 * mbedtls_sha256_process() from ~2KB to ~0.5KB for a performance hit of about
 * 30%.
 *
 * Uncomment to enable the smaller implementation of SHA256.
 */
//#define MBEDTLS_SHA256_SMALLER

/**
 * \def MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT
 *
 * Enable acceleration of the SHA-256 and SHA-224 cryptographic hash algorithms
 * with the ARMv8 cryptographic extensions if they are available at runtime.
 * If not, the library will fall back to the C implementation.
 *
 * \note MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT requires the built-in
 * SHA-256 implementation to be present in the build. This implementation is
 * included only if PSA_WANT_ALG_SHA_256 is enabled and this results in
 * MBEDTLS_PSA_BUILTIN_ALG_SHA_256 being defined internally (i.e., no
 * fully-featured, fallback-free accelerator driver is present).
 *
 * \note If MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT is defined when building
 * for a non-Armv8-A build it will be silently ignored.
 *
 * \note    Minimum compiler versions for this feature are Clang 4.0,
 * armclang 6.6 or GCC 6.0.
 *
 * \note \c CFLAGS must be set to a minimum of \c -march=armv8-a+crypto for
 * armclang <= 6.9
 *
 * \warning MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT cannot be defined at the
 * same time as MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_ONLY.
 *
 * Requires: The SHA-256 built-in implementation
 *
 * Module:  drivers/builtin/src/sha256.c
 *
 * Uncomment to have the library check for the Armv8-A SHA-256 crypto extensions
 * and use them if available.
 */
//#define MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT

/**
 * \def MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_ONLY
 *
 * Enable acceleration of the SHA-256 and SHA-224 cryptographic hash algorithms
 * with the ARMv8 cryptographic extensions, which must be available at runtime
 * or else an illegal instruction fault will occur.
 *
 * \note MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_ONLY requires the built-in SHA-256
 * implementation to be present in the build. This implementation is included
 * only if PSA_WANT_ALG_SHA_256 is enabled and this results in
 * MBEDTLS_PSA_BUILTIN_ALG_SHA_256 being defined internally (i.e., no
 * fully-featured, fallback-free accelerator driver is present).
 *
 * \note This allows builds with a smaller code size than with
 * MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT
 *
 * \note    Minimum compiler versions for this feature are Clang 4.0,
 * armclang 6.6 or GCC 6.0.
 *
 * \note \c CFLAGS must be set to a minimum of \c -march=armv8-a+crypto for
 * armclang <= 6.9
 *
 * \warning MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_ONLY cannot be defined at the same
 * time as MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT.
 *
 * Requires: The SHA-256 built-in implementation
 *
 * Module:  drivers/builtin/src/sha256.c
 *
 * Uncomment to have the library use the Armv8-A SHA-256 crypto extensions
 * unconditionally.
 */
//#define MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_ONLY

/**
 * \def MBEDTLS_SHA512_SMALLER
 *
 * Enable an implementation of SHA-512 that has lower ROM footprint but also
 * lower performance.
 *
 * Uncomment to enable the smaller implementation of SHA512.
 */
//#define MBEDTLS_SHA512_SMALLER

/**
 * \def MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT
 *
 * Enable acceleration of the SHA-512 and SHA-384 cryptographic hash algorithms
 * with the ARMv8 cryptographic extensions if they are available at runtime.
 * If not, the library will fall back to the C implementation.
 *
 * \note MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT requires the built-in
 * SHA-512 implementation to be present in the build. This implementation is
 * included only if PSA_WANT_ALG_SHA_512 is enabled and this results in
 * MBEDTLS_PSA_BUILTIN_ALG_SHA_512 being defined internally (i.e., no
 * fully-featured, fallback-free accelerator driver is present).
 *
 * \note If MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT is defined when building
 * for a non-Aarch64 build it will be silently ignored.
 *
 * \note    Minimum compiler versions for this feature are Clang 7.0,
 * armclang 6.9 or GCC 8.0.
 *
 * \note \c CFLAGS must be set to a minimum of \c -march=armv8.2-a+sha3 for
 * armclang 6.9
 *
 * \warning MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT cannot be defined at the
 * same time as MBEDTLS_SHA512_USE_A64_CRYPTO_ONLY.
 *
 * Requires: The SHA-512 built-in implementation
 *
 * Module:  drivers/builtin/src/sha512.c
 *
 * Uncomment to have the library check for the A64 SHA-512 crypto extensions
 * and use them if available.
 */
//#define MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT

/**
 * \def MBEDTLS_SHA512_USE_A64_CRYPTO_ONLY
 *
 * Enable acceleration of the SHA-512 and SHA-384 cryptographic hash algorithms
 * with the ARMv8 cryptographic extensions, which must be available at runtime
 * or else an illegal instruction fault will occur.
 *
 * \note MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT requires the built-in
 * SHA-512 implementation to be present in the build. This implementation is
 * included only if PSA_WANT_ALG_SHA_512 is enabled and this results in
 * MBEDTLS_PSA_BUILTIN_ALG_SHA_512 being defined internally (i.e., no
 * fully-featured, fallback-free accelerator driver is present).
 *
 * \note This allows builds with a smaller code size than with
 * MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT
 *
 * \note    Minimum compiler versions for this feature are Clang 7.0,
 * armclang 6.9 or GCC 8.0.
 *
 * \note \c CFLAGS must be set to a minimum of \c -march=armv8.2-a+sha3 for
 * armclang 6.9
 *
 * \warning MBEDTLS_SHA512_USE_A64_CRYPTO_ONLY cannot be defined at the same
 * time as MBEDTLS_SHA512_USE_A64_CRYPTO_IF_PRESENT.
 *
 * Requires: The SHA-512 built-in implementation
 *
 * Module:  drivers/builtin/src/sha512.c
 *
 * Uncomment to have the library use the A64 SHA-512 crypto extensions
 * unconditionally.
 */
//#define MBEDTLS_SHA512_USE_A64_CRYPTO_ONLY

/* ECP options */
//#define MBEDTLS_ECP_FIXED_POINT_OPTIM      1 /**< Enable fixed-point speed-up */
//#define MBEDTLS_ECP_WINDOW_SIZE            4 /**< Maximum window size used */

/* MPI / BIGNUM options */
//#define MBEDTLS_MPI_MAX_SIZE            1024 /**< Maximum number of bytes for usable MPIs. */
//#define MBEDTLS_MPI_WINDOW_SIZE            2 /**< Maximum window size used. */

/* RSA OPTIONS */
//#define MBEDTLS_RSA_GEN_KEY_MIN_BITS            1024 /**<  Minimum RSA key size that can be generated in bits (Minimum possible value is 128 bits) */

/**
 * \def TF_PSA_CRYPTO_PQCP_MLDSA_ENABLED
 *
 * Enable mldsa-native from the PQCP (post-quantum code package) driver.
 * This is an integration of https://github.com/pq-code-package/mldsa-native
 * in TF-PSA-Crypto.
 *
 * \warning This option is experimental. It may change or be removed without
 *          notice.
 *
 * Module:  drivers/pqcp/src/wrap_mldsa_native.c
 *
 * Uncomment to include mldsa-native in libtfpsacrypto.
 */
//#define TF_PSA_CRYPTO_PQCP_MLDSA_ENABLED

/**
 * \def TF_PSA_CRYPTO_PQCP_MLDSA_87_ENABLED
 *
 * Enable mldsa-native from the PQCP (post-quantum code package) driver
 * for the security level 87.
 * This is an integration of https://github.com/pq-code-package/mldsa-native
 * in TF-PSA-Crypto.
 *
 * \warning This option is experimental. It may change or be removed without
 *          notice.
 *
 * Requires: TF_PSA_CRYPTO_PQCP_MLDSA_ENABLED
 *
 * Module:  drivers/pqcp/src/wrap_mldsa_native.c
 *
 * Uncomment to include MLDSA-87 from mldsa-native in libtfpsacrypto.
 */
//#define TF_PSA_CRYPTO_PQCP_MLDSA_87_ENABLED

/**
 * \def TF_PSA_CRYPTO_PQCP_OWN_SHAKE
 *
 * If this option is enabled, the MLDSA implementation from mldsa-native
 * (#TF_PSA_CRYPTO_PQCP_MLDSA_ENABLED) will use the SHAKE implementation
 * that's bundled with mldsa-native. Otherwise mldsa-native will use the
 * TF-PSA-Crypto SHAKE builtin that powers the PSA SHA3 and SHAKE APIs.
 *
 * The mldsa-native SHAKE implementation is significantly faster on
 * processors that have vector instructions. On other architectures,
 * your mileage may vary.
 *
 * \warning This option is experimental. It may change or be removed without
 *          notice.
 *
 * Module:  drivers/pqcp/src/wrap_mldsa_native.c
 *
 * Uncomment to have mldsa-native use its own SHAKE.
 */
//#define TF_PSA_CRYPTO_PQCP_OWN_SHAKE

/** \} name SECTION: Builtin drivers */

/* Do not enable except for testing. Will be removed in a future minor version.
 */
//#define TF_PSA_CRYPTO_ALLOW_REMOVED_MECHANISMS
