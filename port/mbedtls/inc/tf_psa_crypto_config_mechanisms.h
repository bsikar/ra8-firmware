/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie */

/**
 * @file port/mbedtls/inc/tf_psa_crypto_config_mechanisms.h
 * @brief PSA crypto configuration -- mechanism selection + PSA core engine.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Split-out sub-header of @ref tf_psa_crypto_config.h carrying the
 * cryptographic-mechanism selection knobs (PSA-API `PSA_WANT_xxx`
 * selectors, the extended-API `MBEDTLS_xxx_C` algorithm wrappers, the
 * data-format support modules) together with the PSA-core engine
 * settings. The thin umbrella `tf_psa_crypto_config.h` includes this
 * file along with its sibling sub-headers so existing consumers that
 * `#include "tf_psa_crypto_config.h"` are unchanged. The original
 * Mbed TLS license header for the moved content is preserved verbatim
 * below.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

/**
 * @brief TF PSA CRYPTO CONFIG VERSION.
 *
 * This is an optional version symbol that enables compatibility handling of
 * config files.
 *
 * It is equal to the #TF_PSA_CRYPTO_VERSION_NUMBER of the TF-PSA-Crypto
 * version introduced the config format we want to be compatible with.
 */
#define TF_PSA_CRYPTO_CONFIG_VERSION (0x01000000)

/**
 * \name SECTION: SECTION Cryptographic mechanism selection (PSA API)
 *
 * This section sets PSA API settings.
 * \{
 */

/** @brief PSA WANT ALG CBC NO PADDING. */
#define PSA_WANT_ALG_CBC_NO_PADDING (1)
/** @brief PSA WANT ALG CBC PKCS7. */
#define PSA_WANT_ALG_CBC_PKCS7 (1)
/** @brief PSA WANT ALG CCM. */
#define PSA_WANT_ALG_CCM (1)
/** @brief PSA WANT ALG CCM STAR NO TAG. */
#define PSA_WANT_ALG_CCM_STAR_NO_TAG (1)
/** @brief PSA WANT ALG CMAC. */
#define PSA_WANT_ALG_CMAC (1)
/** @brief PSA WANT ALG CFB. */
#define PSA_WANT_ALG_CFB (1)
/** @brief PSA WANT ALG CHACHA20 POLY1305. */
#define PSA_WANT_ALG_CHACHA20_POLY1305 (1)
/** @brief PSA WANT ALG CTR. */
#define PSA_WANT_ALG_CTR (1)
/** @brief PSA WANT ALG DETERMINISTIC ECDSA. */
#define PSA_WANT_ALG_DETERMINISTIC_ECDSA (1)
/** @brief PSA WANT ALG ECB NO PADDING. */
#define PSA_WANT_ALG_ECB_NO_PADDING (1)
/** @brief PSA WANT ALG ECDH. */
#define PSA_WANT_ALG_ECDH (1)
/** @brief PSA WANT ALG FFDH. */
#define PSA_WANT_ALG_FFDH (1)
/** @brief PSA WANT ALG ECDSA. */
#define PSA_WANT_ALG_ECDSA (1)
/** @brief PSA WANT ALG JPAKE. */
#define PSA_WANT_ALG_JPAKE (1)
/** @brief PSA WANT ALG GCM. */
#define PSA_WANT_ALG_GCM (1)
/** @brief PSA WANT ALG HKDF. */
#define PSA_WANT_ALG_HKDF (1)
/** @brief PSA WANT ALG HKDF EXTRACT. */
#define PSA_WANT_ALG_HKDF_EXTRACT (1)
/** @brief PSA WANT ALG HKDF EXPAND. */
#define PSA_WANT_ALG_HKDF_EXPAND (1)
/** @brief PSA WANT ALG HMAC. */
#define PSA_WANT_ALG_HMAC (1)
/** @brief PSA WANT ALG MD5. */
#define PSA_WANT_ALG_MD5 (1)
/** @brief PSA WANT ALG OFB. */
#define PSA_WANT_ALG_OFB (1)
/** @brief PSA WANT ALG PBKDF2 HMAC. */
#define PSA_WANT_ALG_PBKDF2_HMAC (1)
/** @brief PSA WANT ALG PBKDF2 AES CMAC PRF 128. */
#define PSA_WANT_ALG_PBKDF2_AES_CMAC_PRF_128 (1)
/** @brief PSA WANT ALG RIPEMD160. */
#define PSA_WANT_ALG_RIPEMD160 (1)
/** @brief PSA WANT ALG RSA OAEP. */
#define PSA_WANT_ALG_RSA_OAEP (1)
/** @brief PSA WANT ALG RSA PKCS1 V15 CRYPT. */
#define PSA_WANT_ALG_RSA_PKCS1V15_CRYPT (1)
/** @brief PSA WANT ALG RSA PKCS1 V15 SIGN. */
#define PSA_WANT_ALG_RSA_PKCS1V15_SIGN (1)
/** @brief PSA WANT ALG RSA PSS. */
#define PSA_WANT_ALG_RSA_PSS (1)
/** @brief PSA WANT ALG SHA 1. */
#define PSA_WANT_ALG_SHA_1 (1)
/** @brief PSA WANT ALG SHA 224. */
#define PSA_WANT_ALG_SHA_224 (1)
/** @brief PSA WANT ALG SHA 256. */
#define PSA_WANT_ALG_SHA_256 (1)
/** @brief PSA WANT ALG SHA 384. */
#define PSA_WANT_ALG_SHA_384 (1)
/** @brief PSA WANT ALG SHA 512. */
#define PSA_WANT_ALG_SHA_512 (1)
/** @brief PSA WANT ALG SHA3 224. */
#define PSA_WANT_ALG_SHA3_224 (1)
/** @brief PSA WANT ALG SHA3 256. */
#define PSA_WANT_ALG_SHA3_256 (1)
/** @brief PSA WANT ALG SHA3 384. */
#define PSA_WANT_ALG_SHA3_384 (1)
/** @brief PSA WANT ALG SHA3 512. */
#define PSA_WANT_ALG_SHA3_512 (1)
/** @brief PSA WANT ALG STREAM CIPHER. */
#define PSA_WANT_ALG_STREAM_CIPHER (1)
/** @brief PSA WANT ALG TLS12 PRF. */
#define PSA_WANT_ALG_TLS12_PRF (1)
/** @brief PSA WANT ALG TLS12 PSK TO MS. */
#define PSA_WANT_ALG_TLS12_PSK_TO_MS (1)
/** @brief PSA WANT ALG TLS12 ECJPAKE TO PMS. */
#define PSA_WANT_ALG_TLS12_ECJPAKE_TO_PMS (1)
/** @brief PSA WANT ALG SHAKE128. */
#define PSA_WANT_ALG_SHAKE128 (1)
/** @brief PSA WANT ALG SHAKE256. */
#define PSA_WANT_ALG_SHAKE256 (1)

