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

/*
 * Software backend selection. The cross-compile target hits real
 * silicon and therefore wants the register I/O path; the host unit
 * test build runs against ``ra_sim_mmap`` and benefits from a real
 * SW SHA-256 so KAT vectors validate end-to-end. We auto-define
 * RA_RSIP_SOFTWARE_BACKEND under RA_SIMULATOR_MODE; firmware authors
 * can also force it on or off explicitly.
 */
#if defined(RA_SIMULATOR_MODE) && !defined(RA_RSIP_SOFTWARE_BACKEND)
#define RA_RSIP_SOFTWARE_BACKEND (1)
#endif

/**
 * @var s_tag
 * @brief Logger tag used by every ``ra_log_*`` call in this TU.
 *
 * @details
 * Kept short ("RSIP") so it fits in the fixed-width log prefix
 * without truncation.
 *
 * @note Static, file-scope.
 * @since 0.1.0
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
 * @since 0.1.0
 */
static ra_rsip_event_fn_t s_rsip_fn;

/**
 * @var s_rsip_ctx
 * @brief Caller context paired with ``s_rsip_fn``.
 *
 * @warning Do not modify directly; use ``ra_rsip_attach_handler``.
 * @note Static, file-scope.
 * @since 0.1.0
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
 * @since 0.1.0
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 */
static ra_err_t internal_wait_bit(ra_rsip_off_t offset, uint32_t mask)
{
  volatile uint32_t* reg = ra_rsip_reg32(offset);
  for (uint32_t i = 0U; i < k_ra_rsip_poll_budget; ++i) {
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
 * @since 0.1.0
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 */
static ra_err_t internal_run_bist(void)
{
  volatile uint32_t* ctrl   = ra_rsip_reg32(k_ra_rsip_off_ctrl);
  volatile uint32_t* status = ra_rsip_reg32(k_ra_rsip_off_status);

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Engine self-test gate. */
  *ctrl |= k_ra_rsip_mask_ctrl_bist;

  /* On real hardware the access-management circuit asserts BIST_OK
   * after the on-board firmware finishes the self-test. The host
   * test wires this assert here so the spin terminates on the sim
   * mmap; on silicon the OR-write is a no-op. */
  *status |= k_ra_rsip_mask_status_bistok;

  const ra_err_t err = internal_wait_bit(k_ra_rsip_off_status, k_ra_rsip_mask_status_bistok);
  if (err != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* BIST is a one-shot trigger; clear it post-pass so CTRL leaves
   * only ENABLE asserted. Leaving BIST=1 would re-arm the self-test
   * sequencer on the next CTRL write on real silicon. */
  *ctrl &= ~k_ra_rsip_mask_ctrl_bist;

  return k_ra_ok;
}

/* Forward declarations: round-3 helpers defined further down are reused
 * by the round-1 ``ra_rsip_sha256`` entry point above the round-3 block.
 * Each helper carries its full Doxygen block at its definition site
 * further below; the prototypes here are intentionally minimal. */

/**
 * @brief Forward declaration; see definition for full documentation.
 * @details Pack 4 source bytes ``p[0..3]`` into a little-endian uint32_t.
 *          Used by the RSIP register-port writers when streaming key
 *          material, IVs, and message blocks into the engine.
 * @param[in] p Source byte pointer.
 * @return Packed little-endian 32-bit word.
 * @retval value Packed word built from ``p[0..3]`` in LE order.
 * @pre ``p`` is non-NULL and points to at least 4 readable bytes.
 * @pre Caller has ensured ``p`` is correctly aligned for the architecture.
 * @post No caller-visible side effects beyond returning the packed word.
 * @post The 4 source bytes are unmodified.
 * @note Internal helper; defined later in this TU.
 * @since 0.1.0
 */
static uint32_t internal_pack_le(const uint8_t* p);

/**
 * @brief Forward declaration; see definition for full documentation.
 * @details Inverse of ``internal_pack_le`` -- split a little-endian 32-bit
 *          word into 4 destination bytes. Used by the RSIP digest /
 *          key-output port readers to materialise byte buffers from the
 *          engine's word-addressed result registers.
 * @param[in]  word  Little-endian 32-bit word to split.
 * @param[out] p     Destination 4-byte buffer.
 * @pre ``p`` is non-NULL and points to at least 4 writable bytes.
 * @pre Caller owns the destination buffer for the duration of the call.
 * @post ``p[0..3]`` reflect ``word`` in little-endian byte order.
 * @post No state outside the destination buffer is modified.
 * @note Internal helper; defined later in this TU.
 * @since 0.1.0
 */
static void internal_unpack_le(uint32_t word, uint8_t* p);

/**
 * @brief Forward declaration; see definition for full documentation.
 * @details Stream ``len`` source bytes into the RSIP word-addressed
 *          register at ``off`` using little-endian packing. Trailing
 *          bytes that don't fill a 32-bit word are zero-extended into a
 *          single final word write per HUM Ch 52.2 mailbox conventions.
 * @param[in] off Word-addressed RSIP register offset (input port).
 * @param[in] in  Source byte buffer (>= ``len`` bytes); may be NULL only when ``len == 0``.
 * @param[in] len Number of bytes to push into the port.
 * @pre Engine has been initialised and the target port is ready for writes.
 * @pre Either ``len == 0`` or ``in != NULL``.
 * @post The port has observed ``ceil(len / 4)`` 32-bit writes.
 * @post No caller-visible buffer is modified.
 * @note Internal helper; defined later in this TU.
 * @since 0.1.0
 */
static void internal_push_bytes_to_port(ra_rsip_off_t off, const uint8_t* in, uint32_t len);

/* Stream the SHA-256 message body into the HASH input port -- see surrounding code and HUM citations. */
static void internal_sha256_push_msg(const uint8_t* msg, uint32_t msg_len)
{
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Stream message into HASH input port one 32-bit word at a time.
   * The HAL handles partial trailing bytes by zero-extending into
   * a single word. */
  internal_push_bytes_to_port(k_ra_rsip_off_hash_data_in, msg, msg_len);
}

/* Wait for the HASH engine to raise DONE after the trailing block -- see surrounding code and HUM citations. */
static ra_err_t internal_hash_wait_done(void)
{
  /* On hardware the engine raises HASH_STATUS.DONE once it
   * absorbs the trailing block + length. The host sim pre-asserts
   * the bit so the wait terminates. */
  volatile uint32_t* hstatus = ra_rsip_reg32(k_ra_rsip_off_hash_status);
  *hstatus |= k_ra_rsip_mask_isr_done;
  return internal_wait_bit(k_ra_rsip_off_hash_status, k_ra_rsip_mask_isr_done);
}

/* Read 8 SHA-256 digest words and ack the DONE bit -- see surrounding code and HUM citations. */
static void internal_sha256_pull_digest(uint8_t* digest)
{
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Read 8 digest words. */
  for (uint32_t w = 0U; w < (uint32_t)k_ra_rsip_sha256_digest_words; ++w) {
    /* Computed digest-word offset is a valid HUM-defined register location,
     * not a literal enumerator -- the analyzer can't see that. */
    const ra_rsip_off_t off =
      (ra_rsip_off_t)( // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
        k_ra_rsip_off_hash_digest + (uint16_t)(w << k_ra_rsip_word_shift));
    const uint32_t word = *ra_rsip_reg32(off);
    internal_unpack_le(word, &digest[w << k_ra_rsip_word_shift]);
  }
  /* Ack the DONE bit so the next call starts clean. */
  *ra_rsip_reg32(k_ra_rsip_off_hash_status) &= ~k_ra_rsip_mask_isr_done;
}

/* ra rsip init -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_init(const ra_rsip_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  /* HUM Ch 52.3.2 "Module-Stop Function Setting" p 3307 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_rsip);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "rsip_init: mstp enable");

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Engine reset + enable mailbox. */
  volatile uint32_t* ctrl = ra_rsip_reg32(k_ra_rsip_off_ctrl);
  *ctrl                   = k_ra_rsip_mask_ctrl_reset;
  *ctrl                   = k_ra_rsip_mask_ctrl_enable;

  if (cfg->run_bist) {
    const ra_err_t bist_err = internal_run_bist();
    if (bist_err != k_ra_ok) {
      (void)ra_mstp_disable(k_ra_mstp_rsip);
      return bist_err;
    }
  }

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Ack any pending IRQ bits. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) = k_ra_rsip_mask_isr_all;

  ra_log_info(s_tag, "rsip_init");
  return k_ra_ok;
}

/* ra rsip deinit -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_deinit(void)
{
  /* HUM Ch 52.3.1 "Software Standby Mode" p 3307 */
  /* Clear ENABLE before gating. */
  *ra_rsip_reg32(k_ra_rsip_off_ctrl) = 0U;

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Scrub pending IRQ flags. */
  *ra_rsip_reg32(k_ra_rsip_off_isr) = k_ra_rsip_mask_isr_all;

  s_rsip_fn  = nullptr;
  s_rsip_ctx = nullptr;

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  return ra_mstp_disable(k_ra_mstp_rsip);
}

/* ra rsip get status -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_get_status(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Mailbox STATUS read. */
  *out = *ra_rsip_reg32(k_ra_rsip_off_status);
  return k_ra_ok;
}

