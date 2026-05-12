/**
 * @file ra8d2_rsip_regs.h
 * @brief Renesas Secure IP (RSIP-E50D) register layout for the RA8D2
 *
 * @details
 * The RSIP-E50D engine on the RA8D2 is an isolated security
 * subsystem (HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" p 3302). It
 * combines an access-management circuit, a symmetric core (AES +
 * ChaCha20-Poly1305), an asymmetric core (RSA + ECC), a hash engine
 * (SHA-2 / SHA-3 / HMAC), a 128-bit true RNG, the Hardware Unique
 * Key (HUK), the Unique ID, the OEM boot loader version counter,
 * and the key data path that feeds DOTF (HUM Ch 52.1 Table 52.1
 * p 3302-3303).
 *
 * Every functional sequence is driven through a small mailbox-style
 * register window starting at ``0x403B0000`` (FSP
 * ``RSIP_PRV_ADDR_BASE`` for the E50D variant). The HUM body is
 * intentionally short (p 3302-3307) because the on-board RSIP
 * firmware interprets opcodes the host pokes through the mailbox.
 *
 * ## Register window layout (mailbox model)
 *
 * The RSIP exposes a small set of mailbox sub-blocks at the offsets
 * the FSP primitive layer touches; the HAL models every cell it
 * actually writes:
 *
 *  | Offset           | Block           | Purpose                              |
 *  |-----------------:|-----------------|--------------------------------------|
 *  | 0x0000 .. 0x000F | CTRL            | Engine control / status / IRQ        |
 *  | 0x0010           | BIST            | Built-in self-test trigger / result  |
 *  | 0x0014 .. 0x002F | TAMPER / DPA    | Tamper config + SPA/DPA enable       |
 *  | 0x0030 .. 0x004F | DOTF route      | DOTF0 / DOTF1 key-feed routing       |
 *  | 0x0050 .. 0x006F | OEM BL Ver.     | OEM boot loader version counter      |
 *  | 0x0070 .. 0x008F | HUK / UID       | Hardware Unique Key + Unique ID ctrl |
 *  | 0x0090 .. 0x00BF | KDF             | Key-derivation request / status      |
 *  | 0x00C0 .. 0x00FF | LIFE            | Device-lifecycle and debug-level     |
 *  | 0x0100 .. 0x01FC | MBOX            | Command mailbox (opcode + arg slots) |
 *  | 0x1400 .. 0x141C | DATA            | Data input/output window             |
 *  | 0x1420           | KEY_STAGE       | Wrapped-key staging port             |
 *  | 0x1440 .. 0x145C | TRNG            | True-RNG control / status / data     |
 *  | 0x1600 .. 0x167C | HASH            | SHA / HMAC streaming + digest read   |
 *  | 0x1700 .. 0x17FC | ASYM            | RSA / ECC scratch + control          |
 *  | 0x1800 .. 0x18FF | KEY_VAULT       | Wrapped-key vault read/write window  |
 *  | 0x1900 .. 0x19FF | KW              | Key-wrap / key-unwrap engine         |
 *  | 0x1A00 .. 0x1AFF | OEM             | OEM-key install primitives (PE5/PE6) |
 *  | 0x1B00 .. 0x1BFF | CMD             | Command-FIFO (opcode + handle)       |
 *
 * Field semantics are inferred from the FSP primitives that touch
 * ``REG_xxxxH`` in
 * ``crypto_procedures_protected/src/rsip/ra/primitive/ra_rsip_e50d/``;
 * the HUM does not publish a register table because the engine is
 * mailbox-driven.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @enum ra_rsip_addr_t
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
  k_ra_rsip_base_addr = 0x403B0000UL, /**< RSIP-E50D mailbox window base. */
} ra_rsip_addr_t;

/**
 * @enum ra_rsip_off_t
 * @brief Byte offsets of every register the HAL touches.
 *
 * @details
 * Offsets are derived from the FSP primitive register file
 * ``r_rsip_reg.h`` and the ``WR1_PROG``/``RD1_MASK``/``WAIT_STS``
 * sites scattered through ``crypto_procedures_protected/.../ra_rsip_e50d``.
 * Each cell is a 32-bit word. The HUM body for Ch 52 (p 3302-3307)
 * does not enumerate an explicit register table; the engine is
 * mailbox-driven.
 */
