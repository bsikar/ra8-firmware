/**
 * @file tf_psa_crypto_config_dfu.h
 * @brief Bootloader-only tf-psa-crypto configuration: just the mechanisms the
 *        DFU launch gate verifies with
 *
 * @par Tag
 * [Ring 3 / App] {World: S}
 *
 * @details
 * `dfu_bootloader` links against a 128 KB MRAM bank (`MRAM_LENGTH 128K`), and
 * it was compiling the entire default tf-psa-crypto mechanism suite to do it.
 * Measured from the link map of a Debug build, of the 221450 B of `.text` +
 * `.rodata` that survived `--gc-sections`, 136770 B (62%) was tf-psa-crypto:
 * RSA, FFDH, J-PAKE, MD5, RIPEMD-160, SHA-1, SHA-512, all four SHA-3
 * variants, SHAKE, ARIA, Camellia, ChaCha20-Poly1305 and nine ECC curves,
 * none of which this image can reach. `ra8_hal` was never a factor:
 * `--gc-sections` already discards 295016 B of it and keeps only 26002 B.
 *
 * Trimming to the reachable set takes the shipping (MinSizeRel) image from
 * 116528 B to 61828 B, 88.90% of the bank down to 47.17%. That is both
 * headroom and attack surface: a Root-of-Trust bootloader has no business
 * carrying an RSA implementation it never calls. See issue RA8FW-353, which also
 * tracks the separate fact that a Debug build of this app does not fit the
 * bank at all.
 *
 * The launch gate (`ra8_dfu_launch`) verifies exactly one thing: an
 * ECDSA-P256 signature over a SHA-256 digest, plus AES-GCM for the sealed
 * anti-rollback record. So this configuration enables that and nothing else.
 *
 * Why this is app-local and not a trim of `port/mbedtls/inc`: that
 * configuration is shared by every mbedtls consumer in the tree
 * (`secure_boot_hil` and the PSA HIL apps among them), which legitimately
 * need the wider suite. Narrowing it there to fit one bootloader would
 * silently remove mechanisms from apps that use them. The bank constraint
 * belongs to this app, so the configuration does too.
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

/* ---- Mechanisms the DFU launch gate actually verifies with ---------------
 * ECDSA-P256 over SHA-256 for the slot signature, AES-GCM for the sealed
 * anti-rollback record. Everything else the shared configuration enables is
 * omitted on purpose; see the file comment. */
#define PSA_WANT_ALG_SHA_256 (1)
#define PSA_WANT_ALG_ECDSA (1)
#define PSA_WANT_ECC_SECP_R1_256 (1)
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY (1)
#define PSA_WANT_ALG_GCM (1)
#define PSA_WANT_KEY_TYPE_AES (1)

/* ---- PSA core and platform options -------------------------------------
 * Taken from the shared header, minus the parts a bootloader that verifies a
 * raw ECDSA signature over a digest cannot reach: PEM and base64 decoding,
 * PK parse/write (the slot key arrives as a raw public point, not DER or PEM),
 * PKCS#5, LMS, NIST key wrap, and both DRBGs (this image draws randomness
 * through MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG, see src/psa_verify_rng.c).
 * Deterministic ECDSA is omitted with them: the bootloader only ever
 * verifies, never signs, so it needs no per-signature nonce derivation. */
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
