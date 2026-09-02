/**
 * @file ra8_rsip.c
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
 * ``ra8_fake_mmap``-backed pages and routes the bounded BIST / DONE
 * polls through the ``ra8_fake_mmio`` wait seam (issue #238): an
 * unarmed register satisfies its wait on the first poll, and a test
 * arms ``ra8_fake_mmio_fail_wait`` / ``ra8_fake_mmio_satisfy_after``
 * to drive the timeout / continuation legs of the real loop. The
 * driver itself runs the identical register sequence on every build
 * and never forges an engine-side status bit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_rsip.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_rsip_internal.h"
#include "ra8_rsip_regs.h"

/*
 * Software backend selection. The RSIP-E50D HASH engine has NO documented
 * register interface -- HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" is a 6-page
 * feature overview (p 3302-3307) with no register map -- and the hand-written
 * register I/O path is NON-FUNCTIONAL on silicon: verified on the EK-RA8D2, the
 * RSIP registers read all-zero, writes do not stick, and ra8_rsip_sha256 returns
 * k_ra8_err_hw_timeout with a zero digest (see
 * examples/ek_ra8d2/hil_needs_revalidation/rsip_sha256_kat). Renesas drives the RSIP through
 * FSP's opaque procedural "primitive" sequences, not registers. Until that FSP
 * driver is ported, the software SHA-256 is the ONLY working backend, so it is
 * enabled unconditionally. The register-sequence model is retained (never
 * compiled) behind RA8_RSIP_HASH_HARDWARE as a reference for the future port and
 * for the host register-plumbing tests, which drive it against ra8_fake_mmap.
 */
#ifndef RA8_RSIP_SOFTWARE_BACKEND
/** @brief RA8 RSIP SOFTWARE BACKEND. */
#define RA8_RSIP_SOFTWARE_BACKEND (1)
#endif

static void internal_sw_sha256(const uint8_t* msg, uint32_t msg_len, uint8_t* digest);

/**
 * @var s_tag
 * @brief Logger tag used by every ``ra8_log_*`` call in this TU.
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
 * Updated by ``ra8_rsip_attach_handler`` and read by
 * ``ra8_rsip_dispatch``. There is one shared slot because the
 * RSIP routes every event through one peripheral IRQ line.
 *
 * @warning Do not modify directly; use ``ra8_rsip_attach_handler``.
 * @note Static, file-scope.
 * @since 0.1.0
 */
static ra8_rsip_event_fn_t s_rsip_fn;

/**
 * @var s_rsip_ctx
 * @brief Caller context paired with ``s_rsip_fn``.
 *
 * @warning Do not modify directly; use ``ra8_rsip_attach_handler``.
 * @note Static, file-scope.
 * @since 0.1.0
 */
static void* s_rsip_ctx;

/** @brief Implementation of `priv_wait_bit()` -- bounded MMIO mask spin. */
ra8_err_t priv_wait_bit(ra8_rsip_off_t offset, uint32_t mask)
{
  volatile const uint32_t* reg = ra8_rsip_reg32(offset);
  for (uint32_t i = 0U; i < k_ra8_rsip_poll_budget; ++i) {
#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* Host unit-test MMIO wait seam (tests/mocks/src/ra8_fake_mmio.c): an unarmed
     * register satisfies the wait on its first poll; a test arms fail_wait /
     * satisfy_after to drive the timeout / continuation legs of this loop. */
    if (ra8_fake_mmio_wait_eval(reg, i, ((*reg & mask) == mask))) {
      return k_ra8_ok;
    }
#else
    if ((*reg & mask) == mask) {
      return k_ra8_ok;
    }
#endif
  }
  return k_ra8_err_hw_timeout;
}

/**
 * @brief Arm the BIST and wait for ``STATUS.BIST_OK``.
 *
 * @details
 * Sets ``CTRL.BIST`` and spins on ``STATUS.BIST_OK``, which the
 * access-management circuit asserts once the on-board firmware
 * finishes the self-test. The driver never forges the bit itself:
 * on the host build the bounded wait routes through the
 * ``ra8_fake_mmio`` seam (unarmed = pass on the first poll; a test
 * arms ``ra8_fake_mmio_fail_wait`` to reach the failure leg).
 *
 * @return ``k_ra8_ok`` on pass, ``k_ra8_err_hw_init_failed`` on fail.
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
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 */
RA8_INTERNAL
static ra8_err_t internal_run_bist(void)
{
  volatile uint32_t* ctrl = ra8_rsip_reg32(k_ra8_rsip_off_ctrl);

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Engine self-test gate. */
  *ctrl |= k_ra8_rsip_mask_ctrl_bist;

  const ra8_err_t err = priv_wait_bit(k_ra8_rsip_off_status, k_ra8_rsip_mask_status_bistok);
  if (err != k_ra8_ok) {
    return k_ra8_err_hw_init_failed;
  }

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* BIST is a one-shot trigger; clear it post-pass so CTRL leaves
   * only ENABLE asserted. Leaving BIST=1 would re-arm the self-test
   * sequencer on the next CTRL write on real silicon. */
  *ctrl &= ~k_ra8_rsip_mask_ctrl_bist;

  return k_ra8_ok;
}