typedef enum : uint16_t {
  /* ---- Control / status mailbox (sub-block @ 0x0000) ------------------- */
  k_ra_rsip_off_ctrl   = 0x0000U, /**< Engine control word.                  */
  k_ra_rsip_off_status = 0x0004U, /**< Status / state monitor.               */
  k_ra_rsip_off_ier    = 0x0008U, /**< Interrupt enable.                     */
  k_ra_rsip_off_isr    = 0x000CU, /**< Interrupt status (W1C).               */
  k_ra_rsip_off_bist   = 0x0010U, /**< Built-in self-test trigger / result.  */

  /* ---- Tamper / SPA-DPA control (HUM Ch 52.3.3 p 3307) ----------------- */
  k_ra_rsip_off_tamper_ctrl   = 0x0014U, /**< Tamper-source enable mask.    */
  k_ra_rsip_off_tamper_status = 0x0018U, /**< Latched tamper-event flags.   */
  k_ra_rsip_off_dpa_ctrl      = 0x001CU, /**< SPA/DPA countermeasure arm.   */

  /* ---- DOTF key delivery routing (HUM Ch 52.1 Table 52.1 p 3303) ------- */
  k_ra_rsip_off_dotf0_ctrl = 0x0030U, /**< DOTF0 key route enable + slot.   */
  k_ra_rsip_off_dotf1_ctrl = 0x0034U, /**< DOTF1 key route enable + slot.   */

  /* ---- OEM boot loader version (HUM Ch 52.1 Table 52.1 p 3303) --------- */
  k_ra_rsip_off_oem_bl_ver  = 0x0050U, /**< Latched OEM BL Ver counter.     */
  k_ra_rsip_off_oem_bl_inc  = 0x0054U, /**< Increment trigger (W1).         */
  k_ra_rsip_off_oem_bl_lock = 0x0058U, /**< Lock-after-validation latch.    */

  /* ---- Hardware Unique Key + Unique ID gates --------------------------- */
  k_ra_rsip_off_huk_ctrl = 0x0070U, /**< HUK access gate (read-only data).  */
  k_ra_rsip_off_uid_ctrl = 0x0074U, /**< Unique-ID access gate.             */

  /* ---- Key Derivation Function mailbox --------------------------------- */
  k_ra_rsip_off_kdf_ctrl   = 0x0090U, /**< KDF request word.               */
  k_ra_rsip_off_kdf_status = 0x0094U, /**< KDF complete / busy.            */
  k_ra_rsip_off_kdf_label  = 0x0098U, /**< KDF context label slot.         */
  k_ra_rsip_off_kdf_out    = 0x009CU, /**< Derived-key output handle.      */

  /* ---- Device lifecycle (HUM Ch 51.1 p 3263 "Device lifecycle") -------- */
  k_ra_rsip_off_life_state  = 0x00C0U, /**< Current lifecycle state.      */
  k_ra_rsip_off_debug_level = 0x00C4U, /**< AL0/AL1/AL2 debug level.      */

  /* ---- Generic command mailbox + arg window ---------------------------- */
  k_ra_rsip_off_mbox_op   = 0x0100U, /**< Opcode written by host.            */
  k_ra_rsip_off_mbox_arg0 = 0x0104U, /**< Argument 0.                        */
  k_ra_rsip_off_mbox_arg1 = 0x0108U, /**< Argument 1.                        */
  k_ra_rsip_off_mbox_arg2 = 0x010CU, /**< Argument 2.                        */
  k_ra_rsip_off_mbox_arg3 = 0x0110U, /**< Argument 3.                        */
  k_ra_rsip_off_mbox_ret  = 0x0140U, /**< Completion code from engine.       */

  /* ---- Data window (encrypt/decrypt input + output) ------------------- */
  k_ra_rsip_off_data_in0  = 0x1400U, /**< Data input lane 0.                 */
  k_ra_rsip_off_data_in1  = 0x1404U, /**< Data input lane 1.                 */
  k_ra_rsip_off_data_in2  = 0x1408U, /**< Data input lane 2.                 */
  k_ra_rsip_off_data_in3  = 0x140CU, /**< Data input lane 3.                 */
  k_ra_rsip_off_data_out0 = 0x1410U, /**< Data output lane 0.                */
  k_ra_rsip_off_data_out1 = 0x1414U, /**< Data output lane 1.                */
  k_ra_rsip_off_data_out2 = 0x1418U, /**< Data output lane 2.                */
  k_ra_rsip_off_data_out3 = 0x141CU, /**< Data output lane 3.                */

  /* ---- Wrapped-key staging port + status ------------------------------- */
  k_ra_rsip_off_key_stage     = 0x1420U, /**< Wrapped-key load port.        */
  k_ra_rsip_off_key_op_status = 0x1424U, /**< Key/data op status (one 32-bit
                                              word past KEY_STAGE, inside the
                                              KEY_STAGE sub-block 0x1420-0x143F
                                              -- the previous 0x1444 collided
                                              with the TRNG status word).     */

  /* ---- Random number generator ----------------------------------------- */
  k_ra_rsip_off_rnd_ctrl   = 0x1440U, /**< TRNG control word.                */
  k_ra_rsip_off_rnd_status = 0x1444U, /**< TRNG ready / health flags.        */
  k_ra_rsip_off_rnd_data   = 0x1458U, /**< TRNG output (32-bit per read).    */

  /* ---- Hash engine ----------------------------------------------------- */
  k_ra_rsip_off_hash_ctrl    = 0x1600U, /**< HASH control (algorithm select).*/
  k_ra_rsip_off_hash_status  = 0x1604U, /**< HASH ready / done flags.        */
  k_ra_rsip_off_hash_data_in = 0x1608U, /**< HASH input window (32-bit each).*/
  k_ra_rsip_off_hash_digest  = 0x1610U, /**< HASH output digest base.        */
  k_ra_rsip_off_hash_hmac    = 0x1660U, /**< HMAC mode + key-handle slot.    */

  /* ---- Asymmetric (RSA / ECC) scratch + control ------------------------ */
  k_ra_rsip_off_asym_ctrl     = 0x1700U, /**< Asymmetric command word.       */
  k_ra_rsip_off_asym_status   = 0x1704U, /**< Asymmetric busy / done.        */
  k_ra_rsip_off_asym_curve    = 0x1708U, /**< ECC curve selector.            */
  k_ra_rsip_off_asym_arg      = 0x170CU, /**< Argument-block descriptor.     */
  k_ra_rsip_off_asym_key      = 0x1710U, /**< Key-handle slot.               */
  k_ra_rsip_off_asym_msg_in   = 0x1714U, /**< Message / digest input port.   */
  k_ra_rsip_off_asym_sig_in   = 0x1718U, /**< Signature input lane.          */
  k_ra_rsip_off_asym_sig_out  = 0x171CU, /**< Signature output lane.         */
  k_ra_rsip_off_asym_pub_x    = 0x1720U, /**< ECC peer X / RSA pubkey lane.  */
  k_ra_rsip_off_asym_pub_y    = 0x1724U, /**< ECC peer Y lane.               */
  k_ra_rsip_off_asym_shared   = 0x1728U, /**< ECDH shared-secret handle.     */
  k_ra_rsip_off_asym_rsa_size = 0x172CU, /**< RSA modulus-size selector.     */

  /* ---- Wrapped-key vault read/write window ----------------------------- */
  k_ra_rsip_off_kv_ctrl  = 0x1800U, /**< Vault command (read/write/erase).  */
  k_ra_rsip_off_kv_slot  = 0x1804U, /**< Vault slot index.                  */
  k_ra_rsip_off_kv_data  = 0x1808U, /**< Vault data port.                   */
  k_ra_rsip_off_kv_count = 0x180CU, /**< Vault populated-slot count.        */

  /* ---- Key-wrap / key-unwrap engine ------------------------------------ */
  k_ra_rsip_off_kw_ctrl     = 0x1900U, /**< Key-wrap command word.           */
  k_ra_rsip_off_kw_kek      = 0x1904U, /**< KEK handle slot.                 */
  k_ra_rsip_off_kw_iv0      = 0x1908U, /**< Wrap IV lane 0.                  */
  k_ra_rsip_off_kw_iv1      = 0x190CU, /**< Wrap IV lane 1.                  */
  k_ra_rsip_off_kw_iv2      = 0x1910U, /**< Wrap IV lane 2.                  */
  k_ra_rsip_off_kw_iv3      = 0x1914U, /**< Wrap IV lane 3.                  */
  k_ra_rsip_off_kw_handle   = 0x1918U, /**< Source / destination handle slot.*/
  k_ra_rsip_off_kw_blob_in  = 0x191CU, /**< Wrapped-blob input lane.         */
  k_ra_rsip_off_kw_blob_out = 0x1920U, /**< Wrapped-blob output lane.        */

  /* ---- Symmetric (AES + ChaCha20) cipher control + state --------------- */
  k_ra_rsip_off_sym_ctrl    = 0x1500U, /**< Cipher command (mode|dir|alg).   */
  k_ra_rsip_off_sym_status  = 0x1504U, /**< Cipher busy / done.              */
  k_ra_rsip_off_sym_keyh    = 0x1508U, /**< Cipher key-handle slot.          */
  k_ra_rsip_off_sym_iv0     = 0x150CU, /**< IV / nonce lane 0.               */
  k_ra_rsip_off_sym_iv1     = 0x1510U, /**< IV / nonce lane 1.               */
  k_ra_rsip_off_sym_iv2     = 0x1514U, /**< IV / nonce lane 2.               */
  k_ra_rsip_off_sym_iv3     = 0x1518U, /**< IV / nonce lane 3.               */
  k_ra_rsip_off_sym_aad_in  = 0x151CU, /**< AEAD additional-data input.      */
  k_ra_rsip_off_sym_tag     = 0x1520U, /**< AEAD authentication tag.         */
  k_ra_rsip_off_sym_aad_len = 0x1524U, /**< AEAD AAD length (bytes).         */
  k_ra_rsip_off_sym_pt_len  = 0x1528U, /**< AEAD plaintext length (bytes).   */

  /* ---- KDF additional context lanes ------------------------------------ */
  k_ra_rsip_off_kdf_info = 0x00A0U, /**< KDF info / context lane.         */
  k_ra_rsip_off_kdf_salt = 0x00A4U, /**< HKDF salt lane.                  */
  k_ra_rsip_off_kdf_len  = 0x00A8U, /**< Output length (bytes).           */
  k_ra_rsip_off_kdf_ikm  = 0x00ACU, /**< Input keying material handle.    */

  /* ---- OEM-key install primitives (FSP r_rsip_pe5*.c / r_rsip_pe6.c) -- */
  k_ra_rsip_off_oem_ctrl = 0x1A00U, /**< OEM install command word.          */
  k_ra_rsip_off_oem_arg  = 0x1A04U, /**< OEM install argument descriptor.   */
  k_ra_rsip_off_oem_iv   = 0x1A08U, /**< OEM install IV.                    */

  /* ---- Command FIFO (opcode + key handle, FSP r_rsip_p47i.c et al) ---- */
  k_ra_rsip_off_cmd_fifo = 0x1B00U, /**< 32-bit opcode descriptor write.    */
} ra_rsip_off_t;

/**
 * @enum ra_rsip_window_t
 * @brief Sub-block boundary constants used by the layout asserts below.
 *
 * @details
 * The HUM body for Ch 52 (p 3302-3307) does not publish a register
 * table because the engine is mailbox-driven, so the only authoritative
 * cross-reference for individual register offsets is the FSP primitive
 * file ``crypto_procedures_protected/.../ra_rsip_e50d``. The
 * sub-block windows below come straight from the file-header table
 * earlier in this file and from the FSP ``RSIP_PRV_ADDR_VAL_*``
 * macros in ``r_rsip_reg.h`` (anchors 0x0000H, 0x1000H, 0x1420H,
 * 0x1440H, 0x1600H). Every offset emitted in ``ra_rsip_off_t`` must
 * land inside one of these windows; the static_asserts that follow
 * lock that contract at compile time.
 */