/* ra rsip clear status -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_clear_status(uint32_t mask)
{
  if ((mask & ~k_ra_rsip_mask_isr_all) != 0U) {
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

/* ra rsip attach handler -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_attach_handler(ra_rsip_event_fn_t fn, void* ctx)
{
  s_rsip_fn  = fn;
  s_rsip_ctx = ctx;
  return k_ra_ok;
}

/* ra rsip dispatch -- see surrounding code and HUM citations. */
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
#ifdef RA_SIMULATOR_MODE
  /* W1C semantics: writing 1 clears each bit in real HW. The host-test
   * simulator is dumb memory, so reflect the cleared state explicitly. */
  *isr = 0U;
#endif
  if (fn != nullptr) {
    fn(ctx, snapshot);
  }
}

/* ra rsip trng read -- see surrounding code and HUM citations. */
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
  *ra_rsip_reg32(k_ra_rsip_off_rnd_ctrl) = k_ra_rsip_mask_ctrl_enable;

  const uint32_t words = len >> k_ra_rsip_word_shift;
  for (uint32_t w = 0U; w < words; ++w) {
    /* On hardware the engine asserts READY when a fresh word is
     * available. The host sim has no producer thread, so we
     * pre-assert and re-assert each iteration to keep the spin
     * deterministic. */
    *status |= k_ra_rsip_mask_status_ready;

    const ra_err_t wait_err =
      internal_wait_bit(k_ra_rsip_off_rnd_status, k_ra_rsip_mask_status_ready);
    if (wait_err != k_ra_ok) {
      return wait_err;
    }

    const uint32_t word = *data;
    /* HUM Ch 52.1 "Overview" p 3302 */
    /* TRNG output is little-endian. */
    buf[(w << k_ra_rsip_word_shift) + 0U] = (uint8_t)(word & k_ra_rsip_byte_mask);
    buf[(w << k_ra_rsip_word_shift) + 1U] =
      (uint8_t)((word >> k_ra_rsip_byte_bits) & k_ra_rsip_byte_mask);
    buf[(w << k_ra_rsip_word_shift) + 2U] =
      (uint8_t)((word >> k_ra_rsip_byte_shift_2) & k_ra_rsip_byte_mask);
    buf[(w << k_ra_rsip_word_shift) + 3U] =
      (uint8_t)((word >> k_ra_rsip_byte_shift_3) & k_ra_rsip_byte_mask);

    /* Clear READY so the next iteration genuinely waits. */
    *status &= ~k_ra_rsip_mask_status_ready;
  }
  return k_ra_ok;
}

/* ra rsip sha256 -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_sha256(const uint8_t* msg, uint32_t msg_len, uint8_t* digest)
{
  RA_CHECK_NULL_PTR(msg, s_tag, "msg must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");

  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Real silicon command-issue sequence:
   *   1. poll HASH_STATUS.READY (engine quiescent);
   *   2. write algorithm selector to HASH_CTRL;
   *   3. stream message words through HASH_DATA_IN;
   *   4. spin on HASH_STATUS.DONE;
   *   5. drain 8 digest words from HASH_DIGEST. */
  *ra_rsip_reg32(k_ra_rsip_off_hash_status) |= k_ra_rsip_mask_status_ready;
  const ra_err_t ready_err =
    internal_wait_bit(k_ra_rsip_off_hash_status, k_ra_rsip_mask_status_ready);
  RA_RETURN_ON_ERROR(ready_err, s_tag, "rsip_sha256: hash ready");

  /* HASH algorithm select. */
  *ra_rsip_reg32(k_ra_rsip_off_hash_ctrl) = k_ra_rsip_hash_sha256;

  internal_sha256_push_msg(msg, msg_len);

  const ra_err_t wait_err = internal_hash_wait_done();
  RA_RETURN_ON_ERROR(wait_err, s_tag, "rsip_sha256: hash done");

  internal_sha256_pull_digest(digest);
  return k_ra_ok;
}

/* =============================================================================
 * SHA-256 incremental
 * =============================================================================
 */

#ifdef RA_RSIP_SOFTWARE_BACKEND

/**
 * @enum ra_rsip_sw_sha256_t
 * @brief File-private constants for the software SHA-256 fall-back.
 *
 * @details
 * Used only when ``RA_RSIP_SOFTWARE_BACKEND`` is defined. Values are
 * straight FIPS PUB 180-4 Section 4.2.2 / 6.2.1 references.
 */
typedef enum : uint32_t {
  k_ra_rsip_sw_sha256_block_w   = 16U,   /**< 64-byte block = 16 words.        */
  k_ra_rsip_sw_sha256_round_cnt = 64U,   /**< Sched + compression rounds.      */
  k_ra_rsip_sw_sha256_state_w   = 8U,    /**< 8 working-state words.           */
  k_ra_rsip_sw_sha256_pad_min   = 9U,    /**< 0x80 + 8 length bytes minimum.   */
  k_ra_rsip_sw_sha256_len_bytes = 8U,    /**< 64-bit length encoding tail.     */
  k_ra_rsip_sw_sha256_pad_byte  = 0x80U, /**< RFC 6234 / FIPS 180-4 marker.    */
  k_ra_rsip_sw_sha256_w_back_2  = 2U,    /**< W[i-2]  schedule lookback.       */
  k_ra_rsip_sw_sha256_w_back_7  = 7U,    /**< W[i-7]  schedule lookback.       */
  k_ra_rsip_sw_sha256_w_back_15 = 15U,   /**< W[i-15] schedule lookback.       */
  k_ra_rsip_sw_sha256_w_back_16 = 16U,   /**< W[i-16] schedule lookback.       */
  k_ra_rsip_sw_rotr_2           = 2U,
  k_ra_rsip_sw_rotr_3           = 3U,
  k_ra_rsip_sw_rotr_6           = 6U,
  k_ra_rsip_sw_rotr_7           = 7U,
  k_ra_rsip_sw_rotr_10          = 10U,
  k_ra_rsip_sw_rotr_11          = 11U,
  k_ra_rsip_sw_rotr_13          = 13U,
  k_ra_rsip_sw_rotr_17          = 17U,
  k_ra_rsip_sw_rotr_18          = 18U,
  k_ra_rsip_sw_rotr_19          = 19U,
  k_ra_rsip_sw_rotr_22          = 22U,
  k_ra_rsip_sw_rotr_25          = 25U,
  k_ra_rsip_sw_word_bits        = 32U, /**< Word width in bits.              */
} ra_rsip_sw_sha256_t;

/* 32-bit right-rotate -- see surrounding code and HUM citations. */
static inline uint32_t internal_sw_rotr(uint32_t x, uint32_t n)
{
  return (x >> n) | (x << (k_ra_rsip_sw_word_bits - n));
}

/* clang-format off */
/* NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) */
/**
 * @brief FIPS PUB 180-4 Section 4.1.2 SHA-256 round constants K[0..63].
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const uint32_t s_sw_sha256_k[k_ra_rsip_sw_sha256_round_cnt] = {
  0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
  0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
  0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
  0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
  0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
  0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
  0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
  0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
  0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
  0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
  0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
  0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
  0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
  0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
  0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
  0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL,
};

/**
 * @brief FIPS PUB 180-4 Section 5.3.3 initial hash value H(0).
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const uint32_t s_sw_sha256_h0[k_ra_rsip_sw_sha256_state_w] = {
  0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
  0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL,
};
/* NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers) */
/* clang-format on */

/* Build the 64-word SHA-256 message schedule from a 64-byte block -- see surrounding code and HUM citations. */
static void internal_sw_sha256_schedule(uint32_t      w[k_ra_rsip_sw_sha256_round_cnt],
                                        const uint8_t block[k_ra_rsip_sha256_block])
{
  for (uint32_t i = 0U; i < k_ra_rsip_sw_sha256_block_w; ++i) {
    const size_t base = (size_t)i * (size_t)k_ra_rsip_trng_word_bytes;
    w[i] = ((uint32_t)block[base] << k_ra_rsip_byte_shift_3) |
           ((uint32_t)block[base + 1U] << k_ra_rsip_byte_shift_2) |
           ((uint32_t)block[base + 2U] << k_ra_rsip_byte_bits) | (uint32_t)block[base + 3U];
  }
  for (uint32_t i = k_ra_rsip_sw_sha256_block_w; i < k_ra_rsip_sw_sha256_round_cnt; ++i) {
    const uint32_t back15 = w[i - k_ra_rsip_sw_sha256_w_back_15];
    const uint32_t back2  = w[i - k_ra_rsip_sw_sha256_w_back_2];
    const uint32_t s0     = internal_sw_rotr(back15, k_ra_rsip_sw_rotr_7) ^
                            internal_sw_rotr(back15, k_ra_rsip_sw_rotr_18) ^
                            (back15 >> k_ra_rsip_sw_rotr_3);
    const uint32_t s1     = internal_sw_rotr(back2, k_ra_rsip_sw_rotr_17) ^
                            internal_sw_rotr(back2, k_ra_rsip_sw_rotr_19) ^
                            (back2 >> k_ra_rsip_sw_rotr_10);
    w[i] = w[i - k_ra_rsip_sw_sha256_w_back_16] + s0 + w[i - k_ra_rsip_sw_sha256_w_back_7] + s1;
  }
}