/** @brief PSA WANT ECC BRAINPOOL p R1 256. */
#define PSA_WANT_ECC_BRAINPOOL_P_R1_256 (1)
/** @brief PSA WANT ECC BRAINPOOL p R1 384. */
#define PSA_WANT_ECC_BRAINPOOL_P_R1_384 (1)
/** @brief PSA WANT ECC BRAINPOOL p R1 512. */
#define PSA_WANT_ECC_BRAINPOOL_P_R1_512 (1)
/** @brief PSA WANT ECC MONTGOMERY 255. */
#define PSA_WANT_ECC_MONTGOMERY_255 (1)
/** @brief PSA WANT ECC MONTGOMERY 448. */
#define PSA_WANT_ECC_MONTGOMERY_448 (1)
/** @brief PSA WANT ECC SECP K1 256. */
#define PSA_WANT_ECC_SECP_K1_256 (1)
/* For secp256r1, consider enabling #MBEDTLS_PSA_P256M_DRIVER_ENABLED
 * (see the description in psa/crypto_config.h for details). */
/** @brief PSA WANT ECC SECP R1 256. */
#define PSA_WANT_ECC_SECP_R1_256 (1)
/** @brief PSA WANT ECC SECP R1 384. */
#define PSA_WANT_ECC_SECP_R1_384 (1)
/** @brief PSA WANT ECC SECP R1 521. */
#define PSA_WANT_ECC_SECP_R1_521 (1)

/** @brief PSA WANT DH RFC7919 2048. */
#define PSA_WANT_DH_RFC7919_2048 (1)
/** @brief PSA WANT DH RFC7919 3072. */
#define PSA_WANT_DH_RFC7919_3072 (1)
/** @brief PSA WANT DH RFC7919 4096. */
#define PSA_WANT_DH_RFC7919_4096 (1)
/** @brief PSA WANT DH RFC7919 6144. */
#define PSA_WANT_DH_RFC7919_6144 (1)
/** @brief PSA WANT DH RFC7919 8192. */
#define PSA_WANT_DH_RFC7919_8192 (1)

/** @brief PSA WANT KEY TYPE DERIVE. */
#define PSA_WANT_KEY_TYPE_DERIVE (1)
/** @brief PSA WANT KEY TYPE PASSWORD. */
#define PSA_WANT_KEY_TYPE_PASSWORD (1)
/** @brief PSA WANT KEY TYPE PASSWORD HASH. */
#define PSA_WANT_KEY_TYPE_PASSWORD_HASH (1)
/** @brief PSA WANT KEY TYPE HMAC. */
#define PSA_WANT_KEY_TYPE_HMAC (1)
/** @brief PSA WANT KEY TYPE AES. */
#define PSA_WANT_KEY_TYPE_AES (1)
/** @brief PSA WANT KEY TYPE ARIA. */
#define PSA_WANT_KEY_TYPE_ARIA (1)
/** @brief PSA WANT KEY TYPE CAMELLIA. */
#define PSA_WANT_KEY_TYPE_CAMELLIA (1)
/** @brief PSA WANT KEY TYPE CHACHA20. */
#define PSA_WANT_KEY_TYPE_CHACHA20 (1)
/** @brief PSA WANT KEY TYPE ECC PUBLIC KEY. */
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY (1)
/** @brief PSA WANT KEY TYPE DH PUBLIC KEY. */
#define PSA_WANT_KEY_TYPE_DH_PUBLIC_KEY (1)
/** @brief PSA WANT KEY TYPE RAW DATA. */
#define PSA_WANT_KEY_TYPE_RAW_DATA (1)
/** @brief PSA WANT KEY TYPE RSA PUBLIC KEY. */
#define PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY (1)

