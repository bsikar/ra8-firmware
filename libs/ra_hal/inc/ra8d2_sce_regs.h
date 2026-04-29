/**
 * @file ra8d2_sce_regs.h
 * @brief Renesas Secure Crypto Engine (SCE) register layout for the RA8D2
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * The Secure Crypto Engine (SCE) on the RA8D2 is the legacy
 * symmetric / hash / TRNG primitive layer that lives behind the
 * RSIP-E50D mailbox (HUM Ch 52 "Renesas Secure IP (RSIP-E50D)"
 * p 3302-3307). The RSIP-E50D drives the same silicon block, but
 * exposes a high-level mailbox protocol; the SCE driver in this
 * file talks to the lower-level register surface that FSP's
 * ``r_sce`` family wraps in its closed-source primitive layer.
 *
 * Module-stop: ``MSTPCRC.MSTPC31`` (HUM Ch 11.2.8 "MSTPCRC: Module
 * Stop Control Register C" p 446-447). Released alongside the RSIP
 * because the two share the silicon block; ``ra_mstp_enable`` is
 * reference counted so they coexist.
 *
 * ## Register window
 *
 * The SCE primitive surface is exposed through a small command /
 * data-FIFO mailbox at ``0x403A0000`` (chosen so the register window
 * does not collide with the RSIP-E50D mailbox at ``0x403B0000``).
 * The address is carried as a typed ``uintptr_t`` enum to satisfy
 * the project's no-magic-numbers + 64-bit unit-test-host rules.
 *
 *  | Offset           | Block           | Purpose                              |
 *  |-----------------:|-----------------|--------------------------------------|
 *  | 0x0000           | CTRL            | Engine enable + reset.               |
 *  | 0x0004           | STATUS          | BUSY / DONE / READY / ERR.           |
 *  | 0x0008           | CMD             | Command opcode.                      |
 *  | 0x000C           | CFG             | Mode + key-bits + dir.               |
 *  | 0x0010 .. 0x002F | KEY[0..7]       | 256-bit key staging FIFO.            |
 *  | 0x0030 .. 0x003F | IV[0..3]        | 128-bit IV / counter / nonce.        |
 *  | 0x0040           | DATA_IN         | Input word (32-bit).                 |
 *  | 0x0044           | DATA_OUT        | Output word (32-bit).                |
 *  | 0x0048           | TAG             | First word of GCM authenticator.     |
 *  | 0x0050           | RND_DATA        | TRNG output word.                    |
 *  | 0x0054           | RND_STATUS      | TRNG ready bit.                      |
 *  | 0x0058           | HASH_LEN        | Streamed message length in bytes.    |
 *  | 0x005C           | HASH_MODE       | Hash flavour selector.               |
 *  | 0x0060 .. 0x007F | HASH_DIGEST[8]  | Up to 256-bit digest (SHA-256).      |
 *  | 0x0080 .. 0x009F | HMAC_DIGEST[8]  | HMAC tag.                            |
 *
 * The implementation in ``ra_sce.c`` is a host-friendly software
 * stub: it never assumes the engine actually computes anything and
 * keeps the entire crypto state in the mailbox window so the host
 * unit-test build (``RA_SIMULATOR_MODE``) can read back deterministic
 * output. A future drop-in real backend (FSP closed-source library
 * or open-source replacement) only has to honour this register map.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra_sce_addr_t
 * @brief SCE register window base address.
 *
 * @details
 * The legacy SCE primitive layer is mapped at ``0x403A0000``. The
 * MSTPC31 release shared with the RSIP gates clocks to the unified
 * security block on the RA8D2.
 *
 * @invariant ``k_ra_sce_base_addr`` is page-aligned.
 *
 * @see ra_sce_open
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_ra_sce_base_addr = 0x403A0000UL, /**< SCE primitive mailbox base. */
} ra_sce_addr_t;

/**
 * @enum ra_sce_off_t
 * @brief Byte offsets into the SCE register window.
 *
 * @details
 * Each value names a 32-bit register or the start of a multi-word
 * staging area. Multi-word areas use ``..._end`` markers so the
 * driver can range-check streaming writes.
 *
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_ra_sce_off_ctrl        = 0x0000UL, /**< CTRL: enable + soft-reset.         */
  k_ra_sce_off_status      = 0x0004UL, /**< STATUS: BUSY/DONE/READY/ERR bits.  */
  k_ra_sce_off_cmd         = 0x0008UL, /**< CMD: opcode dispatch.              */
  k_ra_sce_off_cfg         = 0x000CUL, /**< CFG: mode + key-bits + dir.        */
  k_ra_sce_off_key_base    = 0x0010UL, /**< KEY[0]: first key word.            */
  k_ra_sce_off_key_end     = 0x0030UL, /**< End-of-key sentinel (exclusive).   */
  k_ra_sce_off_iv_base     = 0x0030UL, /**< IV[0]: first IV/counter word.      */
  k_ra_sce_off_iv_end      = 0x0040UL, /**< End-of-IV sentinel.                */
  k_ra_sce_off_data_in     = 0x0040UL, /**< DATA_IN: input word.               */
  k_ra_sce_off_data_out    = 0x0044UL, /**< DATA_OUT: output word.             */
  k_ra_sce_off_tag         = 0x0048UL, /**< TAG: GCM tag word 0.               */
  k_ra_sce_off_rnd_data    = 0x0050UL, /**< RND_DATA: TRNG output word.        */
  k_ra_sce_off_rnd_status  = 0x0054UL, /**< RND_STATUS: TRNG ready bit.        */
  k_ra_sce_off_hash_len    = 0x0058UL, /**< HASH_LEN: total bytes streamed.    */
  k_ra_sce_off_hash_mode   = 0x005CUL, /**< HASH_MODE: hash flavour selector.  */
  k_ra_sce_off_hash_digest = 0x0060UL, /**< HASH_DIGEST[0]: first digest word. */
  k_ra_sce_off_hash_end    = 0x0080UL, /**< End-of-digest sentinel.            */
  k_ra_sce_off_hmac_digest = 0x0080UL, /**< HMAC_DIGEST[0]: HMAC tag word 0.   */
  k_ra_sce_off_hmac_end    = 0x00A0UL, /**< End-of-HMAC sentinel.              */
} ra_sce_off_t;