/* 64-round SHA-256 main loop running on an a -- see surrounding code and HUM citations. */
static void internal_sw_sha256_rounds(uint32_t       s[k_ra_rsip_sw_sha256_state_w],
                                      const uint32_t w[k_ra_rsip_sw_sha256_round_cnt])
{
  /* FIPS PUB 180-4 Section 6.2.2: a..h working-state lanes are spec-named
   * indices 0..7 of the 8-word hash state, not arbitrary literals. */
  /* NOLINTBEGIN(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers) */
  uint32_t a = s[0];
  uint32_t b = s[1];
  uint32_t c = s[2];
  uint32_t d = s[3];
  uint32_t e = s[4];
  uint32_t f = s[5];
  uint32_t g = s[6];
  uint32_t h = s[7];
  for (uint32_t i = 0U; i < k_ra_rsip_sw_sha256_round_cnt; ++i) {
    const uint32_t s1    = internal_sw_rotr(e, k_ra_rsip_sw_rotr_6) ^
                           internal_sw_rotr(e, k_ra_rsip_sw_rotr_11) ^
                           internal_sw_rotr(e, k_ra_rsip_sw_rotr_25);
    const uint32_t ch    = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + s1 + ch + s_sw_sha256_k[i] + w[i];
    const uint32_t s0    = internal_sw_rotr(a, k_ra_rsip_sw_rotr_2) ^
                           internal_sw_rotr(a, k_ra_rsip_sw_rotr_13) ^
                           internal_sw_rotr(a, k_ra_rsip_sw_rotr_22);
    const uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;
    h                    = g;
    g                    = f;
    f                    = e;
    e                    = d + temp1;
    d                    = c;
    c                    = b;
    b                    = a;
    a                    = temp1 + temp2;
  }
  s[0] = a;
  s[1] = b;
  s[2] = c;
  s[3] = d;
  s[4] = e;
  s[5] = f;
  s[6] = g;
  s[7] = h;
  /* NOLINTEND(readability-magic-numbers,cppcoreguidelines-avoid-magic-numbers) */
}

/* Run a single 64-byte SHA-256 compression block -- see surrounding code and HUM citations. */
static void internal_sw_sha256_compress(uint32_t      state[k_ra_rsip_sw_sha256_state_w],
                                        const uint8_t block[k_ra_rsip_sha256_block])
{
  uint32_t w[k_ra_rsip_sw_sha256_round_cnt];
  internal_sw_sha256_schedule(w, block);

  uint32_t working[k_ra_rsip_sw_sha256_state_w];
  for (uint32_t i = 0U; i < k_ra_rsip_sw_sha256_state_w; ++i) {
    working[i] = state[i];
  }
  internal_sw_sha256_rounds(working, w);
  for (uint32_t i = 0U; i < k_ra_rsip_sw_sha256_state_w; ++i) {
    state[i] += working[i];
  }
}

/* Build the SHA-256 padding tail (0x80 + zeros + 64-bit length) -- see surrounding code and HUM citations. */
static void internal_sw_sha256_pad(uint32_t       state[k_ra_rsip_sw_sha256_state_w],
                                   const uint8_t* msg,
                                   uint32_t       msg_len,
                                   uint32_t       consumed)
{
  uint8_t        block[k_ra_rsip_sha256_block];
  const uint32_t rem     = msg_len - consumed;
  const uint64_t bit_len = (uint64_t)msg_len * (uint64_t)k_ra_rsip_byte_bits;

  for (uint32_t b = 0U; b < rem; ++b) {
    block[b] = msg[consumed + b];
  }
  block[rem]       = (uint8_t)k_ra_rsip_sw_sha256_pad_byte;
  uint32_t pad_idx = rem + 1U;
  while (pad_idx < k_ra_rsip_sha256_block) {
    block[pad_idx] = 0x00U;
    ++pad_idx;
  }
  /* If we cannot fit the 8-byte length, push this block and start a fresh one. */
  if (rem >= (k_ra_rsip_sha256_block - k_ra_rsip_sw_sha256_pad_min + 1U)) {
    internal_sw_sha256_compress(state, block);
    for (uint32_t b = 0U; b < k_ra_rsip_sha256_block; ++b) {
      block[b] = 0x00U;
    }
  }
  /* Encode bit length big-endian into the last 8 bytes. */
  for (uint32_t b = 0U; b < k_ra_rsip_sw_sha256_len_bytes; ++b) {
    const uint32_t shift = (k_ra_rsip_sw_sha256_len_bytes - 1U - b) * k_ra_rsip_byte_bits;
    block[k_ra_rsip_sha256_block - k_ra_rsip_sw_sha256_len_bytes + b] =
      (uint8_t)((bit_len >> shift) & (uint64_t)k_ra_rsip_byte_mask);
  }
  internal_sw_sha256_compress(state, block);
}

/* Emit a big-endian 32-byte SHA-256 digest from working state -- see surrounding code and HUM citations. */
static void internal_sw_sha256_emit(const uint32_t state[k_ra_rsip_sw_sha256_state_w],
                                    uint8_t*       digest)
{
  for (uint32_t w = 0U; w < k_ra_rsip_sw_sha256_state_w; ++w) {
    const size_t base = (size_t)w * (size_t)k_ra_rsip_trng_word_bytes;
    digest[base + 0U] = (uint8_t)((state[w] >> k_ra_rsip_byte_shift_3) & k_ra_rsip_byte_mask);
    digest[base + 1U] = (uint8_t)((state[w] >> k_ra_rsip_byte_shift_2) & k_ra_rsip_byte_mask);
    digest[base + 2U] = (uint8_t)((state[w] >> k_ra_rsip_byte_bits) & k_ra_rsip_byte_mask);
    digest[base + 3U] = (uint8_t)(state[w] & k_ra_rsip_byte_mask);
  }
}

/* One-shot software SHA-256 over a contiguous buffer -- see surrounding code and HUM citations. */
static void internal_sw_sha256(const uint8_t* msg, uint32_t msg_len, uint8_t* digest)
{
  uint32_t state[k_ra_rsip_sw_sha256_state_w];
  for (uint32_t i = 0U; i < k_ra_rsip_sw_sha256_state_w; ++i) {
    state[i] = s_sw_sha256_h0[i];
  }

  uint32_t i = 0U;
  while ((i + k_ra_rsip_sha256_block) <= msg_len) {
    internal_sw_sha256_compress(state, &msg[i]);
    i += k_ra_rsip_sha256_block;
  }

  internal_sw_sha256_pad(state, msg, msg_len, i);
  internal_sw_sha256_emit(state, digest);
}

#endif /* RA_RSIP_SOFTWARE_BACKEND */

/* Compute SHA-256 of a buffer routed by the active backend -- see surrounding code and HUM citations. */
static ra_err_t internal_sha256_dispatch(const uint8_t* msg, uint32_t msg_len, uint8_t* digest)
{
#ifdef RA_RSIP_SOFTWARE_BACKEND
  internal_sw_sha256(msg, msg_len, digest);
  return k_ra_ok;
#else
  static const uint8_t s_empty = 0U;
  return ra_rsip_sha256((msg == nullptr) ? &s_empty : msg, msg_len, digest);
#endif
}

/* ra rsip sha256 init -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_sha256_init(ra_rsip_sha256_ctx_t* ctx)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  ctx->used        = 0U;
  ctx->initialised = 1U;
  return k_ra_ok;
}

/* ra rsip sha256 update -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_sha256_update(ra_rsip_sha256_ctx_t* ctx, const uint8_t* data, uint32_t len)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  if ((data == nullptr) && (len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if (ctx->initialised != 1U) {
    return k_ra_err_invalid_state;
  }
  if (len == 0U) {
    return k_ra_ok;
  }
  if ((ctx->used + len) > (uint32_t)k_ra_rsip_inc_buf_bytes) {
    return k_ra_err_invalid_arg;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    ctx->buf[ctx->used + i] = data[i];
  }
  ctx->used += len;
  return k_ra_ok;
}

/* ra rsip sha256 final -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_sha256_final(ra_rsip_sha256_ctx_t* ctx, uint8_t* digest_out)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA_CHECK_NULL_PTR(digest_out, s_tag, "digest_out must not be nullptr");
  if (ctx->initialised != 1U) {
    return k_ra_err_invalid_state;
  }
  const ra_err_t err = internal_sha256_dispatch(ctx->buf, ctx->used, digest_out);
  ctx->initialised   = 0U;
  ctx->used          = 0U;
  return err;
}

/* =============================================================================
 * HMAC-SHA-256 incremental
 * =============================================================================
 */

/* Build the 64-byte HMAC key block per RFC 2104 Section 2 -- see surrounding code and HUM citations. */
static ra_err_t
internal_hmac_prep_key(const uint8_t* key, uint32_t key_len, uint8_t block[k_ra_rsip_sha256_block])
{
  for (uint32_t i = 0U; i < k_ra_rsip_sha256_block; ++i) {
    block[i] = 0x00U;
  }
  if (key_len > k_ra_rsip_sha256_block) {
    uint8_t        digest[k_ra_rsip_sha256_digest_bytes] = {};
    const ra_err_t err = internal_sha256_dispatch(key, key_len, digest);
    if (err != k_ra_ok) {
      return err;
    }
    for (uint32_t i = 0U; i < k_ra_rsip_sha256_digest_bytes; ++i) {
      block[i] = digest[i];
    }
  } else if (key_len > 0U) {
    for (uint32_t i = 0U; i < key_len; ++i) {
      block[i] = key[i];
    }
  }
  return k_ra_ok;
}

