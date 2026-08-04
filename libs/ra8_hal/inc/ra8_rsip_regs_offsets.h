/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_rsip_regs_offsets.h
 * @brief RSIP-E50D register base address, byte offsets, and layout asserts
 * @ingroup grp_hal_crypto
 *
 * @details
 * Sub-header of @ref ra8_rsip_regs.h. Carries the memory-mapped base
 * address (@ref ra8_rsip_addr_t), the full byte-offset table for every
 * register the HAL touches (@ref ra8_rsip_off_t), the sub-block window
 * boundary constants (@ref ra8_rsip_window_t), and the compile-time
 * layout-insurance asserts (alignment + sub-block containment sweep)
 * that lock the offset table at build time.
 *
 * The RSIP-E50D engine on the RA8D2 is an isolated security
 * subsystem (HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" p 3302). It
 * is mailbox-driven; the HUM body for Ch 52 (p 3302-3307) does not
 * publish a register table, so every offset here is cross-referenced
 * to the FSP primitive register file
 * ``crypto_procedures_protected/.../ra8_rsip_e50d/r_rsip_reg.h``.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @enum ra8_rsip_addr_t
 * @brief Memory-mapped base address for the RSIP-E50D engine.
 *
 * @details
 * Verified against FSP ``r_rsip_addr.h`` for the E50D variant
 * (``RSIP_PRV_ADDR_BASE = 0x403B0000UL``). The block lives inside
 * the secure peripheral bus window at 0x40000000, gated by
 * MSTPCRC.MSTPC31 (HUM Ch 11.2.8 "MSTPCRC" p 446 + HUM Ch 52.3.2
 * "Module-Stop Function Setting" p 3307).
 */
typedef enum : uintptr_t {
  k_ra8_rsip_base_addr = 0x403B0000UL, /**< RSIP-E50D mailbox window base. */
} ra8_rsip_addr_t;

/**
 * @enum ra8_rsip_off_t
 * @brief Byte offsets of every register the HAL touches.
 *
 * @details
 * Offsets are derived from the FSP primitive register file
 * ``r_rsip_reg.h`` and the ``WR1_PROG``/``RD1_MASK``/``WAIT_STS``
 * sites scattered through ``crypto_procedures_protected/.../ra8_rsip_e50d``.
 * Each cell is a 32-bit word. The HUM body for Ch 52 (p 3302-3307)
 * does not enumerate an explicit register table; the engine is
 * mailbox-driven.
 */
typedef enum : uint16_t {
  /* ---- Control / status mailbox (sub-block @ 0x0000) ------------------- */
  k_ra8_rsip_off_ctrl   = 0x0000U, /**< Engine control word.                 */
  k_ra8_rsip_off_status = 0x0004U, /**< Status / state monitor.              */
  k_ra8_rsip_off_ier    = 0x0008U, /**< Interrupt enable.                    */
  k_ra8_rsip_off_isr    = 0x000CU, /**< Interrupt status (W1C).              */
  k_ra8_rsip_off_bist   = 0x0010U, /**< Built-in self-test trigger / result. */

  /* ---- Tamper / SPA-DPA control (HUM Ch 52.3.3 p 3307) ----------------- */
  k_ra8_rsip_off_tamper_ctrl   = 0x0014U, /**< Tamper-source enable mask.  */
  k_ra8_rsip_off_tamper_status = 0x0018U, /**< Latched tamper-event flags. */
  k_ra8_rsip_off_dpa_ctrl      = 0x001CU, /**< SPA/DPA countermeasure arm. */

  /* ---- DOTF key delivery routing (modelled mailbox; no HUM reg map) ---- */
  k_ra8_rsip_off_dotf0_ctrl = 0x0030U, /**< DOTF0 key route enable + slot. */
  k_ra8_rsip_off_dotf1_ctrl = 0x0034U, /**< DOTF1 key route enable + slot. */

  /* ---- OEM boot loader version (modelled mailbox; no HUM reg map) ------ */
  k_ra8_rsip_off_oem_bl_ver  = 0x0050U, /**< Latched OEM BL Ver counter.  */
  k_ra8_rsip_off_oem_bl_inc  = 0x0054U, /**< Increment trigger (W1).      */
  k_ra8_rsip_off_oem_bl_lock = 0x0058U, /**< Lock-after-validation latch. */

  /* ---- Hardware Unique Key + Unique ID gates --------------------------- */
  k_ra8_rsip_off_huk_ctrl = 0x0070U, /**< HUK access gate (read-only data). */
  k_ra8_rsip_off_uid_ctrl = 0x0074U, /**< Unique-ID access gate.            */

  /* ---- Key Derivation Function mailbox --------------------------------- */
  k_ra8_rsip_off_kdf_ctrl   = 0x0090U, /**< KDF request word.          */
  k_ra8_rsip_off_kdf_status = 0x0094U, /**< KDF complete / busy.       */
  k_ra8_rsip_off_kdf_label  = 0x0098U, /**< KDF context label slot.    */
  k_ra8_rsip_off_kdf_out    = 0x009CU, /**< Derived-key output handle. */

  /* ---- Device lifecycle (HUM Ch 51.1 p 3263 "Device lifecycle") -------- */
  k_ra8_rsip_off_life_state  = 0x00C0U, /**< Current lifecycle state. */
  k_ra8_rsip_off_debug_level = 0x00C4U, /**< AL0/AL1/AL2 debug level. */

  /* ---- Generic command mailbox + arg window ---------------------------- */
  k_ra8_rsip_off_mbox_op   = 0x0100U, /**< Opcode written by host.      */
  k_ra8_rsip_off_mbox_arg0 = 0x0104U, /**< Argument 0.                  */
  k_ra8_rsip_off_mbox_arg1 = 0x0108U, /**< Argument 1.                  */
  k_ra8_rsip_off_mbox_arg2 = 0x010CU, /**< Argument 2.                  */
  k_ra8_rsip_off_mbox_arg3 = 0x0110U, /**< Argument 3.                  */
  k_ra8_rsip_off_mbox_ret  = 0x0140U, /**< Completion code from engine. */

  /* ---- Data window (encrypt/decrypt input + output) ------------------- */
  k_ra8_rsip_off_data_in0  = 0x1400U, /**< Data input lane 0.  */
  k_ra8_rsip_off_data_in1  = 0x1404U, /**< Data input lane 1.  */
  k_ra8_rsip_off_data_in2  = 0x1408U, /**< Data input lane 2.  */
  k_ra8_rsip_off_data_in3  = 0x140CU, /**< Data input lane 3.  */
  k_ra8_rsip_off_data_out0 = 0x1410U, /**< Data output lane 0. */
  k_ra8_rsip_off_data_out1 = 0x1414U, /**< Data output lane 1. */
  k_ra8_rsip_off_data_out2 = 0x1418U, /**< Data output lane 2. */
  k_ra8_rsip_off_data_out3 = 0x141CU, /**< Data output lane 3. */

  /* ---- Wrapped-key staging port + status ------------------------------- */
  k_ra8_rsip_off_key_stage     = 0x1420U, /**< Wrapped-key load port. */
  k_ra8_rsip_off_key_op_status = 0x1424U, /**< Key/data op status (one 32-bit
                                              word past KEY_STAGE, inside the
                                              KEY_STAGE sub-block 0x1420-0x143F
                                              -- the previous 0x1444 collided
                                              with the TRNG status word).     */

  /* ---- Random number generator ----------------------------------------- */
  k_ra8_rsip_off_rnd_ctrl   = 0x1440U, /**< TRNG control word.             */
  k_ra8_rsip_off_rnd_status = 0x1444U, /**< TRNG ready / health flags.     */
  k_ra8_rsip_off_rnd_data   = 0x1458U, /**< TRNG output (32-bit per read). */

  /* ---- Hash engine ----------------------------------------------------- */
  k_ra8_rsip_off_hash_ctrl    = 0x1600U, /**< HASH control (algorithm select). */
  k_ra8_rsip_off_hash_status  = 0x1604U, /**< HASH ready / done flags.         */
  k_ra8_rsip_off_hash_data_in = 0x1608U, /**< HASH input window (32-bit each). */
  k_ra8_rsip_off_hash_digest  = 0x1610U, /**< HASH output digest base.         */
  k_ra8_rsip_off_hash_hmac    = 0x1660U, /**< HMAC mode + key-handle slot.     */

  /* ---- Asymmetric (RSA / ECC) scratch + control ------------------------ */
  k_ra8_rsip_off_asym_ctrl     = 0x1700U, /**< Asymmetric command word.      */
  k_ra8_rsip_off_asym_status   = 0x1704U, /**< Asymmetric busy / done.       */
  k_ra8_rsip_off_asym_curve    = 0x1708U, /**< ECC curve selector.           */
  k_ra8_rsip_off_asym_arg      = 0x170CU, /**< Argument-block descriptor.    */
  k_ra8_rsip_off_asym_key      = 0x1710U, /**< Key-handle slot.              */
  k_ra8_rsip_off_asym_msg_in   = 0x1714U, /**< Message / digest input port.  */
  k_ra8_rsip_off_asym_sig_in   = 0x1718U, /**< Signature input lane.         */
  k_ra8_rsip_off_asym_sig_out  = 0x171CU, /**< Signature output lane.        */
  k_ra8_rsip_off_asym_pub_x    = 0x1720U, /**< ECC peer X / RSA pubkey lane. */
  k_ra8_rsip_off_asym_pub_y    = 0x1724U, /**< ECC peer Y lane.              */
  k_ra8_rsip_off_asym_shared   = 0x1728U, /**< ECDH shared-secret handle.    */
  k_ra8_rsip_off_asym_rsa_size = 0x172CU, /**< RSA modulus-size selector.    */

  /* ---- Wrapped-key vault read/write window ----------------------------- */
  k_ra8_rsip_off_kv_ctrl  = 0x1800U, /**< Vault command (read/write/erase). */
  k_ra8_rsip_off_kv_slot  = 0x1804U, /**< Vault slot index.                 */
  k_ra8_rsip_off_kv_data  = 0x1808U, /**< Vault data port.                  */
  k_ra8_rsip_off_kv_count = 0x180CU, /**< Vault populated-slot count.       */

  /* ---- Key-wrap / key-unwrap engine ------------------------------------ */
  k_ra8_rsip_off_kw_ctrl     = 0x1900U, /**< Key-wrap command word.            */
  k_ra8_rsip_off_kw_kek      = 0x1904U, /**< KEK handle slot.                  */
  k_ra8_rsip_off_kw_iv0      = 0x1908U, /**< Wrap IV lane 0.                   */
  k_ra8_rsip_off_kw_iv1      = 0x190CU, /**< Wrap IV lane 1.                   */
  k_ra8_rsip_off_kw_iv2      = 0x1910U, /**< Wrap IV lane 2.                   */
  k_ra8_rsip_off_kw_iv3      = 0x1914U, /**< Wrap IV lane 3.                   */
  k_ra8_rsip_off_kw_handle   = 0x1918U, /**< Source / destination handle slot. */
  k_ra8_rsip_off_kw_blob_in  = 0x191CU, /**< Wrapped-blob input lane.          */
  k_ra8_rsip_off_kw_blob_out = 0x1920U, /**< Wrapped-blob output lane.         */

  /* ---- Symmetric (AES + ChaCha20) cipher control + state --------------- */
  k_ra8_rsip_off_sym_ctrl    = 0x1500U, /**< Cipher command (mode|dir|alg). */
  k_ra8_rsip_off_sym_status  = 0x1504U, /**< Cipher busy / done.            */
  k_ra8_rsip_off_sym_keyh    = 0x1508U, /**< Cipher key-handle slot.        */
  k_ra8_rsip_off_sym_iv0     = 0x150CU, /**< IV / nonce lane 0.             */
  k_ra8_rsip_off_sym_iv1     = 0x1510U, /**< IV / nonce lane 1.             */
  k_ra8_rsip_off_sym_iv2     = 0x1514U, /**< IV / nonce lane 2.             */
  k_ra8_rsip_off_sym_iv3     = 0x1518U, /**< IV / nonce lane 3.             */
  k_ra8_rsip_off_sym_aad_in  = 0x151CU, /**< AEAD additional-data input.    */
  k_ra8_rsip_off_sym_tag     = 0x1520U, /**< AEAD authentication tag.       */
  k_ra8_rsip_off_sym_aad_len = 0x1524U, /**< AEAD AAD length (bytes).       */
  k_ra8_rsip_off_sym_pt_len  = 0x1528U, /**< AEAD plaintext length (bytes). */

  /* ---- KDF additional context lanes ------------------------------------ */
  k_ra8_rsip_off_kdf_info = 0x00A0U, /**< KDF info / context lane.      */
  k_ra8_rsip_off_kdf_salt = 0x00A4U, /**< HKDF salt lane.               */
  k_ra8_rsip_off_kdf_len  = 0x00A8U, /**< Output length (bytes).        */
  k_ra8_rsip_off_kdf_ikm  = 0x00ACU, /**< Input keying material handle. */

  /* ---- OEM-key install primitives (FSP r_rsip_pe5*.c / r_rsip_pe6.c) -- */
  k_ra8_rsip_off_oem_ctrl = 0x1A00U, /**< OEM install command word.        */
  k_ra8_rsip_off_oem_arg  = 0x1A04U, /**< OEM install argument descriptor. */
  k_ra8_rsip_off_oem_iv   = 0x1A08U, /**< OEM install IV.                  */

  /* ---- Command FIFO (opcode + key handle, FSP r_rsip_p47i.c et al) ---- */
  k_ra8_rsip_off_cmd_fifo = 0x1B00U, /**< 32-bit opcode descriptor write. */
} ra8_rsip_off_t;

/**
 * @enum ra8_rsip_window_t
 * @brief Sub-block boundary constants used by the layout asserts below.
 *
 * @details
 * The HUM body for Ch 52 (p 3302-3307) does not publish a register
 * table because the engine is mailbox-driven, so the only authoritative
 * cross-reference for individual register offsets is the FSP primitive
 * file ``crypto_procedures_protected/.../ra8_rsip_e50d``. The
 * sub-block windows below come straight from the file-header table
 * earlier in this file and from the FSP ``RSIP_PRV_ADDR_VAL_*``
 * macros in ``r_rsip_reg.h`` (anchors 0x0000H, 0x1000H, 0x1420H,
 * 0x1440H, 0x1600H). Every offset emitted in ``ra8_rsip_off_t`` must
 * land inside one of these windows; the static_asserts that follow
 * lock that contract at compile time.
 */
typedef enum : uint16_t {
  k_ra8_rsip_win_ctrl_lo = 0x0000U, /**< CTRL/status mailbox low.          */
  k_ra8_rsip_win_ctrl_hi = 0x00FFU, /**< CTRL/status mailbox high.         */
  k_ra8_rsip_win_mbox_lo = 0x0100U, /**< Generic mailbox low.              */
  k_ra8_rsip_win_mbox_hi = 0x01FFU, /**< Generic mailbox high.             */
  k_ra8_rsip_win_data_lo = 0x1400U, /**< Data + KEY_STAGE window low.      */
  k_ra8_rsip_win_data_hi = 0x143FU, /**< Data + KEY_STAGE window high.     */
  k_ra8_rsip_win_trng_lo = 0x1440U, /**< TRNG window low.                  */
  k_ra8_rsip_win_trng_hi = 0x145FU, /**< TRNG window high.                 */
  k_ra8_rsip_win_sym_lo  = 0x1500U, /**< Symmetric cipher window low.      */
  k_ra8_rsip_win_sym_hi  = 0x15FFU, /**< Symmetric cipher window high.     */
  k_ra8_rsip_win_hash_lo = 0x1600U, /**< HASH window low.                  */
  k_ra8_rsip_win_hash_hi = 0x167FU, /**< HASH window high.                 */
  k_ra8_rsip_win_asym_lo = 0x1700U, /**< Asymmetric (RSA/ECC) window low.  */
  k_ra8_rsip_win_asym_hi = 0x17FFU, /**< Asymmetric (RSA/ECC) window high. */
  k_ra8_rsip_win_kv_lo   = 0x1800U, /**< Key-vault window low.             */
  k_ra8_rsip_win_kv_hi   = 0x18FFU, /**< Key-vault window high.            */
  k_ra8_rsip_win_kw_lo   = 0x1900U, /**< Key-wrap window low.              */
  k_ra8_rsip_win_kw_hi   = 0x19FFU, /**< Key-wrap window high.             */
  k_ra8_rsip_win_oem_lo  = 0x1A00U, /**< OEM-install window low.           */
  k_ra8_rsip_win_oem_hi  = 0x1AFFU, /**< OEM-install window high.          */
  k_ra8_rsip_win_cmd_lo  = 0x1B00U, /**< Command FIFO window low.          */
  k_ra8_rsip_win_cmd_hi  = 0x1BFFU, /**< Command FIFO window high.         */
} ra8_rsip_window_t;

/* -------------------------------------------------------------------------- *
 *  Layout insurance for ra8_rsip_off_t.
 *
 *  Two-line policy for every register the HAL touches:
 *
 *  1. Pin the exact byte offset (catches accidental edits and rebases).
 *  2. Verify the offset is 4-byte aligned and lands inside the
 *     documented sub-block window (catches typos that would alias
 *     onto an unrelated sub-block -- the bug pattern that put
 *     ``key_op_status`` on top of the TRNG status word).
 *
 *  Cross-references:
 *    * File-header table at top of this file (sub-block layout).
 *    * FSP ``RSIP_PRV_ADDR_VAL_*`` anchors in
 *      ``crypto_procedures_protected/.../common/primitive/r_rsip_reg.h``.
 *    * HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" p 3302-3307 (no
 *      register table -- mailbox model).
 * -------------------------------------------------------------------------- */

/* Every register cell is a 32-bit word. */
/** @brief RA8 RSIP ASSERT ALIGN. */
#define RA8_RSIP_ASSERT_ALIGN(off_)                                                                \
  static_assert(((size_t)(off_) & 0x3U) == 0U, #off_ " must be 4-byte aligned")

/* Range check: low <= off <= high. */
/** @brief RA8 RSIP ASSERT IN WIN. */
#define RA8_RSIP_ASSERT_IN_WIN(off_, lo_, hi_)                                                     \
  static_assert((size_t)(off_) >= (size_t)(lo_) && (size_t)(off_) <= (size_t)(hi_),                \
                #off_ " outside window [" #lo_ ", " #hi_ "]")

/* The pinning asserts below cross-check each named offset against the
 * exact byte value already declared in the enum above. The hex literal
 * IS the value being pinned, so the magic-number lint is suppressed
 * for the assert wall. */
/* NOLINTBEGIN(readability-magic-numbers) -- the assert wall pins each declared offset to its literal value. */
/* ---- CTRL / status mailbox sub-block ------------------------------------- */
static_assert((size_t)k_ra8_rsip_off_ctrl == 0x0000U, "CTRL offset");
static_assert((size_t)k_ra8_rsip_off_status == 0x0004U, "STATUS offset");
static_assert((size_t)k_ra8_rsip_off_ier == 0x0008U, "IER offset");
static_assert((size_t)k_ra8_rsip_off_isr == 0x000CU, "ISR offset");
static_assert((size_t)k_ra8_rsip_off_bist == 0x0010U, "BIST offset");
static_assert((size_t)k_ra8_rsip_off_tamper_ctrl == 0x0014U, "TAMPER_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_tamper_status == 0x0018U, "TAMPER_STATUS offset");
static_assert((size_t)k_ra8_rsip_off_dpa_ctrl == 0x001CU, "DPA_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_dotf0_ctrl == 0x0030U, "DOTF0_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_dotf1_ctrl == 0x0034U, "DOTF1_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_oem_bl_ver == 0x0050U, "OEM_BL_VER offset");
static_assert((size_t)k_ra8_rsip_off_oem_bl_inc == 0x0054U, "OEM_BL_INC offset");
static_assert((size_t)k_ra8_rsip_off_oem_bl_lock == 0x0058U, "OEM_BL_LOCK offset");
static_assert((size_t)k_ra8_rsip_off_huk_ctrl == 0x0070U, "HUK_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_uid_ctrl == 0x0074U, "UID_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_kdf_ctrl == 0x0090U, "KDF_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_kdf_status == 0x0094U, "KDF_STATUS offset");
static_assert((size_t)k_ra8_rsip_off_kdf_label == 0x0098U, "KDF_LABEL offset");
static_assert((size_t)k_ra8_rsip_off_kdf_out == 0x009CU, "KDF_OUT offset");
static_assert((size_t)k_ra8_rsip_off_kdf_info == 0x00A0U, "KDF_INFO offset");
static_assert((size_t)k_ra8_rsip_off_kdf_salt == 0x00A4U, "KDF_SALT offset");
static_assert((size_t)k_ra8_rsip_off_kdf_len == 0x00A8U, "KDF_LEN offset");
static_assert((size_t)k_ra8_rsip_off_kdf_ikm == 0x00ACU, "KDF_IKM offset");
static_assert((size_t)k_ra8_rsip_off_life_state == 0x00C0U, "LIFE_STATE offset");
static_assert((size_t)k_ra8_rsip_off_debug_level == 0x00C4U, "DEBUG_LEVEL offset");

/* ---- Generic command mailbox sub-block ---------------------------------- */
static_assert((size_t)k_ra8_rsip_off_mbox_op == 0x0100U, "MBOX_OP offset");
static_assert((size_t)k_ra8_rsip_off_mbox_arg0 == 0x0104U, "MBOX_ARG0 offset");
static_assert((size_t)k_ra8_rsip_off_mbox_arg1 == 0x0108U, "MBOX_ARG1 offset");
static_assert((size_t)k_ra8_rsip_off_mbox_arg2 == 0x010CU, "MBOX_ARG2 offset");
static_assert((size_t)k_ra8_rsip_off_mbox_arg3 == 0x0110U, "MBOX_ARG3 offset");
static_assert((size_t)k_ra8_rsip_off_mbox_ret == 0x0140U, "MBOX_RET offset");

/* ---- Data + KEY_STAGE window @ 0x1400 ----------------------------------- */
static_assert((size_t)k_ra8_rsip_off_data_in0 == 0x1400U, "DATA_IN0 offset");
static_assert((size_t)k_ra8_rsip_off_data_in1 == 0x1404U, "DATA_IN1 offset");
static_assert((size_t)k_ra8_rsip_off_data_in2 == 0x1408U, "DATA_IN2 offset");
static_assert((size_t)k_ra8_rsip_off_data_in3 == 0x140CU, "DATA_IN3 offset");
static_assert((size_t)k_ra8_rsip_off_data_out0 == 0x1410U, "DATA_OUT0 offset");
static_assert((size_t)k_ra8_rsip_off_data_out1 == 0x1414U, "DATA_OUT1 offset");
static_assert((size_t)k_ra8_rsip_off_data_out2 == 0x1418U, "DATA_OUT2 offset");
static_assert((size_t)k_ra8_rsip_off_data_out3 == 0x141CU, "DATA_OUT3 offset");
static_assert((size_t)k_ra8_rsip_off_key_stage == 0x1420U, "KEY_STAGE offset");
static_assert((size_t)k_ra8_rsip_off_key_op_status == 0x1424U,
              "KEY_OP_STATUS offset (must NOT collide with TRNG status @ 0x1444)");

/* ---- TRNG sub-block @ 0x1440 -------------------------------------------- */
static_assert((size_t)k_ra8_rsip_off_rnd_ctrl == 0x1440U, "RND_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_rnd_status == 0x1444U, "RND_STATUS offset");
static_assert((size_t)k_ra8_rsip_off_rnd_data == 0x1458U, "RND_DATA offset");

/* The KEY_STAGE-vs-TRNG collision is the high-stakes bug this audit
 * prevents from coming back: ``KEY_OP_STATUS`` and ``RND_STATUS``
 * must live in different sub-blocks so a wrapped-key load cannot be
 * acknowledged by reading TRNG health flags. */
static_assert((size_t)k_ra8_rsip_off_key_op_status != (size_t)k_ra8_rsip_off_rnd_status,
              "KEY_OP_STATUS and RND_STATUS must not overlap");

/* ---- Symmetric cipher sub-block @ 0x1500 -------------------------------- */
static_assert((size_t)k_ra8_rsip_off_sym_ctrl == 0x1500U, "SYM_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_sym_status == 0x1504U, "SYM_STATUS offset");
static_assert((size_t)k_ra8_rsip_off_sym_keyh == 0x1508U, "SYM_KEYH offset");
static_assert((size_t)k_ra8_rsip_off_sym_iv0 == 0x150CU, "SYM_IV0 offset");
static_assert((size_t)k_ra8_rsip_off_sym_iv1 == 0x1510U, "SYM_IV1 offset");
static_assert((size_t)k_ra8_rsip_off_sym_iv2 == 0x1514U, "SYM_IV2 offset");
static_assert((size_t)k_ra8_rsip_off_sym_iv3 == 0x1518U, "SYM_IV3 offset");
static_assert((size_t)k_ra8_rsip_off_sym_aad_in == 0x151CU, "SYM_AAD_IN offset");
static_assert((size_t)k_ra8_rsip_off_sym_tag == 0x1520U, "SYM_TAG offset");
static_assert((size_t)k_ra8_rsip_off_sym_aad_len == 0x1524U, "SYM_AAD_LEN offset");
static_assert((size_t)k_ra8_rsip_off_sym_pt_len == 0x1528U, "SYM_PT_LEN offset");

/* ---- HASH sub-block @ 0x1600 -------------------------------------------- */
static_assert((size_t)k_ra8_rsip_off_hash_ctrl == 0x1600U, "HASH_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_hash_status == 0x1604U, "HASH_STATUS offset");
static_assert((size_t)k_ra8_rsip_off_hash_data_in == 0x1608U, "HASH_DATA_IN offset");
static_assert((size_t)k_ra8_rsip_off_hash_digest == 0x1610U, "HASH_DIGEST offset");
static_assert((size_t)k_ra8_rsip_off_hash_hmac == 0x1660U, "HASH_HMAC offset");

/* ---- Asymmetric (RSA / ECC) sub-block @ 0x1700 -------------------------- */
static_assert((size_t)k_ra8_rsip_off_asym_ctrl == 0x1700U, "ASYM_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_asym_status == 0x1704U, "ASYM_STATUS offset");
static_assert((size_t)k_ra8_rsip_off_asym_curve == 0x1708U, "ASYM_CURVE offset");
static_assert((size_t)k_ra8_rsip_off_asym_arg == 0x170CU, "ASYM_ARG offset");
static_assert((size_t)k_ra8_rsip_off_asym_key == 0x1710U, "ASYM_KEY offset");
static_assert((size_t)k_ra8_rsip_off_asym_msg_in == 0x1714U, "ASYM_MSG_IN offset");
static_assert((size_t)k_ra8_rsip_off_asym_sig_in == 0x1718U, "ASYM_SIG_IN offset");
static_assert((size_t)k_ra8_rsip_off_asym_sig_out == 0x171CU, "ASYM_SIG_OUT offset");
static_assert((size_t)k_ra8_rsip_off_asym_pub_x == 0x1720U, "ASYM_PUB_X offset");
static_assert((size_t)k_ra8_rsip_off_asym_pub_y == 0x1724U, "ASYM_PUB_Y offset");
static_assert((size_t)k_ra8_rsip_off_asym_shared == 0x1728U, "ASYM_SHARED offset");
static_assert((size_t)k_ra8_rsip_off_asym_rsa_size == 0x172CU, "ASYM_RSA_SIZE offset");

/* ---- Key-vault sub-block @ 0x1800 --------------------------------------- */
static_assert((size_t)k_ra8_rsip_off_kv_ctrl == 0x1800U, "KV_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_kv_slot == 0x1804U, "KV_SLOT offset");
static_assert((size_t)k_ra8_rsip_off_kv_data == 0x1808U, "KV_DATA offset");
static_assert((size_t)k_ra8_rsip_off_kv_count == 0x180CU, "KV_COUNT offset");

/* ---- Key-wrap sub-block @ 0x1900 ---------------------------------------- */
static_assert((size_t)k_ra8_rsip_off_kw_ctrl == 0x1900U, "KW_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_kw_kek == 0x1904U, "KW_KEK offset");
static_assert((size_t)k_ra8_rsip_off_kw_iv0 == 0x1908U, "KW_IV0 offset");
static_assert((size_t)k_ra8_rsip_off_kw_iv1 == 0x190CU, "KW_IV1 offset");
static_assert((size_t)k_ra8_rsip_off_kw_iv2 == 0x1910U, "KW_IV2 offset");
static_assert((size_t)k_ra8_rsip_off_kw_iv3 == 0x1914U, "KW_IV3 offset");
static_assert((size_t)k_ra8_rsip_off_kw_handle == 0x1918U, "KW_HANDLE offset");
static_assert((size_t)k_ra8_rsip_off_kw_blob_in == 0x191CU, "KW_BLOB_IN offset");
static_assert((size_t)k_ra8_rsip_off_kw_blob_out == 0x1920U, "KW_BLOB_OUT offset");

/* ---- OEM-install sub-block @ 0x1A00 ------------------------------------- */
static_assert((size_t)k_ra8_rsip_off_oem_ctrl == 0x1A00U, "OEM_CTRL offset");
static_assert((size_t)k_ra8_rsip_off_oem_arg == 0x1A04U, "OEM_ARG offset");
static_assert((size_t)k_ra8_rsip_off_oem_iv == 0x1A08U, "OEM_IV offset");

/* ---- Command FIFO sub-block @ 0x1B00 ------------------------------------ */
static_assert((size_t)k_ra8_rsip_off_cmd_fifo == 0x1B00U, "CMD_FIFO offset");

/* ---- Alignment sweep: every register must be a 32-bit word boundary ----- */
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_status);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_ier);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_isr);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_bist);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_tamper_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_tamper_status);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_dpa_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_dotf0_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_dotf1_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_oem_bl_ver);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_oem_bl_inc);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_oem_bl_lock);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_huk_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_uid_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kdf_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kdf_status);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kdf_label);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kdf_out);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kdf_info);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kdf_salt);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kdf_len);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kdf_ikm);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_life_state);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_debug_level);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_mbox_op);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_mbox_arg0);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_mbox_arg1);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_mbox_arg2);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_mbox_arg3);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_mbox_ret);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_data_in0);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_data_in1);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_data_in2);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_data_in3);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_data_out0);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_data_out1);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_data_out2);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_data_out3);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_key_stage);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_key_op_status);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_rnd_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_rnd_status);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_rnd_data);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_status);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_keyh);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_iv0);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_iv1);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_iv2);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_iv3);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_aad_in);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_tag);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_aad_len);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_sym_pt_len);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_hash_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_hash_status);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_hash_data_in);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_hash_digest);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_hash_hmac);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_status);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_curve);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_arg);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_key);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_msg_in);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_sig_in);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_sig_out);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_pub_x);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_pub_y);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_shared);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_asym_rsa_size);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kv_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kv_slot);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kv_data);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kv_count);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kw_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kw_kek);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kw_iv0);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kw_iv1);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kw_iv2);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kw_iv3);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kw_handle);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kw_blob_in);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_kw_blob_out);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_oem_ctrl);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_oem_arg);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_oem_iv);
RA8_RSIP_ASSERT_ALIGN(k_ra8_rsip_off_cmd_fifo);