/*
 * The following symbols extend and deprecate the legacy
 * PSA_WANT_KEY_TYPE_xxx_KEY_PAIR ones. They include the usage of that key in
 * the name's suffix. "_USE" is the most generic and it can be used to describe
 * a generic suport, whereas other ones add more features on top of that and
 * they are more specific.
 */
/** @brief PSA WANT KEY TYPE ECC KEY PAIR BASIC. */
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC (1)
/** @brief PSA WANT KEY TYPE ECC KEY PAIR IMPORT. */
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT (1)
/** @brief PSA WANT KEY TYPE ECC KEY PAIR EXPORT. */
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT (1)
/** @brief PSA WANT KEY TYPE ECC KEY PAIR GENERATE. */
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE (1)
/** @brief PSA WANT KEY TYPE ECC KEY PAIR DERIVE. */
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_DERIVE (1)

/** @brief PSA WANT KEY TYPE RSA KEY PAIR BASIC. */
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_BASIC (1)
/** @brief PSA WANT KEY TYPE RSA KEY PAIR IMPORT. */
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_IMPORT (1)
/** @brief PSA WANT KEY TYPE RSA KEY PAIR EXPORT. */
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_EXPORT (1)
/** @brief PSA WANT KEY TYPE RSA KEY PAIR GENERATE. */
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_GENERATE (1)
//#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_DERIVE   1 /* Not supported */

/** @brief PSA WANT KEY TYPE DH KEY PAIR BASIC. */
#define PSA_WANT_KEY_TYPE_DH_KEY_PAIR_BASIC (1)
/** @brief PSA WANT KEY TYPE DH KEY PAIR IMPORT. */
#define PSA_WANT_KEY_TYPE_DH_KEY_PAIR_IMPORT (1)
/** @brief PSA WANT KEY TYPE DH KEY PAIR EXPORT. */
#define PSA_WANT_KEY_TYPE_DH_KEY_PAIR_EXPORT (1)
/** @brief PSA WANT KEY TYPE DH KEY PAIR GENERATE. */
#define PSA_WANT_KEY_TYPE_DH_KEY_PAIR_GENERATE (1)
//#define PSA_WANT_KEY_TYPE_DH_KEY_PAIR_DERIVE    1 /* Not supported */
/** \} name SECTION Cryptographic mechanism selection (PSA API) */

/**
 * \name SECTION: Cryptographic mechanism selection (extended API)
 *
 * This section sets cryptographic mechanism settings.
 * \{
 */

/**
 * @brief MBEDTLS LMS c.
 *
 * \def MBEDTLS_LMS_C
 *
 * Enable the LMS stateful-hash asymmetric signature algorithm.
 *
 * Module:  extras/lms.c
 * Caller:
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C
 *
 * Uncomment to enable the LMS verification algorithm and public key operations.
 */
#define MBEDTLS_LMS_C

/**
 * \def MBEDTLS_LMS_PRIVATE
 *
 * Enable LMS private-key operations and signing code. Functions enabled by this
 * option are experimental, and should not be used in production.
 *
 * Requires: MBEDTLS_LMS_C
 *
 * Uncomment to enable the LMS signature algorithm and private key operations.
 */
//#define MBEDTLS_LMS_PRIVATE

/**
 * @brief MBEDTLS MD c.
 *
 * \def MBEDTLS_MD_C
 *
 * Enable the generic layer for message digest (hashing).
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C with at least one hash.
 * Module:  extras/md.c
 * Caller:  drivers/builtin/src/ecdsa.c
 *          drivers/builtin/src/ecjpake.c
 *          drivers/builtin/src/hmac_drbg.c
 *          drivers/builtin/src/psa_crypto_ecp.c
 *          drivers/builtin/src/psa_crypto_rsa.c
 *          drivers/builtin/src/rsa.c
 *          extras/pk.c
 *          utilities/constant_time.c
 *          utilities/pkcs5.c
 *
 * Uncomment to enable generic message digest wrappers.
 */
#define MBEDTLS_MD_C

/**
 * @brief MBEDTLS NIST KW c.
 *
 * \def MBEDTLS_NIST_KW_C
 *
 * Enable the 128-bit key wrapping modes from NIST SP 800-38F:
 * KW (also known as RFC 3394) and KWP (RFC 5649).
 * Currently these modes are only supported with AES.
 *
 * Module:  extras/nist_kw.c
 *
 * Auto enables: PSA_WANT_ALG_ECB_NO_PADDING
 */
#define MBEDTLS_NIST_KW_C

