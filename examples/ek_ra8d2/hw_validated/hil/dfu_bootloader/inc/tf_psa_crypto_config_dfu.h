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
 * Removing unused mechanisms (`f43a741ae`), followed by replacing the multi-curve
 * built-in `ecp`/`ecdsa`/`bignum` stack with `p256-m` and omitting the
 * unused AES-GCM, `MBEDTLS_PK_*`, `MBEDTLS_ASN1_*`, and `MBEDTLS_MD_C` modules
 * (`RA8FW-353`), eliminates unused code and attack surface so even a Debug
 * (`-O0 -g3`) build fits well inside the 128 KB bootloader bank.
 *
 * The launch gate (`ra8_dfu_launch`) verifies exactly one thing: an
 * ECDSA-P256 signature over a SHA-256 digest via `ra8_rot_verify_image`,
 * backed by the compact PSA `p256-m` driver (`MBEDTLS_PSA_P256M_DRIVER_ENABLED`).
 * AES-GCM, `MBEDTLS_PK_*`, `MBEDTLS_ASN1_*`, and `MBEDTLS_MD_C` are omitted:
 * the bootloader calls only `psa_crypto_init`, `psa_import_key`,
 * `psa_verify_hash`, and `psa_destroy_key` with a raw SEC1 P-256 point and
 * raw r||s signature, and the anti-rollback counter is an extra-MRAM u32 word.
 * Without PK/ASN.1/GCM/bignum/ecp, even a Debug (`-O0 -g3`) image fits inside
 * the 128 KB bootloader bank (`RA8FW-353`).
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
 * ECDSA-P256 over SHA-256 for the slot signature. Everything else the shared
 * configuration enables is omitted on purpose; see the file comment. */
#define PSA_WANT_ALG_SHA_256 (1)
#define PSA_WANT_ALG_ECDSA (1)
#define PSA_WANT_ECC_SECP_R1_256 (1)
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY (1)

/* ---- PSA core and platform options -------------------------------------
 * Minimal PSA verify surface: p256-m handles SECP256R1 ECDSA verify via the
 * PSA driver interface without pulling the multi-curve builtin ecp/bignum,
 * PK, ASN.1, or cipher stacks, and MBEDTLS_SHA256_SMALLER trims the software
 * SHA-256 footprint alongside external RNG (src/psa_verify_rng.c). */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
#define MBEDTLS_PSA_P256M_DRIVER_ENABLED
#define MBEDTLS_SHA256_SMALLER

#include "tf_psa_crypto_config_drivers.h"
#include "tf_psa_crypto_config_platform.h"

#endif /* PSA_CRYPTO_CONFIG_H */
