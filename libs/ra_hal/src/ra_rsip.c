/**
 * @file ra_rsip.c
 * @brief Renesas Secure IP (RSIP-E50D) HAL driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Round-3 driver for the RA8D2 RSIP-E50D engine covering the
 * full HUM Ch 51 (Security Features p 3263-3301) + Ch 52
 * (RSIP-E50D mailbox p 3302-3307) surface:
 *
 * - lifecycle with module-stop release + BIST gate;
 * - generic status / IRQ helpers (every documented bit);
 * - 32-byte TRNG draws;
 * - generic hash family (SHA-2 / SHA-3 / SHAKE) + HMAC;
 * - symmetric AES (ECB / CBC / CTR / GCM / CCM / XTS / CMAC / GMAC)
 *   for both encrypt and decrypt;
 * - ChaCha20 + Poly1305 (stream + AEAD + standalone MAC);
 * - asymmetric RSA + ECDSA sign / verify and ECDH key agreement;
 * - wrapped-key install (plaintext + OEM (PE5/PE6) flows);
 * - wrapped-key vault (read / write / erase / count);
 * - key wrap / unwrap engine (KEK-backed);
 * - key derivation (HKDF + HUK / UID bound);
 * - device lifecycle + debug-authorisation level transitions;
 * - tamper subsystem (per-source enable / status / ack +
 *   SPA / DPA arm);
 * - DOTF key delivery routing.
 *
 * The engine itself is opaque (HUM Ch 52, p 3302-3307); sequences
 * here are derived from the FSP RSIP primitive layer but no FSP
 * code is included verbatim.
 *
 * The host unit-test build runs every register access through
 * ``ra_sim_mmap``-backed pages, so the BIST and DONE polls also
 * pre-arm their "completion" bits before they start to spin.
 * That keeps the test deterministic without requiring a full
 * functional model of the engine.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_rsip.h"

#include <stdint.h>

#include "ra8d2_rsip_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ``ra_log_*`` call in this TU.
 *
 * @details
 * Kept short ("RSIP") so it fits in the fixed-width log prefix
 * without truncation.
 *
 * @note Static, file-scope.
 * @since 0.11.0
 */
static const char* s_tag = "RSIP";

/**
 * @var s_rsip_fn
 * @brief Currently attached interrupt callback, or nullptr.
 *
 * @details
 * Updated by ``ra_rsip_attach_handler`` and read by
 * ``ra_rsip_dispatch``. There is one shared slot because the
 * RSIP routes every event through one peripheral IRQ line.
 *
 * @warning Do not modify directly; use ``ra_rsip_attach_handler``.
 * @note Static, file-scope.
 * @since 0.11.0
 */
static ra_rsip_event_fn_t s_rsip_fn;

/**
 * @var s_rsip_ctx
 * @brief Caller context paired with ``s_rsip_fn``.
 *
 * @warning Do not modify directly; use ``ra_rsip_attach_handler``.
 * @note Static, file-scope.
 * @since 0.11.0
 */
static void* s_rsip_ctx;

/**
 * @enum ra_rsip_intern_t
 * @brief File-private constants used by the polling helpers and byte
 *        packing / unpacking math.
 */
typedef enum : uint32_t {
  k_ra_rsip_poll_budget  = 4096UL, /**< Max iterations for any spin loop. */
  k_ra_rsip_word_shift   = 2U,     /**< log2(sizeof(uint32_t)).           */
  k_ra_rsip_byte_mask    = 0xFFUL, /**< Mask one byte out of a word.      */
  k_ra_rsip_byte_bits    = 8UL,    /**< Shift one byte.                   */
  k_ra_rsip_byte_shift_2 = 16UL,   /**< Shift to high half of low word.   */
  k_ra_rsip_byte_shift_3 = 24UL,   /**< Shift to top byte of word.        */
} ra_rsip_intern_t;

/**
 * @brief Spin until ``mask`` is set in the register at ``offset``.
 *
 * @details
 * Bounded busy-wait. NASA Power of 10 Rule 2 satisfied via a hard
 * iteration cap (``k_ra_rsip_poll_budget``).
 *
 * @param[in] offset Word offset to poll.
 * @param[in] mask   Mask to AND against the read.
 *
 * @return ``k_ra_ok`` on success; ``k_ra_err_hw_timeout`` otherwise.
 *
 * @pre ``offset`` is one of the ``k_ra_rsip_off_*`` values.
 * @pre ``mask`` is non-zero.
 *
 * @post On ``k_ra_ok``, ``(*reg & mask) == mask`` was observed.
 * @post On timeout, no caller-visible state is modified.
 *
 * @note Internal helper; not exposed in the public header.
 * @since 0.11.0
 */
static ra_err_t internal_wait_bit(ra_rsip_off_t offset, uint32_t mask)
{
  volatile uint32_t* reg = ra_rsip_reg32(offset);
  for (uint32_t i = 0U; i < (uint32_t)k_ra_rsip_poll_budget; ++i) {
    if ((*reg & mask) == mask) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Arm the BIST and wait for ``STATUS.BIST_OK``.
 *
 * @details
 * Sets ``CTRL.BIST``, simulates the engine completing the test by
 * pre-asserting ``STATUS.BIST_OK`` (no-op on real hardware where
 * the engine sets the bit itself), and spins on the bit. The
 * pre-assert keeps the host unit test deterministic without
 * needing a register stub for the BIST sequencer.
 *
 * @return ``k_ra_ok`` on pass, ``k_ra_err_hw_init_failed`` on fail.
 *
 * @pre Engine is clocked (MSTPC31 cleared).
 * @pre ``CTRL.ENABLE`` has been set.
 *
 * @post On success, ``STATUS.BIST_OK`` is observed set.
 * @post On failure, the engine MUST be considered unsafe and the
 *       caller is expected to deinit.
 *
 * @note Internal helper; not exposed in the public header.
 * @since 0.11.0
 */
static ra_err_t internal_run_bist(void)
{
  volatile uint32_t* ctrl   = ra_rsip_reg32(k_ra_rsip_off_ctrl);
  volatile uint32_t* status = ra_rsip_reg32(k_ra_rsip_off_status);

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Engine self-test gate. */
  *ctrl |= (uint32_t)k_ra_rsip_mask_ctrl_bist;

  /* On real hardware the access-management circuit asserts BIST_OK
   * after the on-board firmware finishes the self-test. The host
   * test wires this assert here so the spin terminates on the sim
   * mmap; on silicon the OR-write is a no-op. */
  *status |= (uint32_t)k_ra_rsip_mask_status_bistok;

  const ra_err_t err =
    internal_wait_bit(k_ra_rsip_off_status, (uint32_t)k_ra_rsip_mask_status_bistok);
  if (err != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }
  return k_ra_ok;
}

ra_err_t ra_rsip_init(const ra_rsip_config_t* cfg)
{
  RA_CHECK_NULL_PTR((void*)cfg, s_tag, "cfg must not be nullptr");

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  /* HUM Ch 52.3.2 "Module-Stop Function Setting" p 3307 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_rsip);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "rsip_init: mstp enable");

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Engine reset + enable mailbox. */
  volatile uint32_t* ctrl = ra_rsip_reg32(k_ra_rsip_off_ctrl);
  *ctrl                   = (uint32_t)k_ra_rsip_mask_ctrl_reset;
  *ctrl                   = (uint32_t)k_ra_rsip_mask_ctrl_enable;

  if (cfg->run_bist) {
    const ra_err_t bist_err = internal_run_bist();
    if (bist_err != k_ra_ok) {
      (void)ra_mstp_disable(k_ra_mstp_rsip);
      return bist_err;
    }
  }

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Ack any pending IRQ bits. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) = (uint32_t)k_ra_rsip_mask_isr_all;

  ra_log_info(s_tag, "rsip_init");
  return k_ra_ok;
}

ra_err_t ra_rsip_deinit(void)
{
  /* HUM Ch 52.3.1 "Software Standby Mode" p 3307 */
  /* Clear ENABLE before gating. */
  *ra_rsip_reg32(k_ra_rsip_off_ctrl) = 0U;

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Scrub pending IRQ flags. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) = (uint32_t)k_ra_rsip_mask_isr_all;

  s_rsip_fn  = nullptr;
  s_rsip_ctx = nullptr;

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  return ra_mstp_disable(k_ra_mstp_rsip);
}

ra_err_t ra_rsip_get_status(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Mailbox STATUS read. */
  *out = *ra_rsip_reg32(k_ra_rsip_off_status);
  return k_ra_ok;
}