/* ---- Sub-block containment sweep ---------------------------------------- */
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_ctrl, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_status, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_ier, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_isr, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_bist, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_tamper_ctrl, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_tamper_status,
                       k_ra8_rsip_win_ctrl_lo,
                       k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_dpa_ctrl, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_dotf0_ctrl, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_dotf1_ctrl, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_oem_bl_ver, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_oem_bl_inc, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_oem_bl_lock, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_huk_ctrl, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_uid_ctrl, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kdf_ctrl, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kdf_status, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kdf_label, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kdf_out, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kdf_info, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kdf_salt, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kdf_len, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kdf_ikm, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_life_state, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_debug_level, k_ra8_rsip_win_ctrl_lo, k_ra8_rsip_win_ctrl_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_mbox_op, k_ra8_rsip_win_mbox_lo, k_ra8_rsip_win_mbox_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_mbox_arg0, k_ra8_rsip_win_mbox_lo, k_ra8_rsip_win_mbox_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_mbox_arg1, k_ra8_rsip_win_mbox_lo, k_ra8_rsip_win_mbox_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_mbox_arg2, k_ra8_rsip_win_mbox_lo, k_ra8_rsip_win_mbox_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_mbox_arg3, k_ra8_rsip_win_mbox_lo, k_ra8_rsip_win_mbox_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_mbox_ret, k_ra8_rsip_win_mbox_lo, k_ra8_rsip_win_mbox_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_data_in0, k_ra8_rsip_win_data_lo, k_ra8_rsip_win_data_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_data_in1, k_ra8_rsip_win_data_lo, k_ra8_rsip_win_data_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_data_in2, k_ra8_rsip_win_data_lo, k_ra8_rsip_win_data_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_data_in3, k_ra8_rsip_win_data_lo, k_ra8_rsip_win_data_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_data_out0, k_ra8_rsip_win_data_lo, k_ra8_rsip_win_data_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_data_out1, k_ra8_rsip_win_data_lo, k_ra8_rsip_win_data_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_data_out2, k_ra8_rsip_win_data_lo, k_ra8_rsip_win_data_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_data_out3, k_ra8_rsip_win_data_lo, k_ra8_rsip_win_data_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_key_stage, k_ra8_rsip_win_data_lo, k_ra8_rsip_win_data_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_key_op_status,
                       k_ra8_rsip_win_data_lo,
                       k_ra8_rsip_win_data_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_rnd_ctrl, k_ra8_rsip_win_trng_lo, k_ra8_rsip_win_trng_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_rnd_status, k_ra8_rsip_win_trng_lo, k_ra8_rsip_win_trng_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_rnd_data, k_ra8_rsip_win_trng_lo, k_ra8_rsip_win_trng_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_ctrl, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_status, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_keyh, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_iv0, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_iv1, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_iv2, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_iv3, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_aad_in, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_tag, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_aad_len, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_sym_pt_len, k_ra8_rsip_win_sym_lo, k_ra8_rsip_win_sym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_hash_ctrl, k_ra8_rsip_win_hash_lo, k_ra8_rsip_win_hash_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_hash_status, k_ra8_rsip_win_hash_lo, k_ra8_rsip_win_hash_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_hash_data_in, k_ra8_rsip_win_hash_lo, k_ra8_rsip_win_hash_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_hash_digest, k_ra8_rsip_win_hash_lo, k_ra8_rsip_win_hash_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_hash_hmac, k_ra8_rsip_win_hash_lo, k_ra8_rsip_win_hash_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_ctrl, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_status, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_curve, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_arg, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_key, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_msg_in, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_sig_in, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_sig_out, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_pub_x, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_pub_y, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_shared, k_ra8_rsip_win_asym_lo, k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_asym_rsa_size,
                       k_ra8_rsip_win_asym_lo,
                       k_ra8_rsip_win_asym_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kv_ctrl, k_ra8_rsip_win_kv_lo, k_ra8_rsip_win_kv_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kv_slot, k_ra8_rsip_win_kv_lo, k_ra8_rsip_win_kv_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kv_data, k_ra8_rsip_win_kv_lo, k_ra8_rsip_win_kv_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kv_count, k_ra8_rsip_win_kv_lo, k_ra8_rsip_win_kv_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kw_ctrl, k_ra8_rsip_win_kw_lo, k_ra8_rsip_win_kw_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kw_kek, k_ra8_rsip_win_kw_lo, k_ra8_rsip_win_kw_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kw_iv0, k_ra8_rsip_win_kw_lo, k_ra8_rsip_win_kw_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kw_iv1, k_ra8_rsip_win_kw_lo, k_ra8_rsip_win_kw_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kw_iv2, k_ra8_rsip_win_kw_lo, k_ra8_rsip_win_kw_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kw_iv3, k_ra8_rsip_win_kw_lo, k_ra8_rsip_win_kw_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kw_handle, k_ra8_rsip_win_kw_lo, k_ra8_rsip_win_kw_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kw_blob_in, k_ra8_rsip_win_kw_lo, k_ra8_rsip_win_kw_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_kw_blob_out, k_ra8_rsip_win_kw_lo, k_ra8_rsip_win_kw_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_oem_ctrl, k_ra8_rsip_win_oem_lo, k_ra8_rsip_win_oem_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_oem_arg, k_ra8_rsip_win_oem_lo, k_ra8_rsip_win_oem_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_oem_iv, k_ra8_rsip_win_oem_lo, k_ra8_rsip_win_oem_hi);
RA8_RSIP_ASSERT_IN_WIN(k_ra8_rsip_off_cmd_fifo, k_ra8_rsip_win_cmd_lo, k_ra8_rsip_win_cmd_hi);
/* NOLINTEND(readability-magic-numbers) */