typedef enum : uint16_t {
  k_ra_rsip_win_ctrl_lo = 0x0000U, /**< CTRL/status mailbox low.            */
  k_ra_rsip_win_ctrl_hi = 0x00FFU, /**< CTRL/status mailbox high.           */
  k_ra_rsip_win_mbox_lo = 0x0100U, /**< Generic mailbox low.                */
  k_ra_rsip_win_mbox_hi = 0x01FFU, /**< Generic mailbox high.               */
  k_ra_rsip_win_data_lo = 0x1400U, /**< Data + KEY_STAGE window low.        */
  k_ra_rsip_win_data_hi = 0x143FU, /**< Data + KEY_STAGE window high.       */
  k_ra_rsip_win_trng_lo = 0x1440U, /**< TRNG window low.                    */
  k_ra_rsip_win_trng_hi = 0x145FU, /**< TRNG window high.                   */
  k_ra_rsip_win_sym_lo  = 0x1500U, /**< Symmetric cipher window low.        */
  k_ra_rsip_win_sym_hi  = 0x15FFU, /**< Symmetric cipher window high.       */
  k_ra_rsip_win_hash_lo = 0x1600U, /**< HASH window low.                    */
  k_ra_rsip_win_hash_hi = 0x167FU, /**< HASH window high.                   */
  k_ra_rsip_win_asym_lo = 0x1700U, /**< Asymmetric (RSA/ECC) window low.    */
  k_ra_rsip_win_asym_hi = 0x17FFU, /**< Asymmetric (RSA/ECC) window high.   */
  k_ra_rsip_win_kv_lo   = 0x1800U, /**< Key-vault window low.               */
  k_ra_rsip_win_kv_hi   = 0x18FFU, /**< Key-vault window high.              */
  k_ra_rsip_win_kw_lo   = 0x1900U, /**< Key-wrap window low.                */
  k_ra_rsip_win_kw_hi   = 0x19FFU, /**< Key-wrap window high.               */
  k_ra_rsip_win_oem_lo  = 0x1A00U, /**< OEM-install window low.             */
  k_ra_rsip_win_oem_hi  = 0x1AFFU, /**< OEM-install window high.            */
  k_ra_rsip_win_cmd_lo  = 0x1B00U, /**< Command FIFO window low.            */
  k_ra_rsip_win_cmd_hi  = 0x1BFFU, /**< Command FIFO window high.           */
} ra_rsip_window_t;