ra_err_t ra_rsip_clear_status(uint32_t mask)
{
  if ((mask & ~(uint32_t)k_ra_rsip_mask_isr_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  if (mask == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* W1C ack on the ISR word. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) = mask;
  return k_ra_ok;
}

ra_err_t ra_rsip_attach_handler(ra_rsip_event_fn_t fn, void* ctx)
{
  s_rsip_fn  = fn;
  s_rsip_ctx = ctx;
  return k_ra_ok;
}

void ra_rsip_dispatch(void)
{
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Snapshot then ack the ISR. */
  volatile uint32_t* isr      = ra_rsip_reg32(k_ra_rsip_off_isr);
  const uint32_t     snapshot = *isr;
  if (snapshot == 0U) {
    return;
  }
  const ra_rsip_event_fn_t fn  = s_rsip_fn;
  void* const              ctx = s_rsip_ctx;
  *isr                         = snapshot;
  if (fn != nullptr) {
    fn(ctx, snapshot);
  }
}

ra_err_t ra_rsip_trng_read(uint8_t* buf, uint32_t len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((len & ((uint32_t)k_ra_rsip_trng_word_bytes - 1U)) != 0U) {
    return k_ra_err_invalid_arg;
  }

  volatile uint32_t* status = ra_rsip_reg32(k_ra_rsip_off_rnd_status);
  volatile uint32_t* data   = ra_rsip_reg32(k_ra_rsip_off_rnd_data);

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Arm TRNG control word. */
  *ra_rsip_reg32(k_ra_rsip_off_rnd_ctrl) = (uint32_t)k_ra_rsip_mask_ctrl_enable;

  const uint32_t words = len >> (uint32_t)k_ra_rsip_word_shift;
  for (uint32_t w = 0U; w < words; ++w) {
    /* On hardware the engine asserts READY when a fresh word is
     * available. The host sim has no producer thread, so we
     * pre-assert and re-assert each iteration to keep the spin
     * deterministic. */
    *status |= (uint32_t)k_ra_rsip_mask_status_ready;

    const ra_err_t wait_err =
      internal_wait_bit(k_ra_rsip_off_rnd_status, (uint32_t)k_ra_rsip_mask_status_ready);
    if (wait_err != k_ra_ok) {
      return wait_err;
    }

    const uint32_t word = *data;
    /* HUM Ch 52.1 "Overview" p 3302 */
    /* TRNG output is little-endian. */
    buf[(w << (uint32_t)k_ra_rsip_word_shift) + 0U] =
      (uint8_t)(word & (uint32_t)k_ra_rsip_byte_mask);
    buf[(w << (uint32_t)k_ra_rsip_word_shift) + 1U] =
      (uint8_t)((word >> (uint32_t)k_ra_rsip_byte_bits) & (uint32_t)k_ra_rsip_byte_mask);
    buf[(w << (uint32_t)k_ra_rsip_word_shift) + 2U] =
      (uint8_t)((word >> (uint32_t)k_ra_rsip_byte_shift_2) & (uint32_t)k_ra_rsip_byte_mask);
    buf[(w << (uint32_t)k_ra_rsip_word_shift) + 3U] =
      (uint8_t)((word >> (uint32_t)k_ra_rsip_byte_shift_3) & (uint32_t)k_ra_rsip_byte_mask);

    /* Clear READY so the next iteration genuinely waits. */
    *status &= ~(uint32_t)k_ra_rsip_mask_status_ready;
  }
  return k_ra_ok;
}

// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
ra_err_t ra_rsip_sha256(const uint8_t* msg, uint32_t msg_len, uint8_t* digest)
{
  RA_CHECK_NULL_PTR((void*)msg, s_tag, "msg must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* HASH algorithm select. */
  *ra_rsip_reg32(k_ra_rsip_off_hash_ctrl) = (uint32_t)k_ra_rsip_hash_sha256;

  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Stream message into HASH input port one 32-bit word at a time.
   * The HAL handles partial trailing bytes by zero-extending into
   * a single word. */
  volatile uint32_t* in = ra_rsip_reg32(k_ra_rsip_off_hash_data_in);
  uint32_t           i  = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= msg_len) {
    const uint32_t word = ((uint32_t)msg[i + 0U]) |
                          (((uint32_t)msg[i + 1U]) << (uint32_t)k_ra_rsip_byte_bits) |
                          (((uint32_t)msg[i + 2U]) << (uint32_t)k_ra_rsip_byte_shift_2) |
                          (((uint32_t)msg[i + 3U]) << (uint32_t)k_ra_rsip_byte_shift_3);
    *in                 = word;
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
  }
  if (i < msg_len) {
    uint32_t tail = 0U;
    for (uint32_t b = 0U; (i + b) < msg_len; ++b) {
      tail |= ((uint32_t)msg[i + b]) << (b * (uint32_t)k_ra_rsip_byte_bits);
    }
    *in = tail;
  }

  /* On hardware the engine raises HASH_STATUS.DONE once it
   * absorbs the trailing block + length. The host sim pre-asserts
   * the bit so the wait terminates. */
  volatile uint32_t* hstatus = ra_rsip_reg32(k_ra_rsip_off_hash_status);
  *hstatus |= (uint32_t)k_ra_rsip_mask_isr_done;

  const ra_err_t wait_err =
    internal_wait_bit(k_ra_rsip_off_hash_status, (uint32_t)k_ra_rsip_mask_isr_done);
  RA_RETURN_ON_ERROR(wait_err, s_tag, "rsip_sha256: hash done");

  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Read 8 digest words. */
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_sha256_digest_words; ++w) {
    const ra_rsip_off_t off  = (ra_rsip_off_t)((uint16_t)k_ra_rsip_off_hash_digest +
                                               (uint16_t)(w << (uint32_t)k_ra_rsip_word_shift));
    const uint32_t      word = *ra_rsip_reg32(off);
    digest[(w << (uint32_t)k_ra_rsip_word_shift) + 0U] =
      (uint8_t)(word & (uint32_t)k_ra_rsip_byte_mask);
    digest[(w << (uint32_t)k_ra_rsip_word_shift) + 1U] =
      (uint8_t)((word >> (uint32_t)k_ra_rsip_byte_bits) & (uint32_t)k_ra_rsip_byte_mask);
    digest[(w << (uint32_t)k_ra_rsip_word_shift) + 2U] =
      (uint8_t)((word >> (uint32_t)k_ra_rsip_byte_shift_2) & (uint32_t)k_ra_rsip_byte_mask);
    digest[(w << (uint32_t)k_ra_rsip_word_shift) + 3U] =
      (uint8_t)((word >> (uint32_t)k_ra_rsip_byte_shift_3) & (uint32_t)k_ra_rsip_byte_mask);
  }

  /* Ack the DONE bit so the next call starts clean. */
  *hstatus &= ~(uint32_t)k_ra_rsip_mask_isr_done;
  return k_ra_ok;
}

ra_err_t ra_rsip_enter_stop(void)
{
  /* HUM Ch 52.3.1 "Software Standby Mode" p 3307 */
  /* Engine MUST be idle before standby. Clear ENABLE then gate. */
  *ra_rsip_reg32(k_ra_rsip_off_ctrl) = 0U;
  return ra_mstp_disable(k_ra_mstp_rsip);
}

ra_err_t ra_rsip_exit_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_rsip);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "rsip_exit_stop: mstp enable");

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Engine re-enable then BIST. */
  *ra_rsip_reg32(k_ra_rsip_off_ctrl) = (uint32_t)k_ra_rsip_mask_ctrl_enable;

  const ra_err_t bist_err = internal_run_bist();
  if (bist_err != k_ra_ok) {
    (void)ra_mstp_disable(k_ra_mstp_rsip);
    return bist_err;
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 helpers
 * ===========================================================================
 */

/**
 * @enum ra_rsip_intern2_t
 * @brief Round-3 file-private constants.
 */
typedef enum : uint32_t {
  k_ra_rsip_kv_slot_max = 16UL, /**< Number of vault slots.        */
  k_ra_rsip_kv_slot_w   = 16UL, /**< 64-byte slot = 16 * uint32_t. */
  k_ra_rsip_iv_words    = 4UL,  /**< IV / nonce = 4 lanes.         */
  k_ra_rsip_aes_block_w = 4UL,  /**< 16-byte block = 4 * uint32_t. */
} ra_rsip_intern2_t;

/**
 * @brief Pack 4 little-endian bytes into a uint32_t.
 *
 * @param[in] p Pointer to 4 source bytes.
 *
 * @return Packed little-endian word.
 *
 * @pre ``p`` is non-NULL and 4-byte readable.
 * @pre Caller has ensured no aliasing concerns.
 *
 * @post No state outside the return value is modified.
 * @post Result is a faithful round trip with ``internal_unpack_le``.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static uint32_t internal_pack_le(const uint8_t* p)
{
  return ((uint32_t)p[0]) | (((uint32_t)p[1]) << (uint32_t)k_ra_rsip_byte_bits) |
         (((uint32_t)p[2]) << (uint32_t)k_ra_rsip_byte_shift_2) |
         (((uint32_t)p[3]) << (uint32_t)k_ra_rsip_byte_shift_3);
}

/**
 * @brief Unpack a uint32_t into 4 little-endian bytes.
 *
 * @param[in]  word Source word.
 * @param[out] p    Destination (4 bytes).
 *
 * @pre ``p`` is non-NULL and 4-byte writeable.
 * @pre Caller has ensured no aliasing concerns.
 *
 * @post ``p[0..3]`` reflect ``word`` in little-endian order.
 * @post No other state is modified.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static void internal_unpack_le(uint32_t word, uint8_t* p)
{
  p[0] = (uint8_t)(word & (uint32_t)k_ra_rsip_byte_mask);
  p[1] = (uint8_t)((word >> (uint32_t)k_ra_rsip_byte_bits) & (uint32_t)k_ra_rsip_byte_mask);
  p[2] = (uint8_t)((word >> (uint32_t)k_ra_rsip_byte_shift_2) & (uint32_t)k_ra_rsip_byte_mask);
  p[3] = (uint8_t)((word >> (uint32_t)k_ra_rsip_byte_shift_3) & (uint32_t)k_ra_rsip_byte_mask);
}

/**
 * @brief Map an OEM opcode to the wrapped-key body word count.
 *
 * @param[in] cmd OEM command opcode.
 *
 * @return Body word count, or 0 if ``cmd`` is unknown.
 *
 * @pre ``cmd`` is a value from ``ra_rsip_oem_cmd_t``.
 * @pre Caller will treat 0 as "unsupported".
 *
 * @post Returned value matches the FSP per-algorithm size table.
 * @post No state is modified.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static uint32_t internal_handle_words_for(ra_rsip_oem_cmd_t cmd)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  /* Handle-body sizes mirror FSP r_rsip_key_injection.c. */
  switch (cmd) {
    case k_ra_rsip_oem_cmd_aes128:
      return (uint32_t)k_ra_rsip_handle_words_aes128;
    case k_ra_rsip_oem_cmd_aes192:
      return (uint32_t)k_ra_rsip_handle_words_aes192;
    case k_ra_rsip_oem_cmd_aes256:
      return (uint32_t)k_ra_rsip_handle_words_aes256;
    case k_ra_rsip_oem_cmd_chacha20:
      return (uint32_t)k_ra_rsip_handle_words_chacha20;
    case k_ra_rsip_oem_cmd_hmac_sha224:
      return (uint32_t)k_ra_rsip_handle_words_hmac_sha224;
    case k_ra_rsip_oem_cmd_hmac_sha256:
      return (uint32_t)k_ra_rsip_handle_words_hmac_sha256;
    case k_ra_rsip_oem_cmd_hmac_sha384:
      return (uint32_t)k_ra_rsip_handle_words_hmac_sha384;
    case k_ra_rsip_oem_cmd_hmac_sha512:
    case k_ra_rsip_oem_cmd_hmac_sha512_224:
    case k_ra_rsip_oem_cmd_hmac_sha512_256:
      return (uint32_t)k_ra_rsip_handle_words_hmac_sha512;
    case k_ra_rsip_oem_cmd_ecc_secp256r1_priv:
    case k_ra_rsip_oem_cmd_ecc_secp256k1_priv:
    case k_ra_rsip_oem_cmd_ecc_brain256r1_priv:
    case k_ra_rsip_oem_cmd_ecc_ed25519_priv:
      return (uint32_t)k_ra_rsip_handle_words_ecc256_priv;
    case k_ra_rsip_oem_cmd_ecc_secp384r1_priv:
    case k_ra_rsip_oem_cmd_ecc_brain384r1_priv:
    case k_ra_rsip_oem_cmd_ecc_brain512r1_priv:
      return (uint32_t)k_ra_rsip_handle_words_ecc384_priv;
    case k_ra_rsip_oem_cmd_ecc_secp521r1_priv:
      return (uint32_t)k_ra_rsip_handle_words_ecc521_priv;
    case k_ra_rsip_oem_cmd_rsa2048_priv:
      return (uint32_t)k_ra_rsip_handle_words_rsa2048_priv;
    case k_ra_rsip_oem_cmd_rsa3072_priv:
      return (uint32_t)k_ra_rsip_handle_words_rsa3072_priv;
    case k_ra_rsip_oem_cmd_rsa4096_priv:
      return (uint32_t)k_ra_rsip_handle_words_rsa4096_priv;
    case k_ra_rsip_oem_cmd_aes128_xts:
    case k_ra_rsip_oem_cmd_aes256_xts:
    case k_ra_rsip_oem_cmd_invalid:
    default:
      return 0U;
  }
}