/**
 * @brief MBEDTLS PK c.
 *
 * \def MBEDTLS_PK_C
 *
 * Enable the generic public (asymmetric) key layer.
 *
 * Module:  extras/pk.c
 * Caller:  drivers/builtin/src/psa_crypto_rsa.c
 *
 * Requires: #MBEDTLS_PSA_CRYPTO_CLIENT and at least one between
 *           #PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY and
 *           #PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY.
 *
 * Uncomment to enable generic public key wrappers.
 */
#define MBEDTLS_PK_C

/**
 * @brief MBEDTLS PKCS5 c.
 *
 * \def MBEDTLS_PKCS5_C
 *
 * Enable PKCS#5 functions.
 *
 * Module:  utilities/pkcs5.c
 *
 * Auto-enables: MBEDTLS_MD_C
 *
 * This module adds support for the PKCS#5 functions.
 */
#define MBEDTLS_PKCS5_C

/**
 * @brief MBEDTLS PK PARSE c.
 *
 * \def MBEDTLS_PK_PARSE_C
 *
 * Enable the generic public (asymmetric) key parser.
 *
 * Module:  extras/pkparse.c
 *
 * Requires: MBEDTLS_ASN1_PARSE_C, MBEDTLS_PK_C
 *
 * Uncomment to enable generic public key parse functions.
 */
#define MBEDTLS_PK_PARSE_C

/**
 * @brief MBEDTLS PK PARSE EC EXTENDED.
 *
 * \def MBEDTLS_PK_PARSE_EC_EXTENDED
 *
 * Enhance support for reading EC keys using variants of SEC1 not allowed by
 * RFC 5915 and RFC 5480.
 *
 * Currently this means parsing the SpecifiedECDomain choice of EC
 * parameters (only known groups are supported, not arbitrary domains, to
 * avoid validation issues).
 *
 * Disable if you only need to support RFC 5915 + 5480 key formats.
 */
#define MBEDTLS_PK_PARSE_EC_EXTENDED

/**
 * @brief MBEDTLS PK PARSE EC COMPRESSED.
 *
 * \def MBEDTLS_PK_PARSE_EC_COMPRESSED
 *
 * Enable the support for parsing public keys of type Short Weierstrass
 * (PSA_ECC_FAMILY_SECP_XXX and PSA_ECC_FAMILY_BRAINPOOL_XXX) which are using the
 * compressed point format.
 */
#define MBEDTLS_PK_PARSE_EC_COMPRESSED

/**
 * @brief MBEDTLS PK WRITE c.
 *
 * \def MBEDTLS_PK_WRITE_C
 *
 * Enable the generic public (asymmetric) key writer.
 *
 * Module:  extras/pkwrite.c
 *
 * Requires: MBEDTLS_ASN1_WRITE_C, MBEDTLS_PK_C
 *
 * Uncomment to enable generic public key write functions.
 */
#define MBEDTLS_PK_WRITE_C

/** \} name SECTION: Cryptographic mechanism selection (extended API) */

/**
 * \name SECTION: Data format support
 *
 * This section sets data-format specific settings.
 * \{
 */

/**
 * @brief MBEDTLS ASN1 PARSE c.
 *
 * \def MBEDTLS_ASN1_PARSE_C
 *
 * Enable the generic ASN1 parser.
 *
 * Module:  utilities/asn1parse.c
 * Caller:  extras/pkparse.c
 *          utilities/pkcs5.c
 */
#define MBEDTLS_ASN1_PARSE_C

/**
 * @brief MBEDTLS ASN1 WRITE c.
 *
 * \def MBEDTLS_ASN1_WRITE_C
 *
 * Enable the generic ASN1 writer.
 *
 * Module:  utilities/asn1write.c
 * Caller:  drivers/builtin/src/ecdsa.c
 *          extras/pkwrite.c
 */
#define MBEDTLS_ASN1_WRITE_C

/**
 * @brief MBEDTLS BASE64 c.
 *
 * \def MBEDTLS_BASE64_C
 *
 * Enable the Base64 module.
 *
 * Module:  utilities/base64.c
 * Caller:  utilities/pem.c
 *
 * This module is required for PEM support (required by X.509).
 */
#define MBEDTLS_BASE64_C

/**
 * @brief MBEDTLS PEM PARSE c.
 *
 * \def MBEDTLS_PEM_PARSE_C
 *
 * Enable PEM decoding / parsing.
 *
 * Module:  utilities/pem.c
 * Caller:  extras/pkparse.c
 *
 * Requires: MBEDTLS_BASE64_C
 *           optionally PSA_WANT_ALG_MD5
 *
 * This modules adds support for decoding / parsing PEM files.
 */
#define MBEDTLS_PEM_PARSE_C

/**
 * @brief MBEDTLS PEM WRITE c.
 *
 * \def MBEDTLS_PEM_WRITE_C
 *
 * Enable PEM encoding / writing.
 *
 * Module:  utilities/pem.c
 * Caller:  extras/pkwrite.c
 *
 * Requires: MBEDTLS_BASE64_C
 *
 * This modules adds support for encoding / writing PEM files.
 */
#define MBEDTLS_PEM_WRITE_C

/** \} name SECTION: Data format support */

/**
 * \name SECTION: PSA core
 *
 * This section sets PSA specific settings.
 * \{
 */