/* -------------------------------------------------------------------------- *
 *  Layout insurance for ra_rsip_off_t.
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
#define RA_RSIP_ASSERT_ALIGN(off_)                                                                 \
  static_assert(((size_t)(off_) & 0x3U) == 0U, #off_ " must be 4-byte aligned")

/* Range check: low <= off <= high. */
#define RA_RSIP_ASSERT_IN_WIN(off_, lo_, hi_)                                                      \
  static_assert((size_t)(off_) >= (size_t)(lo_) && (size_t)(off_) <= (size_t)(hi_),                \
                #off_ " outside window [" #lo_ ", " #hi_ "]")

/* The pinning asserts below cross-check each named offset against the
 * exact byte value already declared in the enum above. The hex literal
 * IS the value being pinned, so the magic-number lint is suppressed
 * for the assert wall. */
/* NOLINTBEGIN(readability-magic-numbers) */
/* ---- CTRL / status mailbox sub-block ------------------------------------- */
static_assert((size_t)k_ra_rsip_off_ctrl == 0x0000U, "CTRL offset");
static_assert((size_t)k_ra_rsip_off_status == 0x0004U, "STATUS offset");
static_assert((size_t)k_ra_rsip_off_ier == 0x0008U, "IER offset");
static_assert((size_t)k_ra_rsip_off_isr == 0x000CU, "ISR offset");
static_assert((size_t)k_ra_rsip_off_bist == 0x0010U, "BIST offset");
static_assert((size_t)k_ra_rsip_off_tamper_ctrl == 0x0014U, "TAMPER_CTRL offset");
static_assert((size_t)k_ra_rsip_off_tamper_status == 0x0018U, "TAMPER_STATUS offset");
static_assert((size_t)k_ra_rsip_off_dpa_ctrl == 0x001CU, "DPA_CTRL offset");
static_assert((size_t)k_ra_rsip_off_dotf0_ctrl == 0x0030U, "DOTF0_CTRL offset");
static_assert((size_t)k_ra_rsip_off_dotf1_ctrl == 0x0034U, "DOTF1_CTRL offset");
static_assert((size_t)k_ra_rsip_off_oem_bl_ver == 0x0050U, "OEM_BL_VER offset");
static_assert((size_t)k_ra_rsip_off_oem_bl_inc == 0x0054U, "OEM_BL_INC offset");
static_assert((size_t)k_ra_rsip_off_oem_bl_lock == 0x0058U, "OEM_BL_LOCK offset");
static_assert((size_t)k_ra_rsip_off_huk_ctrl == 0x0070U, "HUK_CTRL offset");
static_assert((size_t)k_ra_rsip_off_uid_ctrl == 0x0074U, "UID_CTRL offset");
static_assert((size_t)k_ra_rsip_off_kdf_ctrl == 0x0090U, "KDF_CTRL offset");
static_assert((size_t)k_ra_rsip_off_kdf_status == 0x0094U, "KDF_STATUS offset");
static_assert((size_t)k_ra_rsip_off_kdf_label == 0x0098U, "KDF_LABEL offset");
static_assert((size_t)k_ra_rsip_off_kdf_out == 0x009CU, "KDF_OUT offset");
static_assert((size_t)k_ra_rsip_off_kdf_info == 0x00A0U, "KDF_INFO offset");
static_assert((size_t)k_ra_rsip_off_kdf_salt == 0x00A4U, "KDF_SALT offset");
static_assert((size_t)k_ra_rsip_off_kdf_len == 0x00A8U, "KDF_LEN offset");
static_assert((size_t)k_ra_rsip_off_kdf_ikm == 0x00ACU, "KDF_IKM offset");
static_assert((size_t)k_ra_rsip_off_life_state == 0x00C0U, "LIFE_STATE offset");
static_assert((size_t)k_ra_rsip_off_debug_level == 0x00C4U, "DEBUG_LEVEL offset");

/* ---- Generic command mailbox sub-block ---------------------------------- */
static_assert((size_t)k_ra_rsip_off_mbox_op == 0x0100U, "MBOX_OP offset");
static_assert((size_t)k_ra_rsip_off_mbox_arg0 == 0x0104U, "MBOX_ARG0 offset");
static_assert((size_t)k_ra_rsip_off_mbox_arg1 == 0x0108U, "MBOX_ARG1 offset");
static_assert((size_t)k_ra_rsip_off_mbox_arg2 == 0x010CU, "MBOX_ARG2 offset");
static_assert((size_t)k_ra_rsip_off_mbox_arg3 == 0x0110U, "MBOX_ARG3 offset");
static_assert((size_t)k_ra_rsip_off_mbox_ret == 0x0140U, "MBOX_RET offset");

/* ---- Data + KEY_STAGE window @ 0x1400 ----------------------------------- */
static_assert((size_t)k_ra_rsip_off_data_in0 == 0x1400U, "DATA_IN0 offset");
static_assert((size_t)k_ra_rsip_off_data_in1 == 0x1404U, "DATA_IN1 offset");
static_assert((size_t)k_ra_rsip_off_data_in2 == 0x1408U, "DATA_IN2 offset");
static_assert((size_t)k_ra_rsip_off_data_in3 == 0x140CU, "DATA_IN3 offset");
static_assert((size_t)k_ra_rsip_off_data_out0 == 0x1410U, "DATA_OUT0 offset");
static_assert((size_t)k_ra_rsip_off_data_out1 == 0x1414U, "DATA_OUT1 offset");
static_assert((size_t)k_ra_rsip_off_data_out2 == 0x1418U, "DATA_OUT2 offset");
static_assert((size_t)k_ra_rsip_off_data_out3 == 0x141CU, "DATA_OUT3 offset");
static_assert((size_t)k_ra_rsip_off_key_stage == 0x1420U, "KEY_STAGE offset");
static_assert((size_t)k_ra_rsip_off_key_op_status == 0x1424U,
              "KEY_OP_STATUS offset (must NOT collide with TRNG status @ 0x1444)");

/* ---- TRNG sub-block @ 0x1440 -------------------------------------------- */
static_assert((size_t)k_ra_rsip_off_rnd_ctrl == 0x1440U, "RND_CTRL offset");
static_assert((size_t)k_ra_rsip_off_rnd_status == 0x1444U, "RND_STATUS offset");
static_assert((size_t)k_ra_rsip_off_rnd_data == 0x1458U, "RND_DATA offset");

/* The KEY_STAGE-vs-TRNG collision is the high-stakes bug this audit
 * prevents from coming back: ``KEY_OP_STATUS`` and ``RND_STATUS``
 * must live in different sub-blocks so a wrapped-key load cannot be
 * acknowledged by reading TRNG health flags. */
static_assert((size_t)k_ra_rsip_off_key_op_status != (size_t)k_ra_rsip_off_rnd_status,
              "KEY_OP_STATUS and RND_STATUS must not overlap");

/* ---- Symmetric cipher sub-block @ 0x1500 -------------------------------- */
static_assert((size_t)k_ra_rsip_off_sym_ctrl == 0x1500U, "SYM_CTRL offset");
static_assert((size_t)k_ra_rsip_off_sym_status == 0x1504U, "SYM_STATUS offset");
static_assert((size_t)k_ra_rsip_off_sym_keyh == 0x1508U, "SYM_KEYH offset");
static_assert((size_t)k_ra_rsip_off_sym_iv0 == 0x150CU, "SYM_IV0 offset");
static_assert((size_t)k_ra_rsip_off_sym_iv1 == 0x1510U, "SYM_IV1 offset");
static_assert((size_t)k_ra_rsip_off_sym_iv2 == 0x1514U, "SYM_IV2 offset");
static_assert((size_t)k_ra_rsip_off_sym_iv3 == 0x1518U, "SYM_IV3 offset");
static_assert((size_t)k_ra_rsip_off_sym_aad_in == 0x151CU, "SYM_AAD_IN offset");
static_assert((size_t)k_ra_rsip_off_sym_tag == 0x1520U, "SYM_TAG offset");
static_assert((size_t)k_ra_rsip_off_sym_aad_len == 0x1524U, "SYM_AAD_LEN offset");
static_assert((size_t)k_ra_rsip_off_sym_pt_len == 0x1528U, "SYM_PT_LEN offset");

/* ---- HASH sub-block @ 0x1600 -------------------------------------------- */
static_assert((size_t)k_ra_rsip_off_hash_ctrl == 0x1600U, "HASH_CTRL offset");
static_assert((size_t)k_ra_rsip_off_hash_status == 0x1604U, "HASH_STATUS offset");
static_assert((size_t)k_ra_rsip_off_hash_data_in == 0x1608U, "HASH_DATA_IN offset");
static_assert((size_t)k_ra_rsip_off_hash_digest == 0x1610U, "HASH_DIGEST offset");
static_assert((size_t)k_ra_rsip_off_hash_hmac == 0x1660U, "HASH_HMAC offset");

/* ---- Asymmetric (RSA / ECC) sub-block @ 0x1700 -------------------------- */
static_assert((size_t)k_ra_rsip_off_asym_ctrl == 0x1700U, "ASYM_CTRL offset");
static_assert((size_t)k_ra_rsip_off_asym_status == 0x1704U, "ASYM_STATUS offset");
static_assert((size_t)k_ra_rsip_off_asym_curve == 0x1708U, "ASYM_CURVE offset");
static_assert((size_t)k_ra_rsip_off_asym_arg == 0x170CU, "ASYM_ARG offset");
static_assert((size_t)k_ra_rsip_off_asym_key == 0x1710U, "ASYM_KEY offset");
static_assert((size_t)k_ra_rsip_off_asym_msg_in == 0x1714U, "ASYM_MSG_IN offset");
static_assert((size_t)k_ra_rsip_off_asym_sig_in == 0x1718U, "ASYM_SIG_IN offset");
static_assert((size_t)k_ra_rsip_off_asym_sig_out == 0x171CU, "ASYM_SIG_OUT offset");
static_assert((size_t)k_ra_rsip_off_asym_pub_x == 0x1720U, "ASYM_PUB_X offset");
static_assert((size_t)k_ra_rsip_off_asym_pub_y == 0x1724U, "ASYM_PUB_Y offset");
static_assert((size_t)k_ra_rsip_off_asym_shared == 0x1728U, "ASYM_SHARED offset");
static_assert((size_t)k_ra_rsip_off_asym_rsa_size == 0x172CU, "ASYM_RSA_SIZE offset");

/* ---- Key-vault sub-block @ 0x1800 --------------------------------------- */
static_assert((size_t)k_ra_rsip_off_kv_ctrl == 0x1800U, "KV_CTRL offset");
static_assert((size_t)k_ra_rsip_off_kv_slot == 0x1804U, "KV_SLOT offset");
static_assert((size_t)k_ra_rsip_off_kv_data == 0x1808U, "KV_DATA offset");
static_assert((size_t)k_ra_rsip_off_kv_count == 0x180CU, "KV_COUNT offset");

/* ---- Key-wrap sub-block @ 0x1900 ---------------------------------------- */
static_assert((size_t)k_ra_rsip_off_kw_ctrl == 0x1900U, "KW_CTRL offset");
static_assert((size_t)k_ra_rsip_off_kw_kek == 0x1904U, "KW_KEK offset");
static_assert((size_t)k_ra_rsip_off_kw_iv0 == 0x1908U, "KW_IV0 offset");
static_assert((size_t)k_ra_rsip_off_kw_iv1 == 0x190CU, "KW_IV1 offset");
static_assert((size_t)k_ra_rsip_off_kw_iv2 == 0x1910U, "KW_IV2 offset");
static_assert((size_t)k_ra_rsip_off_kw_iv3 == 0x1914U, "KW_IV3 offset");
static_assert((size_t)k_ra_rsip_off_kw_handle == 0x1918U, "KW_HANDLE offset");
static_assert((size_t)k_ra_rsip_off_kw_blob_in == 0x191CU, "KW_BLOB_IN offset");
static_assert((size_t)k_ra_rsip_off_kw_blob_out == 0x1920U, "KW_BLOB_OUT offset");

/* ---- OEM-install sub-block @ 0x1A00 ------------------------------------- */
static_assert((size_t)k_ra_rsip_off_oem_ctrl == 0x1A00U, "OEM_CTRL offset");
static_assert((size_t)k_ra_rsip_off_oem_arg == 0x1A04U, "OEM_ARG offset");
static_assert((size_t)k_ra_rsip_off_oem_iv == 0x1A08U, "OEM_IV offset");

/* ---- Command FIFO sub-block @ 0x1B00 ------------------------------------ */
static_assert((size_t)k_ra_rsip_off_cmd_fifo == 0x1B00U, "CMD_FIFO offset");

/* ---- Alignment sweep: every register must be a 32-bit word boundary ----- */
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_status);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_ier);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_isr);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_bist);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_tamper_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_tamper_status);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_dpa_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_dotf0_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_dotf1_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_oem_bl_ver);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_oem_bl_inc);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_oem_bl_lock);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_huk_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_uid_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kdf_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kdf_status);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kdf_label);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kdf_out);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kdf_info);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kdf_salt);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kdf_len);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kdf_ikm);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_life_state);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_debug_level);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_mbox_op);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_mbox_arg0);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_mbox_arg1);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_mbox_arg2);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_mbox_arg3);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_mbox_ret);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_data_in0);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_data_in1);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_data_in2);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_data_in3);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_data_out0);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_data_out1);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_data_out2);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_data_out3);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_key_stage);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_key_op_status);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_rnd_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_rnd_status);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_rnd_data);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_status);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_keyh);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_iv0);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_iv1);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_iv2);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_iv3);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_aad_in);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_tag);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_aad_len);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_sym_pt_len);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_hash_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_hash_status);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_hash_data_in);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_hash_digest);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_hash_hmac);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_status);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_curve);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_arg);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_key);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_msg_in);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_sig_in);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_sig_out);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_pub_x);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_pub_y);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_shared);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_asym_rsa_size);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kv_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kv_slot);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kv_data);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kv_count);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kw_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kw_kek);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kw_iv0);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kw_iv1);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kw_iv2);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kw_iv3);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kw_handle);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kw_blob_in);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_kw_blob_out);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_oem_ctrl);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_oem_arg);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_oem_iv);
RA_RSIP_ASSERT_ALIGN(k_ra_rsip_off_cmd_fifo);

