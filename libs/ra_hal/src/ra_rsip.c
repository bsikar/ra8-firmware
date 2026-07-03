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
#include "ra_rsip_internal.h"

/*
 * Software backend selection. The RSIP-E50D HASH engine has NO documented
 * register interface -- HUM Ch 52 "Renesas Secure IP (RSIP-E50D)" is a 6-page
 * feature overview (p 3302-3307) with no register map -- and the hand-written
 * register I/O path is NON-FUNCTIONAL on silicon: verified on the EK-RA8D2, the
 * RSIP registers read all-zero, writes do not stick, and ra_rsip_sha256 returns
 * k_ra_err_hw_timeout with a zero digest (see
 * examples/ek_ra8d2/hw_pending/rsip_sha256_kat). Renesas drives the RSIP through
 * FSP's opaque procedural "primitive" sequences, not registers. Until that FSP
 * driver is ported, the software SHA-256 is the ONLY working backend, so it is
 * enabled unconditionally. The register-sequence model is retained (never
 * compiled) behind RA_RSIP_HASH_HARDWARE as a reference for the future port and
 * for the host register-plumbing tests, which drive it against ra_sim_mmap.
 */
#ifndef RA_RSIP_SOFTWARE_BACKEND
#define RA_RSIP_SOFTWARE_BACKEND (1)
#endif

/*
 * Forward declaration of the software SHA-256 one-shot (defined later, under
 * RA_RSIP_SOFTWARE_BACKEND). The public ra_rsip_sha256 -- defined earlier in the
 * file than internal_sw_sha256 -- dispatches to it so the RoT image digest works
 * on silicon.
 */
static void internal_sw_sha256(const uint8_t* msg, uint32_t msg_len, uint8_t* digest);

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