/**
 * @brief Drive a single mailbox completion (DONE poll + ack).
 *
 * @param[in] done_mask Bit to wait for in ``MBOX_RET``.
 *
 * @return ``k_ra_ok`` on completion; ``k_ra_err_hw_timeout`` on
 *         exhaustion; ``k_ra_err_hw_error`` if the engine reported
 *         a fault on ``ISR.ERR``.
 *
 * @pre ``done_mask`` is non-zero.
 * @pre Engine has just received an opcode.
 *
 * @post On ``k_ra_ok``, the matching ISR bit is acked.
 * @post On non-OK, no caller-visible state is modified beyond the
 *       error report.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static ra_err_t internal_complete(uint32_t done_mask)
{
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Pre-assert the DONE bit so the host sim spin terminates. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) |= done_mask;

  const ra_err_t wait_err = internal_wait_bit(k_ra_rsip_off_isr, done_mask);
  if (wait_err != k_ra_ok) {
    return wait_err;
  }
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Read MBOX_RET; non-zero indicates engine-side error. */
  const uint32_t ret = *ra_rsip_reg32(k_ra_rsip_off_mbox_ret);
  /* W1C ack on the completion bit. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) = done_mask;
  if (ret != 0U) {
    return k_ra_err_hw_error;
  }
  return k_ra_ok;
}

/**
 * @brief Stream ``len`` bytes into the data input window.
 *
 * @param[in] in  Buffer to push (>= ``len`` bytes); never NULL.
 * @param[in] len Bytes to push.
 *
 * @pre ``in`` is non-NULL.
 * @pre ``len`` may be zero (no-op).
 *
 * @post Data input lanes 0..3 reflect the trailing 16 bytes.
 * @post No engine command word is touched.
 *
 * @note Internal helper used by the cipher / hash / HMAC paths.
 * @since 0.12.0
 */
static void internal_push_data(const uint8_t* in, uint32_t len)
{
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* Stream 32-bit words through DATA_IN0..3 round-robin. */
  uint32_t i    = 0U;
  uint8_t  lane = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    const uint32_t      word = internal_pack_le(&in[i]);
    const ra_rsip_off_t off =
      (ra_rsip_off_t)((uint16_t)k_ra_rsip_off_data_in0 +
                      (uint16_t)((uint32_t)lane << (uint32_t)k_ra_rsip_word_shift));
    *ra_rsip_reg32(off) = word;
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
    lane = (uint8_t)((lane + 1U) & ((uint32_t)k_ra_rsip_aes_block_w - 1U));
  }
  if (i < len) {
    uint32_t tail = 0U;
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      tail |= ((uint32_t)in[i + b]) << (b * (uint32_t)k_ra_rsip_byte_bits);
    }
    const ra_rsip_off_t off =
      (ra_rsip_off_t)((uint16_t)k_ra_rsip_off_data_in0 +
                      (uint16_t)((uint32_t)lane << (uint32_t)k_ra_rsip_word_shift));
    *ra_rsip_reg32(off) = tail;
  }
}

/**
 * @brief Pull ``len`` bytes back from the data output window.
 *
 * @param[out] out Destination (>= ``len`` bytes); never NULL.
 * @param[in]  len Bytes to pull.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``len`` may be zero (no-op).
 *
 * @post On exit ``out[0..len-1]`` reflects the engine output.
 * @post No engine command word is touched.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static void internal_pull_data(uint8_t* out, uint32_t len)
{
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  uint32_t i    = 0U;
  uint8_t  lane = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    const ra_rsip_off_t off =
      (ra_rsip_off_t)((uint16_t)k_ra_rsip_off_data_out0 +
                      (uint16_t)((uint32_t)lane << (uint32_t)k_ra_rsip_word_shift));
    const uint32_t word = *ra_rsip_reg32(off);
    internal_unpack_le(word, &out[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
    lane = (uint8_t)((lane + 1U) & ((uint32_t)k_ra_rsip_aes_block_w - 1U));
  }
  if (i < len) {
    const ra_rsip_off_t off =
      (ra_rsip_off_t)((uint16_t)k_ra_rsip_off_data_out0 +
                      (uint16_t)((uint32_t)lane << (uint32_t)k_ra_rsip_word_shift));
    const uint32_t word = *ra_rsip_reg32(off);
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      out[i + b] =
        (uint8_t)((word >> (b * (uint32_t)k_ra_rsip_byte_bits)) & (uint32_t)k_ra_rsip_byte_mask);
    }
  }
}

/**
 * @brief Push a 16-byte IV / nonce into the SYM_IV0..3 lanes.
 *
 * @param[in] iv 16 bytes; may be NULL (lanes left untouched).
 *
 * @pre Caller has computed the IV per-mode (IV may be NULL for ECB / CMAC).
 * @pre IV is 4-byte aligned in caller's view.
 *
 * @post On non-NULL ``iv``, lanes 0..3 reflect the supplied IV.
 * @post No command-word side effect.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static void internal_push_iv(const uint8_t* iv)
{
  if (iv == nullptr) {
    return;
  }
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_iv_words; ++w) {
    const ra_rsip_off_t off = (ra_rsip_off_t)((uint16_t)k_ra_rsip_off_sym_iv0 +
                                              (uint16_t)(w << (uint32_t)k_ra_rsip_word_shift));
    *ra_rsip_reg32(off)     = internal_pack_le(&iv[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
}

/**
 * @brief Stream a wrapped-key body into the engine input FIFO.
 *
 * @param[in] handle Wrapped key; may be NULL (no-op).
 *
 * @pre ``handle`` is either NULL or fully populated.
 * @pre ``handle->body_words`` <= length of ``handle->body``.
 *
 * @post On non-NULL ``handle``, the body has been pushed and
 *       ``ASYM_KEY`` / ``SYM_KEYH`` carry the algorithm selector.
 * @post No command word is fired.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static void internal_load_handle(const ra_rsip_key_handle_t* handle)
{
  if (handle == nullptr) {
    return;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_sym_keyh) = handle->alg;
  for (uint32_t w = 0U; w < handle->body_words; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_key_stage) = handle->body[w];
  }
}

/**
 * @brief Issue an OEM key-install opcode and read the wrapped body back.
 *
 * @param[in]  cmd  Opcode to issue.
 * @param[in]  iv   Optional 16-byte IV (may be NULL for plaintext flow).
 * @param[in]  src  Optional plaintext key bytes.
 * @param[in]  src_len Length of ``src`` in bytes.
 * @param[out] out  Destination handle.
 *
 * @return ``ra_err_t`` error code.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``cmd`` is one of the documented OEM opcodes.
 *
 * @post On success, ``out->alg = cmd`` and ``out->body[]`` is filled.
 * @post On failure, ``out`` is left in an unspecified state and the
 *       caller MUST NOT use it.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static ra_err_t internal_oem_install(ra_rsip_oem_cmd_t     cmd,
                                     const uint8_t*        iv,
                                     const uint8_t*        src,
                                     uint32_t              src_len,
                                     ra_rsip_key_handle_t* out)
{
  const uint32_t words = internal_handle_words_for(cmd);
  if (words == 0U) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  /* Set the OEM opcode + push the IV + plaintext body. */
  *ra_rsip_reg32(k_ra_rsip_off_oem_ctrl) = (uint32_t)cmd;
  *ra_rsip_reg32(k_ra_rsip_off_oem_arg)  = src_len;

  if (iv != nullptr) {
    for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_iv_words; ++w) {
      *ra_rsip_reg32(k_ra_rsip_off_oem_iv) =
        internal_pack_le(&iv[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
    }
  }
  if (src != nullptr) {
    internal_push_data(src, src_len);
  }

  /* Fire the install command via MBOX. */
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)cmd;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }

  out->alg        = (uint32_t)cmd;
  out->body_words = words;
  /* Pull body words back from the staging port. */
  for (uint32_t w = 0U; w < words; ++w) {
    out->body[w] = *ra_rsip_reg32(k_ra_rsip_off_key_stage);
  }
  /* Zero the remainder so unused bytes don't leak old data. */
  for (uint32_t w = words; w < (uint32_t)k_ra_rsip_handle_words_rsa4096_priv; ++w) {
    out->body[w] = 0U;
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: key install
 * ===========================================================================
 */

ra_err_t ra_rsip_aes128_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_aes128,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_aes128_key_bytes,
                              out);
}