/* ---- Sub-block containment sweep ---------------------------------------- */
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_ctrl, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_status, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_ier, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_isr, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_bist, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_tamper_ctrl, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_tamper_status, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_dpa_ctrl, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_dotf0_ctrl, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_dotf1_ctrl, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_oem_bl_ver, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_oem_bl_inc, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_oem_bl_lock, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_huk_ctrl, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_uid_ctrl, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kdf_ctrl, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kdf_status, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kdf_label, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kdf_out, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kdf_info, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kdf_salt, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kdf_len, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kdf_ikm, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_life_state, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_debug_level, k_ra_rsip_win_ctrl_lo, k_ra_rsip_win_ctrl_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_mbox_op, k_ra_rsip_win_mbox_lo, k_ra_rsip_win_mbox_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_mbox_arg0, k_ra_rsip_win_mbox_lo, k_ra_rsip_win_mbox_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_mbox_arg1, k_ra_rsip_win_mbox_lo, k_ra_rsip_win_mbox_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_mbox_arg2, k_ra_rsip_win_mbox_lo, k_ra_rsip_win_mbox_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_mbox_arg3, k_ra_rsip_win_mbox_lo, k_ra_rsip_win_mbox_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_mbox_ret, k_ra_rsip_win_mbox_lo, k_ra_rsip_win_mbox_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_data_in0, k_ra_rsip_win_data_lo, k_ra_rsip_win_data_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_data_in1, k_ra_rsip_win_data_lo, k_ra_rsip_win_data_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_data_in2, k_ra_rsip_win_data_lo, k_ra_rsip_win_data_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_data_in3, k_ra_rsip_win_data_lo, k_ra_rsip_win_data_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_data_out0, k_ra_rsip_win_data_lo, k_ra_rsip_win_data_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_data_out1, k_ra_rsip_win_data_lo, k_ra_rsip_win_data_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_data_out2, k_ra_rsip_win_data_lo, k_ra_rsip_win_data_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_data_out3, k_ra_rsip_win_data_lo, k_ra_rsip_win_data_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_key_stage, k_ra_rsip_win_data_lo, k_ra_rsip_win_data_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_key_op_status, k_ra_rsip_win_data_lo, k_ra_rsip_win_data_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_rnd_ctrl, k_ra_rsip_win_trng_lo, k_ra_rsip_win_trng_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_rnd_status, k_ra_rsip_win_trng_lo, k_ra_rsip_win_trng_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_rnd_data, k_ra_rsip_win_trng_lo, k_ra_rsip_win_trng_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_ctrl, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_status, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_keyh, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_iv0, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_iv1, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_iv2, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_iv3, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_aad_in, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_tag, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_aad_len, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_sym_pt_len, k_ra_rsip_win_sym_lo, k_ra_rsip_win_sym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_hash_ctrl, k_ra_rsip_win_hash_lo, k_ra_rsip_win_hash_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_hash_status, k_ra_rsip_win_hash_lo, k_ra_rsip_win_hash_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_hash_data_in, k_ra_rsip_win_hash_lo, k_ra_rsip_win_hash_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_hash_digest, k_ra_rsip_win_hash_lo, k_ra_rsip_win_hash_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_hash_hmac, k_ra_rsip_win_hash_lo, k_ra_rsip_win_hash_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_ctrl, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_status, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_curve, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_arg, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_key, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_msg_in, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_sig_in, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_sig_out, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_pub_x, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_pub_y, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_shared, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_asym_rsa_size, k_ra_rsip_win_asym_lo, k_ra_rsip_win_asym_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kv_ctrl, k_ra_rsip_win_kv_lo, k_ra_rsip_win_kv_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kv_slot, k_ra_rsip_win_kv_lo, k_ra_rsip_win_kv_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kv_data, k_ra_rsip_win_kv_lo, k_ra_rsip_win_kv_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kv_count, k_ra_rsip_win_kv_lo, k_ra_rsip_win_kv_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kw_ctrl, k_ra_rsip_win_kw_lo, k_ra_rsip_win_kw_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kw_kek, k_ra_rsip_win_kw_lo, k_ra_rsip_win_kw_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kw_iv0, k_ra_rsip_win_kw_lo, k_ra_rsip_win_kw_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kw_iv1, k_ra_rsip_win_kw_lo, k_ra_rsip_win_kw_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kw_iv2, k_ra_rsip_win_kw_lo, k_ra_rsip_win_kw_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kw_iv3, k_ra_rsip_win_kw_lo, k_ra_rsip_win_kw_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kw_handle, k_ra_rsip_win_kw_lo, k_ra_rsip_win_kw_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kw_blob_in, k_ra_rsip_win_kw_lo, k_ra_rsip_win_kw_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_kw_blob_out, k_ra_rsip_win_kw_lo, k_ra_rsip_win_kw_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_oem_ctrl, k_ra_rsip_win_oem_lo, k_ra_rsip_win_oem_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_oem_arg, k_ra_rsip_win_oem_lo, k_ra_rsip_win_oem_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_oem_iv, k_ra_rsip_win_oem_lo, k_ra_rsip_win_oem_hi);
RA_RSIP_ASSERT_IN_WIN(k_ra_rsip_off_cmd_fifo, k_ra_rsip_win_cmd_lo, k_ra_rsip_win_cmd_hi);

/**
 * @enum ra_rsip_ctrl_bits_t
 * @brief Field positions inside the CTRL mailbox word.
 *
 * @details
 * The CTRL word gates the engine clock, arms the BIST / TRNG
 * health checks, and routes command FIFOs. Bit positions are
 * derived from the FSP primitives that touch ``REG_0000H`` /
 * ``REG_0004H``.
 */
typedef enum : uint8_t {
  k_ra_rsip_ctrl_bit_enable   = 0U, /**< 1 = clock the engine.               */
  k_ra_rsip_ctrl_bit_bist     = 1U, /**< 1 = arm built-in self-test.         */
  k_ra_rsip_ctrl_bit_reset    = 2U, /**< 1 = soft-reset the engine.          */
  k_ra_rsip_ctrl_bit_dpa_arm  = 3U, /**< 1 = enable SPA/DPA countermeasures. */
  k_ra_rsip_ctrl_bit_irq_glob = 4U, /**< 1 = globally enable RSIP IRQ line.  */
} ra_rsip_ctrl_bits_t;

/**
 * @enum ra_rsip_status_bits_t
 * @brief Field positions inside the STATUS mailbox word.
 */
typedef enum : uint8_t {
  k_ra_rsip_status_bit_ready   = 0U, /**< 1 = idle and ready for command.   */
  k_ra_rsip_status_bit_busy    = 1U, /**< 1 = engine is running a command.  */
  k_ra_rsip_status_bit_bist_ok = 2U, /**< 1 = self-test passed.             */
  k_ra_rsip_status_bit_kv_ok   = 3U, /**< 1 = key vault initialized.        */
  k_ra_rsip_status_bit_huk_ok  = 4U, /**< 1 = HUK loaded and unwrapped.     */
  k_ra_rsip_status_bit_dpa_on  = 5U, /**< 1 = SPA/DPA countermeasures live. */
  k_ra_rsip_status_bit_err     = 7U, /**< 1 = access-management error.      */
} ra_rsip_status_bits_t;

/**
 * @enum ra_rsip_isr_bits_t
 * @brief Field positions inside the IER / ISR words.
 *
 * @details
 * The RSIP routes a small handful of edge-triggered events into one
 * peripheral IRQ line; HUM Ch 52.1 Table 52.1 p 3302 names the
 * tamper line ``RSIP_TADI``.
 */
typedef enum : uint8_t {
  k_ra_rsip_isr_bit_done      = 0U, /**< Generic command-complete.         */
  k_ra_rsip_isr_bit_rnd       = 1U, /**< RNG fresh-data ready.             */
  k_ra_rsip_isr_bit_hash      = 2U, /**< Hash absorption complete.         */
  k_ra_rsip_isr_bit_asym_done = 3U, /**< RSA/ECC operation complete.       */
  k_ra_rsip_isr_bit_kv_done   = 4U, /**< Key vault op complete.            */
  k_ra_rsip_isr_bit_kdf_done  = 5U, /**< KDF complete.                     */
  k_ra_rsip_isr_bit_err       = 6U, /**< Generic access/algorithm error.   */
  k_ra_rsip_isr_bit_tamper    = 7U, /**< Tamper / RSIP_TADI interrupt.     */
} ra_rsip_isr_bits_t;

/**
 * @enum ra_rsip_mask_t
 * @brief Pre-computed bit masks for the CTRL / STATUS / ISR words.
 */
