/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rsip_regs_fields.h
 * @brief RSIP-E50D register field/value enums and the reg32 accessor
 * @ingroup grp_hal_crypto
 *
 * @details
 * Sub-header of @ref ra8_rsip_regs.h. Carries every register
 * field-position / bit-mask / command-value enum for the RSIP-E50D
 * engine (CTRL / STATUS / ISR fields and masks, HASH algorithm
 * selectors, OEM-key install opcodes, key-vault / key-wrap / DOTF /
 * KDF command words, lifecycle + debug-level states, AES mode +
 * direction, ECC curve + RSA size selectors, asymmetric opcodes,
 * tamper sources, ChaCha20 opcodes, and the published buffer sizes),
 * plus the @ref ra8_rsip_reg32 register-pointer accessor.
 *
 * The RSIP-E50D engine on the RA8D2 is an isolated security
 * subsystem (HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" p 3302). It
 * is mailbox-driven; field semantics are inferred from the FSP
 * primitives that touch ``REG_xxxxH`` in
 * ``crypto_procedures_protected/.../ra8_rsip_e50d``.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_rsip_regs_offsets.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_rsip_ctrl_bits_t
 * @brief Field positions inside the CTRL mailbox word.
 *
 * @details
 * The CTRL word gates the engine clock, arms the BIST / TRNG
 * health checks, and routes command FIFOs. Bit positions are
 * derived from the FSP primitives that touch ``REG_0000H`` /
 * ``REG_0004H``.
 */
typedef enum : uint8_t {
  k_ra8_rsip_ctrl_bit_enable   = 0U, /**< 1 = clock the engine.               */
  k_ra8_rsip_ctrl_bit_bist     = 1U, /**< 1 = arm built-in self-test.         */
  k_ra8_rsip_ctrl_bit_reset    = 2U, /**< 1 = soft-reset the engine.          */
  k_ra8_rsip_ctrl_bit_dpa_arm  = 3U, /**< 1 = enable SPA/DPA countermeasures. */
  k_ra8_rsip_ctrl_bit_irq_glob = 4U, /**< 1 = globally enable RSIP IRQ line.  */
} ra8_rsip_ctrl_bits_t;

/**
 * @enum ra8_rsip_status_bits_t
 * @brief Field positions inside the STATUS mailbox word.
 */
typedef enum : uint8_t {
  k_ra8_rsip_status_bit_ready   = 0U, /**< 1 = idle and ready for command.   */
  k_ra8_rsip_status_bit_busy    = 1U, /**< 1 = engine is running a command.  */
  k_ra8_rsip_status_bit_bist_ok = 2U, /**< 1 = self-test passed.             */
  k_ra8_rsip_status_bit_kv_ok   = 3U, /**< 1 = key vault initialized.        */
  k_ra8_rsip_status_bit_huk_ok  = 4U, /**< 1 = HUK loaded and unwrapped.     */
  k_ra8_rsip_status_bit_dpa_on  = 5U, /**< 1 = SPA/DPA countermeasures live. */
  k_ra8_rsip_status_bit_err     = 7U, /**< 1 = access-management error.      */
} ra8_rsip_status_bits_t;

/**
 * @enum ra8_rsip_isr_bits_t
 * @brief Field positions inside the IER / ISR words.
 *
 * @details
 * The RSIP routes a small handful of edge-triggered events into one
 * peripheral IRQ line; HUM Ch 52.1 Table 52.1 p 3302 names the
 * tamper line ``RSIP_TADI``.
 */
typedef enum : uint8_t {
  k_ra8_rsip_isr_bit_done      = 0U, /**< Generic command-complete.       */
  k_ra8_rsip_isr_bit_rnd       = 1U, /**< RNG fresh-data ready.           */
  k_ra8_rsip_isr_bit_hash      = 2U, /**< Hash absorption complete.       */
  k_ra8_rsip_isr_bit_asym_done = 3U, /**< RSA/ECC operation complete.     */
  k_ra8_rsip_isr_bit_kv_done   = 4U, /**< Key vault op complete.          */
  k_ra8_rsip_isr_bit_kdf_done  = 5U, /**< KDF complete.                   */
  k_ra8_rsip_isr_bit_err       = 6U, /**< Generic access/algorithm error. */
  k_ra8_rsip_isr_bit_tamper    = 7U, /**< Tamper / RSIP_TADI interrupt.   */
} ra8_rsip_isr_bits_t;

/**
 * @enum ra8_rsip_mask_t
 * @brief Pre-computed bit masks for the CTRL / STATUS / ISR words.
 */
typedef enum : uint32_t {
  k_ra8_rsip_mask_ctrl_enable   = 0x00000001UL, /**< CTRL.ENABLE  bit.    */
  k_ra8_rsip_mask_ctrl_bist     = 0x00000002UL, /**< CTRL.BIST    bit.    */
  k_ra8_rsip_mask_ctrl_reset    = 0x00000004UL, /**< CTRL.RESET   bit.    */
  k_ra8_rsip_mask_ctrl_dpa_arm  = 0x00000008UL, /**< CTRL.DPA_ARM bit.    */
  k_ra8_rsip_mask_ctrl_irq_glob = 0x00000010UL, /**< CTRL.IRQ_GLOB bit.   */
  k_ra8_rsip_mask_status_ready  = 0x00000001UL, /**< STATUS.READY bit.    */
  k_ra8_rsip_mask_status_busy   = 0x00000002UL, /**< STATUS.BUSY  bit.    */
  k_ra8_rsip_mask_status_bistok = 0x00000004UL, /**< STATUS.BIST_OK.      */
  k_ra8_rsip_mask_status_kvok   = 0x00000008UL, /**< STATUS.KV_OK.        */
  k_ra8_rsip_mask_status_hukok  = 0x00000010UL, /**< STATUS.HUK_OK.       */
  k_ra8_rsip_mask_status_dpa_on = 0x00000020UL, /**< STATUS.DPA_ON.       */
  k_ra8_rsip_mask_status_err    = 0x00000080UL, /**< STATUS.ERR   bit.    */
  k_ra8_rsip_mask_isr_done      = 0x00000001UL, /**< ISR.DONE      bit.   */
  k_ra8_rsip_mask_isr_rnd       = 0x00000002UL, /**< ISR.RND       bit.   */
  k_ra8_rsip_mask_isr_hash      = 0x00000004UL, /**< ISR.HASH      bit.   */
  k_ra8_rsip_mask_isr_asym_done = 0x00000008UL, /**< ISR.ASYM_DONE bit.   */
  k_ra8_rsip_mask_isr_kv_done   = 0x00000010UL, /**< ISR.KV_DONE   bit.   */
  k_ra8_rsip_mask_isr_kdf_done  = 0x00000020UL, /**< ISR.KDF_DONE  bit.   */
  k_ra8_rsip_mask_isr_err       = 0x00000040UL, /**< ISR.ERR       bit.   */
  k_ra8_rsip_mask_isr_tamper    = 0x00000080UL, /**< ISR.TAMPER    bit.   */
  k_ra8_rsip_mask_isr_all       = 0x000000FFUL, /**< Union of ISR bits.   */
  k_ra8_rsip_mask_kv_busy       = 0x80000000UL, /**< Key-op status BUSY.  */
  k_ra8_rsip_mask_kv_ready      = 0x80000000UL, /**< Key-op status READY. */
} ra8_rsip_mask_t;

/**
 * @enum ra8_rsip_hash_alg_t
 * @brief Algorithm selector value written to the HASH CTRL word.
 *
 * @details
 * Modelled algorithm-selector values for the RSIP HASH CTRL word. The
 * shipping hash path is software SHA-256 (``ra8_rsip_sha256``), not this
 * selector; HUM Ch 52 is a feature overview with no hash register map
 * (issue #215).
 */
typedef enum : uint32_t {
  k_ra8_rsip_hash_sha224     = 0x00000001UL, /**< SHA-224.               */
  k_ra8_rsip_hash_sha256     = 0x00000002UL, /**< SHA-256.               */
  k_ra8_rsip_hash_sha384     = 0x00000003UL, /**< SHA-384.               */
  k_ra8_rsip_hash_sha512     = 0x00000004UL, /**< SHA-512.               */
  k_ra8_rsip_hash_sha512_224 = 0x00000005UL, /**< SHA-512/224.           */
  k_ra8_rsip_hash_sha512_256 = 0x00000006UL, /**< SHA-512/256.           */
  k_ra8_rsip_hash_sha3_224   = 0x00000011UL, /**< SHA3-224 FIPS PUB 202. */
  k_ra8_rsip_hash_sha3_256   = 0x00000012UL, /**< SHA3-256 FIPS PUB 202. */
  k_ra8_rsip_hash_sha3_384   = 0x00000013UL, /**< SHA3-384 FIPS PUB 202. */
  k_ra8_rsip_hash_sha3_512   = 0x00000014UL, /**< SHA3-512 FIPS PUB 202. */
  k_ra8_rsip_hash_shake128   = 0x00000018UL, /**< SHAKE128 FIPS PUB 202. */
  k_ra8_rsip_hash_shake256   = 0x00000019UL, /**< SHAKE256 FIPS PUB 202. */
} ra8_rsip_hash_alg_t;

/**
 * @enum ra8_rsip_oem_cmd_t
 * @brief OEM-key install opcode written to ``OEM_CTRL``.
 *
 * @details
 * Mirrors the ``rsip_oem_cmd_t`` table in the FSP key-injection
 * driver (``r_rsip_key_injection.c``). These opcodes select which
 * algorithm + key length the OEM-flow installs into the wrapped
 * key vault. HUM Ch 52 documents no key-vault register map (issue
 * #215); the opcode values come from the FSP primitive above.
 */
typedef enum : uint32_t {
  k_ra8_rsip_oem_cmd_invalid             = 0U,  /**< Sentinel / unused.      */
  k_ra8_rsip_oem_cmd_aes128              = 5U,  /**< AES-128.                */
  k_ra8_rsip_oem_cmd_aes192              = 6U,  /**< AES-192.                */
  k_ra8_rsip_oem_cmd_aes256              = 7U,  /**< AES-256.                */
  k_ra8_rsip_oem_cmd_aes128_xts          = 8U,  /**< AES-128 XTS.            */
  k_ra8_rsip_oem_cmd_aes256_xts          = 9U,  /**< AES-256 XTS.            */
  k_ra8_rsip_oem_cmd_rsa2048_priv        = 13U, /**< RSA-2048 private.       */
  k_ra8_rsip_oem_cmd_rsa3072_priv        = 15U, /**< RSA-3072 private.       */
  k_ra8_rsip_oem_cmd_rsa4096_priv        = 17U, /**< RSA-4096 private.       */
  k_ra8_rsip_oem_cmd_ecc_secp256r1_priv  = 23U, /**< ECC NIST P-256 private. */
  k_ra8_rsip_oem_cmd_ecc_secp384r1_priv  = 25U, /**< ECC NIST P-384 private. */
  k_ra8_rsip_oem_cmd_hmac_sha224         = 26U, /**< HMAC-SHA-224.           */
  k_ra8_rsip_oem_cmd_hmac_sha256         = 27U, /**< HMAC-SHA-256.           */
  k_ra8_rsip_oem_cmd_ecc_brain256r1_priv = 29U, /**< ECC Brainpool P-256.    */
  k_ra8_rsip_oem_cmd_ecc_brain384r1_priv = 31U, /**< ECC Brainpool P-384.    */
  k_ra8_rsip_oem_cmd_ecc_brain512r1_priv = 33U, /**< ECC Brainpool P-512.    */
  k_ra8_rsip_oem_cmd_ecc_secp256k1_priv  = 35U, /**< ECC secp256k1 private.  */
  k_ra8_rsip_oem_cmd_ecc_secp521r1_priv  = 37U, /**< ECC NIST P-521 private. */
  k_ra8_rsip_oem_cmd_ecc_ed25519_priv    = 39U, /**< Ed25519 private.        */
  k_ra8_rsip_oem_cmd_hmac_sha384         = 40U, /**< HMAC-SHA-384.           */
  k_ra8_rsip_oem_cmd_hmac_sha512         = 41U, /**< HMAC-SHA-512.           */
  k_ra8_rsip_oem_cmd_hmac_sha512_224     = 42U, /**< HMAC-SHA-512/224.       */
  k_ra8_rsip_oem_cmd_hmac_sha512_256     = 43U, /**< HMAC-SHA-512/256.       */
  k_ra8_rsip_oem_cmd_chacha20            = 48U, /**< ChaCha20.               */
} ra8_rsip_oem_cmd_t;

/**
 * @enum ra8_rsip_kv_op_t
 * @brief Key-vault command word values written to ``KV_CTRL``.
 */
typedef enum : uint32_t {
  k_ra8_rsip_kv_op_idle  = 0x00000000UL, /**< No-op.                         */
  k_ra8_rsip_kv_op_read  = 0x00000001UL, /**< Read slot -> ``KV_DATA``.      */
  k_ra8_rsip_kv_op_write = 0x00000002UL, /**< Write ``KV_DATA`` -> slot.     */
  k_ra8_rsip_kv_op_erase = 0x00000003UL, /**< Zeroise slot.                  */
  k_ra8_rsip_kv_op_count = 0x00000004UL, /**< Snapshot populated-slot count. */
} ra8_rsip_kv_op_t;

/**
 * @enum ra8_rsip_sym_alg_t
 * @brief Symmetric-cipher algorithm selector low byte of ``SYM_CTRL``.
 *
 * @details
 * The cipher engine multiplexes AES-128/192/256 and ChaCha20 across
 * the same data path. The low byte selects the algorithm + key
 * length; the next byte selects the mode (``ra8_rsip_aes_mode_t``);
 * the next byte selects the direction (``ra8_rsip_aes_dir_t``).
 */
typedef enum : uint8_t {
  k_ra8_rsip_sym_alg_aes128   = 1U, /**< AES with a 128-bit key. */
  k_ra8_rsip_sym_alg_aes192   = 2U, /**< AES with a 192-bit key. */
  k_ra8_rsip_sym_alg_aes256   = 3U, /**< AES with a 256-bit key. */
  k_ra8_rsip_sym_alg_chacha20 = 4U, /**< ChaCha20 stream cipher. */
} ra8_rsip_sym_alg_t;

/**
 * @enum ra8_rsip_handle_word_t
 * @brief Wrapped-key handle word counts (FSP ``rsip_wrapped_key_t``).
 *
 * @details
 * A wrapped key blob is a 32-bit ``alg`` selector plus a number of
 * 32-bit body words; the body word counts come from
 * ``RSIP_WORD_SIZE_WRAPPED_KEY_*`` in the FSP private header.
 */
typedef enum : uint16_t {
  k_ra8_rsip_handle_words_aes128       = 8U,   /**< AES-128 wrapped-key body.  */
  k_ra8_rsip_handle_words_aes192       = 12U,  /**< AES-192 wrapped-key body.  */
  k_ra8_rsip_handle_words_aes256       = 12U,  /**< AES-256 wrapped-key body.  */
  k_ra8_rsip_handle_words_chacha20     = 12U,  /**< ChaCha20 wrapped key body. */
  k_ra8_rsip_handle_words_hmac_sha224  = 12U,  /**< HMAC-SHA-224 wrapped key.  */
  k_ra8_rsip_handle_words_hmac_sha256  = 12U,  /**< HMAC-SHA-256 wrapped key.  */
  k_ra8_rsip_handle_words_hmac_sha384  = 16U,  /**< HMAC-SHA-384 wrapped key.  */
  k_ra8_rsip_handle_words_hmac_sha512  = 20U,  /**< HMAC-SHA-512 wrapped key.  */
  k_ra8_rsip_handle_words_ecc256_priv  = 12U,  /**< ECC P-256 wrapped private. */
  k_ra8_rsip_handle_words_ecc384_priv  = 16U,  /**< ECC P-384 wrapped private. */
  k_ra8_rsip_handle_words_ecc521_priv  = 24U,  /**< ECC P-521 wrapped private. */
  k_ra8_rsip_handle_words_rsa2048_priv = 132U, /**< RSA-2048 wrapped private.  */
  k_ra8_rsip_handle_words_rsa3072_priv = 196U, /**< RSA-3072 wrapped private.  */
  k_ra8_rsip_handle_words_rsa4096_priv = 260U, /**< RSA-4096 wrapped private.  */
} ra8_rsip_handle_word_t;

/**
 * @enum ra8_rsip_kw_op_t
 * @brief Key-wrap command word values written to ``KW_CTRL``.
 */
typedef enum : uint32_t {
  k_ra8_rsip_kw_op_wrap   = 0x00000001UL, /**< Wrap a plaintext handle -> blob. */
  k_ra8_rsip_kw_op_unwrap = 0x00000002UL, /**< Unwrap a blob -> handle.         */
} ra8_rsip_kw_op_t;

/**
 * @enum ra8_rsip_dotf_route_t
 * @brief DOTF key route command value written to ``DOTFn_CTRL``.
 */
typedef enum : uint32_t {
  k_ra8_rsip_dotf_off = 0x00000000UL, /**< Disable key feed.               */
  k_ra8_rsip_dotf_on  = 0x00000001UL, /**< Feed key from slot bits 31..16. */
} ra8_rsip_dotf_route_t;

/**
 * @enum ra8_rsip_kdf_op_t
 * @brief KDF mode selector written to ``KDF_CTRL``.
 */
typedef enum : uint32_t {
  k_ra8_rsip_kdf_op_huk_label   = 0x00000001UL, /**< Derive from HUK + label. */
  k_ra8_rsip_kdf_op_uid_label   = 0x00000002UL, /**< Derive from UID + label. */
  k_ra8_rsip_kdf_op_hkdf_sha256 = 0x00000010UL, /**< HKDF SHA-256.            */
  k_ra8_rsip_kdf_op_hkdf_sha384 = 0x00000011UL, /**< HKDF SHA-384.            */
  k_ra8_rsip_kdf_op_hkdf_sha512 = 0x00000012UL, /**< HKDF SHA-512.            */
} ra8_rsip_kdf_op_t;

/**
 * @enum ra8_rsip_life_state_t
 * @brief Device-lifecycle state values (off-target-only model -- no documented
 *        RA8D2 register; real state is in the DLM, see ra8_rsip_devsec.c).
 */
typedef enum : uint32_t {
  k_ra8_rsip_life_cm     = 0x00000000UL, /**< Chip-manufacturing.          */
  k_ra8_rsip_life_ssd    = 0x00000001UL, /**< Secure software development. */
  k_ra8_rsip_life_nsecsd = 0x00000002UL, /**< Non-secure software dev.     */
  k_ra8_rsip_life_dpl    = 0x00000003UL, /**< Deployed.                    */
  k_ra8_rsip_life_lcm    = 0x00000004UL, /**< Locked.                      */
  k_ra8_rsip_life_rma    = 0x00000005UL, /**< Return-merchandise-auth.     */
} ra8_rsip_life_state_t;

/**
 * @enum ra8_rsip_debug_level_t
 * @brief Debug authorisation levels AL0/AL1/AL2 (off-target-only model -- no
 *        documented RA8D2 register; real state is in the DLM, see
 *        ra8_rsip_devsec.c).
 */
typedef enum : uint8_t {
  k_ra8_rsip_debug_al0 = 0U, /**< AL0 -- no debug functions available. */
  k_ra8_rsip_debug_al1 = 1U, /**< AL1 -- non-secure debug only.        */
  k_ra8_rsip_debug_al2 = 2U, /**< AL2 -- secure + non-secure debug.    */
} ra8_rsip_debug_level_t;

/**
 * @enum ra8_rsip_aes_mode_t
 * @brief Block-cipher mode selector for the AES engine.
 *
 * @details
 * HUM Ch 52.1 Table 52.1 p 3302: AES supports ECB, CBC, CTR
 * (NIST SP 800-38A), CCM (38C), GCM (38D), XTS (IEEE 1619-2007),
 * CMAC (38B), GMAC (38D).
 */
typedef enum : uint8_t {
  k_ra8_rsip_aes_mode_ecb  = 0U, /**< ECB.        */
  k_ra8_rsip_aes_mode_cbc  = 1U, /**< CBC.        */
  k_ra8_rsip_aes_mode_ctr  = 2U, /**< CTR.        */
  k_ra8_rsip_aes_mode_gcm  = 3U, /**< GCM (AEAD). */
  k_ra8_rsip_aes_mode_ccm  = 4U, /**< CCM (AEAD). */
  k_ra8_rsip_aes_mode_xts  = 5U, /**< XTS.        */
  k_ra8_rsip_aes_mode_cmac = 6U, /**< CMAC.       */
  k_ra8_rsip_aes_mode_gmac = 7U, /**< GMAC.       */
} ra8_rsip_aes_mode_t;

/**
 * @enum ra8_rsip_aes_dir_t
 * @brief Direction selector for symmetric ciphers.
 */
typedef enum : uint8_t {
  k_ra8_rsip_dir_encrypt = 0U, /**< Plaintext -> Ciphertext. */
  k_ra8_rsip_dir_decrypt = 1U, /**< Ciphertext -> Plaintext. */
} ra8_rsip_aes_dir_t;

/**
 * @enum ra8_rsip_curve_t
 * @brief ECC curve selector (HUM Ch 52.1 Table 52.1 "ECC" p 3302).
 */
typedef enum : uint8_t {
  k_ra8_rsip_curve_secp192r1  = 0U, /**< NIST P-192.        */
  k_ra8_rsip_curve_secp224r1  = 1U, /**< NIST P-224.        */
  k_ra8_rsip_curve_secp256r1  = 2U, /**< NIST P-256.        */
  k_ra8_rsip_curve_secp384r1  = 3U, /**< NIST P-384.        */
  k_ra8_rsip_curve_secp521r1  = 4U, /**< NIST P-521.        */
  k_ra8_rsip_curve_brain256r1 = 5U, /**< Brainpool P-256r1. */
  k_ra8_rsip_curve_brain384r1 = 6U, /**< Brainpool P-384r1. */
  k_ra8_rsip_curve_brain512r1 = 7U, /**< Brainpool P-512r1. */
  k_ra8_rsip_curve_ed25519    = 8U, /**< Ed25519.           */
  k_ra8_rsip_curve_secp256k1  = 9U, /**< secp256k1.         */
} ra8_rsip_curve_t;

/**
 * @enum ra8_rsip_rsa_size_t
 * @brief RSA key-size selector (HUM Ch 52.1 Table 52.1 "RSA" p 3302).
 */
typedef enum : uint16_t {
  k_ra8_rsip_rsa_1024 = 1024U, /**< 1024-bit RSA. */
  k_ra8_rsip_rsa_2048 = 2048U, /**< 2048-bit RSA. */
  k_ra8_rsip_rsa_3072 = 3072U, /**< 3072-bit RSA. */
  k_ra8_rsip_rsa_4096 = 4096U, /**< 4096-bit RSA. */
} ra8_rsip_rsa_size_t;

/**
 * @enum ra8_rsip_asym_op_t
 * @brief Opcode written to ``ASYM_CTRL`` to select the operation.
 *
 * @details
 * EdDSA (Ed25519 PureEdDSA, RFC 8032) is a distinct opcode from
 * ECDSA: the Edwards-curve signature scheme hashes the message
 * internally (SHA-512) and is not interchangeable with the
 * Weierstrass-curve ECDSA modular-exponentiation path. Routing an
 * Ed25519 key through ``..._ecdsa_sign`` would NOT produce a valid
 * RFC 8032 signature, so the EdDSA sign / verify operations carry
 * their own opcodes here.
 */
typedef enum : uint32_t {
  k_ra8_rsip_asym_op_rsa_sign     = 0x00000001UL, /**< RSA signature gen.        */
  k_ra8_rsip_asym_op_rsa_verify   = 0x00000002UL, /**< RSA signature ver.        */
  k_ra8_rsip_asym_op_rsa_encrypt  = 0x00000003UL, /**< RSA public encrypt.       */
  k_ra8_rsip_asym_op_rsa_decrypt  = 0x00000004UL, /**< RSA private decrypt.      */
  k_ra8_rsip_asym_op_ecdsa_sign   = 0x00000005UL, /**< ECDSA signature gen.      */
  k_ra8_rsip_asym_op_ecdsa_verify = 0x00000006UL, /**< ECDSA signature ver.      */
  k_ra8_rsip_asym_op_ecdh_compute = 0x00000007UL, /**< ECDH shared secret.       */
  k_ra8_rsip_asym_op_ecc_keygen   = 0x00000008UL, /**< ECC keypair gen.          */
  k_ra8_rsip_asym_op_eddsa_sign   = 0x00000009UL, /**< Ed25519 PureEdDSA sign.   */
  k_ra8_rsip_asym_op_eddsa_verify = 0x0000000AUL, /**< Ed25519 PureEdDSA verify. */
} ra8_rsip_asym_op_t;

/**
 * @enum ra8_rsip_rsa_pad_t
 * @brief RSA encryption padding-scheme selector written to ``ASYM_ARG``.
 *
 * @details
 * Selects the RSAES padding applied around the RSA primitive for the
 * public-encrypt (``ra8_rsip_rsa_encrypt``) and private-decrypt
 * (``ra8_rsip_rsa_decrypt``) entry points. RSAES-OAEP (PKCS #1 v2.2
 * Section 7.1, SHA-256 MGF1) is the recommended scheme for RSA-OAEP
 * key transport; RSAES-PKCS1-v1_5 (Section 7.2) is provided for
 * interop with legacy peers.
 */
typedef enum : uint32_t {
  k_ra8_rsip_rsa_pad_pkcs1 = 0x00000001UL, /**< RSAES-PKCS1-v1_5 (legacy). */
  k_ra8_rsip_rsa_pad_oaep  = 0x00000002UL, /**< RSAES-OAEP (SHA-256 MGF1). */
} ra8_rsip_rsa_pad_t;

/**
 * @enum ra8_rsip_tamper_src_t
 * @brief Tamper-source identifier bits inside ``TAMPER_CTRL`` /
 *        ``TAMPER_STATUS``.
 *
 * @details
 * Invented off-target-only model: six external tamper lines plus three
 * internal sources (RSIP self-fault, voltage glitch, clock glitch).
 * The RA8D2 exposes no such RSIP tamper register -- real tamper is an
 * I/O-port feature whose response is handled by the RTC / MRAM / VBATT
 * blocks. See ra8_rsip_devsec.c.
 */
typedef enum : uint32_t {
  k_ra8_rsip_tamper_src_ext0 = 0x00000001UL, /**< External tamper input 0.  */
  k_ra8_rsip_tamper_src_ext1 = 0x00000002UL, /**< External tamper input 1.  */
  k_ra8_rsip_tamper_src_ext2 = 0x00000004UL, /**< External tamper input 2.  */
  k_ra8_rsip_tamper_src_ext3 = 0x00000008UL, /**< External tamper input 3.  */
  k_ra8_rsip_tamper_src_ext4 = 0x00000010UL, /**< External tamper input 4.  */
  k_ra8_rsip_tamper_src_ext5 = 0x00000020UL, /**< External tamper input 5.  */
  k_ra8_rsip_tamper_src_volt = 0x00010000UL, /**< Voltage-glitch detector.  */
  k_ra8_rsip_tamper_src_clk  = 0x00020000UL, /**< Clock-glitch detector.    */
  k_ra8_rsip_tamper_src_self = 0x00040000UL, /**< RSIP internal self-fault. */
  k_ra8_rsip_tamper_src_all  = 0x0007003FUL, /**< Union of all sources.     */
} ra8_rsip_tamper_src_t;

/**
 * @enum ra8_rsip_chacha_op_t
 * @brief ChaCha20 / Poly1305 operation selector.
 */
typedef enum : uint32_t {
  k_ra8_rsip_chacha_op_encrypt      = 0x00000001UL, /**< ChaCha20 stream encrypt. */
  k_ra8_rsip_chacha_op_decrypt      = 0x00000002UL, /**< ChaCha20 stream decrypt. */
  k_ra8_rsip_chacha_op_aead_encrypt = 0x00000003UL, /**< ChaCha20-Poly1305 enc.   */
  k_ra8_rsip_chacha_op_aead_decrypt = 0x00000004UL, /**< ChaCha20-Poly1305 dec.   */
  k_ra8_rsip_chacha_op_poly1305_mac = 0x00000005UL, /**< Poly1305 MAC only.       */
} ra8_rsip_chacha_op_t;

/**
 * @enum ra8_rsip_sizes_t
 * @brief Buffer sizes published to callers.
 *
 * @details
 * AES block size = 128 bits (HUM Ch 52.1 Table 52.1 "Symmetric
 * cryptography" p 3302). Hash digest sizes follow FIPS PUB 180-4 /
 * FIPS PUB 202.
 */
typedef enum : uint16_t {
  k_ra8_rsip_sha224_digest_bytes     = 28U, /**< SHA-224 digest length.  */
  k_ra8_rsip_sha256_digest_bytes     = 32U, /**< SHA-256 digest length.  */
  k_ra8_rsip_sha256_digest_words     = 8U,  /**< SHA-256 = 8 * uint32_t. */
  k_ra8_rsip_sha384_digest_bytes     = 48U, /**< SHA-384 digest length.  */
  k_ra8_rsip_sha512_digest_bytes     = 64U, /**< SHA-512 digest length.  */
  k_ra8_rsip_sha512_224_digest_bytes = 28U, /**< SHA-512/224 length.     */
  k_ra8_rsip_sha512_256_digest_bytes = 32U, /**< SHA-512/256 length.     */
  k_ra8_rsip_sha3_224_digest_bytes   = 28U, /**< SHA3-224 length.        */
  k_ra8_rsip_sha3_256_digest_bytes   = 32U, /**< SHA3-256 length.        */
  k_ra8_rsip_sha3_384_digest_bytes   = 48U, /**< SHA3-384 length.        */
  k_ra8_rsip_sha3_512_digest_bytes   = 64U, /**< SHA3-512 length.        */
  k_ra8_rsip_aes_block_bytes         = 16U, /**< AES block = 128 bits.   */
  k_ra8_rsip_aes128_key_bytes        = 16U, /**< AES-128 key length.     */
  k_ra8_rsip_aes192_key_bytes        = 24U, /**< AES-192 key length.     */
  k_ra8_rsip_aes256_key_bytes        = 32U, /**< AES-256 key length.     */
  k_ra8_rsip_chacha_key_bytes        = 32U, /**< ChaCha20 256-bit key.   */
  k_ra8_rsip_chacha_nonce_bytes      = 12U, /**< RFC 7539 12-byte nonce. */
  k_ra8_rsip_poly1305_tag_bytes      = 16U, /**< Poly1305 16-byte MAC.   */
  k_ra8_rsip_gcm_tag_bytes           = 16U, /**< AES-GCM 128-bit tag.    */
  k_ra8_rsip_ccm_tag_bytes           = 16U, /**< AES-CCM max tag = 128b. */
  k_ra8_rsip_huk_bytes               = 32U, /**< 256-bit HUK.            */
  k_ra8_rsip_uid_bytes               = 16U, /**< 128-bit Unique ID.      */
  k_ra8_rsip_trng_word_bytes         = 4U,  /**< Bytes per TRNG read.    */
  k_ra8_rsip_kv_slot_bytes           = 64U, /**< Wrapped-key blob size.  */
  k_ra8_rsip_kv_slot_count           = 16U, /**< Number of vault slots.  */
} ra8_rsip_sizes_t;

/**
 * @brief Volatile pointer to a 32-bit RSIP register at ``offset``.
 *
 * @details
 * Mirrors the ``ra8_glcdc_reg32`` accessor pattern so the host test
 * build can intercept reads and writes via ``ra8_fake_mmap``. The
 * accessor performs no validation; callers pass a value from
 * ``ra8_rsip_off_t``.
 *
 * @param[in] offset One of the ``k_ra8_rsip_off_*`` values.
 * @return Volatile pointer to the register cell.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile uint32_t* ra8_rsip_reg32(ra8_rsip_off_t offset)
{
  return (volatile uint32_t*)(k_ra8_rsip_base_addr + (uintptr_t)offset);
}

#ifdef __cplusplus
}
#endif