/**
 * @brief MBEDTLS CTR DRBG c.
 *
 * \def MBEDTLS_CTR_DRBG_C
 *
 * Enable the CTR_DRBG AES-based random generator.
 * The CTR_DRBG generator uses AES-256 by default.
 * To use AES-128 instead, set #MBEDTLS_PSA_CRYPTO_RNG_STRENGTH to 128.
 *
 * AES support can either be achieved through built-in AES or PSA. Built-in is
 * the default option when present otherwise PSA is used.
 *
 * Module:  drivers/builtin/src/ctr_drbg.c
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C, PSA_WANT_KEY_TYPE_AES and
 *           PSA_WANT_ALG_ECB_NO_PADDING
 *
 * This module provides the CTR_DRBG AES random number generator.
 */
#define MBEDTLS_CTR_DRBG_C

/**
 * \def MBEDTLS_ENTROPY_NO_SOURCES_OK
 *
 * Normally, TF-PSA-Crypto requires at least one "true" entropy source, such
 * #MBEDTLS_PSA_BUILTIN_GET_ENTROPY or #MBEDTLS_PSA_DRIVER_GET_ENTROPY.
 *
 * It is possible to build the library with a seed injected during device
 * provisioning, thanks to #MBEDTLS_ENTROPY_NV_SEED.
 * This is only an initial entropy input: without a true entropy source,
 * the device will not obtain additional entropy during its lifetime.
 * Thus, if the seed value is leaked, it is impossible to recover from
 * this compromise.
 *
 * Enable this option if this loss of security is acceptable to you.
 */
//#define MBEDTLS_ENTROPY_NO_SOURCES_OK

/**
 * \def MBEDTLS_ENTROPY_NV_SEED
 *
 * Enable the non-volatile (NV) seed file-based entropy source.
 * (Also enables the NV seed read/write functions in the platform layer)
 *
 * This is crucial (if not required) on systems that do not have a
 * cryptographic entropy source (in hardware or kernel) available.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C,
 *           !MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
 *           MBEDTLS_PLATFORM_C
 *
 * \note The read/write functions that are used by the entropy source are
 *       determined in the platform layer, and can be modified at runtime and/or
 *       compile-time depending on the flags (MBEDTLS_PLATFORM_NV_SEED_*) used.
 *
 * \note If you use the default implementation functions that read a seedfile
 *       with regular fopen(), please make sure you make a seedfile with the
 *       proper name (defined in MBEDTLS_PLATFORM_STD_NV_SEED_FILE) and at
 *       least MBEDTLS_ENTROPY_BLOCK_SIZE bytes in size that can be read from
 *       and written to or you will get an entropy source error! The default
 *       implementation will only use the first MBEDTLS_ENTROPY_BLOCK_SIZE
 *       bytes from the file.
 *
 * \note The entropy collector will write to the seed file before entropy is
 *       given to an external source, to update it.
 */
//#define MBEDTLS_ENTROPY_NV_SEED

/**
 * @brief MBEDTLS HMAC DRBG c.
 *
 * \def MBEDTLS_HMAC_DRBG_C
 *
 * Enable the HMAC_DRBG random generator.
 *
 * Module:  drivers/builtin/src/hmac_drbg.c
 * Caller:
 *
 * Requires: MBEDTLS_MD_C
 *
 * Uncomment to enable the HMAC_DRBG random number generator.
 */
#define MBEDTLS_HMAC_DRBG_C

/**
 * @brief MBEDTLS PSA CRYPTO c.
 *
 * \def MBEDTLS_PSA_CRYPTO_C
 *
 * Enable the Platform Security Architecture cryptography API.
 *
 * Module:  core/psa_crypto.c
 *
 * Requires: one of the following:
 *           - MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
 *           - MBEDTLS_CTR_DRBG_C
 *           - MBEDTLS_HMAC_DRBG_C
 *
 *           If MBEDTLS_CTR_DRBG_C or MBEDTLS_HMAC_DRBG_C is used as the PSA
 *           random generator, then either PSA_WANT_ALG_SHA_256 or
 *           PSA_WANT_ALG_SHA_512 must be enabled for the entropy module.
 *
 * \note The PSA crypto subsystem prioritizes DRBG mechanisms as follows:
 *       - #MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG, if enabled
 *       - CTR_DRBG (AES), seeded by the entropy module, if
 *         #MBEDTLS_CTR_DRBG_C is enabled
 *       - HMAC_DRBG, seeded by the entropy module, if
 *         #MBEDTLS_HMAC_DRBG_C is enabled
 *
 *       A future version may reevaluate the prioritization of DRBG mechanisms.
 */
#define MBEDTLS_PSA_CRYPTO_C

/**
 * \def MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS
 *
 * Assume all buffers passed to PSA functions are owned exclusively by the
 * PSA function and are not stored in shared memory.
 *
 * This option may be enabled if all buffers passed to any PSA function reside
 * in memory that is accessible only to the PSA function during its execution.
 *
 * This option MUST be disabled whenever buffer arguments are in memory shared
 * with an untrusted party, for example where arguments to PSA calls are passed
 * across a trust boundary.
 *
 * \note Enabling this option reduces memory usage and code size.
 *
 * \note Enabling this option causes overlap of input and output buffers
 *       not to be supported by PSA functions.
 */