/**
 * @enum ra_sce_ctrl_bit_t
 * @brief Bit positions in the CTRL register.
 *
 * @details
 * ``ENABLE`` clocks the engine; ``RESET`` triggers a one-shot soft
 * reset that auto-clears once the internal pipeline drains.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_ctrl_enable = 1UL << 0, /**< 1 -> engine clocked.       */
  k_ra_sce_ctrl_reset  = 1UL << 1, /**< 1 -> request soft reset.   */
} ra_sce_ctrl_bit_t;

/**
 * @enum ra_sce_status_bit_t
 * @brief Bit positions in the STATUS register.
 *
 * @details
 * ``READY`` flags engine power-up + reset complete. ``BUSY`` is
 * latched while a command runs. ``DONE`` signals operation complete
 * (write-1-to-clear). ``ERR`` flags illegal-config / tag-mismatch.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_status_ready = 1UL << 0, /**< 1 -> engine ready for a command.   */
  k_ra_sce_status_busy  = 1UL << 1, /**< 1 -> a command is in flight.       */
  k_ra_sce_status_done  = 1UL << 2, /**< 1 -> last command completed.       */
  k_ra_sce_status_err   = 1UL << 3, /**< 1 -> last command produced error.  */
} ra_sce_status_bit_t;

/**
 * @enum ra_sce_cmd_op_t
 * @brief Opcode values written to CMD to dispatch an operation.
 *
 * @details
 * Mirrors the FSP r_sce primitive opcode space (``HW_SCE_*`` family)
 * but condensed to the operations exposed by the public HAL.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_cmd_aes_encrypt = 0x10UL, /**< AES single-shot encrypt.   */
  k_ra_sce_cmd_aes_decrypt = 0x11UL, /**< AES single-shot decrypt.   */
  k_ra_sce_cmd_aes_finish  = 0x12UL, /**< Finalise (GCM tag emit).   */
  k_ra_sce_cmd_sha_init    = 0x20UL, /**< Hash context init.         */
  k_ra_sce_cmd_sha_update  = 0x21UL, /**< Hash absorb block.         */
  k_ra_sce_cmd_sha_final   = 0x22UL, /**< Hash squeeze digest.       */
  k_ra_sce_cmd_trng_read   = 0x30UL, /**< TRNG fill output word.     */
  k_ra_sce_cmd_hmac_init   = 0x40UL, /**< HMAC context init.         */
  k_ra_sce_cmd_hmac_update = 0x41UL, /**< HMAC absorb block.         */
  k_ra_sce_cmd_hmac_final  = 0x42UL, /**< HMAC squeeze MAC.          */
} ra_sce_cmd_op_t;

/**
 * @enum ra_sce_const_t
 * @brief Sizing constants used across the SCE register layout.
 *
 * @details
 * The block size for AES is fixed at 16 bytes; SHA-256 emits 32
 * bytes; the TRNG word is 4 bytes wide. The maximum key payload is
 * sized for AES-256 (32 bytes).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_aes_block_bytes     = 16UL, /**< AES block size.               */
  k_ra_sce_aes_max_key_bytes   = 32UL, /**< AES-256 key size.             */
  k_ra_sce_iv_bytes            = 16UL, /**< IV staging area size.         */
  k_ra_sce_trng_word_bytes     = 4UL,  /**< TRNG read granule.            */
  k_ra_sce_sha1_digest_bytes   = 20UL, /**< SHA-1 digest size.            */
  k_ra_sce_sha224_digest_bytes = 28UL, /**< SHA-224 digest size.          */
  k_ra_sce_sha256_digest_bytes = 32UL, /**< SHA-256 digest size.          */
  k_ra_sce_sha384_digest_bytes = 48UL, /**< SHA-384 digest size.          */
  k_ra_sce_sha512_digest_bytes = 64UL, /**< SHA-512 digest size.          */
  k_ra_sce_max_digest_bytes    = 64UL, /**< Largest digest the HAL emits. */
  k_ra_sce_gcm_tag_bytes       = 16UL, /**< AES-GCM tag size.             */
  k_ra_sce_status_poll_budget  = 64UL, /**< Status-poll loop bound.       */
} ra_sce_const_t;

#ifdef __cplusplus
}
#endif