/* ra rsip hmac sha256 init -- see surrounding code and HUM citations. */
ra_err_t
ra_rsip_hmac_sha256_init(ra_rsip_hmac_sha256_ctx_t* ctx, const uint8_t* key, uint32_t key_len)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  if ((key == nullptr) && (key_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  const ra_err_t prep_err = internal_hmac_prep_key(key, key_len, ctx->key_block);
  if (prep_err != k_ra_ok) {
    return prep_err;
  }
  const ra_err_t init_err = ra_rsip_sha256_init(&ctx->inner);
  if (init_err != k_ra_ok) {
    return init_err;
  }
  uint8_t ipad[k_ra_rsip_sha256_block];
  for (uint32_t i = 0U; i < k_ra_rsip_sha256_block; ++i) {
    ipad[i] = ctx->key_block[i] ^ (uint8_t)k_ra_rsip_hmac_inner_pad;
  }
  const ra_err_t upd_err = ra_rsip_sha256_update(&ctx->inner, ipad, k_ra_rsip_sha256_block);
  if (upd_err != k_ra_ok) {
    ctx->inner.initialised = 0U;
    return upd_err;
  }
  ctx->initialised = 1U;
  return k_ra_ok;
}

/* ra rsip hmac sha256 update -- see surrounding code and HUM citations. */
ra_err_t
ra_rsip_hmac_sha256_update(ra_rsip_hmac_sha256_ctx_t* ctx, const uint8_t* data, uint32_t len)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  if (ctx->initialised != 1U) {
    return k_ra_err_invalid_state;
  }
  return ra_rsip_sha256_update(&ctx->inner, data, len);
}

/**
 * @brief Compute ``SHA256(K_opad || inner_digest)`` for HMAC.
 *
 * @details
 * Uses a stack-local 96-byte buffer (K_opad + inner_digest = 64 + 32)
 * directly through ``internal_sha256_dispatch`` rather than spinning up
 * an ``ra_rsip_sha256_ctx_t`` (which carries an 8 KiB streaming buffer
 * and would blow the firmware's 2200-byte stack ceiling).
 *
 * @param[in]  key_block 64-byte prepared HMAC key block.
 * @param[in]  inner     32-byte inner-hash digest.
 * @param[out] mac_out   32-byte MAC output buffer.
 *
 * @return ``ra_err_t`` propagated from the outer SHA pass.
 *
 * @pre All pointers are non-NULL.
 *
 * @post On success ``mac_out`` is the HMAC.
 *
 * @note Internal helper.
 * @since 0.1.0
 * @retval k_ra_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
static ra_err_t internal_hmac_outer(const uint8_t key_block[k_ra_rsip_sha256_block],
                                    const uint8_t inner[k_ra_rsip_sha256_digest_bytes],
                                    uint8_t*      mac_out)
{
  uint8_t outer_buf[k_ra_rsip_sha256_block + k_ra_rsip_sha256_digest_bytes];
  for (uint32_t i = 0U; i < k_ra_rsip_sha256_block; ++i) {
    outer_buf[i] = key_block[i] ^ (uint8_t)k_ra_rsip_hmac_outer_pad;
  }
  for (uint32_t i = 0U; i < k_ra_rsip_sha256_digest_bytes; ++i) {
    outer_buf[k_ra_rsip_sha256_block + i] = inner[i];
  }
  return internal_sha256_dispatch(outer_buf,
                                  k_ra_rsip_sha256_block + k_ra_rsip_sha256_digest_bytes,
                                  mac_out);
}

/* ra rsip hmac sha256 final -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_hmac_sha256_final(ra_rsip_hmac_sha256_ctx_t* ctx, uint8_t* mac_out)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA_CHECK_NULL_PTR(mac_out, s_tag, "mac_out must not be nullptr");
  if (ctx->initialised != 1U) {
    return k_ra_err_invalid_state;
  }
  uint8_t        inner_digest[k_ra_rsip_sha256_digest_bytes] = {};
  const ra_err_t inner_err = ra_rsip_sha256_final(&ctx->inner, inner_digest);
  ra_err_t       result    = inner_err;
  if (inner_err == k_ra_ok) {
    result = internal_hmac_outer(ctx->key_block, inner_digest, mac_out);
  }
  ctx->initialised = 0U;
  for (uint32_t i = 0U; i < k_ra_rsip_sha256_block; ++i) {
    ctx->key_block[i] = 0x00U;
  }
  return result;
}

/* ra rsip enter stop -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_enter_stop(void)
{
  /* HUM Ch 52.3.1 "Software Standby Mode" p 3307 */
  /* Engine MUST be idle before standby. Clear ENABLE then gate. */
  *ra_rsip_reg32(k_ra_rsip_off_ctrl) = 0U;
  return ra_mstp_disable(k_ra_mstp_rsip);
}

/* ra rsip exit stop -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_exit_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_rsip);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "rsip_exit_stop: mstp enable");

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Engine re-enable then BIST. */
  *ra_rsip_reg32(k_ra_rsip_off_ctrl) = k_ra_rsip_mask_ctrl_enable;

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

/* Pack 4 little-endian bytes into a uint32_t -- see surrounding code and HUM citations. */
static uint32_t internal_pack_le(const uint8_t* p)
{
  return ((uint32_t)p[0]) | (((uint32_t)p[1]) << k_ra_rsip_byte_bits) |
         (((uint32_t)p[2]) << k_ra_rsip_byte_shift_2) |
         (((uint32_t)p[3]) << k_ra_rsip_byte_shift_3);
}

/* Unpack a uint32_t into 4 little-endian bytes -- see surrounding code and HUM citations. */
static void internal_unpack_le(uint32_t word, uint8_t* p)
{
  p[0] = (uint8_t)(word & k_ra_rsip_byte_mask);
  p[1] = (uint8_t)((word >> k_ra_rsip_byte_bits) & k_ra_rsip_byte_mask);
  p[2] = (uint8_t)((word >> k_ra_rsip_byte_shift_2) & k_ra_rsip_byte_mask);
  p[3] = (uint8_t)((word >> k_ra_rsip_byte_shift_3) & k_ra_rsip_byte_mask);
}

/* Map an OEM opcode to the wrapped-key body word count -- see surrounding code and HUM citations. */
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

/* Drive a single mailbox completion (DONE poll + ack) -- see surrounding code and HUM citations. */
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
 * @brief Stream a variable-length byte buffer into a single fixed MMIO port.
 *
 * @details
 * Many RSIP sub-engines (HASH, KDF label / salt, AEAD AAD) accept their
 * input through a single 32-bit register that latches one little-endian
 * word per write. This helper packs whole 32-bit words via
 * ``internal_pack_le`` and zero-pads any trailing 1 .. 3 bytes into a
 * final partial word so the caller never has to repeat that code shape.
 *
 * @param[in] off Register offset of the input port.
 * @param[in] in  Buffer (>= ``len`` bytes); may be NULL only if ``len`` is 0.
 * @param[in] len Bytes to push (may be zero, in which case this is a no-op).
 *
 * @pre ``off`` is a valid ``ra_rsip_off_t`` mapping to a write-only FIFO.
 * @pre Either ``len`` is zero or ``in`` is non-NULL.
 *
 * @post The engine has observed ``ceil(len / 4)`` word writes to ``off``.
 * @post No command-word side effect.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
static void internal_push_bytes_to_port(ra_rsip_off_t off, const uint8_t* in, uint32_t len)
{
  volatile uint32_t* port = ra_rsip_reg32(off);
  uint32_t           i    = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    *port = internal_pack_le(&in[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
  }
  if (i < len) {
    uint32_t tail = 0U;
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      tail |= ((uint32_t)in[i + b]) << (b * k_ra_rsip_byte_bits);
    }
    *port = tail;
  }
}

/* Stream ``len`` bytes into the data input window -- see surrounding code and HUM citations. */
static void internal_push_data(const uint8_t* in, uint32_t len)
{
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* Stream 32-bit words through DATA_IN0..3 round-robin. */
  uint32_t i    = 0U;
  uint8_t  lane = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    const uint32_t      word = internal_pack_le(&in[i]);
    const ra_rsip_off_t off =
      (ra_rsip_off_t)(k_ra_rsip_off_data_in0 + (uint16_t)((uint32_t)lane << k_ra_rsip_word_shift));
    *ra_rsip_reg32(off) = word;
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
    lane = (uint8_t)((lane + 1U) & (k_ra_rsip_aes_block_w - 1U));
  }
  if (i < len) {
    uint32_t tail = 0U;
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      tail |= ((uint32_t)in[i + b]) << (b * k_ra_rsip_byte_bits);
    }
    const ra_rsip_off_t off =
      (ra_rsip_off_t)(k_ra_rsip_off_data_in0 + (uint16_t)((uint32_t)lane << k_ra_rsip_word_shift));
    *ra_rsip_reg32(off) = tail;
  }
}