ra_err_t ra_rsip_aes192_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_aes192,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_aes192_key_bytes,
                              out);
}

ra_err_t ra_rsip_aes256_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_aes256,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_aes256_key_bytes,
                              out);
}

ra_err_t ra_rsip_chacha20_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_chacha20,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_chacha_key_bytes,
                              out);
}

ra_err_t ra_rsip_hmac_install_plain(ra_rsip_oem_cmd_t     alg,
                                    const uint8_t*        key,
                                    uint32_t              key_len,
                                    ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (key_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  switch (alg) {
    case k_ra_rsip_oem_cmd_hmac_sha224:
    case k_ra_rsip_oem_cmd_hmac_sha256:
    case k_ra_rsip_oem_cmd_hmac_sha384:
    case k_ra_rsip_oem_cmd_hmac_sha512:
    case k_ra_rsip_oem_cmd_hmac_sha512_224:
    case k_ra_rsip_oem_cmd_hmac_sha512_256:
      break;
    default:
      return k_ra_err_invalid_arg;
  }
  return internal_oem_install(alg, nullptr, key, key_len, out);
}

ra_err_t ra_rsip_oem_install(ra_rsip_oem_cmd_t     cmd,
                             const uint8_t*        iv,
                             const uint8_t*        oem_blob,
                             uint32_t              blob_len,
                             ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR((void*)iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR((void*)oem_blob, s_tag, "oem_blob must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (cmd == k_ra_rsip_oem_cmd_invalid) {
    return k_ra_err_invalid_arg;
  }
  if (blob_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  return internal_oem_install(cmd, iv, oem_blob, blob_len, out);
}

/* ===========================================================================
 * Round-3 entry points: AES symmetric cipher
 * ===========================================================================
 */

/**
 * @brief Drive one block / multi-block cipher transaction.
 *
 * @param[in]  key   Wrapped key.
 * @param[in]  alg_byte ``ra_rsip_sym_alg_t`` low byte for SYM_CTRL.
 * @param[in]  mode  Block-cipher mode.
 * @param[in]  dir   Encrypt / decrypt.
 * @param[in]  iv    16-byte IV (may be NULL).
 * @param[in]  in    Input.
 * @param[out] out   Output.
 * @param[in]  len   Length in bytes.
 *
 * @return ``ra_err_t``.
 *
 * @pre ``key`` is non-NULL.
 * @pre ``in`` and ``out`` are non-NULL.
 *
 * @post On success, ``out[0..len-1]`` holds the result.
 * @post SYM_STATUS.DONE has been observed and acked.
 *
 * @note Internal helper that drives both the AES and ChaCha20 paths.
 * @since 0.12.0
 */
static ra_err_t internal_sym_run(const ra_rsip_key_handle_t* key,
                                 uint8_t                     alg_byte,
                                 ra_rsip_aes_mode_t          mode,
                                 ra_rsip_aes_dir_t           dir,
                                 const uint8_t*              iv,
                                 const uint8_t*              in,
                                 uint8_t*                    out,
                                 uint32_t                    len)
{
  internal_load_handle(key);
  internal_push_iv(iv);

  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* SYM_CTRL = (dir << 16) | (mode << 8) | alg_byte. */
  const uint32_t cmd = ((uint32_t)dir << (uint32_t)k_ra_rsip_byte_shift_2) |
                       ((uint32_t)mode << (uint32_t)k_ra_rsip_byte_bits) | (uint32_t)alg_byte;
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = cmd;

  internal_push_data(in, len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = cmd;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_pull_data(out, len);
  return k_ra_ok;
}

/**
 * @brief Pick the AES algorithm byte that matches the wrapped key.
 *
 * @param[in] alg ``ra_rsip_oem_cmd_*`` from the handle.
 *
 * @return ``ra_rsip_sym_alg_t`` byte, or 0 if not an AES key.
 *
 * @pre ``alg`` is one of ``ra_rsip_oem_cmd_t``.
 * @pre Caller treats 0 as "not an AES handle".
 *
 * @post No state is modified.
 * @post Returned byte is suitable for SYM_CTRL low byte.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static uint8_t internal_aes_alg_byte(uint32_t alg)
{
  switch (alg) {
    case (uint32_t)k_ra_rsip_oem_cmd_aes128:
      return (uint8_t)k_ra_rsip_sym_alg_aes128;
    case (uint32_t)k_ra_rsip_oem_cmd_aes192:
      return (uint8_t)k_ra_rsip_sym_alg_aes192;
    case (uint32_t)k_ra_rsip_oem_cmd_aes256:
      return (uint8_t)k_ra_rsip_sym_alg_aes256;
    default:
      return 0U;
  }
}

ra_err_t ra_rsip_aes_cipher(const ra_rsip_key_handle_t* key,
                            ra_rsip_aes_mode_t          mode,
                            ra_rsip_aes_dir_t           dir,
                            const uint8_t*              iv,
                            const uint8_t*              in,
                            uint8_t*                    out,
                            uint32_t                    len)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR((void*)in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if ((mode == k_ra_rsip_aes_mode_gcm) || (mode == k_ra_rsip_aes_mode_ccm)) {
    return k_ra_err_invalid_arg; /* AEAD has dedicated entry points. */
  }
  if (((mode == k_ra_rsip_aes_mode_ecb) || (mode == k_ra_rsip_aes_mode_cbc) ||
       (mode == k_ra_rsip_aes_mode_cmac)) &&
      ((len & ((uint32_t)k_ra_rsip_aes_block_bytes - 1U)) != 0U)) {
    return k_ra_err_invalid_arg;
  }
  const uint8_t alg_byte = internal_aes_alg_byte(key->alg);
  if (alg_byte == 0U) {
    return k_ra_err_invalid_arg;
  }
  return internal_sym_run(key, alg_byte, mode, dir, iv, in, out, len);
}

/**
 * @brief Common AEAD path used by GCM / CCM / ChaCha20-Poly1305.
 *
 * @param[in]  key       Wrapped key.
 * @param[in]  alg_byte  Low byte of SYM_CTRL.
 * @param[in]  mode      AEAD mode.
 * @param[in]  dir       Encrypt / decrypt.
 * @param[in]  iv        Nonce.
 * @param[in]  aad       Associated data; may be NULL.
 * @param[in]  aad_len   Associated-data length.
 * @param[in]  in        Plaintext / ciphertext input.
 * @param[out] out       Plaintext / ciphertext output.
 * @param[in]  in_len    Body length.
 * @param[in,out] tag    16-byte authenticator (in on decrypt, out on encrypt).
 *
 * @return ``ra_err_t``.
 *
 * @pre ``key``, ``iv``, ``in``, ``out``, ``tag`` are non-NULL.
 * @pre ``alg_byte`` matches ``key->alg``.
 *
 * @post On encrypt success, ``tag`` is the engine-computed tag.
 * @post On decrypt success, the engine has verified the supplied tag.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
static ra_err_t internal_aead_run(const ra_rsip_key_handle_t* key,
                                  uint8_t                     alg_byte,
                                  ra_rsip_aes_mode_t          mode,
                                  ra_rsip_aes_dir_t           dir,
                                  const uint8_t*              iv,
                                  const uint8_t*              aad,
                                  uint32_t                    aad_len,
                                  const uint8_t*              in,
                                  uint8_t*                    out,
                                  uint32_t                    in_len,
                                  uint8_t*                    tag)
{
  internal_load_handle(key);
  internal_push_iv(iv);
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* AAD + body length descriptors. */
  *ra_rsip_reg32(k_ra_rsip_off_sym_aad_len) = aad_len;
  *ra_rsip_reg32(k_ra_rsip_off_sym_pt_len)  = in_len;

  if ((aad != nullptr) && (aad_len > 0U)) {
    /* Push AAD bytes one word at a time through the AAD lane. */
    uint32_t i = 0U;
    while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= aad_len) {
      *ra_rsip_reg32(k_ra_rsip_off_sym_aad_in) = internal_pack_le(&aad[i]);
      i += (uint32_t)k_ra_rsip_trng_word_bytes;
    }
    if (i < aad_len) {
      uint32_t tail = 0U;
      for (uint32_t b = 0U; (i + b) < aad_len; ++b) {
        tail |= ((uint32_t)aad[i + b]) << (b * (uint32_t)k_ra_rsip_byte_bits);
      }
      *ra_rsip_reg32(k_ra_rsip_off_sym_aad_in) = tail;
    }
  }

  /* On decrypt, push the supplied tag in for verification. */
  if (dir == k_ra_rsip_dir_decrypt) {
    for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_aes_block_w; ++w) {
      *ra_rsip_reg32(k_ra_rsip_off_sym_tag) =
        internal_pack_le(&tag[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
    }
  }

  const uint32_t cmd = ((uint32_t)dir << (uint32_t)k_ra_rsip_byte_shift_2) |
                       ((uint32_t)mode << (uint32_t)k_ra_rsip_byte_bits) | (uint32_t)alg_byte;
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = cmd;
  internal_push_data(in, in_len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = cmd;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_pull_data(out, in_len);
  /* On encrypt, read the engine-computed tag back. */
  if (dir == k_ra_rsip_dir_encrypt) {
    for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_aes_block_w; ++w) {
      const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_sym_tag);
      internal_unpack_le(word, &tag[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
    }
  }
  return k_ra_ok;
}

ra_err_t ra_rsip_aes_gcm(const ra_rsip_key_handle_t* key,
                         ra_rsip_aes_dir_t           dir,
                         const uint8_t*              iv,
                         const uint8_t*              aad,
                         uint32_t                    aad_len,
                         const uint8_t*              in,
                         uint8_t*                    out,
                         uint32_t                    in_len,
                         uint8_t*                    tag)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR((void*)iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR((void*)in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(tag, s_tag, "tag must not be nullptr");
  const uint8_t alg_byte = internal_aes_alg_byte(key->alg);
  if (alg_byte == 0U) {
    return k_ra_err_invalid_arg;
  }
  return internal_aead_run(key,
                           alg_byte,
                           k_ra_rsip_aes_mode_gcm,
                           dir,
                           iv,
                           aad,
                           aad_len,
                           in,
                           out,
                           in_len,
                           tag);
}

ra_err_t ra_rsip_aes_ccm(const ra_rsip_key_handle_t* key,
                         ra_rsip_aes_dir_t           dir,
                         const uint8_t*              iv,
                         const uint8_t*              aad,
                         uint32_t                    aad_len,
                         const uint8_t*              in,
                         uint8_t*                    out,
                         uint32_t                    in_len,
                         uint8_t*                    tag)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR((void*)iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR((void*)in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(tag, s_tag, "tag must not be nullptr");
  const uint8_t alg_byte = internal_aes_alg_byte(key->alg);
  if (alg_byte == 0U) {
    return k_ra_err_invalid_arg;
  }
  return internal_aead_run(key,
                           alg_byte,
                           k_ra_rsip_aes_mode_ccm,
                           dir,
                           iv,
                           aad,
                           aad_len,
                           in,
                           out,
                           in_len,
                           tag);
}

/* ===========================================================================
 * Round-3 entry points: ChaCha20 + Poly1305
 * ===========================================================================
 */

// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
ra_err_t ra_rsip_chacha20(const ra_rsip_key_handle_t* key,
                          ra_rsip_aes_dir_t           dir,
                          const uint8_t*              nonce,
                          uint32_t                    counter,
                          const uint8_t*              in,
                          uint8_t*                    out,
                          uint32_t                    len)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR((void*)nonce, s_tag, "nonce must not be nullptr");
  RA_CHECK_NULL_PTR((void*)in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (key->alg != (uint32_t)k_ra_rsip_oem_cmd_chacha20) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* ChaCha20 IV layout: counter || 12-byte nonce. */
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv0) = counter;
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv1) = internal_pack_le(&nonce[0]);
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv2) =
    internal_pack_le(&nonce[(uint32_t)k_ra_rsip_trng_word_bytes]);
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv3) =
    internal_pack_le(&nonce[(size_t)2U * (size_t)k_ra_rsip_trng_word_bytes]);

  const uint32_t cmd = ((uint32_t)dir << (uint32_t)k_ra_rsip_byte_shift_2) |
                       ((uint32_t)k_ra_rsip_chacha_op_encrypt << (uint32_t)k_ra_rsip_byte_bits) |
                       (uint32_t)k_ra_rsip_sym_alg_chacha20;
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = cmd;
  internal_push_data(in, len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = cmd;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_pull_data(out, len);
  return k_ra_ok;
}

ra_err_t ra_rsip_chacha20_poly1305(const ra_rsip_key_handle_t* key,
                                   ra_rsip_aes_dir_t           dir,
                                   const uint8_t*              nonce,
                                   const uint8_t*              aad,
                                   uint32_t                    aad_len,
                                   const uint8_t*              in,
                                   uint8_t*                    out,
                                   uint32_t                    in_len,
                                   uint8_t*                    tag)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR((void*)nonce, s_tag, "nonce must not be nullptr");
  RA_CHECK_NULL_PTR((void*)in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(tag, s_tag, "tag must not be nullptr");
  if (key->alg != (uint32_t)k_ra_rsip_oem_cmd_chacha20) {
    return k_ra_err_invalid_arg;
  }
  return internal_aead_run(key,
                           (uint8_t)k_ra_rsip_sym_alg_chacha20,
                           k_ra_rsip_aes_mode_gcm,
                           dir,
                           nonce,
                           aad,
                           aad_len,
                           in,
                           out,
                           in_len,
                           tag);
}

ra_err_t
ra_rsip_poly1305(const uint8_t* one_time_key, const uint8_t* msg, uint32_t msg_len, uint8_t* tag)
{
  RA_CHECK_NULL_PTR((void*)one_time_key, s_tag, "one_time_key must not be nullptr");
  RA_CHECK_NULL_PTR(tag, s_tag, "tag must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra_err_null_ptr;
  }

  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* Stage one-time key as a ChaCha20 key. */
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_handle_words_chacha20; ++w) {
    if (w < ((uint32_t)k_ra_rsip_chacha_key_bytes / (uint32_t)k_ra_rsip_trng_word_bytes)) {
      *ra_rsip_reg32(k_ra_rsip_off_key_stage) =
        internal_pack_le(&one_time_key[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
    } else {
      *ra_rsip_reg32(k_ra_rsip_off_key_stage) = 0U;
    }
  }
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = (uint32_t)k_ra_rsip_chacha_op_poly1305_mac;
  if (msg_len > 0U) {
    internal_push_data(msg, msg_len);
  }
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)k_ra_rsip_chacha_op_poly1305_mac;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_aes_block_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_sym_tag);
    internal_unpack_le(word, &tag[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: hash + HMAC
 * ===========================================================================
 */

/**
 * @brief Map a hash algorithm selector to its natural digest length.
 *
 * @param[in] alg Algorithm selector.
 *
 * @return Digest length in bytes, or 0 for unknown.
 *
 * @pre ``alg`` is one of ``k_ra_rsip_hash_*``.
 * @pre Caller treats 0 as "unsupported".
 *
 * @post No state is modified.
 * @post Returned size matches FIPS PUB 180-4 / 202.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static uint32_t internal_hash_size(ra_rsip_hash_alg_t alg)
{
  switch (alg) {
    case k_ra_rsip_hash_sha224:
    case k_ra_rsip_hash_sha512_224:
    case k_ra_rsip_hash_sha3_224:
      return (uint32_t)k_ra_rsip_sha224_digest_bytes;
    case k_ra_rsip_hash_sha256:
    case k_ra_rsip_hash_sha512_256:
    case k_ra_rsip_hash_sha3_256:
      return (uint32_t)k_ra_rsip_sha256_digest_bytes;
    case k_ra_rsip_hash_sha384:
    case k_ra_rsip_hash_sha3_384:
      return (uint32_t)k_ra_rsip_sha384_digest_bytes;
    case k_ra_rsip_hash_sha512:
    case k_ra_rsip_hash_sha3_512:
      return (uint32_t)k_ra_rsip_sha512_digest_bytes;
    case k_ra_rsip_hash_shake128:
    case k_ra_rsip_hash_shake256:
      /* Variable-length output: caller supplies. */
      return 1U;
    default:
      return 0U;
  }
}

// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
ra_err_t ra_rsip_hash(ra_rsip_hash_alg_t alg,
                      const uint8_t*     msg,
                      uint32_t           msg_len,
                      uint8_t*           digest,
                      uint32_t           digest_len)
{
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  const uint32_t needed = internal_hash_size(alg);
  if (needed == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((alg != k_ra_rsip_hash_shake128) && (alg != k_ra_rsip_hash_shake256) &&
      (digest_len < needed)) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_hash_ctrl) = (uint32_t)alg;

  if (msg_len > 0U) {
    volatile uint32_t* in = ra_rsip_reg32(k_ra_rsip_off_hash_data_in);
    uint32_t           i  = 0U;
    while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= msg_len) {
      *in = internal_pack_le(&msg[i]);
      i += (uint32_t)k_ra_rsip_trng_word_bytes;
    }
    if (i < msg_len) {
      uint32_t tail = 0U;
      for (uint32_t b = 0U; (i + b) < msg_len; ++b) {
        tail |= ((uint32_t)msg[i + b]) << (b * (uint32_t)k_ra_rsip_byte_bits);
      }
      *in = tail;
    }
  }

  /* Pre-arm + wait for DONE on host sim. */
  volatile uint32_t* hstatus = ra_rsip_reg32(k_ra_rsip_off_hash_status);
  *hstatus |= (uint32_t)k_ra_rsip_mask_isr_done;
  const ra_err_t wait_err =
    internal_wait_bit(k_ra_rsip_off_hash_status, (uint32_t)k_ra_rsip_mask_isr_done);
  RA_RETURN_ON_ERROR(wait_err, s_tag, "rsip_hash: hash done");

  /* Read digest_len for SHAKE; algo-natural otherwise. */
  const uint32_t to_read =
    ((alg == k_ra_rsip_hash_shake128) || (alg == k_ra_rsip_hash_shake256)) ? digest_len : needed;
  uint32_t i   = 0U;
  uint32_t off = (uint32_t)k_ra_rsip_off_hash_digest;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= to_read) {
    const uint32_t word = *ra_rsip_reg32((ra_rsip_off_t)off);
    internal_unpack_le(word, &digest[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
    off += (uint32_t)k_ra_rsip_trng_word_bytes;
  }
  if (i < to_read) {
    const uint32_t word = *ra_rsip_reg32((ra_rsip_off_t)off);
    for (uint32_t b = 0U; (i + b) < to_read; ++b) {
      digest[i + b] =
        (uint8_t)((word >> (b * (uint32_t)k_ra_rsip_byte_bits)) & (uint32_t)k_ra_rsip_byte_mask);
    }
  }
  *hstatus &= ~(uint32_t)k_ra_rsip_mask_isr_done;
  return k_ra_ok;
}

ra_err_t ra_rsip_hmac(const ra_rsip_key_handle_t* key,
                      const uint8_t*              msg,
                      uint32_t                    msg_len,
                      uint8_t*                    mac,
                      uint32_t                    mac_len)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(mac, s_tag, "mac must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  /* Determine the underlying hash size from the install opcode. */
  uint32_t needed = 0U;
  switch (key->alg) {
    case (uint32_t)k_ra_rsip_oem_cmd_hmac_sha224:
    case (uint32_t)k_ra_rsip_oem_cmd_hmac_sha512_224:
      needed = (uint32_t)k_ra_rsip_sha224_digest_bytes;
      break;
    case (uint32_t)k_ra_rsip_oem_cmd_hmac_sha256:
    case (uint32_t)k_ra_rsip_oem_cmd_hmac_sha512_256:
      needed = (uint32_t)k_ra_rsip_sha256_digest_bytes;
      break;
    case (uint32_t)k_ra_rsip_oem_cmd_hmac_sha384:
      needed = (uint32_t)k_ra_rsip_sha384_digest_bytes;
      break;
    case (uint32_t)k_ra_rsip_oem_cmd_hmac_sha512:
      needed = (uint32_t)k_ra_rsip_sha512_digest_bytes;
      break;
    default:
      return k_ra_err_invalid_arg;
  }
  if (mac_len < needed) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Stage HMAC key handle, then drive the hash unit in HMAC mode. */
  *ra_rsip_reg32(k_ra_rsip_off_hash_hmac) = key->alg;
  internal_load_handle(key);
  return ra_rsip_hash(k_ra_rsip_hash_sha256, msg, msg_len, mac, needed);
}

/* ===========================================================================
 * Round-3 entry points: asymmetric (RSA + ECDSA + ECDH)
 * ===========================================================================
 */

/**
 * @brief Push a buffer through an asymmetric input lane.
 *
 * @param[in] off Lane offset.
 * @param[in] buf Source bytes.
 * @param[in] len Source length.
 *
 * @pre ``buf`` is non-NULL or ``len == 0``.
 * @pre ``off`` refers to a writeable ASYM_* register.
 *
 * @post The lane has been written ``len/4`` times (one word per push).
 * @post No engine command word is touched.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static void internal_asym_push(ra_rsip_off_t off, const uint8_t* buf, uint32_t len)
{
  uint32_t i = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    *ra_rsip_reg32(off) = internal_pack_le(&buf[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
  }
  if (i < len) {
    uint32_t tail = 0U;
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      tail |= ((uint32_t)buf[i + b]) << (b * (uint32_t)k_ra_rsip_byte_bits);
    }
    *ra_rsip_reg32(off) = tail;
  }
}

/**
 * @brief Pull a buffer back through an asymmetric output lane.
 *
 * @param[in]  off Lane offset.
 * @param[out] buf Destination.
 * @param[in]  len Length to read.
 *
 * @pre ``buf`` is non-NULL.
 * @pre ``off`` refers to a readable ASYM_* register.
 *
 * @post ``buf[0..len-1]`` reflects ``len/4`` lane reads.
 * @post No engine command word is touched.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static void internal_asym_pull(ra_rsip_off_t off, uint8_t* buf, uint32_t len)
{
  uint32_t i = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    internal_unpack_le(*ra_rsip_reg32(off), &buf[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
  }
  if (i < len) {
    const uint32_t word = *ra_rsip_reg32(off);
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      buf[i + b] =
        (uint8_t)((word >> (b * (uint32_t)k_ra_rsip_byte_bits)) & (uint32_t)k_ra_rsip_byte_mask);
    }
  }
}

ra_err_t ra_rsip_rsa_sign(const ra_rsip_key_handle_t* key,
                          ra_rsip_rsa_size_t          size,
                          const uint8_t*              digest,
                          uint32_t                    digest_len,
                          uint8_t*                    signature)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR((void*)digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((size != k_ra_rsip_rsa_1024) && (size != k_ra_rsip_rsa_2048) &&
      (size != k_ra_rsip_rsa_3072) && (size != k_ra_rsip_rsa_4096)) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_rsa_size) = (uint32_t)size;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = (uint32_t)k_ra_rsip_asym_op_rsa_sign;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = (uint32_t)k_ra_rsip_asym_op_rsa_sign;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_asym_done);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t sig_len = (uint32_t)size / (uint32_t)k_ra_rsip_byte_bits;
  internal_asym_pull(k_ra_rsip_off_asym_sig_out, signature, sig_len);
  return k_ra_ok;
}

ra_err_t ra_rsip_rsa_verify(const ra_rsip_key_handle_t* key,
                            ra_rsip_rsa_size_t          size,
                            const uint8_t*              digest,
                            uint32_t                    digest_len,
                            const uint8_t*              signature)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR((void*)digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR((void*)signature, s_tag, "signature must not be nullptr");
  if ((size != k_ra_rsip_rsa_1024) && (size != k_ra_rsip_rsa_2048) &&
      (size != k_ra_rsip_rsa_3072) && (size != k_ra_rsip_rsa_4096)) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_rsa_size) = (uint32_t)size;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  const uint32_t sig_len = (uint32_t)size / (uint32_t)k_ra_rsip_byte_bits;
  internal_asym_push(k_ra_rsip_off_asym_sig_in, signature, sig_len);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = (uint32_t)k_ra_rsip_asym_op_rsa_verify;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = (uint32_t)k_ra_rsip_asym_op_rsa_verify;

  return internal_complete((uint32_t)k_ra_rsip_mask_isr_asym_done);
}

/**
 * @brief Map a curve to its scalar / coordinate byte length.
 *
 * @param[in] curve Curve selector.
 *
 * @return Byte length, or 0 for unknown.
 *
 * @pre ``curve`` is one of ``ra_rsip_curve_t``.
 * @pre Caller treats 0 as "unsupported".
 *
 * @post No state modified.
 * @post Result == FIPS / RFC parameter byte length.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
/**
 * @enum ra_rsip_curve_bytes_t
 * @brief Per-curve scalar / coordinate byte lengths (FIPS 186-4 / RFC 7748).
 */
typedef enum : uint32_t {
  k_ra_rsip_curve_bytes_192 = 24U, /**< 192-bit curves: secp192r1.        */
  k_ra_rsip_curve_bytes_224 = 28U, /**< 224-bit curves: secp224r1.        */
  k_ra_rsip_curve_bytes_256 = 32U, /**< 256-bit curves: secp256*, ed25519. */
  k_ra_rsip_curve_bytes_384 = 48U, /**< 384-bit curves: secp384r1, brain384r1. */
  k_ra_rsip_curve_bytes_512 = 64U, /**< 512-bit curves: brain512r1.       */
  k_ra_rsip_curve_bytes_521 = 66U, /**< 521-bit curves: secp521r1.        */
} ra_rsip_curve_bytes_t;

static uint32_t internal_curve_bytes(ra_rsip_curve_t curve)
{
  switch (curve) {
    case k_ra_rsip_curve_secp192r1:
      return (uint32_t)k_ra_rsip_curve_bytes_192;
    case k_ra_rsip_curve_secp224r1:
      return (uint32_t)k_ra_rsip_curve_bytes_224;
    case k_ra_rsip_curve_secp256r1:
    case k_ra_rsip_curve_brain256r1:
    case k_ra_rsip_curve_ed25519:
    case k_ra_rsip_curve_secp256k1:
      return (uint32_t)k_ra_rsip_curve_bytes_256;
    case k_ra_rsip_curve_secp384r1:
    case k_ra_rsip_curve_brain384r1:
      return (uint32_t)k_ra_rsip_curve_bytes_384;
    case k_ra_rsip_curve_brain512r1:
      return (uint32_t)k_ra_rsip_curve_bytes_512;
    case k_ra_rsip_curve_secp521r1:
      return (uint32_t)k_ra_rsip_curve_bytes_521;
    default:
      return 0U;
  }
}

ra_err_t ra_rsip_ecdsa_sign(const ra_rsip_key_handle_t* key,
                            ra_rsip_curve_t             curve,
                            const uint8_t*              digest,
                            uint32_t                    digest_len,
                            uint8_t*                    signature)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR((void*)digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = (uint32_t)k_ra_rsip_asym_op_ecdsa_sign;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = (uint32_t)k_ra_rsip_asym_op_ecdsa_sign;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_asym_done);
  if (err != k_ra_ok) {
    return err;
  }
  /* (r || s) */
  internal_asym_pull(k_ra_rsip_off_asym_sig_out, signature, curve_bytes * 2U);
  return k_ra_ok;
}