typedef enum : uint32_t {
  k_ra_rsip_mask_ctrl_enable   = 0x00000001UL, /**< CTRL.ENABLE  bit.    */
  k_ra_rsip_mask_ctrl_bist     = 0x00000002UL, /**< CTRL.BIST    bit.    */
  k_ra_rsip_mask_ctrl_reset    = 0x00000004UL, /**< CTRL.RESET   bit.    */
  k_ra_rsip_mask_ctrl_dpa_arm  = 0x00000008UL, /**< CTRL.DPA_ARM bit.    */
  k_ra_rsip_mask_ctrl_irq_glob = 0x00000010UL, /**< CTRL.IRQ_GLOB bit.   */
  k_ra_rsip_mask_status_ready  = 0x00000001UL, /**< STATUS.READY bit.    */
  k_ra_rsip_mask_status_busy   = 0x00000002UL, /**< STATUS.BUSY  bit.    */
  k_ra_rsip_mask_status_bistok = 0x00000004UL, /**< STATUS.BIST_OK.      */
  k_ra_rsip_mask_status_kvok   = 0x00000008UL, /**< STATUS.KV_OK.        */
  k_ra_rsip_mask_status_hukok  = 0x00000010UL, /**< STATUS.HUK_OK.       */
  k_ra_rsip_mask_status_dpa_on = 0x00000020UL, /**< STATUS.DPA_ON.       */
  k_ra_rsip_mask_status_err    = 0x00000080UL, /**< STATUS.ERR   bit.    */
  k_ra_rsip_mask_isr_done      = 0x00000001UL, /**< ISR.DONE      bit.   */
  k_ra_rsip_mask_isr_rnd       = 0x00000002UL, /**< ISR.RND       bit.   */
  k_ra_rsip_mask_isr_hash      = 0x00000004UL, /**< ISR.HASH      bit.   */
  k_ra_rsip_mask_isr_asym_done = 0x00000008UL, /**< ISR.ASYM_DONE bit.   */
  k_ra_rsip_mask_isr_kv_done   = 0x00000010UL, /**< ISR.KV_DONE   bit.   */
  k_ra_rsip_mask_isr_kdf_done  = 0x00000020UL, /**< ISR.KDF_DONE  bit.   */
  k_ra_rsip_mask_isr_err       = 0x00000040UL, /**< ISR.ERR       bit.   */
  k_ra_rsip_mask_isr_tamper    = 0x00000080UL, /**< ISR.TAMPER    bit.   */
  k_ra_rsip_mask_isr_all       = 0x000000FFUL, /**< Union of ISR bits.   */
  k_ra_rsip_mask_kv_busy       = 0x80000000UL, /**< Key-op status BUSY.  */
  k_ra_rsip_mask_kv_ready      = 0x80000000UL, /**< Key-op status READY. */
} ra_rsip_mask_t;

/**
 * @enum ra_rsip_hash_alg_t
 * @brief Algorithm selector value written to the HASH CTRL word.
 *
 * @details
 * The RSIP supports the SHA-2 family, the SHA-3 family, and the
 * SHAKE extendable-output functions (HUM Ch 52 Table 52.1 p 3302).
 */
typedef enum : uint32_t {
  k_ra_rsip_hash_sha224     = 0x00000001UL, /**< SHA-224.                 */
  k_ra_rsip_hash_sha256     = 0x00000002UL, /**< SHA-256.                 */
  k_ra_rsip_hash_sha384     = 0x00000003UL, /**< SHA-384.                 */
  k_ra_rsip_hash_sha512     = 0x00000004UL, /**< SHA-512.                 */
  k_ra_rsip_hash_sha512_224 = 0x00000005UL, /**< SHA-512/224.             */
  k_ra_rsip_hash_sha512_256 = 0x00000006UL, /**< SHA-512/256.             */
  k_ra_rsip_hash_sha3_224   = 0x00000011UL, /**< SHA3-224 FIPS PUB 202.   */
  k_ra_rsip_hash_sha3_256   = 0x00000012UL, /**< SHA3-256 FIPS PUB 202.   */
  k_ra_rsip_hash_sha3_384   = 0x00000013UL, /**< SHA3-384 FIPS PUB 202.   */
  k_ra_rsip_hash_sha3_512   = 0x00000014UL, /**< SHA3-512 FIPS PUB 202.   */
  k_ra_rsip_hash_shake128   = 0x00000018UL, /**< SHAKE128 FIPS PUB 202.   */
  k_ra_rsip_hash_shake256   = 0x00000019UL, /**< SHAKE256 FIPS PUB 202.   */
} ra_rsip_hash_alg_t;

/**
 * @enum ra_rsip_oem_cmd_t
 * @brief OEM-key install opcode written to ``OEM_CTRL``.
 *
 * @details
 * Mirrors the ``rsip_oem_cmd_t`` table in the FSP key-injection
 * driver (``r_rsip_key_injection.c``). These opcodes select which
 * algorithm + key length the OEM-flow installs into the wrapped
 * key vault (HUM Ch 52.1 Table 52.1 "Application Key Management"
 * p 3303).
 */
typedef enum : uint32_t {
  k_ra_rsip_oem_cmd_invalid             = 0U,  /**< Sentinel / unused.       */
  k_ra_rsip_oem_cmd_aes128              = 5U,  /**< AES-128.                 */
  k_ra_rsip_oem_cmd_aes192              = 6U,  /**< AES-192.                 */
  k_ra_rsip_oem_cmd_aes256              = 7U,  /**< AES-256.                 */
  k_ra_rsip_oem_cmd_aes128_xts          = 8U,  /**< AES-128 XTS.             */
  k_ra_rsip_oem_cmd_aes256_xts          = 9U,  /**< AES-256 XTS.             */
  k_ra_rsip_oem_cmd_rsa2048_priv        = 13U, /**< RSA-2048 private.        */
  k_ra_rsip_oem_cmd_rsa3072_priv        = 15U, /**< RSA-3072 private.        */
  k_ra_rsip_oem_cmd_rsa4096_priv        = 17U, /**< RSA-4096 private.        */
  k_ra_rsip_oem_cmd_ecc_secp256r1_priv  = 23U, /**< ECC NIST P-256 private.  */
  k_ra_rsip_oem_cmd_ecc_secp384r1_priv  = 25U, /**< ECC NIST P-384 private.  */
  k_ra_rsip_oem_cmd_hmac_sha224         = 26U, /**< HMAC-SHA-224.            */
  k_ra_rsip_oem_cmd_hmac_sha256         = 27U, /**< HMAC-SHA-256.            */
  k_ra_rsip_oem_cmd_ecc_brain256r1_priv = 29U, /**< ECC Brainpool P-256.     */
  k_ra_rsip_oem_cmd_ecc_brain384r1_priv = 31U, /**< ECC Brainpool P-384.     */
  k_ra_rsip_oem_cmd_ecc_brain512r1_priv = 33U, /**< ECC Brainpool P-512.     */
  k_ra_rsip_oem_cmd_ecc_secp256k1_priv  = 35U, /**< ECC secp256k1 private.   */
  k_ra_rsip_oem_cmd_ecc_secp521r1_priv  = 37U, /**< ECC NIST P-521 private.  */
  k_ra_rsip_oem_cmd_ecc_ed25519_priv    = 39U, /**< Ed25519 private.         */
  k_ra_rsip_oem_cmd_hmac_sha384         = 40U, /**< HMAC-SHA-384.            */
  k_ra_rsip_oem_cmd_hmac_sha512         = 41U, /**< HMAC-SHA-512.            */
  k_ra_rsip_oem_cmd_hmac_sha512_224     = 42U, /**< HMAC-SHA-512/224.        */
  k_ra_rsip_oem_cmd_hmac_sha512_256     = 43U, /**< HMAC-SHA-512/256.        */
  k_ra_rsip_oem_cmd_chacha20            = 48U, /**< ChaCha20.                */
} ra_rsip_oem_cmd_t;

/**
 * @enum ra_rsip_kv_op_t
 * @brief Key-vault command word values written to ``KV_CTRL``.
 */
typedef enum : uint32_t {
  k_ra_rsip_kv_op_idle  = 0x00000000UL, /**< No-op.                          */
  k_ra_rsip_kv_op_read  = 0x00000001UL, /**< Read slot -> ``KV_DATA``.       */
  k_ra_rsip_kv_op_write = 0x00000002UL, /**< Write ``KV_DATA`` -> slot.      */
  k_ra_rsip_kv_op_erase = 0x00000003UL, /**< Zeroise slot.                   */
  k_ra_rsip_kv_op_count = 0x00000004UL, /**< Snapshot populated-slot count.  */
} ra_rsip_kv_op_t;