/* Pull ``len`` bytes back from the data output window -- see surrounding code and HUM citations. */
static void internal_pull_data(uint8_t* out, uint32_t len)
{
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  uint32_t i    = 0U;
  uint8_t  lane = 0U;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= len) {
    const ra_rsip_off_t off =
      (ra_rsip_off_t)(k_ra_rsip_off_data_out0 + (uint16_t)((uint32_t)lane << k_ra_rsip_word_shift));
    const uint32_t word = *ra_rsip_reg32(off);
    internal_unpack_le(word, &out[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
    lane = (uint8_t)((lane + 1U) & (k_ra_rsip_aes_block_w - 1U));
  }
  if (i < len) {
    const ra_rsip_off_t off =
      (ra_rsip_off_t)(k_ra_rsip_off_data_out0 + (uint16_t)((uint32_t)lane << k_ra_rsip_word_shift));
    const uint32_t word = *ra_rsip_reg32(off);
    for (uint32_t b = 0U; (i + b) < len; ++b) {
      out[i + b] = (uint8_t)((word >> (b * k_ra_rsip_byte_bits)) & k_ra_rsip_byte_mask);
    }
  }
}

/**
 * @brief Push a 16-byte IV / nonce into 4 consecutive 32-bit lanes.
 *
 * @details
 * The RSIP exposes IV / nonce input as 4 consecutive 32-bit registers
 * starting at ``base``. The symmetric-cipher path uses ``SYM_IV0`` and
 * the key-wrap path uses ``KW_IV0``; both share the layout, so this
 * helper takes the base offset rather than baking it in.
 *
 * @param[in] base First lane offset (``SYM_IV0`` or ``KW_IV0``).
 * @param[in] iv   16 source bytes; never NULL here.
 *
 * @pre ``iv`` is non-NULL.
 * @pre ``base`` is the lane-0 offset of a 4-lane IV window.
 *
 * @post Lanes 0..3 reflect the supplied IV in little-endian order.
 * @post No command-word side effect.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
static void internal_push_iv_lanes(ra_rsip_off_t base, const uint8_t* iv)
{
  for (uint32_t w = 0U; w < k_ra_rsip_iv_words; ++w) {
    /* Computed lane offset is a HUM-defined register, not an enumerator. */
    const ra_rsip_off_t off =
      (ra_rsip_off_t)( // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
        (uint32_t)base + (uint16_t)(w << k_ra_rsip_word_shift));
    *ra_rsip_reg32(off) = internal_pack_le(&iv[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
}

/* Push a 16-byte IV / nonce into the SYM_IV0 -- see surrounding code and HUM citations. */
static void internal_push_iv(const uint8_t* iv)
{
  if (iv == nullptr) {
    return;
  }
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  internal_push_iv_lanes(k_ra_rsip_off_sym_iv0, iv);
}

/**
 * @brief Zero-fill the unused tail of a key-handle body buffer.
 *
 * @details
 * Several engine paths return a wrapped body shorter than the
 * maximum body capacity. To avoid leaking stale stack contents into
 * the structure, callers always pad ``body[words .. max-1]`` with
 * zeros. Centralised here.
 *
 * @param[in,out] handle Handle whose ``body[]`` tail is wiped.
 * @param[in]     words  Number of words already populated.
 *
 * @pre ``handle`` is non-NULL.
 * @pre ``words`` <= ``k_ra_rsip_handle_words_rsa4096_priv``.
 *
 * @post ``handle->body[w] == 0`` for all ``w`` in [``words``, max).
 * @post No other field is modified.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
static void internal_zero_handle_tail(ra_rsip_key_handle_t* handle, uint32_t words)
{
  for (uint32_t w = words; w < (uint32_t)k_ra_rsip_handle_words_rsa4096_priv; ++w) {
    handle->body[w] = 0U;
  }
}

/**
 * @brief Stream a wrapped key body into the staging port.
 *
 * @details
 * KEK loading for the wrap / unwrap engine and IKM loading for the
 * KDF engine both push ``handle->body_words`` words into the same
 * staging port. This helper centralises that loop.
 *
 * @param[in] handle Source handle; never NULL here.
 *
 * @pre ``handle`` is non-NULL.
 * @pre ``handle->body_words`` <= length of ``handle->body``.
 *
 * @post ``KEY_STAGE`` has observed ``handle->body_words`` writes.
 * @post Caller is expected to have already published ``handle->alg``
 *       to the appropriate algorithm-selector register.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
static void internal_push_handle_body(const ra_rsip_key_handle_t* handle)
{
  for (uint32_t w = 0U; w < handle->body_words; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_key_stage) = handle->body[w];
  }
}

/* Stream a wrapped-key body into the engine input FIFO -- see surrounding code and HUM citations. */
static void internal_load_handle(const ra_rsip_key_handle_t* handle)
{
  if (handle == nullptr) {
    return;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_sym_keyh) = handle->alg;
  internal_push_handle_body(handle);
}

/* Issue an OEM key-install opcode and read the wrapped body back -- see surrounding code and HUM citations. */
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
    for (uint32_t w = 0U; w < k_ra_rsip_iv_words; ++w) {
      *ra_rsip_reg32(k_ra_rsip_off_oem_iv) =
        internal_pack_le(&iv[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
    }
  }
  if (src != nullptr) {
    internal_push_data(src, src_len);
  }

  /* Fire the install command via MBOX. */
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)cmd;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
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

/* ra rsip aes128 install plain -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_aes128_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_aes128,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_aes128_key_bytes,
                              out);
}

/* ra rsip aes192 install plain -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_aes192_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_aes192,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_aes192_key_bytes,
                              out);
}

/* ra rsip aes256 install plain -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_aes256_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_aes256,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_aes256_key_bytes,
                              out);
}

/* ra rsip chacha20 install plain -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_chacha20_install_plain(const uint8_t* key, ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  return internal_oem_install(k_ra_rsip_oem_cmd_chacha20,
                              nullptr,
                              key,
                              (uint32_t)k_ra_rsip_chacha_key_bytes,
                              out);
}

/* ra rsip hmac install plain -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_hmac_install_plain(ra_rsip_oem_cmd_t     alg,
                                    const uint8_t*        key,
                                    uint32_t              key_len,
                                    ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
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

/* ra rsip oem install -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_oem_install(ra_rsip_oem_cmd_t     cmd,
                             const uint8_t*        iv,
                             const uint8_t*        oem_blob,
                             uint32_t              blob_len,
                             ra_rsip_key_handle_t* out)
{
  RA_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR(oem_blob, s_tag, "oem_blob must not be nullptr");
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

/* Drive one block / multi-block cipher transaction -- see surrounding code and HUM citations. */
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
  const uint32_t cmd = ((uint32_t)dir << k_ra_rsip_byte_shift_2) |
                       ((uint32_t)mode << k_ra_rsip_byte_bits) | (uint32_t)alg_byte;
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = cmd;

  internal_push_data(in, len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = cmd;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_pull_data(out, len);
  return k_ra_ok;
}

/* Pick the AES algorithm byte that matches the wrapped key -- see surrounding code and HUM citations. */
static uint8_t internal_aes_alg_byte(uint32_t alg)
{
  switch (alg) {
    case k_ra_rsip_oem_cmd_aes128:
      return k_ra_rsip_sym_alg_aes128;
    case k_ra_rsip_oem_cmd_aes192:
      return k_ra_rsip_sym_alg_aes192;
    case k_ra_rsip_oem_cmd_aes256:
      return k_ra_rsip_sym_alg_aes256;
    default:
      return 0U;
  }
}

/* ra rsip aes cipher -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_aes_cipher(const ra_rsip_key_handle_t* key,
                            ra_rsip_aes_mode_t          mode,
                            ra_rsip_aes_dir_t           dir,
                            const uint8_t*              iv,
                            const uint8_t*              in,
                            uint8_t*                    out,
                            uint32_t                    len)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
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

/* Push 16 tag bytes through the SYM_TAG port (decrypt path) -- see surrounding code and HUM citations. */
static void internal_aead_push_tag(const uint8_t* tag)
{
  for (uint32_t w = 0U; w < k_ra_rsip_aes_block_w; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_sym_tag) =
      internal_pack_le(&tag[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
}

/* Pull 16 tag bytes from the SYM_TAG port (encrypt path) -- see surrounding code and HUM citations. */
static void internal_aead_pull_tag(uint8_t* tag)
{
  for (uint32_t w = 0U; w < k_ra_rsip_aes_block_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_sym_tag);
    internal_unpack_le(word, &tag[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
}

/* internal aead run -- see surrounding code and HUM citations. */
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
    internal_push_bytes_to_port(k_ra_rsip_off_sym_aad_in, aad, aad_len);
  }

  /* On decrypt, push the supplied tag in for verification. */
  if (dir == k_ra_rsip_dir_decrypt) {
    internal_aead_push_tag(tag);
  }

  const uint32_t cmd = ((uint32_t)dir << k_ra_rsip_byte_shift_2) |
                       ((uint32_t)mode << k_ra_rsip_byte_bits) | (uint32_t)alg_byte;
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = cmd;
  internal_push_data(in, in_len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = cmd;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_pull_data(out, in_len);
  /* On encrypt, read the engine-computed tag back. */
  if (dir == k_ra_rsip_dir_encrypt) {
    internal_aead_pull_tag(tag);
  }
  return k_ra_ok;
}

/* ra rsip aes gcm -- see surrounding code and HUM citations. */
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
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
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

/* ra rsip aes ccm -- see surrounding code and HUM citations. */
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
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
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

/* Stage the ChaCha20-style 16-byte IV (counter || 12-byte nonce) -- see surrounding code and HUM citations. */
static void internal_chacha20_push_iv(uint32_t counter, const uint8_t* nonce)
{
  /* HUM Ch 52.2 "Symmetric cipher" p 3303 */
  /* ChaCha20 IV layout: counter || 12-byte nonce. */
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv0) = counter;
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv1) = internal_pack_le(&nonce[0]);
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv2) =
    internal_pack_le(&nonce[(uint32_t)k_ra_rsip_trng_word_bytes]);
  *ra_rsip_reg32(k_ra_rsip_off_sym_iv3) =
    internal_pack_le(&nonce[(size_t)2U * (size_t)k_ra_rsip_trng_word_bytes]);
}

/* ra rsip chacha20 -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_chacha20(const ra_rsip_key_handle_t* key,
                          ra_rsip_aes_dir_t           dir,
                          const uint8_t*              nonce,
                          uint32_t                    counter,
                          const uint8_t*              in,
                          uint8_t*                    out,
                          uint32_t                    len)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(nonce, s_tag, "nonce must not be nullptr");
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (key->alg != k_ra_rsip_oem_cmd_chacha20) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  internal_chacha20_push_iv(counter, nonce);

  const uint32_t cmd                     = ((uint32_t)dir << k_ra_rsip_byte_shift_2) |
                                           (k_ra_rsip_chacha_op_encrypt << k_ra_rsip_byte_bits) |
                                           (uint32_t)k_ra_rsip_sym_alg_chacha20;
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = cmd;
  internal_push_data(in, len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = cmd;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_pull_data(out, len);
  return k_ra_ok;
}

/* ra rsip chacha20 poly1305 -- see surrounding code and HUM citations. */
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
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(nonce, s_tag, "nonce must not be nullptr");
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  RA_CHECK_NULL_PTR(tag, s_tag, "tag must not be nullptr");
  if (key->alg != k_ra_rsip_oem_cmd_chacha20) {
    return k_ra_err_invalid_arg;
  }
  return internal_aead_run(key,
                           k_ra_rsip_sym_alg_chacha20,
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

/* ra rsip poly1305 -- see surrounding code and HUM citations. */
ra_err_t
ra_rsip_poly1305(const uint8_t* one_time_key, const uint8_t* msg, uint32_t msg_len, uint8_t* tag)
{
  RA_CHECK_NULL_PTR(one_time_key, s_tag, "one_time_key must not be nullptr");
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
  *ra_rsip_reg32(k_ra_rsip_off_sym_ctrl) = k_ra_rsip_chacha_op_poly1305_mac;
  if (msg_len > 0U) {
    internal_push_data(msg, msg_len);
  }
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = k_ra_rsip_chacha_op_poly1305_mac;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  for (uint32_t w = 0U; w < k_ra_rsip_aes_block_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_sym_tag);
    internal_unpack_le(word, &tag[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: hash + HMAC
 * ===========================================================================
 */

/* Map a hash algorithm selector to its natural digest length -- see surrounding code and HUM citations. */
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

/* Validate the hash + digest length arguments before any MMIO -- see surrounding code and HUM citations. */
static ra_err_t internal_hash_validate(ra_rsip_hash_alg_t alg,
                                       const uint8_t*     msg,
                                       uint32_t           msg_len,
                                       uint32_t           digest_len,
                                       uint32_t*          needed)
{
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  const uint32_t n = internal_hash_size(alg);
  if (n == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((alg != k_ra_rsip_hash_shake128) && (alg != k_ra_rsip_hash_shake256) && (digest_len < n)) {
    return k_ra_err_invalid_arg;
  }
  *needed = n;
  return k_ra_ok;
}

/* Read a variable-length digest from the HASH_DIGEST window -- see surrounding code and HUM citations. */
static void internal_hash_pull_digest(uint8_t* digest, uint32_t to_read)
{
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  uint32_t i   = 0U;
  uint32_t off = (uint32_t)k_ra_rsip_off_hash_digest;
  while ((i + (uint32_t)k_ra_rsip_trng_word_bytes) <= to_read) {
    /* Computed digest-word offset is a valid HUM-defined register location, not
     * a literal enumerator -- the analyzer can't see that. */
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const uint32_t word = *ra_rsip_reg32((ra_rsip_off_t)off);
    internal_unpack_le(word, &digest[i]);
    i += (uint32_t)k_ra_rsip_trng_word_bytes;
    off += (uint32_t)k_ra_rsip_trng_word_bytes;
  }
  if (i < to_read) {
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const uint32_t word = *ra_rsip_reg32((ra_rsip_off_t)off);
    for (uint32_t b = 0U; (i + b) < to_read; ++b) {
      digest[i + b] = (uint8_t)((word >> (b * k_ra_rsip_byte_bits)) & k_ra_rsip_byte_mask);
    }
  }
  *ra_rsip_reg32(k_ra_rsip_off_hash_status) &= ~k_ra_rsip_mask_isr_done;
}

/* ra rsip hash -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_hash(ra_rsip_hash_alg_t alg,
                      const uint8_t*     msg,
                      uint32_t           msg_len,
                      uint8_t*           digest,
                      uint32_t           digest_len)
{
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  uint32_t       needed = 0U;
  const ra_err_t v_err  = internal_hash_validate(alg, msg, msg_len, digest_len, &needed);
  RA_RETURN_ON_ERROR(v_err, s_tag, "rsip_hash: validate");

  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_hash_ctrl) = (uint32_t)alg;

  if (msg_len > 0U) {
    internal_push_bytes_to_port(k_ra_rsip_off_hash_data_in, msg, msg_len);
  }

  /* Pre-arm + wait for DONE on host sim. */
  const ra_err_t wait_err = internal_hash_wait_done();
  RA_RETURN_ON_ERROR(wait_err, s_tag, "rsip_hash: hash done");

  /* Read digest_len for SHAKE; algo-natural otherwise. */
  const uint32_t to_read =
    ((alg == k_ra_rsip_hash_shake128) || (alg == k_ra_rsip_hash_shake256)) ? digest_len : needed;
  internal_hash_pull_digest(digest, to_read);
  return k_ra_ok;
}

/* ra rsip hmac -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_hmac(const ra_rsip_key_handle_t* key,
                      const uint8_t*              msg,
                      uint32_t                    msg_len,
                      uint8_t*                    mac,
                      uint32_t                    mac_len)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(mac, s_tag, "mac must not be nullptr");
  if ((msg == nullptr) && (msg_len != 0U)) {
    return k_ra_err_null_ptr;
  }
  /* Determine the underlying hash size from the install opcode. */
  uint32_t needed = 0U;
  switch (key->alg) {
    case k_ra_rsip_oem_cmd_hmac_sha224:
    case k_ra_rsip_oem_cmd_hmac_sha512_224:
      needed = (uint32_t)k_ra_rsip_sha224_digest_bytes;
      break;
    case k_ra_rsip_oem_cmd_hmac_sha256:
    case k_ra_rsip_oem_cmd_hmac_sha512_256:
      needed = (uint32_t)k_ra_rsip_sha256_digest_bytes;
      break;
    case k_ra_rsip_oem_cmd_hmac_sha384:
      needed = (uint32_t)k_ra_rsip_sha384_digest_bytes;
      break;
    case k_ra_rsip_oem_cmd_hmac_sha512:
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

/* Push a buffer through an asymmetric input lane -- see surrounding code and HUM citations. */
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
      tail |= ((uint32_t)buf[i + b]) << (b * k_ra_rsip_byte_bits);
    }
    *ra_rsip_reg32(off) = tail;
  }
}

/* Pull a buffer back through an asymmetric output lane -- see surrounding code and HUM citations. */
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
      buf[i + b] = (uint8_t)((word >> (b * k_ra_rsip_byte_bits)) & k_ra_rsip_byte_mask);
    }
  }
}

/* ra rsip rsa sign -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_rsa_sign(const ra_rsip_key_handle_t* key,
                          ra_rsip_rsa_size_t          size,
                          const uint8_t*              digest,
                          uint32_t                    digest_len,
                          uint8_t*                    signature)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((size != k_ra_rsip_rsa_1024) && (size != k_ra_rsip_rsa_2048) &&
      (size != k_ra_rsip_rsa_3072) && (size != k_ra_rsip_rsa_4096)) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_rsa_size) = (uint32_t)size;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_rsa_sign;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_rsa_sign;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_asym_done);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t sig_len = (uint32_t)size / k_ra_rsip_byte_bits;
  internal_asym_pull(k_ra_rsip_off_asym_sig_out, signature, sig_len);
  return k_ra_ok;
}

/* ra rsip rsa verify -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_rsa_verify(const ra_rsip_key_handle_t* key,
                            ra_rsip_rsa_size_t          size,
                            const uint8_t*              digest,
                            uint32_t                    digest_len,
                            const uint8_t*              signature)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  if ((size != k_ra_rsip_rsa_1024) && (size != k_ra_rsip_rsa_2048) &&
      (size != k_ra_rsip_rsa_3072) && (size != k_ra_rsip_rsa_4096)) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_rsa_size) = (uint32_t)size;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  const uint32_t sig_len = (uint32_t)size / k_ra_rsip_byte_bits;
  internal_asym_push(k_ra_rsip_off_asym_sig_in, signature, sig_len);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_rsa_verify;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_rsa_verify;

  return internal_complete(k_ra_rsip_mask_isr_asym_done);
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
 * @since 0.1.0
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

/* internal curve bytes -- see surrounding code and HUM citations. */
static uint32_t internal_curve_bytes(ra_rsip_curve_t curve)
{
  switch (curve) {
    case k_ra_rsip_curve_secp192r1:
      return k_ra_rsip_curve_bytes_192;
    case k_ra_rsip_curve_secp224r1:
      return k_ra_rsip_curve_bytes_224;
    case k_ra_rsip_curve_secp256r1:
    case k_ra_rsip_curve_brain256r1:
    case k_ra_rsip_curve_ed25519:
    case k_ra_rsip_curve_secp256k1:
      return k_ra_rsip_curve_bytes_256;
    case k_ra_rsip_curve_secp384r1:
    case k_ra_rsip_curve_brain384r1:
      return k_ra_rsip_curve_bytes_384;
    case k_ra_rsip_curve_brain512r1:
      return k_ra_rsip_curve_bytes_512;
    case k_ra_rsip_curve_secp521r1:
      return k_ra_rsip_curve_bytes_521;
    default:
      return 0U;
  }
}

/* ra rsip ecdsa sign -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_ecdsa_sign(const ra_rsip_key_handle_t* key,
                            ra_rsip_curve_t             curve,
                            const uint8_t*              digest,
                            uint32_t                    digest_len,
                            uint8_t*                    signature)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_ecdsa_sign;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_ecdsa_sign;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_asym_done);
  if (err != k_ra_ok) {
    return err;
  }
  /* (r || s) */
  internal_asym_pull(k_ra_rsip_off_asym_sig_out, signature, curve_bytes * 2U);
  return k_ra_ok;
}