#ifdef RA8_RSIP_HASH_HARDWARE /* retained RSIP HASH model, never compiled (see note above) */

/* Stream the SHA-256 message body into the HASH input port -- see surrounding code and HUM citations. */
RA8_INTERNAL
static void internal_sha256_push_msg(const uint8_t* msg, uint32_t msg_len)
{
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Stream message into HASH input port one 32-bit word at a time.
   * The HAL handles partial trailing bytes by zero-extending into
   * a single word. */
  priv_push_bytes_to_port(k_ra8_rsip_off_hash_data_in, msg, msg_len);
}

#endif /* RA8_RSIP_HASH_HARDWARE */

/* priv_hash_wait_done stays compiled: it is shared with ra8_rsip_asym.c. */
ra8_err_t priv_hash_wait_done(void)
{
  /* On hardware the engine raises HASH_STATUS.DONE once it absorbs the
   * trailing block + length; the bounded wait routes through the host
   * ra8_fake_mmio seam inside priv_wait_bit. */
  return priv_wait_bit(k_ra8_rsip_off_hash_status, k_ra8_rsip_mask_isr_done);
}

#ifdef RA8_RSIP_HASH_HARDWARE

/* Read 8 SHA-256 digest words and ack the DONE bit -- see surrounding code and HUM citations. */
RA8_INTERNAL
static void internal_sha256_pull_digest(uint8_t* digest)
{
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Read 8 digest words. */
  for (uint32_t w = 0U; w < (uint32_t)k_ra8_rsip_sha256_digest_words; ++w) {
    /* Computed digest-word offset is a valid HUM-defined register location,
     * not a literal enumerator -- the analyzer can't see that. */
    const ra8_rsip_off_t off =
      (ra8_rsip_off_t)(k_ra8_rsip_off_hash_digest + (uint16_t)(w << k_ra8_rsip_word_shift));
    const uint32_t word = *ra8_rsip_reg32(off);
    priv_unpack_le(word, &digest[w << k_ra8_rsip_word_shift]);
  }
  /* Ack the DONE bit so the next call starts clean. */
  *ra8_rsip_reg32(k_ra8_rsip_off_hash_status) &= ~k_ra8_rsip_mask_isr_done;
}

#endif /* RA8_RSIP_HASH_HARDWARE */