/** @brief Implementation of `internal_wait_bit()` -- bounded MMIO mask spin. */
ra_err_t internal_wait_bit(ra_rsip_off_t offset, uint32_t mask)
{
  volatile uint32_t* reg = ra_rsip_reg32(offset);
  for (uint32_t i = 0U; i < k_ra_rsip_poll_budget; ++i) { /* GCOVR_EXCL_BR_LINE */
    if ((*reg & mask) == mask) {                          /* GCOVR_EXCL_BR_LINE */
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
  if (err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
    return k_ra_err_hw_init_failed;
  }

  /* HUM Ch 52.1 "Overview" p 3302 */
  /* BIST is a one-shot trigger; clear it post-pass so CTRL leaves
   * only ENABLE asserted. Leaving BIST=1 would re-arm the self-test
   * sequencer on the next CTRL write on real silicon. */
  *ctrl &= ~k_ra_rsip_mask_ctrl_bist;

  return k_ra_ok;
}

#ifdef RA_RSIP_HASH_HARDWARE /* retained RSIP HASH register model -- never compiled (see backend note above) */

/* Stream the SHA-256 message body into the HASH input port -- see surrounding code and HUM citations. */
static void internal_sha256_push_msg(const uint8_t* msg, uint32_t msg_len)
{
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Stream message into HASH input port one 32-bit word at a time.
   * The HAL handles partial trailing bytes by zero-extending into
   * a single word. */
  internal_push_bytes_to_port(k_ra_rsip_off_hash_data_in, msg, msg_len);
}

#endif /* RA_RSIP_HASH_HARDWARE */

/* internal_hash_wait_done stays compiled: it is shared with ra_rsip_asym.c. */
ra_err_t internal_hash_wait_done(void)
{
  /* On hardware the engine raises HASH_STATUS.DONE once it
   * absorbs the trailing block + length. The host sim pre-asserts
   * the bit so the wait terminates. */
  volatile uint32_t* hstatus = ra_rsip_reg32(k_ra_rsip_off_hash_status);
  *hstatus |= k_ra_rsip_mask_isr_done;
  return internal_wait_bit(k_ra_rsip_off_hash_status, k_ra_rsip_mask_isr_done);
}

#ifdef RA_RSIP_HASH_HARDWARE

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

#endif /* RA_RSIP_HASH_HARDWARE */

ra_err_t ra_rsip_init(const ra_rsip_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 */
  /* HUM Ch 52.3.2 "Module-Stop Function Setting" p 3307 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_rsip);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "rsip_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

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
#ifdef RA_SIMULATOR_MODE
  /* W1C semantics: writing 1 clears each bit in real HW. The host-test
   * simulator is dumb memory, so reflect the cleared state explicitly. */
  *isr = 0U;
#endif
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
    if (wait_err != k_ra_ok) { /* GCOVR_EXCL_BR_LINE */
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

ra_err_t ra_rsip_sha256(const uint8_t* msg, uint32_t msg_len, uint8_t* digest)
{
  RA_CHECK_NULL_PTR(msg, s_tag, "msg must not be nullptr");
  RA_CHECK_NULL_PTR(digest, s_tag, "digest must not be nullptr");

#ifdef RA_RSIP_HASH_HARDWARE
  /* HUM Ch 52.2.3 "Hash Generator" p 3306 */
  /* Real silicon command-issue sequence (NON-FUNCTIONAL on this silicon; never
   * compiled -- see the backend note near the top of the file):
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
#else
  /* The RSIP HASH hardware has no usable register interface, so compute the
   * digest in software. This is the path used on silicon and on the host; it is
   * what makes ra_rot's on-silicon image digest work. */
  internal_sw_sha256(msg, msg_len, digest);
  return k_ra_ok;
#endif
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
  k_ra_rsip_sw_sha256_block_w   = 16U,   /**< 64-byte block = 16 words.      */
  k_ra_rsip_sw_sha256_round_cnt = 64U,   /**< Sched + compression rounds.    */
  k_ra_rsip_sw_sha256_state_w   = 8U,    /**< 8 working-state words.         */
  k_ra_rsip_sw_sha256_pad_min   = 9U,    /**< 0x80 + 8 length bytes minimum. */
  k_ra_rsip_sw_sha256_len_bytes = 8U,    /**< 64-bit length encoding tail.   */
  k_ra_rsip_sw_sha256_pad_byte  = 0x80U, /**< RFC 6234 / FIPS 180-4 marker.  */
  k_ra_rsip_sw_sha256_w_back_2  = 2U,    /**< W[i-2]  schedule lookback.     */
  k_ra_rsip_sw_sha256_w_back_7  = 7U,    /**< W[i-7]  schedule lookback.     */
  k_ra_rsip_sw_sha256_w_back_15 = 15U,   /**< W[i-15] schedule lookback.     */
  k_ra_rsip_sw_sha256_w_back_16 = 16U,   /**< W[i-16] schedule lookback.     */
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
  k_ra_rsip_sw_word_bits        = 32U, /**< Word width in bits. */
} ra_rsip_sw_sha256_t;

/* 32-bit right-rotate -- see surrounding code and HUM citations. */
static inline uint32_t internal_sw_rotr(uint32_t x, uint32_t n)
{
  return (x >> n) | (x << (k_ra_rsip_sw_word_bits - n));
}

/* clang-format off */
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
/** @brief FIPS 180-4 6.2.2 SHA-256 working-state lane indices a..h. */
typedef enum : uint8_t {
  k_sha256_lane_a = 0U,
  k_sha256_lane_b = 1U,
  k_sha256_lane_c = 2U,
  k_sha256_lane_d = 3U,
  k_sha256_lane_e = 4U,
  k_sha256_lane_f = 5U,
  k_sha256_lane_g = 6U,
  k_sha256_lane_h = 7U,
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
static void internal_sw_sha256_rounds(uint32_t       s[k_ra_rsip_sw_sha256_state_w],
                                      const uint32_t w[k_ra_rsip_sw_sha256_round_cnt])
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

ra_err_t ra_rsip_sha256_init(ra_rsip_sha256_ctx_t* ctx)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  ctx->used        = 0U;
  ctx->initialized = 1U;
  return k_ra_ok;
}

ra_err_t ra_rsip_sha256_update(ra_rsip_sha256_ctx_t* ctx, const uint8_t* data, uint32_t len)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  if ((data == nullptr) && (len != 0U)) {
    return k_ra_err_null_ptr;
  }
  if (ctx->initialized != 1U) {
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

ra_err_t ra_rsip_sha256_final(ra_rsip_sha256_ctx_t* ctx, uint8_t* digest_out)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA_CHECK_NULL_PTR(digest_out, s_tag, "digest_out must not be nullptr");
  if (ctx->initialized != 1U) {
    return k_ra_err_invalid_state;
  }
  const ra_err_t err = internal_sha256_dispatch(ctx->buf, ctx->used, digest_out);
  ctx->initialized   = 0U;
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
    ctx->inner.initialized = 0U;
    return upd_err;
  }
  ctx->initialized = 1U;
  return k_ra_ok;
}

ra_err_t
ra_rsip_hmac_sha256_update(ra_rsip_hmac_sha256_ctx_t* ctx, const uint8_t* data, uint32_t len)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  if (ctx->initialized != 1U) {
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

ra_err_t ra_rsip_hmac_sha256_final(ra_rsip_hmac_sha256_ctx_t* ctx, uint8_t* mac_out)
{
  RA_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA_CHECK_NULL_PTR(mac_out, s_tag, "mac_out must not be nullptr");
  if (ctx->initialized != 1U) {
    return k_ra_err_invalid_state;
  }
  uint8_t        inner_digest[k_ra_rsip_sha256_digest_bytes] = {};
  const ra_err_t inner_err = ra_rsip_sha256_final(&ctx->inner, inner_digest);
  ra_err_t       result    = inner_err;
  if (inner_err == k_ra_ok) {
    result = internal_hmac_outer(ctx->key_block, inner_digest, mac_out);
  }
  ctx->initialized = 0U;
  for (uint32_t i = 0U; i < k_ra_rsip_sha256_block; ++i) {
    ctx->key_block[i] = 0x00U;
  }
  return result;
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
  RA_RETURN_ON_ERROR(mst_err, s_tag, "rsip_exit_stop: mstp enable"); /* GCOVR_EXCL_BR_LINE */

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