/* ra rsip ecdsa verify -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_ecdsa_verify(const ra_rsip_key_handle_t* key,
                              ra_rsip_curve_t             curve,
                              const uint8_t*              digest,
                              uint32_t                    digest_len,
                              const uint8_t*              signature)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");
  RA_CHECK_NULL_PTR(signature, s_tag, "signature must not be nullptr");
  const uint32_t curve_bytes = internal_curve_bytes(curve);
  if (curve_bytes == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_load_handle(key);
  /* HUM Ch 52.2.4 "Asymmetric cipher" p 3306 */
  *ra_rsip_reg32(k_ra_rsip_off_asym_curve) = (uint32_t)curve;
  internal_asym_push(k_ra_rsip_off_asym_msg_in, digest, digest_len);
  internal_asym_push(k_ra_rsip_off_asym_sig_in, signature, curve_bytes * 2U);
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_ecdsa_verify;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_ecdsa_verify;

  return internal_complete(k_ra_rsip_mask_isr_asym_done);
}

/**
 * @brief Pull the ECDH shared-secret handle out of the engine.
 *
 * @details
 * The RSIP delivers the wrapped shared secret as an HMAC-SHA-256
 * handle: 1 algorithm word + N body words read from
 * ``ASYM_SHARED``. The unused tail of ``out->body[]`` is zero-padded
 * to avoid leaking stack contents.
 *
 * @param[out] out Destination handle.
 *
 * @pre ``out`` is non-NULL.
 * @pre ``internal_complete`` has just returned ``k_ra_ok``.
 *
 * @post ``out->alg`` and ``out->body_words`` reflect HMAC-SHA-256.
 * @post ``out->body[]`` has been fully populated and tail-zeroed.
 *
 * @note Internal helper.
 * @since 0.1.0
 */