/**
 * @enum ra_rsip_sym_alg_t
 * @brief Symmetric-cipher algorithm selector low byte of ``SYM_CTRL``.
 *
 * @details
 * The cipher engine multiplexes AES-128/192/256 and ChaCha20 across
 * the same data path. The low byte selects the algorithm + key
 * length; the next byte selects the mode (``ra_rsip_aes_mode_t``);
 * the next byte selects the direction (``ra_rsip_aes_dir_t``).
 */
typedef enum : uint8_t {
  k_ra_rsip_sym_alg_aes128   = 1U, /**< AES with a 128-bit key.            */
  k_ra_rsip_sym_alg_aes192   = 2U, /**< AES with a 192-bit key.            */
  k_ra_rsip_sym_alg_aes256   = 3U, /**< AES with a 256-bit key.            */
  k_ra_rsip_sym_alg_chacha20 = 4U, /**< ChaCha20 stream cipher.            */
} ra_rsip_sym_alg_t;

/**
 * @enum ra_rsip_handle_word_t
 * @brief Wrapped-key handle word counts (FSP ``rsip_wrapped_key_t``).
 *
 * @details
 * A wrapped key blob is a 32-bit ``alg`` selector plus a number of
 * 32-bit body words; the body word counts come from
 * ``RSIP_WORD_SIZE_WRAPPED_KEY_*`` in the FSP private header.
 */
typedef enum : uint16_t {
  k_ra_rsip_handle_words_aes128       = 8U,   /**< AES-128 wrapped-key body. */
  k_ra_rsip_handle_words_aes192       = 12U,  /**< AES-192 wrapped-key body. */
  k_ra_rsip_handle_words_aes256       = 12U,  /**< AES-256 wrapped-key body. */
  k_ra_rsip_handle_words_chacha20     = 12U,  /**< ChaCha20 wrapped key body.*/
  k_ra_rsip_handle_words_hmac_sha224  = 12U,  /**< HMAC-SHA-224 wrapped key. */
  k_ra_rsip_handle_words_hmac_sha256  = 12U,  /**< HMAC-SHA-256 wrapped key. */
  k_ra_rsip_handle_words_hmac_sha384  = 16U,  /**< HMAC-SHA-384 wrapped key. */
  k_ra_rsip_handle_words_hmac_sha512  = 20U,  /**< HMAC-SHA-512 wrapped key. */
  k_ra_rsip_handle_words_ecc256_priv  = 12U,  /**< ECC P-256 wrapped private.*/
  k_ra_rsip_handle_words_ecc384_priv  = 16U,  /**< ECC P-384 wrapped private.*/
  k_ra_rsip_handle_words_ecc521_priv  = 24U,  /**< ECC P-521 wrapped private.*/
  k_ra_rsip_handle_words_rsa2048_priv = 132U, /**< RSA-2048 wrapped private. */
  k_ra_rsip_handle_words_rsa3072_priv = 196U, /**< RSA-3072 wrapped private. */
  k_ra_rsip_handle_words_rsa4096_priv = 260U, /**< RSA-4096 wrapped private. */
} ra_rsip_handle_word_t;

/**
 * @enum ra_rsip_kw_op_t
 * @brief Key-wrap command word values written to ``KW_CTRL``.
 */
typedef enum : uint32_t {
  k_ra_rsip_kw_op_wrap   = 0x00000001UL, /**< Wrap a plaintext handle -> blob. */
  k_ra_rsip_kw_op_unwrap = 0x00000002UL, /**< Unwrap a blob -> handle.         */
} ra_rsip_kw_op_t;

/**
 * @enum ra_rsip_dotf_route_t
 * @brief DOTF key route command value written to ``DOTFn_CTRL``.
 */
typedef enum : uint32_t {
  k_ra_rsip_dotf_off = 0x00000000UL, /**< Disable key feed.                 */
  k_ra_rsip_dotf_on  = 0x00000001UL, /**< Feed key from slot bits 31..16.   */
} ra_rsip_dotf_route_t;

/**
 * @enum ra_rsip_kdf_op_t
 * @brief KDF mode selector written to ``KDF_CTRL``.
 */
typedef enum : uint32_t {
  k_ra_rsip_kdf_op_huk_label   = 0x00000001UL, /**< Derive from HUK + label. */
  k_ra_rsip_kdf_op_uid_label   = 0x00000002UL, /**< Derive from UID + label. */
  k_ra_rsip_kdf_op_hkdf_sha256 = 0x00000010UL, /**< HKDF SHA-256.            */
  k_ra_rsip_kdf_op_hkdf_sha384 = 0x00000011UL, /**< HKDF SHA-384.            */
  k_ra_rsip_kdf_op_hkdf_sha512 = 0x00000012UL, /**< HKDF SHA-512.            */
} ra_rsip_kdf_op_t;

/**
 * @enum ra_rsip_life_state_t
 * @brief Device-lifecycle state values (HUM Ch 51.1 p 3263).
 */
typedef enum : uint32_t {
  k_ra_rsip_life_cm     = 0x00000000UL, /**< Chip-manufacturing.            */
  k_ra_rsip_life_ssd    = 0x00000001UL, /**< Secure software development.   */
  k_ra_rsip_life_nsecsd = 0x00000002UL, /**< Non-secure software dev.       */
  k_ra_rsip_life_dpl    = 0x00000003UL, /**< Deployed.                      */
  k_ra_rsip_life_lcm    = 0x00000004UL, /**< Locked.                        */
  k_ra_rsip_life_rma    = 0x00000005UL, /**< Return-merchandise-auth.       */
} ra_rsip_life_state_t;

/**
 * @enum ra_rsip_debug_level_t
 * @brief Debug authorisation level (HUM Ch 51.1 p 3263 "Three debug levels").
 */
typedef enum : uint8_t {
  k_ra_rsip_debug_al0 = 0U, /**< AL0 -- no debug functions available.      */
  k_ra_rsip_debug_al1 = 1U, /**< AL1 -- non-secure debug only.             */
  k_ra_rsip_debug_al2 = 2U, /**< AL2 -- secure + non-secure debug.         */
} ra_rsip_debug_level_t;

/**
 * @enum ra_rsip_aes_mode_t
 * @brief Block-cipher mode selector for the AES engine.
 *
 * @details
 * HUM Ch 52.1 Table 52.1 p 3302: AES supports ECB, CBC, CTR
 * (NIST SP 800-38A), CCM (38C), GCM (38D), XTS (IEEE 1619-2007),
 * CMAC (38B), GMAC (38D).
 */
typedef enum : uint8_t {
  k_ra_rsip_aes_mode_ecb  = 0U, /**< ECB.                                  */
  k_ra_rsip_aes_mode_cbc  = 1U, /**< CBC.                                  */
  k_ra_rsip_aes_mode_ctr  = 2U, /**< CTR.                                  */
  k_ra_rsip_aes_mode_gcm  = 3U, /**< GCM (AEAD).                           */
  k_ra_rsip_aes_mode_ccm  = 4U, /**< CCM (AEAD).                           */
  k_ra_rsip_aes_mode_xts  = 5U, /**< XTS.                                  */
  k_ra_rsip_aes_mode_cmac = 6U, /**< CMAC.                                 */
  k_ra_rsip_aes_mode_gmac = 7U, /**< GMAC.                                 */
} ra_rsip_aes_mode_t;

/**
 * @enum ra_rsip_aes_dir_t
 * @brief Direction selector for symmetric ciphers.
 */
typedef enum : uint8_t {
  k_ra_rsip_dir_encrypt = 0U, /**< Plaintext -> Ciphertext.                */
  k_ra_rsip_dir_decrypt = 1U, /**< Ciphertext -> Plaintext.                */
} ra_rsip_aes_dir_t;

/**
 * @enum ra_rsip_curve_t
 * @brief ECC curve selector (HUM Ch 52.1 Table 52.1 "ECC" p 3302).
 */
typedef enum : uint8_t {
  k_ra_rsip_curve_secp192r1  = 0U, /**< NIST P-192.                       */
  k_ra_rsip_curve_secp224r1  = 1U, /**< NIST P-224.                       */
  k_ra_rsip_curve_secp256r1  = 2U, /**< NIST P-256.                       */
  k_ra_rsip_curve_secp384r1  = 3U, /**< NIST P-384.                       */
  k_ra_rsip_curve_secp521r1  = 4U, /**< NIST P-521.                       */
  k_ra_rsip_curve_brain256r1 = 5U, /**< Brainpool P-256r1.                */
  k_ra_rsip_curve_brain384r1 = 6U, /**< Brainpool P-384r1.                */
  k_ra_rsip_curve_brain512r1 = 7U, /**< Brainpool P-512r1.                */
  k_ra_rsip_curve_ed25519    = 8U, /**< Ed25519.                          */
  k_ra_rsip_curve_secp256k1  = 9U, /**< secp256k1.                        */
} ra_rsip_curve_t;