//#define MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS

/**
 * \def MBEDTLS_PSA_BUILTIN_GET_ENTROPY
 *
 * Enable entropy sources for which the library has a built-in driver.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C, !MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
 *
 * These are:
 * - getrandom() on Linux (if syscall() is available at compile time);
 * - getrandom() on FreeBSD and DragonFlyBSD (if available at compile time);
 * - `sysctl(KERN_ARND)` on FreeBSD and NetBSD;
 * - #MBEDTLS_PLATFORM_DEV_RANDOM on Unix-like platforms (unless one of the
 *   above is used);
 * - BCryptGenRandom() on Windows.
 *
 * You should enable this option if your platform has one of these. If not:
 *
 * - You can enable #MBEDTLS_PSA_DRIVER_GET_ENTROPY instead, and provide
 *   an entropy source callback for your platform.
 * - If your platform has a fast cryptographic-quality random generator,
 *   enable #MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG and provide a random generator
 *   callback instead.
 * - If your platform has no source of entropy at all, you can enable
 *   #MBEDTLS_ENTROPY_NV_SEED and provide a seed in nonvolatile memory
 *   during the provisioning of the device.
 * - The random generator requires a random generator callback,
 *   an entropy source or a seed in nonvolatile memory.
 *   Builds with no random generator are not officially supported yet, except
 *   client-only builds (#MBEDTLS_PSA_CRYPTO_CLIENT enabled and
 *   #MBEDTLS_PSA_CRYPTO_C disabled).
 *
 * Disabled: bare-metal RA8 has neither getrandom() nor /dev/urandom
 * nor BCryptGenRandom. The example app routes RSIP-TRNG into Mbed TLS
 * via MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG (see below).
 */
//#define MBEDTLS_PSA_BUILTIN_GET_ENTROPY
/** @brief MBEDTLS PSA CRYPTO EXTERNAL RNG. */
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

/** \def MBEDTLS_PSA_CRYPTO_BUILTIN_KEYS
 *
 * Enable support for platform built-in keys. If you enable this feature,
 * you must implement the function mbedtls_psa_platform_get_builtin_key().
 * See the documentation of that function for more information.
 *
 * Built-in keys are typically derived from a hardware unique key or
 * stored in a secure element.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C.
 *
 * \warning This interface is experimental and may change or be removed
 * without notice.
 */
//#define MBEDTLS_PSA_CRYPTO_BUILTIN_KEYS

/** \def MBEDTLS_PSA_CRYPTO_CLIENT
 *
 * Enable support for PSA crypto client.
 *
 * \note This option allows to include the code necessary for a PSA
 *       crypto client when the PSA crypto implementation is not included in
 *       the library (MBEDTLS_PSA_CRYPTO_C disabled). The code included is the
 *       code to set and get PSA key attributes.
 *       The development of PSA drivers partially relying on the library to
 *       fulfill the hardware gaps is another possible usage of this option.
 *
 * \warning This interface is experimental and may change or be removed
 * without notice.
 */
//#define MBEDTLS_PSA_CRYPTO_CLIENT

/** \def MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
 *
 * Make the PSA Crypto module use an external random generator provided
 * by a driver, instead of Mbed TLS's entropy and DRBG modules.
 *
 * \note This random generator must deliver random numbers with cryptographic
 *       quality and high performance. It must supply unpredictable numbers
 *       with a uniform distribution. The implementation of this function
 *       is responsible for ensuring that the random generator is seeded
 *       with sufficient entropy. If you have a hardware TRNG which is slow
 *       or delivers non-uniform output, declare it as an entropy source
 *       with mbedtls_entropy_add_source() instead of enabling this option.
 *
 * If you enable this option, you must configure the type
 * ::mbedtls_psa_external_random_context_t in psa/crypto_platform.h
 * and define a function called mbedtls_psa_external_get_random()
 * with the following prototype:
 * ```
 * psa_status_t mbedtls_psa_external_get_random(
 *     mbedtls_psa_external_random_context_t *context,
 *     uint8_t *output, size_t output_size, size_t *output_length);
 * );
 * ```
 * The \c context value is initialized to 0 before the first call.
 * The function must fill the \c output buffer with \c output_size bytes
 * of random data and set \c *output_length to \c output_size.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C
 *
 * \warning If you enable this option, code that uses the PSA cryptography
 *          interface will not use any of the entropy sources set up for
 *          the entropy module, nor the NV seed that MBEDTLS_ENTROPY_NV_SEED
 *          enables.
 *
 * \note This option is experimental and may be removed without notice.
 */
//#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

/* MBEDTLS_PSA_CRYPTO_KEY_ID_ENCODES_OWNER
 *
 * Enable key identifiers that encode a key owner identifier.
 *
 * The owner of a key is identified by a value of type ::mbedtls_key_owner_id_t
 * which is currently hard-coded to be int32_t.
 *
 * Note that this option is meant for internal use only and may be removed
 * without notice.
 */
