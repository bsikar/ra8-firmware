/**
 * @file tf_psa_crypto_config_sb.h
 * @brief Secure-boot-only tf-psa-crypto configuration: just the mechanisms the
 *        on-silicon boot gate verifies with
 *
 * @par Tag
 * [Ring 3 / App] {World: S}
 *
 * @details
 * `secure_boot_hil` links against a 128 KB MRAM bank (`MRAM_LENGTH 128K`) and
 * was compiling the entire default tf-psa-crypto mechanism suite to do it:
 * RSA, FFDH, J-PAKE, MD5, RIPEMD-160, SHA-1, SHA-512, all four SHA-3
 * variants, SHAKE, ARIA, Camellia, ChaCha20-Poly1305 and nine ECC curves,
 * none of which this image can reach. This is the same trim already applied
 * to `dfu_bootloader` in #2502, for the same reason and against the same
 * vendored tree.
 *
 * What this image verifies with, taken from its own sources plus the
 * `ra8_dfu` and `ra8_psa_crypto` translation units it pulls in through
 * `EXTRA_SRCS`: an ECDSA-P256 signature over a SHA-256 digest for the slot
 * signature, AES-GCM for the sealed anti-rollback record, and the HMAC and
 * SECP-R1 key-pair types that `ra8_psa_crypto_key_type()` can return. The
 * last two are kept deliberately: that mapping is reachable from this image,
 * so narrowing it to the verify-only subset would turn a supported call into
 * a runtime `PSA_ERROR_NOT_SUPPORTED`.
 *
 * Why this is app-local and not a trim of `port/mbedtls/inc`: that
 * configuration is shared by every mbedtls consumer in the tree, which
 * legitimately need the wider suite. Narrowing it there to fit one 128 KB
 * bank would silently remove mechanisms from apps that use them. The bank
 * constraint belongs to this app, so the configuration does too.
 *
 * The platform and builtin-driver sub-headers are included unchanged; only
 * the mechanism selection differs from `tf_psa_crypto_config.h`, whose
 * include guard this header deliberately reuses so the two can never both
 * be active in one translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef PSA_CRYPTO_CONFIG_H
/** @brief PSA CRYPTO CONFIG h. */
#define PSA_CRYPTO_CONFIG_H

/** @brief Vendor ABI version stamp, identical to the shared configuration. */
#define TF_PSA_CRYPTO_CONFIG_VERSION (0x01000000)

/* ---- Mechanisms the secure boot gate actually verifies with -------------
 * ECDSA-P256 over SHA-256 for the slot signature, AES-GCM for the sealed
 * anti-rollback record. Everything else the shared configuration enables is
 * omitted on purpose; see the file comment. */
#define PSA_WANT_ALG_SHA_256 (1)
#define PSA_WANT_ALG_ECDSA (1)
#define PSA_WANT_ECC_SECP_R1_256 (1)
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY (1)
#define PSA_WANT_ALG_GCM (1)
#define PSA_WANT_KEY_TYPE_AES (1)

/* ---- Key types `ra8_psa_crypto_key_type()` can hand back -----------------
 * Reachable from this image, so they stay enabled even though the boot gate
 * itself only verifies. */
#define PSA_WANT_ALG_HMAC (1)
#define PSA_WANT_KEY_TYPE_HMAC (1)
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC (1)
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT (1)
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT (1)
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE (1)

/* ---- PSA core and platform options -------------------------------------
 * Taken from the shared header, minus the parts an image that verifies a raw
 * ECDSA signature over a digest cannot reach: PEM and base64 decoding, PKCS#5,
 * LMS, NIST key wrap, and both DRBGs (this image draws randomness through
 * MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG). */
#define MBEDTLS_MD_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

#include "tf_psa_crypto_config_drivers.h"
#include "tf_psa_crypto_config_platform.h"

#endif /* PSA_CRYPTO_CONFIG_H */