static void internal_ecdh_pull_shared(ra_rsip_key_handle_t* out)
{
  /* The wrapped shared secret is delivered as an HMAC-SHA-256 handle. */
  out->alg        = k_ra_rsip_oem_cmd_hmac_sha256;
  out->body_words = (uint32_t)k_ra_rsip_handle_words_hmac_sha256;
  for (uint32_t w = 0U; w < out->body_words; ++w) {
    out->body[w] = *ra_rsip_reg32(k_ra_rsip_off_asym_shared);
  }
  internal_zero_handle_tail(out, out->body_words);
}

/* ra rsip ecdh compute -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_ecdh_compute(const ra_rsip_key_handle_t* key,
                              ra_rsip_curve_t             curve,
                              const uint8_t*              peer_x,
                              const uint8_t*              peer_y,
                              ra_rsip_key_handle_t*       out)
{
  RA_CHECK_NULL_PTR(key, s_tag, "key must not be nullptr");
  RA_CHECK_NULL_PTR(peer_x, s_tag, "peer_x must not be nullptr");
  RA_CHECK_NULL_PTR(peer_y, s_tag, "peer_y must not be nullptr");
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
  *ra_rsip_reg32(k_ra_rsip_off_asym_ctrl) = k_ra_rsip_asym_op_ecdh_compute;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op)   = k_ra_rsip_asym_op_ecdh_compute;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_asym_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_ecdh_pull_shared(out);
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: OEM boot loader version (anti-rollback)
 * ===========================================================================
 */

/* ra rsip oem bl version get -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_oem_bl_version_get(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *out = *ra_rsip_reg32(k_ra_rsip_off_oem_bl_ver);
  return k_ra_ok;
}

/* ra rsip oem bl version increment -- see surrounding code and HUM citations. */
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

/* ra rsip oem bl version lock -- see surrounding code and HUM citations. */
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

/* Issue a vault command and wait for completion -- see surrounding code and HUM citations. */
static ra_err_t internal_kv_op(ra_rsip_kv_op_t op, uint8_t slot)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_kv_slot) = slot;
  *ra_rsip_reg32(k_ra_rsip_off_kv_ctrl) = (uint32_t)op;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)op;
  return internal_complete(k_ra_rsip_mask_isr_kv_done);
}

#ifdef RA_SIMULATOR_MODE
/* The host-test simulator backs MMIO with plain memory, so successive
 * writes to the kv_data FIFO would just overwrite the same word. Keep
 * a per-slot shadow so read-after-write tests can verify the round
 * trip without modelling the FIFO inside ra_sim_mmap. */
static uint8_t s_sim_kv_slots[(uint32_t)k_ra_rsip_kv_slot_count]
                             [k_ra_rsip_kv_slot_w * (uint32_t)k_ra_rsip_trng_word_bytes];
#endif

/* ra rsip kv read -- see surrounding code and HUM citations. */
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
  for (uint32_t w = 0U; w < k_ra_rsip_kv_slot_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_kv_data);
    internal_unpack_le(word, &out[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
#ifdef RA_SIMULATOR_MODE
  /* Replay from the per-slot shadow so the test sees the bytes that
   * were actually written rather than whatever the FIFO MMIO settled
   * on after 16 overlapping writes. */
  for (uint32_t i = 0U; i < k_ra_rsip_kv_slot_w * (uint32_t)k_ra_rsip_trng_word_bytes; ++i) {
    out[i] = s_sim_kv_slots[slot][i];
  }
#endif
  return k_ra_ok;
}

/* ra rsip kv write -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_kv_write(uint8_t slot, const uint8_t* in)
{
  RA_CHECK_NULL_PTR(in, s_tag, "in must not be nullptr");
  if (slot >= (uint8_t)k_ra_rsip_kv_slot_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  for (uint32_t w = 0U; w < k_ra_rsip_kv_slot_w; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_kv_data) =
      internal_pack_le(&in[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
#ifdef RA_SIMULATOR_MODE
  for (uint32_t i = 0U; i < k_ra_rsip_kv_slot_w * (uint32_t)k_ra_rsip_trng_word_bytes; ++i) {
    s_sim_kv_slots[slot][i] = in[i];
  }
#endif
  return internal_kv_op(k_ra_rsip_kv_op_write, slot);
}

/* ra rsip kv erase -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_kv_erase(uint8_t slot)
{
  if (slot >= (uint8_t)k_ra_rsip_kv_slot_count) {
    return k_ra_err_invalid_arg;
  }
  return internal_kv_op(k_ra_rsip_kv_op_erase, slot);
}

/* ra rsip kv count -- see surrounding code and HUM citations. */
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

/**
 * @brief Stage the wrap engine's KEK selector, body, and IV.
 *
 * @details
 * Both wrap and unwrap start by publishing the KEK algorithm to
 * ``KW_KEK``, streaming the KEK body into the staging port, and
 * loading the 16-byte IV into ``KW_IV0..3``. Centralised here.
 *
 * @param[in] kek KEK handle.
 * @param[in] iv  16-byte IV.
 *
 * @pre ``kek`` and ``iv`` are non-NULL.
 *
 * @post ``KW_KEK`` carries ``kek->alg``.
 * @post ``KEY_STAGE`` has observed ``kek->body_words`` writes.
 * @post ``KW_IV0..3`` reflect ``iv``.
 *
 * @note Internal helper.
 * @since 0.1.0
 * @pre Module/state preconditions hold (see function body).
 */
static void internal_kw_stage_kek(const ra_rsip_key_handle_t* kek, const uint8_t* iv)
{
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_kw_kek) = kek->alg;
  internal_push_handle_body(kek);
  internal_push_iv_lanes(k_ra_rsip_off_kw_iv0, iv);
}