//#define MBEDTLS_PSA_CRYPTO_KEY_ID_ENCODES_OWNER

/**
 * \def MBEDTLS_PSA_CRYPTO_SPM
 *
 * When MBEDTLS_PSA_CRYPTO_SPM is defined, the code is built for SPM (Secure
 * Partition Manager) integration which separates the code into two parts: a
 * NSPE (Non-Secure Process Environment) and an SPE (Secure Process
 * Environment).
 *
 * If you enable this option, your build environment must include a header
 * file `"crypto_spe.h"` (either in the `psa` subdirectory of the Mbed TLS
 * header files, or in another directory on the compiler's include search
 * path). Alternatively, your platform may customize the header
 * `psa/crypto_platform.h`, in which case it can skip or replace the
 * inclusion of `"crypto_spe.h"`.
 *
 * Module:  core/psa_crypto.c
 * Requires: MBEDTLS_PSA_CRYPTO_C
 *
 */
//#define MBEDTLS_PSA_CRYPTO_SPM

/**
 * \def MBEDTLS_PSA_CRYPTO_STORAGE_C
 *
 * Enable the Platform Security Architecture persistent key storage.
 *
 * Module:  core/psa_crypto_storage.c
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C,
 *           either MBEDTLS_PSA_ITS_FILE_C or a native implementation of
 *           the PSA ITS interface
 *
 * Disabled: this firmware has no persistent key storage (no fs).
 */
//#define MBEDTLS_PSA_CRYPTO_STORAGE_C

/**
 * \def MBEDTLS_PSA_DRIVER_GET_ENTROPY
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C, !MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
 *
 * Enable the custom entropy callback mbedtls_platform_get_entropy()
 * (declared in mbedtls/platform.h). You need to provide this callback
 * if you need an entropy source and the built-in entropy callback
 * provided by #MBEDTLS_PSA_BUILTIN_GET_ENTROPY does not work on your platform.
 *
 * Enabling both #MBEDTLS_PSA_BUILTIN_GET_ENTROPY and
 * #MBEDTLS_PSA_DRIVER_GET_ENTROPY is currently not supported.
 *
 * You do not need any entropy source in the following circumstances:
 *
 * - If your platform has a fast cryptographic-quality random generator, and
 *   you enable #MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG and provide a random generator
 *   callback instead.
 * - If your platform has no source of entropy at all, and you enable
 *   #MBEDTLS_ENTROPY_NV_SEED and provide a seed in nonvolatile memory
 *   during the provisioning of the device.
 * - If you build the library with no random generator.
 *   Builds with no random generator are not officially supported yet, except
 *   client-only builds (#MBEDTLS_PSA_CRYPTO_CLIENT enabled and
 *   #MBEDTLS_PSA_CRYPTO_C disabled).
 */
//#define MBEDTLS_PSA_DRIVER_GET_ENTROPY

/**
 * \def MBEDTLS_PSA_ITS_FILE_C
 *
 * Enable the emulation of the Platform Security Architecture
 * Internal Trusted Storage (PSA ITS) over files.
 *
 * Module:  core/psa_its_file.c
 *
 * Requires: MBEDTLS_FS_IO
 *
 * Disabled: depends on MBEDTLS_FS_IO which is off in this firmware.
 */
//#define MBEDTLS_PSA_ITS_FILE_C

/**
 * @brief MBEDTLS PSA KEY STORE DYNAMIC.
 *
 * \def MBEDTLS_PSA_KEY_STORE_DYNAMIC
 *
 * Dynamically resize the PSA key store to accommodate any number of
 * volatile keys (until the heap memory is exhausted).
 *
 * If this option is disabled, the key store has a fixed size
 * #MBEDTLS_PSA_KEY_SLOT_COUNT for volatile keys and loaded persistent keys
 * together.
 *
 * This option has no effect when #MBEDTLS_PSA_CRYPTO_C is disabled.
 *
 * Module:  core/psa_crypto.c
 * Requires: MBEDTLS_PSA_CRYPTO_C
 */
#define MBEDTLS_PSA_KEY_STORE_DYNAMIC

/**
 * \def MBEDTLS_PSA_STATIC_KEY_SLOTS
 *
 * Statically preallocate memory to store keys' material in PSA instead
 * of allocating it dynamically when required. This allows builds without a
 * heap, if none of the enabled cryptographic implementations or other features
 * require it.
 * This feature affects both volatile and persistent keys which means that
 * it's not possible to persistently store a key which is larger than
 * #MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE.
 *
 * \note This feature comes with a (potentially) higher RAM usage since:
 *       - All the key slots are allocated no matter if they are used or not.
 *       - Each key buffer's length is #MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE bytes.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C
 *
 */
//#define MBEDTLS_PSA_STATIC_KEY_SLOTS

/* Entropy options */