/**
 * @enum ra_rsip_rsa_size_t
 * @brief RSA key-size selector (HUM Ch 52.1 Table 52.1 "RSA" p 3302).
 */
typedef enum : uint16_t {
  k_ra_rsip_rsa_1024 = 1024U, /**< 1024-bit RSA.                           */
  k_ra_rsip_rsa_2048 = 2048U, /**< 2048-bit RSA.                           */
  k_ra_rsip_rsa_3072 = 3072U, /**< 3072-bit RSA.                           */
  k_ra_rsip_rsa_4096 = 4096U, /**< 4096-bit RSA.                           */
} ra_rsip_rsa_size_t;

/**
 * @enum ra_rsip_asym_op_t
 * @brief Opcode written to ``ASYM_CTRL`` to select the operation.
 */
typedef enum : uint32_t {
  k_ra_rsip_asym_op_rsa_sign     = 0x00000001UL, /**< RSA signature gen.   */
  k_ra_rsip_asym_op_rsa_verify   = 0x00000002UL, /**< RSA signature ver.   */
  k_ra_rsip_asym_op_rsa_encrypt  = 0x00000003UL, /**< RSA public encrypt.  */
  k_ra_rsip_asym_op_rsa_decrypt  = 0x00000004UL, /**< RSA private decrypt. */
  k_ra_rsip_asym_op_ecdsa_sign   = 0x00000005UL, /**< ECDSA signature gen. */
  k_ra_rsip_asym_op_ecdsa_verify = 0x00000006UL, /**< ECDSA signature ver. */
  k_ra_rsip_asym_op_ecdh_compute = 0x00000007UL, /**< ECDH shared secret.  */
  k_ra_rsip_asym_op_ecc_keygen   = 0x00000008UL, /**< ECC keypair gen.     */
} ra_rsip_asym_op_t;

/**
 * @enum ra_rsip_tamper_src_t
 * @brief Tamper-source identifier bits inside ``TAMPER_CTRL`` /
 *        ``TAMPER_STATUS``.
 *
 * @details
 * HUM Ch 51.6 "Tamper Detection" p 3294 lists six external tamper
 * lines plus the three internal sources (RSIP self-fault, voltage
 * glitch, clock glitch).
 */
typedef enum : uint32_t {
  k_ra_rsip_tamper_src_ext0 = 0x00000001UL, /**< External tamper input 0. */
  k_ra_rsip_tamper_src_ext1 = 0x00000002UL, /**< External tamper input 1. */
  k_ra_rsip_tamper_src_ext2 = 0x00000004UL, /**< External tamper input 2. */
  k_ra_rsip_tamper_src_ext3 = 0x00000008UL, /**< External tamper input 3. */
  k_ra_rsip_tamper_src_ext4 = 0x00000010UL, /**< External tamper input 4. */
  k_ra_rsip_tamper_src_ext5 = 0x00000020UL, /**< External tamper input 5. */
  k_ra_rsip_tamper_src_volt = 0x00010000UL, /**< Voltage-glitch detector. */
  k_ra_rsip_tamper_src_clk  = 0x00020000UL, /**< Clock-glitch detector.   */
  k_ra_rsip_tamper_src_self = 0x00040000UL, /**< RSIP internal self-fault.*/
  k_ra_rsip_tamper_src_all  = 0x0007003FUL, /**< Union of all sources.    */
} ra_rsip_tamper_src_t;

/**
 * @enum ra_rsip_chacha_op_t
 * @brief ChaCha20 / Poly1305 operation selector.
 */
typedef enum : uint32_t {
  k_ra_rsip_chacha_op_encrypt      = 0x00000001UL, /**< ChaCha20 stream encrypt. */
  k_ra_rsip_chacha_op_decrypt      = 0x00000002UL, /**< ChaCha20 stream decrypt. */
  k_ra_rsip_chacha_op_aead_encrypt = 0x00000003UL, /**< ChaCha20-Poly1305 enc. */
  k_ra_rsip_chacha_op_aead_decrypt = 0x00000004UL, /**< ChaCha20-Poly1305 dec. */
  k_ra_rsip_chacha_op_poly1305_mac = 0x00000005UL, /**< Poly1305 MAC only.    */
} ra_rsip_chacha_op_t;

/**
 * @enum ra_rsip_sizes_t
 * @brief Buffer sizes published to callers.
 *
 * @details
 * AES block size = 128 bits (HUM Ch 52.1 Table 52.1 "Symmetric
 * cryptography" p 3302). Hash digest sizes follow FIPS PUB 180-4 /
 * FIPS PUB 202.
 */
typedef enum : uint16_t {
  k_ra_rsip_sha224_digest_bytes     = 28U, /**< SHA-224 digest length.    */
  k_ra_rsip_sha256_digest_bytes     = 32U, /**< SHA-256 digest length.    */
  k_ra_rsip_sha256_digest_words     = 8U,  /**< SHA-256 = 8 * uint32_t.   */
  k_ra_rsip_sha384_digest_bytes     = 48U, /**< SHA-384 digest length.    */
  k_ra_rsip_sha512_digest_bytes     = 64U, /**< SHA-512 digest length.    */
  k_ra_rsip_sha512_224_digest_bytes = 28U, /**< SHA-512/224 length.       */
  k_ra_rsip_sha512_256_digest_bytes = 32U, /**< SHA-512/256 length.       */
  k_ra_rsip_sha3_224_digest_bytes   = 28U, /**< SHA3-224 length.          */
  k_ra_rsip_sha3_256_digest_bytes   = 32U, /**< SHA3-256 length.          */
  k_ra_rsip_sha3_384_digest_bytes   = 48U, /**< SHA3-384 length.          */
  k_ra_rsip_sha3_512_digest_bytes   = 64U, /**< SHA3-512 length.          */
  k_ra_rsip_aes_block_bytes         = 16U, /**< AES block = 128 bits.     */
  k_ra_rsip_aes128_key_bytes        = 16U, /**< AES-128 key length.       */
  k_ra_rsip_aes192_key_bytes        = 24U, /**< AES-192 key length.       */
  k_ra_rsip_aes256_key_bytes        = 32U, /**< AES-256 key length.       */
  k_ra_rsip_chacha_key_bytes        = 32U, /**< ChaCha20 256-bit key.     */
  k_ra_rsip_chacha_nonce_bytes      = 12U, /**< RFC 7539 12-byte nonce.   */
  k_ra_rsip_poly1305_tag_bytes      = 16U, /**< Poly1305 16-byte MAC.     */
  k_ra_rsip_gcm_tag_bytes           = 16U, /**< AES-GCM 128-bit tag.      */
  k_ra_rsip_ccm_tag_bytes           = 16U, /**< AES-CCM max tag = 128b.   */
  k_ra_rsip_huk_bytes               = 32U, /**< 256-bit HUK.              */
  k_ra_rsip_uid_bytes               = 16U, /**< 128-bit Unique ID.        */
  k_ra_rsip_trng_word_bytes         = 4U,  /**< Bytes per TRNG read.      */
  k_ra_rsip_kv_slot_bytes           = 64U, /**< Wrapped-key blob size.    */
  k_ra_rsip_kv_slot_count           = 16U, /**< Number of vault slots.    */
} ra_rsip_sizes_t;

/* NOLINTEND(readability-magic-numbers) */

/**
 * @brief Volatile pointer to a 32-bit RSIP register at ``offset``.
 *
 * @details
 * Mirrors the ``ra_glcdc_reg32`` accessor pattern so the host test
 * build can intercept reads and writes via ``ra_sim_mmap``. The
 * accessor performs no validation; callers pass a value from
 * ``ra_rsip_off_t``.
 *
 * @param[in] offset One of the ``k_ra_rsip_off_*`` values.
 * @return Volatile pointer to the register cell.
 */
static inline volatile uint32_t* ra_rsip_reg32(ra_rsip_off_t offset)
{
  return (volatile uint32_t*)(k_ra_rsip_base_addr + (uintptr_t)offset);
}

#ifdef __cplusplus
}
#endif