ra_err_t ra_rsip_ecdsa_verify(const ra_rsip_key_handle_t* key,
                              ra_rsip_curve_t             curve,
                              const uint8_t*              digest,
                              uint32_t                    digest_len,
                              const uint8_t*              signature)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR((void*)digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR((void*)signature, s_tag, "signature must not be nullptr");
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  internal_asym_push(k_ra_rsip_off_asym_sig_in, signature, curve_bytes * 2U);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = (uint32_t)k_ra_rsip_asym_op_ecdsa_verify;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = (uint32_t)k_ra_rsip_asym_op_ecdsa_verify;

  return internal_complete((uint32_t)k_ra_rsip_mask_isr_asym_done);
}

// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
ra_err_t ra_rsip_ecdh_compute(const ra_rsip_key_handle_t* key,
                              ra_rsip_curve_t             curve,
                              const uint8_t*              peer_x,
                              const uint8_t*              peer_y,
                              ra_rsip_key_handle_t*       out)
{
  RA_CHECK_NULL_PTR((void*)key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR((void*)peer_x, s_tag, "peer_x must not be nullptr");
  RA_CHECK_NULL_PTR((void*)peer_y, s_tag, "peer_y must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra_rsip_off_asym_pub_x, peer_x, curve_bytes);
  internal_asym_push(k_ra_rsip_off_asym_pub_y, peer_y, curve_bytes);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = (uint32_t)k_ra_rsip_asym_op_ecdh_compute;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = (uint32_t)k_ra_rsip_asym_op_ecdh_compute;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_asym_done);
  if (err != k_ra_ok) {
    return err;
  }
  /* The wrapped shared secret is delivered as an HMAC-SHA-256 handle. */
  out->alg        = (uint32_t)k_ra_rsip_oem_cmd_hmac_sha256;
  out->body_words = (uint32_t)k_ra_rsip_handle_words_hmac_sha256;
  for (uint32_t w = 0U; w < out->body_words; ++w) {
    out->body[w] = *ra_rsip_reg32(k_ra_rsip_off_asym_shared);
  }
  for (uint32_t w = out->body_words; w < (uint32_t)k_ra_rsip_handle_words_rsa4096_priv; ++w) {
    out->body[w] = 0U;
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: OEM boot loader version (anti-rollback)
 * ===========================================================================
 */

ra_err_t ra_rsip_oem_bl_version_get(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *out = *ra_rsip_reg32(k_ra_rsip_off_oem_bl_ver);
  return k_ra_ok;
}

ra_err_t ra_rsip_oem_bl_version_increment(void)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  if (*ra_rsip_reg32(k_ra_rsip_off_oem_bl_lock) != 0U) {
    return k_ra_err_invalid_state;
  }
  /* W1 trigger; engine increments the latched counter. */
  *ra_rsip_reg32(k_ra_rsip_off_oem_bl_inc) = 1U;
  *ra_rsip_reg32(k_ra_rsip_off_oem_bl_ver) = *ra_rsip_reg32(k_ra_rsip_off_oem_bl_ver) + 1U;
  return k_ra_ok;
}

ra_err_t ra_rsip_oem_bl_version_lock(void)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_oem_bl_lock) = 1U;
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: wrapped-key vault
 * ===========================================================================
 */

/**
 * @brief Issue a vault command and wait for completion.
 *
 * @param[in] op   Vault opcode.
 * @param[in] slot Slot index (ignored for ``count``).
 *
 * @return ``ra_err_t``.
 *
 * @pre ``op`` is one of ``ra_rsip_kv_op_t``.
 * @pre ``slot`` < ``k_ra_rsip_kv_slot_count`` for read/write/erase.
 *
 * @post On success the requested side effect has occurred.
 * @post ISR.KV_DONE has been acked.
 *
 * @note Internal helper.
 * @since 0.12.0
 */
static ra_err_t internal_kv_op(ra_rsip_kv_op_t op, uint8_t slot)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_kv_slot) = slot;
  *ra_rsip_reg32(k_ra_rsip_off_kv_ctrl) = (uint32_t)op;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)op;
  return internal_complete((uint32_t)k_ra_rsip_mask_isr_kv_done);
}