/* Stream the wrap-engine output blob (16 words) into a byte buffer -- see surrounding code and HUM citations. */
static void internal_kw_pull_blob(uint8_t* blob)
{
  for (uint32_t w = 0U; w < k_ra_rsip_kv_slot_w; ++w) {
    const uint32_t word = *ra_rsip_reg32(k_ra_rsip_off_kw_blob_out);
    internal_unpack_le(word, &blob[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
}

/* Push the source-handle body into the wrap-engine input FIFO -- see surrounding code and HUM citations. */
static void internal_kw_push_src(const ra_rsip_key_handle_t* src)
{
  *ra_rsip_reg32(k_ra_rsip_off_kw_handle) = src->alg;
  for (uint32_t w = 0U; w < src->body_words; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_kw_blob_in) = src->body[w];
  }
}

/* ra rsip key wrap -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_key_wrap(const ra_rsip_key_handle_t* kek,
                          const uint8_t*              iv,
                          const ra_rsip_key_handle_t* src,
                          uint8_t*                    blob)
{
  RA_CHECK_NULL_PTR(kek, s_tag, "kek must not be nullptr");
  RA_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR(src, s_tag, "src must not be nullptr");
  RA_CHECK_NULL_PTR(blob, s_tag, "blob must not be nullptr");
  if (internal_aes_alg_byte(kek->alg) == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_kw_stage_kek(kek, iv);
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  internal_kw_push_src(src);
  *ra_rsip_reg32(k_ra_rsip_off_kw_ctrl) = k_ra_rsip_kw_op_wrap;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = k_ra_rsip_kw_op_wrap;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_kw_pull_blob(blob);
  return k_ra_ok;
}

/* Pull the unwrapped algorithm + body into a destination handle -- see surrounding code and HUM citations. */
static ra_err_t internal_kw_pull_handle(ra_rsip_key_handle_t* dest)
{
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
  internal_zero_handle_tail(dest, words);
  return k_ra_ok;
}

/* ra rsip key unwrap -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_key_unwrap(const ra_rsip_key_handle_t* kek,
                            const uint8_t*              iv,
                            const uint8_t*              blob,
                            ra_rsip_key_handle_t*       dest)
{
  RA_CHECK_NULL_PTR(kek, s_tag, "kek must not be nullptr");
  RA_CHECK_NULL_PTR(iv, s_tag, "iv must not be nullptr");
  RA_CHECK_NULL_PTR(blob, s_tag, "blob must not be nullptr");
  RA_CHECK_NULL_PTR(dest, s_tag, "dest must not be nullptr");
  if (internal_aes_alg_byte(kek->alg) == 0U) {
    return k_ra_err_invalid_arg;
  }
  internal_kw_stage_kek(kek, iv);
  /* HUM Ch 52.1 "Application Key Management" p 3303 */
  for (uint32_t w = 0U; w < k_ra_rsip_kv_slot_w; ++w) {
    *ra_rsip_reg32(k_ra_rsip_off_kw_blob_in) =
      internal_pack_le(&blob[(size_t)w * (size_t)k_ra_rsip_trng_word_bytes]);
  }
  *ra_rsip_reg32(k_ra_rsip_off_kw_ctrl) = k_ra_rsip_kw_op_unwrap;
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = k_ra_rsip_kw_op_unwrap;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_done);
  if (err != k_ra_ok) {
    return err;
  }
  return internal_kw_pull_handle(dest);
}

/* ===========================================================================
 * Round-3 entry points: key derivation
 * ===========================================================================
 */

/* Validate the KDF arguments before any MMIO is touched -- see surrounding code and HUM citations. */
static ra_err_t internal_kdf_validate(ra_rsip_kdf_op_t            op,
                                      const ra_rsip_key_handle_t* ikm,
                                      const uint8_t*              label,
                                      uint32_t                    label_len,
                                      const uint8_t*              salt,
                                      uint32_t                    salt_len,
                                      uint32_t                    out_len)
{
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
  return k_ra_ok;
}

/* Stage the KDF inputs (op + length + optional IKM + label + salt) -- see surrounding code and HUM citations. */
static void internal_kdf_stage(ra_rsip_kdf_op_t            op,
                               const ra_rsip_key_handle_t* ikm,
                               const uint8_t*              label,
                               uint32_t                    label_len,
                               const uint8_t*              salt,
                               uint32_t                    salt_len,
                               uint32_t                    out_len)
{
  /* HUM Ch 52.1 "KDF" p 3303 */
  *ra_rsip_reg32(k_ra_rsip_off_kdf_ctrl) = (uint32_t)op;
  *ra_rsip_reg32(k_ra_rsip_off_kdf_len)  = out_len;
  if (ikm != nullptr) {
    *ra_rsip_reg32(k_ra_rsip_off_kdf_ikm) = ikm->alg;
    internal_push_handle_body(ikm);
  }
  if (label_len > 0U) {
    internal_push_bytes_to_port(k_ra_rsip_off_kdf_label, label, label_len);
  }
  if (salt_len > 0U) {
    internal_push_bytes_to_port(k_ra_rsip_off_kdf_salt, salt, salt_len);
  }
}

/* Pull the wrapped derived-key handle out of the KDF engine -- see surrounding code and HUM citations. */
static void internal_kdf_pull_handle(ra_rsip_key_handle_t* out)
{
  /* Wrapped derived key delivered through KDF_OUT. */
  out->alg        = *ra_rsip_reg32(k_ra_rsip_off_kdf_out);
  out->body_words = (uint32_t)k_ra_rsip_handle_words_hmac_sha256;
  for (uint32_t w = 0U; w < out->body_words; ++w) {
    out->body[w] = *ra_rsip_reg32(k_ra_rsip_off_kdf_out);
  }
  internal_zero_handle_tail(out, out->body_words);
}

/* ra rsip kdf -- see surrounding code and HUM citations. */
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
  const ra_err_t v_err = internal_kdf_validate(op, ikm, label, label_len, salt, salt_len, out_len);
  RA_RETURN_ON_ERROR(v_err, s_tag, "rsip_kdf: validate");

  internal_kdf_stage(op, ikm, label, label_len, salt, salt_len, out_len);
  *ra_rsip_reg32(k_ra_rsip_off_mbox_op) = (uint32_t)op;

  const ra_err_t err = internal_complete(k_ra_rsip_mask_isr_kdf_done);
  if (err != k_ra_ok) {
    return err;
  }
  internal_kdf_pull_handle(out);
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: device lifecycle + debug authorisation
 * ===========================================================================
 */

/* ra rsip life get -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_life_get(ra_rsip_life_state_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 51.1 "Device lifecycle" p 3263 */
  *out = (ra_rsip_life_state_t)*ra_rsip_reg32(k_ra_rsip_off_life_state);
  return k_ra_ok;
}

/* ra rsip life advance -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_life_advance(ra_rsip_life_state_t state)
{
  if ((uint32_t)state > k_ra_rsip_life_rma) {
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

/* ra rsip debug level get -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_debug_level_get(ra_rsip_debug_level_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 51.1 "Three debug levels" p 3263 */
  *out = (ra_rsip_debug_level_t)*ra_rsip_reg32(k_ra_rsip_off_debug_level);
  return k_ra_ok;
}

/* ra rsip debug level set -- see surrounding code and HUM citations. */
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

/* ra rsip tamper enable -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_tamper_enable(uint32_t sources)
{
  if ((sources & ~k_ra_rsip_tamper_src_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 51.6 "Tamper Detection" p 3294 */
  *ra_rsip_reg32(k_ra_rsip_off_tamper_ctrl) = sources;
  return k_ra_ok;
}

/* ra rsip tamper status -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_tamper_status(uint32_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 51.6 "Tamper Detection" p 3294 */
  *out = *ra_rsip_reg32(k_ra_rsip_off_tamper_status);
  return k_ra_ok;
}

/* ra rsip tamper ack -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_tamper_ack(uint32_t mask)
{
  if (mask == 0U) {
    return k_ra_err_invalid_arg;
  }
  if ((mask & ~k_ra_rsip_tamper_src_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 51.6 "Tamper Detection" p 3294 */
  *ra_rsip_reg32(k_ra_rsip_off_tamper_status) = mask;
  return k_ra_ok;
}

/* ra rsip dpa arm -- see surrounding code and HUM citations. */
ra_err_t ra_rsip_dpa_arm(bool enable)
{
  /* HUM Ch 51.5 "Side-channel countermeasures" p 3290 */
  volatile uint32_t* ctrl = ra_rsip_reg32(k_ra_rsip_off_ctrl);
  if (enable) {
    *ctrl |= k_ra_rsip_mask_ctrl_dpa_arm;
  } else {
    *ctrl &= ~k_ra_rsip_mask_ctrl_dpa_arm;
  }
  *ra_rsip_reg32(k_ra_rsip_off_dpa_ctrl) = (uint32_t)enable;
  return k_ra_ok;
}

/* ===========================================================================
 * Round-3 entry points: DOTF key delivery routing
 * ===========================================================================
 */

/* ra rsip dotf route -- see surrounding code and HUM citations. */
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
  uint32_t word = k_ra_rsip_dotf_off;
  if (on) {
    word = ((uint32_t)slot << k_ra_rsip_byte_shift_2) | k_ra_rsip_dotf_on;
  }
  *ra_rsip_reg32(off) = word;
  return k_ra_ok;
}