/**
 * \def MBEDTLS_PSA_CRYPTO_PLATFORM_FILE
 *
 * If defined, this is a header which will be included instead of
 * `"psa/crypto_platform.h"`. This file should declare the same identifiers
 * as the one in Mbed TLS, but with definitions adapted to the platform on
 * which the library code will run.
 *
 * \note The required content of this header can vary from one version of
 *       Mbed TLS to the next. Integrators who provide an alternative file
 *       should review the changes in the original file whenever they
 *       upgrade Mbed TLS.
 *
 * This macro is expanded after an <tt>\#include</tt> directive. This is a popular but
 * non-standard feature of the C language, so this feature is only available
 * with compilers that perform macro expansion on an <tt>\#include</tt> line.
 *
 * The value of this symbol is typically a path in double quotes, either
 * absolute or relative to a directory on the include search path.
 */
//#define MBEDTLS_PSA_CRYPTO_PLATFORM_FILE "psa/crypto_platform_alt.h"

/**
 * \def MBEDTLS_PSA_CRYPTO_STRUCT_FILE
 *
 * If defined, this is a header which will be included instead of
 * `"psa/crypto_struct.h"`. This file should declare the same identifiers
 * as the one in Mbed TLS, but with definitions adapted to the environment
 * in which the library code will run. The typical use for this feature
 * is to provide alternative type definitions on the client side in
 * client-server integrations of PSA crypto, where operation structures
 * contain handles instead of cryptographic data.
 *
 * \note The required content of this header can vary from one version of
 *       Mbed TLS to the next. Integrators who provide an alternative file
 *       should review the changes in the original file whenever they
 *       upgrade Mbed TLS.
 *
 * This macro is expanded after an <tt>\#include</tt> directive. This is a popular but
 * non-standard feature of the C language, so this feature is only available
 * with compilers that perform macro expansion on an <tt>\#include</tt> line.
 *
 * The value of this symbol is typically a path in double quotes, either
 * absolute or relative to a directory on the include search path.
 */
//#define MBEDTLS_PSA_CRYPTO_STRUCT_FILE "psa/crypto_struct_alt.h"

/** \def MBEDTLS_PSA_KEY_SLOT_COUNT
 *
 * When #MBEDTLS_PSA_KEY_STORE_DYNAMIC is disabled,
 * the maximum amount of PSA keys simultaneously in memory. This counts all
 * volatile keys, plus loaded persistent keys.
 *
 * When #MBEDTLS_PSA_KEY_STORE_DYNAMIC is enabled,
 * the maximum number of loaded persistent keys.
 *
 * Currently, persistent keys do not need to be loaded all the time while
 * a multipart operation is in progress, only while the operation is being
 * set up. This may change in future versions of the library.
 *
 * Currently, the library traverses of the whole table on each access to a
 * persistent key. Therefore large values may cause poor performance.
 *
 * This option has no effect when #MBEDTLS_PSA_CRYPTO_C is disabled.
 */
//#define MBEDTLS_PSA_KEY_SLOT_COUNT 32

/**
 * \def MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE
 *
 * Define the size (in bytes) of each static key buffer when
 * #MBEDTLS_PSA_STATIC_KEY_SLOTS is set. If not
 * explicitly defined then it's automatically guessed from available PSA keys
 * enabled in the build through PSA_WANT_xxx symbols.
 * If required by the application this parameter can be set to higher values
 * in order to store larger objects (ex: raw keys), but please note that this
 * will increase RAM usage.
 */
//#define MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE       256

/**
 * \def MBEDTLS_PSA_CRYPTO_RNG_STRENGTH
 *
 * Minimum security strength (in bits) of the PSA RNG.
 *
 * \note Valid values: 128 or default of 256.
 */
//#define MBEDTLS_PSA_CRYPTO_RNG_STRENGTH               256

/**
 * \def MBEDTLS_PSA_CRYPTO_RNG_HASH
 *
 * \brief Hash algorithm to use for the entropy module and for HMAC_DRBG if configured.
 *
 * The hash size (in bits) must be at least #MBEDTLS_PSA_CRYPTO_RNG_STRENGTH.
 *
 * In addition, if the entropy module is enabled (#MBEDTLS_PSA_CRYPTO_C is enabled
 * and #MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG is disabled):
 * - The hash size must be at least 32 bytes (i.e., 256 bits).
 * - Only two values are currently allowed: PSA_ALG_SHA_256 and PSA_ALG_SHA_512.
 *   A future version may lift this limitation.
 *
 * If #MBEDTLS_PSA_CRYPTO_RNG_HASH is not explicitly set in the configuration,
 * a default hash that satisfies the above constraints is selected automatically.
 * If no suitable default can be selected, this will result in a build error.
 */
//#define MBEDTLS_PSA_CRYPTO_RNG_HASH PSA_ALG_SHA_256

/**
 * \def MBEDTLS_PSA_RNG_RESEED_INTERVAL
 *
 * In CTR_DRBG and HMAC_DRBG, the interval before the DRBG is reseeded from entropy.
 * The interval is the number of requests to the random generator, for any purpose.
 *
 * \note Requests have a maximum size (which depends on the library configuration
 * and is currently unspecified), so the maximum number of bytes before a reseed
 * is the interval multiplied by the maximum request size.
 */
//#define MBEDTLS_PSA_RNG_RESEED_INTERVAL 1000

/** \} name SECTION: PSA core */