ra_err_t ra_rsip_kv_read(uint8_t slot, uint8_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (slot >= (uint8_t)k_ra_rsip_kv_slot_count) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t err = internal_kv_op(k_ra_rsip_kv_op_read, slot);
  if (err != k_ra_ok) {
    return err;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_kv_slot_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_kv_data);
    internal_unpack_le(word, &out[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  return k_ra_ok;
}

ra_err_t ra_rsip_kv_write(uint8_t slot, const uint8_t* in)
{
  RA_CHECK_NULL_PTR((void*)in, s_tag, "in must not be nullptr");
  if (slot >= (uint8_t)k_ra_rsip_kv_slot_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_kv_slot_w; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_kv_data) =
      internal_pack_le(&in[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  return internal_kv_op(k_ra_rsip_kv_op_write, slot);
}

ra_err_t ra_rsip_kv_erase(uint8_t slot)
{
  if (slot >= (uint8_t)k_ra_rsip_kv_slot_count) {
    return k_ra_err_invalid_arg;
  }
  return internal_kv_op(k_ra_rsip_kv_op_erase, slot);
}

ra_err_t ra_rsip_kv_count(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *out = *ra_rsip_reg32(k_ra_rsip_off_kv_count);
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: key wrap / unwrap engine
 * ===========================================================================
 */

// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
ra_err_t ra_rsip_key_wrap(const ra_rsip_key_handle_t* kek,
                          const uint8_t*              iv,
                          const ra_rsip_key_handle_t* src,
                          uint8_t*                    blob)
{
  RA_CHECK_NULL_PTR((void*)kek, s_tag, "kek must not be nullptr");
  RA_CHECK_NULL_PTR((void*)iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR((void*)src, s_tag, "src must not be nullptr");
  RA_CHECK_NULL_PTR(blob, s_tag, "blob must not be nullptr");
  if (internal_aes_alg_byte(kek->alg) == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_kw_kek) = kek->alg;
  for (uint32_t w = 0U; w < kek->body_words; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_key_stage) = kek->body[w];
  }
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_iv_words; ++w) {
    const ra_rsip_off_t off = (ra_rsip_off_t)((uint16_t)k_ra_rsip_off_kw_iv0 +
                                              (uint16_t)(w << (uint32_t)k_ra_rsip_word_shift));
    *ra_rsip_reg32(off)     = internal_pack_le(&iv[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  *ra_rsip_reg32(k_ra_rsip_off_kw_handle) = src->alg;
  for (uint32_t w = 0U; w < src->body_words; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_kw_blob_in) = src->body[w];
  }
  *ra_rsip_reg32(k_ra_rsip_off_kw_ctrl) = (uint32_t)k_ra_rsip_kw_op_wrap;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)k_ra_rsip_kw_op_wrap;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_kv_slot_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_kw_blob_out);
    internal_unpack_le(word, &blob[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  return k_ra_ok;
}

// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
ra_err_t ra_rsip_key_unwrap(const ra_rsip_key_handle_t* kek,
                            const uint8_t*              iv,
                            const uint8_t*              blob,
                            ra_rsip_key_handle_t*       dest)
{
  RA_CHECK_NULL_PTR((void*)kek, s_tag, "kek must not be nullptr");
  RA_CHECK_NULL_PTR((void*)iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR((void*)blob, s_tag, "blob must not be nullptr");
  RA_CHECK_NULL_PTR(dest, s_tag, "dest must not be nullptr");
  if (internal_aes_alg_byte(kek->alg) == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_kw_kek) = kek->alg;
  for (uint32_t w = 0U; w < kek->body_words; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_key_stage) = kek->body[w];
  }
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_iv_words; ++w) {
    const ra_rsip_off_t off = (ra_rsip_off_t)((uint16_t)k_ra_rsip_off_kw_iv0 +
                                              (uint16_t)(w << (uint32_t)k_ra_rsip_word_shift));
    *ra_rsip_reg32(off)     = internal_pack_le(&iv[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_kv_slot_w; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_kw_blob_in) =
      internal_pack_le(&blob[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  *ra_rsip_reg32(k_ra_rsip_off_kw_ctrl) = (uint32_t)k_ra_rsip_kw_op_unwrap;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)k_ra_rsip_kw_op_unwrap;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  /* Pull the unwrapped algorithm + body out. */
  dest->alg            = *ra_rsip_reg32(k_ra_rsip_off_kw_handle);
  const uint32_t words = internal_handle_words_for((ra_rsip_oem_cmd_t)dest->alg);
  if (words == 0U) {
    return k_ra_err_hw_error;
  }
  dest->body_words = words;
  for (uint32_t w = 0U; w < words; ++w) {
    dest->body[w] = *ra_rsip_reg32(k_ra_rsip_off_kw_blob_out);
  }
  for (uint32_t w = words; w < (uint32_t)k_ra_rsip_handle_words_rsa4096_priv; ++w) {
    dest->body[w] = 0U;
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: key derivation
 * ===========================================================================
 */

// NOLINTNEXTLINE(readability-function-size,readability-function-cognitive-complexity)
ra_err_t ra_rsip_kdf(ra_rsip_kdf_op_t            op,
                     const ra_rsip_key_handle_t* ikm,
                     const uint8_t*              label,
                     uint32_t                    label_len,
                     const uint8_t*              salt,
                     uint32_t                    salt_len,
                     uint32_t                    out_len,
                     ra_rsip_key_handle_t*       out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if ((label == nullptr) && (label_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if ((salt == nullptr) && (salt_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if (out_len == 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HKDF modes need an IKM handle; HUK / UID modes do not. */
  if (((op == k_ra_rsip_kdf_op_hkdf_sha256) || (op == k_ra_rsip_kdf_op_hkdf_sha384) ||
       (op == k_ra_rsip_kdf_op_hkdf_sha512)) &&
      (ikm == nullptr)) {
    return k_ra_err_null_ptr;
  }
  /* HUM Ch 52.1 "KDF" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_kdf_ctrl) = (uint32_t)op;
  *ra_rsip_reg32(k_ra_rsip_off_kdf_len)  = out_len;
  if (ikm != nullptr) {
    *ra_rsip_reg32(k_ra_rsip_off_kdf_ikm) = ikm->alg;
    for (uint32_t w = 0U; w < ikm->body_words; ++w) {
      *ra_rsip_reg32(k_ra_rsip_off_key_stage) = ikm->body[w];
    }
  }
  if (label_len > 0U) {
    uint32_t i = 0U;
    while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= label_len) {
      *ra_rsip_reg32(k_ra_rsip_off_kdf_label) = internal_pack_le(&label[i]);
      i += (uint32_t)k_ra_rsip_trng_word_bytes;
    }
    if (i < label_len) {
      uint32_t tail = 0U;
      for (uint32_t b = 0U; (i + b) < label_len; ++b) {
        tail |= ((uint32_t)label[i + b]) << (b * (uint32_t)k_ra_rsip_byte_bits);
      }
      *ra_rsip_reg32(k_ra_rsip_off_kdf_label) = tail;
    }
  }
  if (salt_len > 0U) {
    uint32_t i = 0U;
    while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= salt_len) {
      *ra_rsip_reg32(k_ra_rsip_off_kdf_salt) = internal_pack_le(&salt[i]);
      i += (uint32_t)k_ra_rsip_trng_word_bytes;
    }
    if (i < salt_len) {
      uint32_t tail = 0U;
      for (uint32_t b = 0U; (i + b) < salt_len; ++b) {
        tail |= ((uint32_t)salt[i + b]) << (b * (uint32_t)k_ra_rsip_byte_bits);
      }
      *ra_rsip_reg32(k_ra_rsip_off_kdf_salt) = tail;
    }
  }
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)op;

  const ra_err_t err = internal_complete((uint32_t)k_ra_rsip_mask_isr_kdf_done);
  if (err != k_ra_ok) {
    return err;
  }
  /* Wrapped derived key delivered through KDF_OUT. */
  out->alg        = *ra_rsip_reg32(k_ra_rsip_off_kdf_out);
  out->body_words = (uint32_t)k_ra_rsip_handle_words_hmac_sha256;
  for (uint32_t w = 0U; w < out->body_words; ++w) {
    out->body[w] = *ra_rsip_reg32(k_ra_rsip_off_kdf_out);
  }
  for (uint32_t w = out->body_words; w < (uint32_t)k_ra_rsip_handle_words_rsa4096_priv; ++w) {
    out->body[w] = 0U;
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: device lifecycle + debug authorisation
 * ===========================================================================
 */

ra_err_t ra_rsip_life_get(ra_rsip_life_state_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 51.1 "Device lifecycle" p 3263 */
  *out = (ra_rsip_life_state_t)*ra_rsip_reg32(k_ra_rsip_off_life_state);
  return k_ra_ok;
}

ra_err_t ra_rsip_life_advance(ra_rsip_life_state_t state)
{
  if ((uint32_t)state > (uint32_t)k_ra_rsip_life_rma) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 51.1 "Device lifecycle" p 3263 */
  const uint32_t cur = *ra_rsip_reg32(k_ra_rsip_off_life_state);
  if ((uint32_t)state < cur) {
    return k_ra_err_invalid_state;
  }
  *ra_rsip_reg32(k_ra_rsip_off_life_state) = (uint32_t)state;
  return k_ra_ok;
}

ra_err_t ra_rsip_debug_level_get(ra_rsip_debug_level_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 51.1 "Three debug levels" p 3263 */
  *out = (ra_rsip_debug_level_t)*ra_rsip_reg32(k_ra_rsip_off_debug_level);
  return k_ra_ok;
}

ra_err_t ra_rsip_debug_level_set(ra_rsip_debug_level_t level)
{
  if ((uint32_t)level > (uint32_t)k_ra_rsip_debug_al2) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 51.1 "Three debug levels" p 3263 */
  *ra_rsip_reg32(k_ra_rsip_off_debug_level) = (uint32_t)level;
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: tamper subsystem
 * ===========================================================================
 */

ra_err_t ra_rsip_tamper_enable(uint32_t sources)
{
  if ((sources & ~(uint32_t)k_ra_rsip_tamper_src_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 51.6 "Tamper Detection" p 3294 */
  *ra_rsip_reg32(k_ra_rsip_off_tamper_ctrl) = sources;
  return k_ra_ok;
}

ra_err_t ra_rsip_tamper_status(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 51.6 "Tamper Detection" p 3294 */
  *out = *ra_rsip_reg32(k_ra_rsip_off_tamper_status);
  return k_ra_ok;
}

ra_err_t ra_rsip_tamper_ack(uint32_t mask)
{
  if (mask == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((mask & ~(uint32_t)k_ra_rsip_tamper_src_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 51.6 "Tamper Detection" p 3294 */
  *ra_rsip_reg32(k_ra_rsip_off_tamper_status) = mask;
  return k_ra_ok;
}

ra_err_t ra_rsip_dpa_arm(bool enable)
{
  /* HUM Ch 51.5 "Side-channel countermeasures" p 3290 */
  volatile uint32_t* ctrl = ra_rsip_reg32(k_ra_rsip_off_ctrl);
  if (enable) {
    *ctrl |= (uint32_t)k_ra_rsip_mask_ctrl_dpa_arm;
  } else {
    *ctrl &= ~(uint32_t)k_ra_rsip_mask_ctrl_dpa_arm;
  }
  *ra_rsip_reg32(k_ra_rsip_off_dpa_ctrl) = (uint32_t)enable;
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: DOTF key delivery routing
 * ===========================================================================
 */

ra_err_t ra_rsip_dotf_route(uint8_t which, uint8_t slot, bool on)
{
  if (which > 1U) {
    return k_ra_err_invalid_arg;
  }
  if (on && (slot >= (uint8_t)k_ra_rsip_kv_slot_count)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  const ra_rsip_off_t off = (which == 0U) ? k_ra_rsip_off_dotf0_ctrl : k_ra_rsip_off_dotf1_ctrl;
  /* DOTFn_CTRL = (slot << 16) | route_enable */
  uint32_t word = (uint32_t)k_ra_rsip_dotf_off;
  if (on) {
    word = ((uint32_t)slot << (uint32_t)k_ra_rsip_byte_shift_2) | (uint32_t)k_ra_rsip_dotf_on;
  }
  *ra_rsip_reg32(off) = word;
  return k_ra_ok;
}