ra8_err_t ra8_rsip_init(const ra8_rsip_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  /* HUM Ch 52.3.2 "Module-Stop Function Setting" p 3307 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_rsip);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "rsip_init: mstp enable");
  /* GCOVR_EXCL_BR_STOP */

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Engine reset + enable mailbox. */
  volatile uint32_t* ctrl = ra8_rsip_reg32(k_ra8_rsip_off_ctrl);
  *ctrl                   = k_ra8_rsip_mask_ctrl_reset;
  *ctrl                   = k_ra8_rsip_mask_ctrl_enable;

  if (cfg->run_bist) {
    const ra8_err_t bist_err = internal_run_bist();
    if (bist_err != k_ra8_ok) {
      (void)ra8_mstp_disable(k_ra8_mstp_rsip);
      return bist_err;
    }
  }

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Ack any pending IRQ bits. */
  *ra8_rsip_reg32(k_ra8_rsip_off_isr) = k_ra8_rsip_mask_isr_all;

  ra8_log_info(s_tag, "rsip_init");
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_deinit(void)
{
  /* HUM Ch 52.3.1 "Software Standby Mode" p 3307 */
  /* Clear ENABLE before gating. */
  *ra8_rsip_reg32(k_ra8_rsip_off_ctrl) = 0U;

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Scrub pending IRQ flags. */
  *ra8_rsip_reg32(k_ra8_rsip_off_isr) = k_ra8_rsip_mask_isr_all;

  s_rsip_fn  = nullptr;
  s_rsip_ctx = nullptr;

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  return ra8_mstp_disable(k_ra8_mstp_rsip);
}

ra8_err_t ra8_rsip_get_status(uint32_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Mailbox STATUS read. */
  *out = *ra8_rsip_reg32(k_ra8_rsip_off_status);
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_clear_status(uint32_t mask)
{
  if ((mask & ~k_ra8_rsip_mask_isr_all) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (mask == 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* W1C ack on the ISR word. */
  *ra8_rsip_reg32(k_ra8_rsip_off_isr) = mask;
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_attach_handler(ra8_rsip_event_fn_t fn, void* ctx)
{
  s_rsip_fn  = fn;
  s_rsip_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_rsip_dispatch(void)
{
  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Snapshot then ack the ISR. */
  volatile uint32_t* isr      = ra8_rsip_reg32(k_ra8_rsip_off_isr);
  const uint32_t     snapshot = *isr;
  if (snapshot == 0U) {
    return;
  }
  const ra8_rsip_event_fn_t fn  = s_rsip_fn;
  void* const               ctx = s_rsip_ctx;
  /* W1C ack: writing the snapshot back clears each pending bit on real
   * hardware. The host-test RAM backing has no W1C semantics, so a test
   * that needs the post-ack state stages the ISR word itself. */
  *isr = snapshot;
  if (fn != nullptr) {
    fn(ctx, snapshot);
  }
}

// `buf` is an output the RA8_RSIP_TRNG_HARDWARE path fills; the fail-closed build
// writes nothing, so clang-tidy cannot see a write and wrongly wants it const.
// NOLINTNEXTLINE(readability-non-const-parameter) -- buf is filled only by the RA8_RSIP_TRNG_HARDWARE path; the fail-closed build writes nothing.
ra8_err_t ra8_rsip_trng_read(uint8_t* buf, uint32_t len)
{
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if (len == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((len & ((uint32_t)k_ra8_rsip_trng_word_bytes - 1U)) != 0U) {
    return k_ra8_err_invalid_arg;
  }

#ifdef RA8_RSIP_TRNG_HARDWARE
  volatile uint32_t*       status = ra8_rsip_reg32(k_ra8_rsip_off_rnd_status);
  volatile const uint32_t* data   = ra8_rsip_reg32(k_ra8_rsip_off_rnd_data);

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Arm TRNG control word. */
  *ra8_rsip_reg32(k_ra8_rsip_off_rnd_ctrl) = k_ra8_rsip_mask_ctrl_enable;

  const uint32_t words = len >> k_ra8_rsip_word_shift;
  for (uint32_t w = 0U; w < words; ++w) {
    /* On hardware the engine asserts READY when a fresh word is
     * available. The host fake has no producer thread, so we
     * pre-assert and re-assert each iteration to keep the spin
     * deterministic. */
    *status |= k_ra8_rsip_mask_status_ready;

    const ra8_err_t wait_err =
      priv_wait_bit(k_ra8_rsip_off_rnd_status, k_ra8_rsip_mask_status_ready);
    if (wait_err != k_ra8_ok) {
      return wait_err;
    }

    const uint32_t word = *data;
    /* HUM Ch 52.1 "Overview" p 3302 */
    /* TRNG output is little-endian. */
    buf[(w << k_ra8_rsip_word_shift) + 0U] = (uint8_t)(word & k_ra8_rsip_byte_mask);
    buf[(w << k_ra8_rsip_word_shift) + 1U] =
      (uint8_t)((word >> k_ra8_rsip_byte_bits) & k_ra8_rsip_byte_mask);
    buf[(w << k_ra8_rsip_word_shift) + 2U] =
      (uint8_t)((word >> k_ra8_rsip_byte_shift_2) & k_ra8_rsip_byte_mask);
    buf[(w << k_ra8_rsip_word_shift) + 3U] =
      (uint8_t)((word >> k_ra8_rsip_byte_shift_3) & k_ra8_rsip_byte_mask);

    /* Clear READY so the next iteration genuinely waits. */
    *status &= ~k_ra8_rsip_mask_status_ready;
  }
  return k_ra8_ok;
#else
  /* The RSIP-E50D TRNG has no documented register interface (HUM Ch 52 is a
   * 6-page feature overview with no register map); the `RND_*` offsets above
   * are invented and do not work on silicon -- the READY bit never asserts.
   * Fail closed with a clear status rather than spin to a timeout or, worse,
   * hand back an all-zero or deterministic value: predictable "entropy" is far
   * more dangerous than an honest error. A real TRNG needs an FSP-derived
   * RSIP primitive sequence; a software PRNG is NOT a substitute here. */
  return k_ra8_err_not_supported;
#endif
}

ra8_err_t ra8_rsip_sha256(const uint8_t* msg, uint32_t msg_len, uint8_t* digest)
{
  RA8_CHECK_NULL_PTR(msg, s_tag, "msg must not be nullptr");
  RA8_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");

#ifdef RA8_RSIP_HASH_HARDWARE
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Real silicon command-issue sequence (NON-FUNCTIONAL on this silicon; never
   * compiled -- see the backend note near the top of the file):
   *   1. poll HASH_STATUS.READY (engine quiescent);
   *   2. write algorithm selector to HASH_CTRL;
   *   3. stream message words through HASH_DATA_IN;
   *   4. spin on HASH_STATUS.DONE;
   *   5. drain 8 digest words from HASH_DIGEST. */
  *ra8_rsip_reg32(k_ra8_rsip_off_hash_status) |= k_ra8_rsip_mask_status_ready;
  const ra8_err_t ready_err =
    priv_wait_bit(k_ra8_rsip_off_hash_status, k_ra8_rsip_mask_status_ready);
  RA8_RETURN_ON_ERROR(ready_err, s_tag, "rsip_sha256: hash ready");

  /* HASH algorithm select. */
  *ra8_rsip_reg32(k_ra8_rsip_off_hash_ctrl) = k_ra8_rsip_hash_sha256;

  internal_sha256_push_msg(msg, msg_len);

  const ra8_err_t wait_err = priv_hash_wait_done();
  RA8_RETURN_ON_ERROR(wait_err, s_tag, "rsip_sha256: hash done");

  internal_sha256_pull_digest(digest);
  return k_ra8_ok;
#else
  /* The RSIP HASH hardware has no usable register interface, so compute the
   * digest in software. This is the path used on silicon and on the host; it is
   * what makes ra8_rot's on-silicon image digest work. */
  internal_sw_sha256(msg, msg_len, digest);
  return k_ra8_ok;
#endif
}

/* =============================================================================
 * SHA-256 incremental
 * =============================================================================
 */

#ifdef RA8_RSIP_SOFTWARE_BACKEND

/**
 * @enum ra8_rsip_sw_sha256_t
 * @brief File-private constants for the software SHA-256 fall-back.
 *
 * @details
 * Used only when ``RA8_RSIP_SOFTWARE_BACKEND`` is defined. Values are
 * straight FIPS PUB 180-4 Section 4.2.2 / 6.2.1 references.
 */
typedef enum : uint32_t {
  k_ra8_rsip_sw_sha256_block_w   = 16U,   /**< 64-byte block = 16 words.     */
  k_ra8_rsip_sw_sha256_round_cnt = 64U,   /**< Sched + compression rounds.   */
  k_ra8_rsip_sw_sha256_state_w   = 8U,    /**< 8 working-state words.        */
  k_ra8_rsip_sw_sha256_len_bytes = 8U,    /**< 64-bit length encoding tail.  */
  k_ra8_rsip_sw_sha256_pad_byte  = 0x80U, /**< RFC 6234 / FIPS 180-4 marker. */
  k_ra8_rsip_sw_sha256_w_back_2  = 2U,    /**< W[i-2]  schedule lookback.    */
  k_ra8_rsip_sw_sha256_w_back_7  = 7U,    /**< W[i-7]  schedule lookback.    */
  k_ra8_rsip_sw_sha256_w_back_15 = 15U,   /**< W[i-15] schedule lookback.    */
  k_ra8_rsip_sw_sha256_w_back_16 = 16U,   /**< W[i-16] schedule lookback.    */
  k_ra8_rsip_sw_rotr_2           = 2U,    /**< RA8 rsip sw rotr 2.           */
  k_ra8_rsip_sw_rotr_3           = 3U,    /**< RA8 rsip sw rotr 3.           */
  k_ra8_rsip_sw_rotr_6           = 6U,    /**< RA8 rsip sw rotr 6.           */
  k_ra8_rsip_sw_rotr_7           = 7U,    /**< RA8 rsip sw rotr 7.           */
  k_ra8_rsip_sw_rotr_10          = 10U,   /**< RA8 rsip sw rotr 10.          */
  k_ra8_rsip_sw_rotr_11          = 11U,   /**< RA8 rsip sw rotr 11.          */
  k_ra8_rsip_sw_rotr_13          = 13U,   /**< RA8 rsip sw rotr 13.          */
  k_ra8_rsip_sw_rotr_17          = 17U,   /**< RA8 rsip sw rotr 17.          */
  k_ra8_rsip_sw_rotr_18          = 18U,   /**< RA8 rsip sw rotr 18.          */
  k_ra8_rsip_sw_rotr_19          = 19U,   /**< RA8 rsip sw rotr 19.          */
  k_ra8_rsip_sw_rotr_22          = 22U,   /**< RA8 rsip sw rotr 22.          */
  k_ra8_rsip_sw_rotr_25          = 25U,   /**< RA8 rsip sw rotr 25.          */
  k_ra8_rsip_sw_word_bits        = 32U,   /**< Word width in bits.           */
} ra8_rsip_sw_sha256_t;

/* 32-bit right-rotate -- see surrounding code and HUM citations. */
RA8_INTERNAL
static inline uint32_t internal_sw_rotr(uint32_t x, uint32_t n)
{
  return (x >> n) | (x << (k_ra8_rsip_sw_word_bits - n));
}

// clang-format off: FIPS 180-4 K[0..63] table, four constants per row.
/**
 * @brief FIPS PUB 180-4 Section 4.1.2 SHA-256 round constants K[0..63].
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const uint32_t s_sw_sha256_k[k_ra8_rsip_sw_sha256_round_cnt] = {
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
static const uint32_t s_sw_sha256_h0[k_ra8_rsip_sw_sha256_state_w] = {
  0x6a09e667UL, 0xbb67ae85UL, 0x3c6ef372UL, 0xa54ff53aUL,
  0x510e527fUL, 0x9b05688cUL, 0x1f83d9abUL, 0x5be0cd19UL,
};
// clang-format on

/* Build the 64-word SHA-256 message schedule from a 64-byte block -- see surrounding code and HUM citations. */
RA8_INTERNAL
static void internal_sw_sha256_schedule(uint32_t      w[k_ra8_rsip_sw_sha256_round_cnt],
                                        const uint8_t block[k_ra8_rsip_sha256_block])
{
  for (uint32_t i = 0U; i < k_ra8_rsip_sw_sha256_block_w; ++i) {
    const size_t base = (size_t)i * (size_t)k_ra8_rsip_trng_word_bytes;
    w[i] = ((uint32_t)block[base] << k_ra8_rsip_byte_shift_3) |
           ((uint32_t)block[base + 1U] << k_ra8_rsip_byte_shift_2) |
           ((uint32_t)block[base + 2U] << k_ra8_rsip_byte_bits) | (uint32_t)block[base + 3U];
  }
  for (uint32_t i = k_ra8_rsip_sw_sha256_block_w; i < k_ra8_rsip_sw_sha256_round_cnt; ++i) {
    const uint32_t back15 = w[i - k_ra8_rsip_sw_sha256_w_back_15];
    const uint32_t back2  = w[i - k_ra8_rsip_sw_sha256_w_back_2];
    const uint32_t s0     = internal_sw_rotr(back15, k_ra8_rsip_sw_rotr_7) ^
                            internal_sw_rotr(back15, k_ra8_rsip_sw_rotr_18) ^
                            (back15 >> k_ra8_rsip_sw_rotr_3);
    const uint32_t s1     = internal_sw_rotr(back2, k_ra8_rsip_sw_rotr_17) ^
                            internal_sw_rotr(back2, k_ra8_rsip_sw_rotr_19) ^
                            (back2 >> k_ra8_rsip_sw_rotr_10);
    w[i] = w[i - k_ra8_rsip_sw_sha256_w_back_16] + s0 + w[i - k_ra8_rsip_sw_sha256_w_back_7] + s1;
  }
}

/* 64-round SHA-256 main loop running on an a -- see surrounding code and HUM citations. */
/** @brief FIPS 180-4 6.2.2 SHA-256 working-state lane indices a..h. */
typedef enum : uint8_t {
  k_sha256_lane_a = 0U, /**< Sha256 lane a. */
  k_sha256_lane_b = 1U, /**< Sha256 lane b. */
  k_sha256_lane_c = 2U, /**< Sha256 lane c. */
  k_sha256_lane_d = 3U, /**< Sha256 lane d. */
  k_sha256_lane_e = 4U, /**< Sha256 lane e. */
  k_sha256_lane_f = 5U, /**< Sha256 lane f. */
  k_sha256_lane_g = 6U, /**< Sha256 lane g. */
  k_sha256_lane_h = 7U, /**< Sha256 lane h. */
} sha256_lane_t;

/**
 * @brief Run the 64-round SHA-256 compression loop over one message schedule.
 *
 * @details
 * FIPS PUB 180-4 Section 6.2.2: copies the eight working-state lanes a..h out
 * of the hash state, iterates the round function across the expanded message
 * schedule plus the round constants, and folds the results back into the state
 * in place. Pure software fallback used when the RSIP hardware path is
 * unavailable.
 *
 * @param[in,out] s  Eight-word SHA-256 hash state (lanes a..h), updated in place.
 * @param[in]     w  Expanded 64-word message schedule for the current block.
 *
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_sw_sha256_rounds(uint32_t       s[k_ra8_rsip_sw_sha256_state_w],
                                      const uint32_t w[k_ra8_rsip_sw_sha256_round_cnt])
{
  /* FIPS PUB 180-4 Section 6.2.2: a..h working-state lanes are spec-named
   * indices 0..7 of the 8-word hash state, not arbitrary literals. */
  uint32_t a = s[k_sha256_lane_a];
  uint32_t b = s[k_sha256_lane_b];
  uint32_t c = s[k_sha256_lane_c];
  uint32_t d = s[k_sha256_lane_d];
  uint32_t e = s[k_sha256_lane_e];
  uint32_t f = s[k_sha256_lane_f];
  uint32_t g = s[k_sha256_lane_g];
  uint32_t h = s[k_sha256_lane_h];
  for (uint32_t i = 0U; i < k_ra8_rsip_sw_sha256_round_cnt; ++i) {
    const uint32_t s1    = internal_sw_rotr(e, k_ra8_rsip_sw_rotr_6) ^
                           internal_sw_rotr(e, k_ra8_rsip_sw_rotr_11) ^
                           internal_sw_rotr(e, k_ra8_rsip_sw_rotr_25);
    const uint32_t ch    = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + s1 + ch + s_sw_sha256_k[i] + w[i];
    const uint32_t s0    = internal_sw_rotr(a, k_ra8_rsip_sw_rotr_2) ^
                           internal_sw_rotr(a, k_ra8_rsip_sw_rotr_13) ^
                           internal_sw_rotr(a, k_ra8_rsip_sw_rotr_22);
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
  s[k_sha256_lane_a] = a;
  s[k_sha256_lane_b] = b;
  s[k_sha256_lane_c] = c;
  s[k_sha256_lane_d] = d;
  s[k_sha256_lane_e] = e;
  s[k_sha256_lane_f] = f;
  s[k_sha256_lane_g] = g;
  s[k_sha256_lane_h] = h;
}

/* Run a single 64-byte SHA-256 compression block -- see surrounding code and HUM citations. */
RA8_INTERNAL
static void internal_sw_sha256_compress(uint32_t      state[k_ra8_rsip_sw_sha256_state_w],
                                        const uint8_t block[k_ra8_rsip_sha256_block])
{
  uint32_t w[k_ra8_rsip_sw_sha256_round_cnt];
  internal_sw_sha256_schedule(w, block);

  uint32_t working[k_ra8_rsip_sw_sha256_state_w];
  for (uint32_t i = 0U; i < k_ra8_rsip_sw_sha256_state_w; ++i) {
    working[i] = state[i];
  }
  internal_sw_sha256_rounds(working, w);
  for (uint32_t i = 0U; i < k_ra8_rsip_sw_sha256_state_w; ++i) {
    state[i] += working[i];
  }
}

/**
 * @brief Finalize one streaming SHA-256 chaining state.
 *
 * @details Appends the FIPS 180-4 marker, zero padding, and big-endian
 * 64-bit message length, compressing one or two final blocks as required.
 *
 * @param[in,out] ctx Initialized streaming context to pad and compress.
 *
 * @pre ``ctx`` is non-NULL and initialized by ::ra8_rsip_sha256_init.
 * @pre ``ctx->used`` is less than ::k_ra8_rsip_sha256_block.
 *
 * @post ``ctx->state`` contains the final chaining words.
 * @post No storage outside ``ctx`` is modified.
 *
 * @note File-local helper; the caller emits and clears the resulting state.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_sw_sha256_finalize(ra8_rsip_sha256_ctx_t* ctx)
{
  const uint64_t bit_len  = ctx->total_bytes * (uint64_t)k_ra8_rsip_byte_bits;
  ctx->block[ctx->used++] = (uint8_t)k_ra8_rsip_sw_sha256_pad_byte;
  if (ctx->used > ((uint32_t)k_ra8_rsip_sha256_block - k_ra8_rsip_sw_sha256_len_bytes)) {
    while (ctx->used < k_ra8_rsip_sha256_block) {
      ctx->block[ctx->used++] = 0U;
    }
    internal_sw_sha256_compress(ctx->state, ctx->block);
    ctx->used = 0U;
  }
  while (ctx->used < ((uint32_t)k_ra8_rsip_sha256_block - k_ra8_rsip_sw_sha256_len_bytes)) {
    ctx->block[ctx->used++] = 0U;
  }
  for (uint32_t b = 0U; b < k_ra8_rsip_sw_sha256_len_bytes; ++b) {
    const uint32_t shift = (k_ra8_rsip_sw_sha256_len_bytes - 1U - b) * k_ra8_rsip_byte_bits;
    ctx->block[(uint32_t)k_ra8_rsip_sha256_block - k_ra8_rsip_sw_sha256_len_bytes + b] =
      (uint8_t)((bit_len >> shift) & (uint64_t)k_ra8_rsip_byte_mask);
  }
  internal_sw_sha256_compress(ctx->state, ctx->block);
}

/* Emit a big-endian 32-byte SHA-256 digest from working state -- see surrounding code and HUM citations. */
RA8_INTERNAL
static void internal_sw_sha256_emit(const uint32_t state[k_ra8_rsip_sw_sha256_state_w],
                                    uint8_t*       digest)
{
  for (uint32_t w = 0U; w < k_ra8_rsip_sw_sha256_state_w; ++w) {
    const size_t base = (size_t)w * (size_t)k_ra8_rsip_trng_word_bytes;
    digest[base + 0U] = (uint8_t)((state[w] >> k_ra8_rsip_byte_shift_3) & k_ra8_rsip_byte_mask);
    digest[base + 1U] = (uint8_t)((state[w] >> k_ra8_rsip_byte_shift_2) & k_ra8_rsip_byte_mask);
    digest[base + 2U] = (uint8_t)((state[w] >> k_ra8_rsip_byte_bits) & k_ra8_rsip_byte_mask);
    digest[base + 3U] = (uint8_t)(state[w] & k_ra8_rsip_byte_mask);
  }
}

/* One-shot software SHA-256 over a contiguous buffer -- see surrounding code and HUM citations. */
/**
 * @brief One-shot software SHA-256 over a contiguous message buffer.
 *
 * @details
 * Pure-software FIPS PUB 180-4 SHA-256 and the only working RSIP HASH backend:
 * seeds the eight-word hash state from the standard initial constants
 * (``s_sw_sha256_h0``), folds in each full 64-byte block through
 * ``internal_sw_sha256_compress``, length-pads and compresses the trailing
 * partial block via ``internal_sw_sha256_pad``, then serialises the state as a
 * big-endian 32-byte digest with ``internal_sw_sha256_emit``. Compiled under
 * ``RA8_RSIP_SOFTWARE_BACKEND``; a forward declaration near the top of the file
 * lets the earlier ``ra8_rsip_sha256`` dispatch to it so the RoT image digest
 * works on silicon.
 *
 * @param[in]  msg     Message bytes to hash; read-only, ``msg_len`` bytes long.
 * @param[in]  msg_len Message length in bytes.
 * @param[out] digest  Output buffer receiving the 32-byte big-endian digest.
 *
 * @pre ``msg`` points to at least ``msg_len`` readable bytes.
 * @pre ``digest`` points to at least 32 writable bytes.
 *
 * @post ``digest`` holds the SHA-256 of ``msg[0 .. msg_len)``.
 * @post The input buffer ``msg`` is left unmodified.
 *
 * @note Re-entrant: all working state lives on the caller's stack and the only
 *       global read (``s_sw_sha256_h0``) is const, so concurrent calls on
 *       disjoint buffers do not interfere.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_sw_sha256(const uint8_t* msg, uint32_t msg_len, uint8_t* digest)
{
  ra8_rsip_sha256_ctx_t ctx = {};
  (void)ra8_rsip_sha256_init(&ctx);
  (void)ra8_rsip_sha256_update(&ctx, msg, msg_len);
  (void)ra8_rsip_sha256_final(&ctx, digest);
}

#endif /* RA8_RSIP_SOFTWARE_BACKEND */

/* Compute SHA-256 of a buffer routed by the active backend -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t internal_sha256_dispatch(const uint8_t* msg, uint32_t msg_len, uint8_t* digest)
{
#ifdef RA8_RSIP_SOFTWARE_BACKEND
  internal_sw_sha256(msg, msg_len, digest);
  return k_ra8_ok;
#else
  static const uint8_t s_empty = 0U;
  return ra8_rsip_sha256((msg == nullptr) ? &s_empty : msg, msg_len, digest);
#endif
}

ra8_err_t ra8_rsip_sha256_init(ra8_rsip_sha256_ctx_t* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  for (uint32_t i = 0U; i < k_ra8_rsip_sha256_state_words; ++i) {
    ctx->state[i] = s_sw_sha256_h0[i];
  }
  ctx->used        = 0U;
  ctx->total_bytes = 0U;
  ctx->initialized = 1U;
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_sha256_update(ra8_rsip_sha256_ctx_t* ctx, const uint8_t* data, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  if ((data == nullptr) && (len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  if (ctx->initialized != 1U) {
    return k_ra8_err_invalid_state;
  }
  if (len == 0U) {
    return k_ra8_ok;
  }
  const uint64_t max_bytes = UINT64_MAX / (uint64_t)k_ra8_rsip_byte_bits;
  if ((ctx->total_bytes > max_bytes) || ((uint64_t)len > (max_bytes - ctx->total_bytes))) {
    return k_ra8_err_invalid_size;
  }
  uint32_t consumed = 0U;
  while ((ctx->used != 0U) && (consumed < len)) {
    ctx->block[ctx->used++] = data[consumed++];
    if (ctx->used == k_ra8_rsip_sha256_block) {
      internal_sw_sha256_compress(ctx->state, ctx->block);
      ctx->used = 0U;
    }
  }
  while ((len - consumed) >= k_ra8_rsip_sha256_block) {
    internal_sw_sha256_compress(ctx->state, &data[consumed]);
    consumed += k_ra8_rsip_sha256_block;
  }
  while (consumed < len) {
    ctx->block[ctx->used++] = data[consumed++];
  }
  ctx->total_bytes += len;
  return k_ra8_ok;
}

ra8_err_t ra8_rsip_sha256_final(ra8_rsip_sha256_ctx_t* ctx, uint8_t* digest_out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(digest_out, s_tag, "digest_out must not be nullptr");
  if (ctx->initialized != 1U) {
    return k_ra8_err_invalid_state;
  }
  internal_sw_sha256_finalize(ctx);
  internal_sw_sha256_emit(ctx->state, digest_out);
  for (uint32_t i = 0U; i < k_ra8_rsip_sha256_state_words; ++i) {
    ctx->state[i] = 0U;
  }
  for (uint32_t i = 0U; i < k_ra8_rsip_sha256_block; ++i) {
    ctx->block[i] = 0U;
  }
  ctx->total_bytes = 0U;
  ctx->used        = 0U;
  ctx->initialized = 0U;
  return k_ra8_ok;
}

/* =============================================================================
 * HMAC-SHA-256 incremental
 * =============================================================================
 */

/* Build the 64-byte HMAC key block per RFC 2104 Section 2 -- see surrounding code and HUM citations. */
RA8_INTERNAL
static ra8_err_t
internal_hmac_prep_key(const uint8_t* key, uint32_t key_len, uint8_t block[k_ra8_rsip_sha256_block])
{
  for (uint32_t i = 0U; i < k_ra8_rsip_sha256_block; ++i) {
    block[i] = 0x00U;
  }
  if (key_len > k_ra8_rsip_sha256_block) {
    uint8_t         digest[k_ra8_rsip_sha256_digest_bytes] = {};
    const ra8_err_t err = internal_sha256_dispatch(key, key_len, digest);
    if (err != k_ra8_ok) {
      return err;
    }
    for (uint32_t i = 0U; i < k_ra8_rsip_sha256_digest_bytes; ++i) {
      block[i] = digest[i];
    }
  } else if (key_len > 0U) {
    for (uint32_t i = 0U; i < key_len; ++i) {
      block[i] = key[i];
    }
  }
  return k_ra8_ok;
}

ra8_err_t
ra8_rsip_hmac_sha256_init(ra8_rsip_hmac_sha256_ctx_t* ctx, const uint8_t* key, uint32_t key_len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  if ((key == nullptr) && (key_len != 0U)) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t prep_err = internal_hmac_prep_key(key, key_len, ctx->key_block);
  if (prep_err != k_ra8_ok) {
    return prep_err;
  }
  const ra8_err_t init_err = ra8_rsip_sha256_init(&ctx->inner);
  if (init_err != k_ra8_ok) {
    return init_err;
  }
  uint8_t ipad[k_ra8_rsip_sha256_block];
  for (uint32_t i = 0U; i < k_ra8_rsip_sha256_block; ++i) {
    ipad[i] = ctx->key_block[i] ^ (uint8_t)k_ra8_rsip_hmac_inner_pad;
  }
  const ra8_err_t upd_err = ra8_rsip_sha256_update(&ctx->inner, ipad, k_ra8_rsip_sha256_block);
  if (upd_err != k_ra8_ok) {
    ctx->inner.initialized = 0U;
    return upd_err;
  }
  ctx->initialized = 1U;
  return k_ra8_ok;
}

ra8_err_t
ra8_rsip_hmac_sha256_update(ra8_rsip_hmac_sha256_ctx_t* ctx, const uint8_t* data, uint32_t len)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  if (ctx->initialized != 1U) {
    return k_ra8_err_invalid_state;
  }
  return ra8_rsip_sha256_update(&ctx->inner, data, len);
}

/**
 * @brief Compute ``SHA256(K_opad || inner_digest)`` for HMAC.
 *
 * @details
 * Uses a stack-local 96-byte buffer (K_opad + inner_digest = 64 + 32)
 * directly through ``internal_sha256_dispatch``. The dispatcher's streaming
 * context retains only one partial SHA-256 block, so this composition remains
 * comfortably inside the firmware's 2200-byte stack ceiling.
 *
 * @param[in]  key_block 64-byte prepared HMAC key block.
 * @param[in]  inner     32-byte inner-hash digest.
 * @param[out] mac_out   32-byte MAC output buffer.
 *
 * @return ``ra8_err_t`` propagated from the outer SHA pass.
 *
 * @pre All pointers are non-NULL.
 *
 * @post On success ``mac_out`` is the HMAC.
 *
 * @note Internal helper.
 * @since 0.1.0
 * @retval k_ra8_ok Operation completed successfully.
 * @retval other Non-zero error code from the underlying operation.
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 */
RA8_INTERNAL
static ra8_err_t internal_hmac_outer(const uint8_t key_block[k_ra8_rsip_sha256_block],
                                     const uint8_t inner[k_ra8_rsip_sha256_digest_bytes],
                                     uint8_t*      mac_out)
{
  uint8_t outer_buf[(size_t)k_ra8_rsip_sha256_block + (size_t)k_ra8_rsip_sha256_digest_bytes];
  for (uint32_t i = 0U; i < k_ra8_rsip_sha256_block; ++i) {
    outer_buf[i] = key_block[i] ^ (uint8_t)k_ra8_rsip_hmac_outer_pad;
  }
  for (uint32_t i = 0U; i < k_ra8_rsip_sha256_digest_bytes; ++i) {
    outer_buf[k_ra8_rsip_sha256_block + i] = inner[i];
  }
  return internal_sha256_dispatch(outer_buf,
                                  (size_t)k_ra8_rsip_sha256_block +
                                    (size_t)k_ra8_rsip_sha256_digest_bytes,
                                  mac_out);
}

ra8_err_t ra8_rsip_hmac_sha256_final(ra8_rsip_hmac_sha256_ctx_t* ctx, uint8_t* mac_out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(mac_out, s_tag, "mac_out must not be nullptr");
  if (ctx->initialized != 1U) {
    return k_ra8_err_invalid_state;
  }
  uint8_t         inner_digest[k_ra8_rsip_sha256_digest_bytes] = {};
  const ra8_err_t inner_err = ra8_rsip_sha256_final(&ctx->inner, inner_digest);
  ra8_err_t       result    = inner_err;
  if (inner_err == k_ra8_ok) {
    result = internal_hmac_outer(ctx->key_block, inner_digest, mac_out);
  }
  ctx->initialized = 0U;
  for (uint32_t i = 0U; i < k_ra8_rsip_sha256_block; ++i) {
    ctx->key_block[i] = 0x00U;
  }
  return result;
}

ra8_err_t ra8_rsip_enter_stop(void)
{
  /* HUM Ch 52.3.1 "Software Standby Mode" p 3307 */
  /* Engine MUST be idle before standby. Clear ENABLE then gate. */
  *ra8_rsip_reg32(k_ra8_rsip_off_ctrl) = 0U;
  return ra8_mstp_disable(k_ra8_mstp_rsip);
}

ra8_err_t ra8_rsip_exit_stop(void)
{
  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_rsip);
  /* GCOVR_EXCL_BR_START -- MSTP HW readback */
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "rsip_exit_stop: mstp enable");
  /* GCOVR_EXCL_BR_STOP */

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* Engine re-enable then BIST. */
  *ra8_rsip_reg32(k_ra8_rsip_off_ctrl) = k_ra8_rsip_mask_ctrl_enable;

  const ra8_err_t bist_err = internal_run_bist();
  if (bist_err != k_ra8_ok) {
    (void)ra8_mstp_disable(k_ra8_mstp_rsip);
    return bist_err;
  }
  return k_ra8_ok;
}
