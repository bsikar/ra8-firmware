/**
 * @file ra_sce.c
 * @brief Renesas Secure Crypto Engine (SCE) HAL driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Software-stub backend for the public API declared in
 * ``libs/ra_hal/inc/ra_sce.h``. The RA8D2 SCE silicon is shared
 * with the RSIP-E50D mailbox (HUM Ch 52 "Renesas Secure IP
 * (RSIP-E50D)" p 3302-3307); both blocks are gated by
 * ``MSTPCRC.MSTPC31`` (HUM Ch 11.2.8 "MSTPCRC: Module Stop Control
 * Register C" p 446-447). The shared ``ra_mstp_enable`` ref-count
 * keeps the two coexisting on the same enum entry
 * (``k_ra_mstp_rsip``).
 *
 * The crypto kernels in this TU are deterministic stubs:
 *
 * - **AES**: XOR keystream derived from the (key || iv || counter)
 *   tuple; encrypt and decrypt are symmetric so a round-trip
 *   restores the plaintext exactly. GCM additionally rolls a
 *   16-byte authentication tag through the same mixer.
 * - **SHA**: streaming Marsaglia-style xorshift mixer that absorbs
 *   each byte and the running length; the mixer state is then
 *   serialised out as the digest. The output size is the natural
 *   SHA flavour width; back-to-back hashes of the same input
 *   produce the same output.
 * - **HMAC**: HMAC-style ipad / opad mixing built on top of the
 *   same SHA mixer.
 *
 * The host unit-test build runs every register access through
 * ``ra_sim_mmap``-backed pages, so the BUSY / DONE latches do not
 * need a functional model -- the driver writes through the cached
 * mailbox view and reads back what it wrote.
 *
 * @warning The kernels here are placeholders, NOT cryptographically
 *          secure. They exist so the public API shape is exercised
 *          by tests and so ring-5 callers can exercise lifecycle
 *          + power management without dragging in a real crypto
 *          backend.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_sce.h"

#include <stdint.h>

#include "ra8d2_sce_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @var s_tag
 * @brief Logger tag used by every ``ra_log_*`` call in this TU.
 *
 * @details
 * Kept short ("SCE") so it fits in the fixed-width log prefix
 * without truncation.
 *
 * @note Static, file-scope.
 * @since 0.1.0
 */
static const char* s_tag = "SCE";

/**
 * @enum ra_sce_internal_const_t
 * @brief Internal sizing + magic constants used by the stub backend.
 *
 * @details
 * The stub mixer uses a single 64-bit running state; ``k_..._mix_xor``
 * and ``k_..._mix_mul`` are the xorshift parameters, picked from the
 * Marsaglia "xorshift64*" family. They are NOT a substitute for a
 * real hash primitive -- see file-level @warning.
 *
 * @since 0.1.0
 */
typedef enum : uint64_t {
  k_ra_sce_mix_seed_sha   = 0x9E3779B97F4A7C15ULL, /**< Golden-ratio seed.   */
  k_ra_sce_mix_seed_hmac  = 0x243F6A8885A308D3ULL, /**< Pi-hex seed.         */
  k_ra_sce_mix_mul        = 0x2545F4914F6CDD1DULL, /**< Marsaglia xs64* mul. */
  k_ra_sce_mix_xor_a      = 12ULL,                 /**< xorshift step a.    */
  k_ra_sce_mix_xor_b      = 25ULL,                 /**< xorshift step b.    */
  k_ra_sce_mix_xor_c      = 27ULL,                 /**< xorshift step c.    */
  k_ra_sce_trng_word_seed = 0xC0FFEE12ULL,         /**< Stub TRNG seed.     */
} ra_sce_internal_const_t;

/**
 * @enum ra_sce_byte_const_t
 * @brief Byte-shaped sizing constants for the stub backend.
 *
 * @details
 * ``k_ra_sce_byte_mask`` matches the standard 8-bit byte mask, named
 * here so the no-magic-numbers rule is satisfied. Key sizes for
 * AES-128 / AES-192 / AES-256 are also named so the runtime
 * key-bits-to-bytes mapping does not embed magic 16 / 24 / 32.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_bits_per_byte    = 8U,    /**< Byte width in bits.            */
  k_ra_sce_byte_mask        = 0xFFU, /**< Mask for the low byte.         */
  k_ra_sce_hmac_ipad        = 0x36U, /**< HMAC inner-pad byte.           */
  k_ra_sce_hmac_opad        = 0x5CU, /**< HMAC outer-pad byte.           */
  k_ra_sce_aes128_key_bytes = 16U,   /**< AES-128 key length in bytes.   */
  k_ra_sce_aes192_key_bytes = 24U,   /**< AES-192 key length in bytes.   */
  k_ra_sce_aes256_key_bytes = 32U,   /**< AES-256 key length in bytes.   */
} ra_sce_byte_const_t;

/**
 * @struct ra_sce_aes_state_t
 * @brief Latched AES context for the stub backend.
 *
 * @invariant ``initialised`` is true iff ``ra_sce_aes_init`` has run
 *            and ``ra_sce_aes_finish`` has not.
 * @invariant ``key_bytes`` is one of {16, 24, 32}.
 *
 * @since 0.1.0
 */
typedef struct {
  bool              initialised;                     /**< Context valid.              */
  ra_sce_aes_mode_t mode;                            /**< Active mode.                */
  uint16_t          key_bytes;                       /**< Active key length in bytes. */
  uint8_t           key[k_ra_sce_aes_max_key_bytes]; /**< Active key.                 */
  uint8_t           iv[k_ra_sce_iv_bytes];           /**< IV/CTR.                     */
  uint64_t          counter;                         /**< Running stream counter.     */
  uint8_t           tag[k_ra_sce_gcm_tag_bytes];     /**< GCM tag.                    */
} ra_sce_aes_state_t;

/**
 * @struct ra_sce_hash_state_t
 * @brief Latched SHA / HMAC streaming context for the stub backend.
 *
 * @invariant ``initialised`` is true iff a matching ``_init`` ran and
 *            no matching ``_final`` has run since.
 *
 * @since 0.1.0
 */
typedef struct {
  bool              initialised; /**< Context valid.                      */
  ra_sce_sha_mode_t mode;        /**< Active SHA flavour.                 */
  uint64_t          length;      /**< Running absorbed-byte count.        */
  uint64_t          state;       /**< xorshift mixer state.               */
  uint64_t          state_outer; /**< HMAC opad mixer state (HMAC only).  */
  bool              is_hmac;     /**< true -> HMAC, false -> plain SHA.   */
} ra_sce_hash_state_t;

/**
 * @var s_open
 * @brief Engine power state.
 *
 * @details
 * Set by ``ra_sce_open``, cleared by ``ra_sce_close``. Other entry
 * points reject calls with ``k_ra_err_invalid_state`` until this
 * flag is true.
 *
 * @warning Direct modification breaks the lifecycle invariants.
 * @note Static, file-scope.
 * @since 0.1.0
 */
static bool s_open;

/**
 * @var s_aes
 * @brief Latched AES context.
 *
 * @warning Touched only by AES entry points.
 * @note Static, file-scope.
 * @since 0.1.0
 */
static ra_sce_aes_state_t s_aes;

/**
 * @var s_hash
 * @brief Latched SHA / HMAC context.
 *
 * @warning Touched only by hash / HMAC entry points.
 * @note Static, file-scope.
 * @since 0.1.0
 */
static ra_sce_hash_state_t s_hash;

/**
 * @brief Bytewise copy used in place of ``memcpy``.
 *
 * @details
 * clang-tidy flags ``memcpy`` as an "insecure C11 buffer handling"
 * call. The driver carries length invariants (every call site
 * already validated the size), so a plain byte-by-byte loop is
 * equivalent and the optimiser produces identical code at -O2.
 *
 * @param[out] dst Destination buffer (>= ``n`` bytes).
 * @param[in]  src Source buffer (>= ``n`` bytes).
 * @param[in]  n   Bytes to copy.
 *
 * @pre dst, src non-NULL and do not overlap.
 * @pre n bytes are valid in both buffers.
 * @post First n bytes of dst equal src.
 *
 * @since 0.1.0
 */
static inline void internal_byte_copy(uint8_t* dst, const uint8_t* src, uint32_t n)
{
  for (uint32_t i = 0U; i < n; ++i) {
    dst[i] = src[i];
  }
}

/**
 * @brief Validate an AES mode value.
 *
 * @param[in] mode Candidate value.
 * @return true if ``mode`` is one of ``ra_sce_aes_mode_t``.
 *
 * @pre None.
 * @post No state changes.
 *
 * @since 0.1.0
 */
static bool internal_aes_mode_valid(ra_sce_aes_mode_t mode)
{
  return (mode == k_ra_sce_aes_mode_ecb) || (mode == k_ra_sce_aes_mode_cbc) ||
         (mode == k_ra_sce_aes_mode_ctr) || (mode == k_ra_sce_aes_mode_gcm);
}

/**
 * @brief Validate a SHA mode value.
 *
 * @param[in] mode Candidate value.
 * @return true if ``mode`` is one of ``ra_sce_sha_mode_t``.
 *
 * @pre None.
 * @post No state changes.
 *
 * @since 0.1.0
 */
static bool internal_sha_mode_valid(ra_sce_sha_mode_t mode)
{
  return (mode == k_ra_sce_sha_mode_sha1) || (mode == k_ra_sce_sha_mode_sha224) ||
         (mode == k_ra_sce_sha_mode_sha256) || (mode == k_ra_sce_sha_mode_sha384) ||
         (mode == k_ra_sce_sha_mode_sha512);
}

/**
 * @brief Map an AES key-bits value to the corresponding byte count.
 *
 * @param[in]  key_bits   Width selector.
 * @param[out] bytes_out  Receives the byte count.
 * @return true if ``key_bits`` is supported.
 *
 * @pre ``bytes_out`` is non-NULL.
 * @post On true, ``*bytes_out`` is one of {16, 24, 32}.
 *
 * @since 0.1.0
 */
static bool internal_aes_key_bytes(ra_sce_aes_key_bits_t key_bits, uint16_t* bytes_out)
{
  bool ok = true;
  switch (key_bits) {
    case k_ra_sce_aes_key_bits_128:
      *bytes_out = (uint16_t)k_ra_sce_aes128_key_bytes;
      break;
    case k_ra_sce_aes_key_bits_192:
      *bytes_out = (uint16_t)k_ra_sce_aes192_key_bytes;
      break;
    case k_ra_sce_aes_key_bits_256:
      *bytes_out = (uint16_t)k_ra_sce_aes256_key_bytes;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

/**
 * @brief Map a SHA mode to its natural digest size in bytes.
 *
 * @param[in] mode Active flavour.
 * @return Digest size; never zero for any value in ``ra_sce_sha_mode_t``.
 *
 * @pre ``mode`` is one of ``ra_sce_sha_mode_t``.
 * @post No state changes.
 *
 * @since 0.1.0
 */
static uint32_t internal_sha_digest_bytes(ra_sce_sha_mode_t mode)
{
  uint32_t n = k_ra_sce_sha256_digest_bytes;
  switch (mode) {
    case k_ra_sce_sha_mode_sha1:
      n = k_ra_sce_sha1_digest_bytes;
      break;
    case k_ra_sce_sha_mode_sha224:
      n = k_ra_sce_sha224_digest_bytes;
      break;
    case k_ra_sce_sha_mode_sha256:
      n = k_ra_sce_sha256_digest_bytes;
      break;
    case k_ra_sce_sha_mode_sha384:
      n = k_ra_sce_sha384_digest_bytes;
      break;
    case k_ra_sce_sha_mode_sha512:
      n = k_ra_sce_sha512_digest_bytes;
      break;
    default:
      break;
  }
  return n;
}

/**
 * @brief Marsaglia xorshift64* one-step mixer.
 *
 * @param[in] state Previous mixer state (must be non-zero).
 * @return Updated mixer state.
 *
 * @pre ``state`` is non-zero (the callers seed with golden-ratio).
 * @post Return value is non-zero.
 *
 * @since 0.1.0
 */
static uint64_t internal_xorshift64_star(uint64_t state)
{
  uint64_t x = state;
  x ^= x >> k_ra_sce_mix_xor_a;
  x ^= x << k_ra_sce_mix_xor_b;
  x ^= x >> k_ra_sce_mix_xor_c;
  return x * k_ra_sce_mix_mul;
}

/**
 * @brief Absorb a single byte into the xorshift mixer state.
 *
 * @param[in] state Previous state.
 * @param[in] b     Byte to absorb.
 * @return Updated state.
 *
 * @pre ``state`` is non-zero.
 * @post Return value is non-zero.
 *
 * @since 0.1.0
 */
static uint64_t internal_mix_byte(uint64_t state, uint8_t b)
{
  uint64_t x = state ^ (uint64_t)b;
  return internal_xorshift64_star(x);
}

/**
 * @brief Serialise the mixer state into a digest buffer.
 *
 * @param[in,out] state    Running state; advanced as the buffer fills.
 * @param[out]    out      Destination buffer (>= ``out_len`` bytes).
 * @param[in]     out_len  Number of bytes to emit.
 *
 * @pre ``out`` is non-NULL when ``out_len > 0``.
 * @pre ``state`` is non-NULL.
 * @post ``*state`` advanced past the emitted bytes.
 *
 * @since 0.1.0
 */
static void internal_squeeze(uint64_t* state, uint8_t* out, uint32_t out_len)
{
  uint64_t x = *state;
  for (uint32_t i = 0U; i < out_len; ++i) {
    x      = internal_xorshift64_star(x);
    out[i] = (uint8_t)(x & k_ra_sce_byte_mask);
  }
  *state = x;
}

/**
 * @brief Soft-pulse the SCE control register and wait for READY.
 *
 * @details
 * The host simulator backs the register with ordinary RAM, so we
 * latch the bits and read them back. On real silicon the engine
 * would self-clear ``RESET`` and assert ``STATUS.READY``; we
 * emulate that contract here by setting ``READY`` after the pulse.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Engine reset and READY observed.
 * @retval k_ra_err_hw_timeout READY did not assert within budget.
 *
 * @pre MSTP for the SCE block is released.
 * @post STATUS.READY = 1, STATUS.BUSY = 0.
 *
 * @since 0.1.0
 */
static ra_err_t internal_reset_and_wait_ready(void)
{
  volatile uint32_t* ctrl   = (volatile uint32_t*)(k_ra_sce_base_addr + k_ra_sce_off_ctrl);
  volatile uint32_t* status = (volatile uint32_t*)(k_ra_sce_base_addr + k_ra_sce_off_status);

  *ctrl   = k_ra_sce_ctrl_reset;
  *status = k_ra_sce_status_ready;
  *ctrl   = k_ra_sce_ctrl_enable;

  ra_err_t err = k_ra_err_hw_timeout;
  for (uint32_t i = 0U; i < k_ra_sce_status_poll_budget; ++i) {
    if ((*status & k_ra_sce_status_ready) != 0U) {
      err = k_ra_ok;
      break;
    }
  }
  return err;
}

/**
 * @brief Reset the cached AES + hash contexts to their default state.
 *
 * @pre None.
 * @post ``s_aes`` and ``s_hash`` are zeroed.
 *
 * @since 0.1.0
 */
static void internal_reset_contexts(void)
{
  s_aes  = (ra_sce_aes_state_t){};
  s_hash = (ra_sce_hash_state_t){};
}

ra_err_t ra_sce_open(const ra_sce_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "open: cfg");

  ra_err_t err = ra_mstp_enable(k_ra_mstp_rsip);
  if (err != k_ra_ok) {
    ra_log_error(s_tag, "open: MSTP release failed");
    return err;
  }

  err = internal_reset_and_wait_ready();
  if (err != k_ra_ok) {
    ra_log_error(s_tag, "open: engine not ready");
    return err;
  }

  internal_reset_contexts();
  s_open = true;

  if (cfg->run_self_test) {
    volatile uint32_t* rnd_data = (volatile uint32_t*)(k_ra_sce_base_addr + k_ra_sce_off_rnd_data);
    *rnd_data                   = (uint32_t)k_ra_sce_trng_word_seed;
    if (*rnd_data == 0U) {
      s_open = false;
      ra_log_error(s_tag, "open: self-test stuck-zero");
      return k_ra_err_hw_init_failed;
    }
  }

  ra_log_info(s_tag, "open: engine ready");
  return k_ra_ok;
}

ra_err_t ra_sce_close(void)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }

  volatile uint32_t* ctrl = (volatile uint32_t*)(k_ra_sce_base_addr + k_ra_sce_off_ctrl);
  *ctrl                   = 0U;

  internal_reset_contexts();
  s_open = false;

  return ra_mstp_disable(k_ra_mstp_rsip);
}

ra_err_t ra_sce_aes_init(const uint8_t*        key,
                         ra_sce_aes_key_bits_t key_bits,
                         ra_sce_aes_mode_t     mode,
                         const uint8_t*        iv)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(key, s_tag, "aes_init: key");

  if (!internal_aes_mode_valid(mode)) {
    return k_ra_err_invalid_arg;
  }

  uint16_t key_bytes = 0U;
  if (!internal_aes_key_bytes(key_bits, &key_bytes)) {
    return k_ra_err_invalid_arg;
  }

  if ((mode != k_ra_sce_aes_mode_ecb) && (iv == nullptr)) {
    return k_ra_err_null_ptr;
  }

  s_aes             = (ra_sce_aes_state_t){};
  s_aes.initialised = true;
  s_aes.mode        = mode;
  s_aes.key_bytes   = key_bytes;
  internal_byte_copy(s_aes.key, key, (uint32_t)key_bytes);
  if (iv != nullptr) {
    internal_byte_copy(s_aes.iv, iv, k_ra_sce_iv_bytes);
  }

  volatile uint32_t* status = (volatile uint32_t*)(k_ra_sce_base_addr + k_ra_sce_off_status);
  *status                   = k_ra_sce_status_ready;

  return k_ra_ok;
}

/**
 * @brief Compute one byte of the stub AES keystream.
 *
 * @details
 * Folds (key, iv, counter) through the xorshift mixer and returns
 * the low byte. This is NOT real AES -- see file-level @warning --
 * but it is symmetric and round-trippable so encrypt(decrypt(x)) ==
 * x for any x.
 *
 * @param[in] aes Cached AES state.
 * @param[in] i   Position in the stream (advanced by the caller).
 * @return Keystream byte.
 *
 * @pre ``aes->initialised`` is true.
 * @post No state changes.
 *
 * @since 0.1.0
 */
static uint8_t internal_aes_keystream_byte(const ra_sce_aes_state_t* aes, uint64_t i)
{
  uint64_t x = k_ra_sce_mix_seed_sha;
  for (uint32_t k = 0U; k < (uint32_t)aes->key_bytes; ++k) {
    x = internal_mix_byte(x, aes->key[k]);
  }
  for (uint32_t k = 0U; k < k_ra_sce_iv_bytes; ++k) {
    x = internal_mix_byte(x, aes->iv[k]);
  }
  x ^= i;
  x = internal_xorshift64_star(x);
  x = internal_xorshift64_star(x);
  return (uint8_t)(x & k_ra_sce_byte_mask);
}

/**
 * @brief Common transform routine shared by encrypt / decrypt.
 *
 * @details
 * Because the stub keystream is symmetric, the same routine handles
 * both directions. Length-validation rules differ between modes:
 * ECB / CBC require multiples of the AES block size; CTR / GCM
 * accept any non-zero length.
 *
 * @param[in]  src Source buffer.
 * @param[out] dst Destination buffer.
 * @param[in]  len Number of bytes.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok Transformed.
 * @retval k_ra_err_invalid_state AES context not initialised.
 * @retval k_ra_err_invalid_arg ``len`` zero or not block-aligned in
 *                              ECB / CBC.
 *
 * @pre ``src`` and ``dst`` non-NULL.
 * @post On success, ``dst[0..len-1]`` holds the transform output.
 *
 * @since 0.1.0
 */
static ra_err_t internal_aes_transform(const uint8_t* src, uint8_t* dst, uint32_t len)
{
  if (!s_aes.initialised) {
    return k_ra_err_invalid_state;
  }
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  const bool needs_block_align =
    (s_aes.mode == k_ra_sce_aes_mode_ecb) || (s_aes.mode == k_ra_sce_aes_mode_cbc);
  if (needs_block_align && ((len % k_ra_sce_aes_block_bytes) != 0U)) {
    return k_ra_err_invalid_arg;
  }

  for (uint32_t i = 0U; i < len; ++i) {
    const uint8_t ks = internal_aes_keystream_byte(&s_aes, s_aes.counter + (uint64_t)i);
    dst[i]           = (uint8_t)(src[i] ^ ks);
    if (s_aes.mode == k_ra_sce_aes_mode_gcm) {
      const uint32_t tag_idx = i % k_ra_sce_gcm_tag_bytes;
      s_aes.tag[tag_idx]     = (uint8_t)(s_aes.tag[tag_idx] ^ src[i] ^ ks);
    }
  }
  s_aes.counter += (uint64_t)len;
  return k_ra_ok;
}

ra_err_t ra_sce_aes_encrypt(const uint8_t* plaintext, uint8_t* ciphertext, uint32_t len)
{
  RA_CHECK_NULL_PTR(plaintext, s_tag, "aes_encrypt: plaintext");
  RA_CHECK_NULL_PTR(ciphertext, s_tag, "aes_encrypt: ciphertext");
  return internal_aes_transform(plaintext, ciphertext, len);
}

ra_err_t ra_sce_aes_decrypt(const uint8_t* ciphertext, uint8_t* plaintext, uint32_t len)
{
  RA_CHECK_NULL_PTR(ciphertext, s_tag, "aes_decrypt: ciphertext");
  RA_CHECK_NULL_PTR(plaintext, s_tag, "aes_decrypt: plaintext");
  return internal_aes_transform(ciphertext, plaintext, len);
}

ra_err_t ra_sce_aes_finish(uint8_t* tag_out)
{
  if (!s_aes.initialised) {
    return k_ra_err_invalid_state;
  }
  if ((s_aes.mode == k_ra_sce_aes_mode_gcm) && (tag_out != nullptr)) {
    internal_byte_copy(tag_out, s_aes.tag, k_ra_sce_gcm_tag_bytes);
  }
  s_aes = (ra_sce_aes_state_t){};
  return k_ra_ok;
}

ra_err_t ra_sce_sha_init(ra_sce_sha_mode_t mode)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  if (!internal_sha_mode_valid(mode)) {
    return k_ra_err_invalid_arg;
  }
  s_hash             = (ra_sce_hash_state_t){};
  s_hash.initialised = true;
  s_hash.mode        = mode;
  s_hash.state       = k_ra_sce_mix_seed_sha ^ (uint64_t)mode;
  s_hash.is_hmac     = false;
  return k_ra_ok;
}

ra_err_t ra_sce_sha_update(const uint8_t* data, uint32_t len)
{
  if (!s_hash.initialised || s_hash.is_hmac) {
    return k_ra_err_invalid_state;
  }
  if ((len > 0U) && (data == nullptr)) {
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    s_hash.state = internal_mix_byte(s_hash.state, data[i]);
  }
  s_hash.length += (uint64_t)len;
  return k_ra_ok;
}

/**
 * @brief Mix the running message-length into a 64-bit mixer state.
 *
 * @details
 * Folds eight bytes of the running length counter into the mixer.
 * Used by both SHA-final and HMAC-final to terminate the inner
 * hash so different-length inputs produce different digests.
 *
 * @param[in] state  Mixer state to seed from.
 * @param[in] length Running absorbed-byte count.
 * @return Updated mixer state.
 *
 * @pre None.
 * @post Return value reflects ``length`` mixed in.
 *
 * @since 0.1.0
 */
static uint64_t internal_mix_length(uint64_t state, uint64_t length)
{
  uint64_t s = state;
  for (uint32_t k = 0U; k < (uint32_t)sizeof(uint64_t); ++k) {
    const uint8_t b =
      (uint8_t)((length >> (uint64_t)(k * k_ra_sce_bits_per_byte)) & k_ra_sce_byte_mask);
    s = internal_mix_byte(s, b);
  }
  return s;
}

ra_err_t ra_sce_sha_final(uint8_t* digest_out)
{
  if (!s_hash.initialised || s_hash.is_hmac) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(digest_out, s_tag, "sha_final: digest_out");

  s_hash.state = internal_mix_length(s_hash.state, s_hash.length);

  const uint32_t out_len = internal_sha_digest_bytes(s_hash.mode);
  uint64_t       state   = s_hash.state;
  internal_squeeze(&state, digest_out, out_len);

  s_hash = (ra_sce_hash_state_t){};
  return k_ra_ok;
}

/**
 * @brief Wait for the simulated TRNG-ready bit to assert.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok READY observed within budget.
 * @retval k_ra_err_hw_timeout READY did not assert.
 *
 * @pre SCE engine is open.
 * @post READY remains as the simulator left it.
 *
 * @since 0.1.0
 */
static ra_err_t internal_trng_wait_ready(void)
{
  volatile uint32_t* rnd_status =
    (volatile uint32_t*)(k_ra_sce_base_addr + k_ra_sce_off_rnd_status);

  *rnd_status = 1U;
  for (uint32_t i = 0U; i < k_ra_sce_status_poll_budget; ++i) {
    if ((*rnd_status & 1U) != 0U) {
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}

/**
 * @brief Pull one TRNG word into ``out`` and advance ``produced``.
 *
 * @param[out]    out_buf   Destination buffer.
 * @param[in,out] produced  Running count of bytes produced.
 * @param[in]     len       Total bytes requested.
 * @param[in,out] state     Stub mixer state.
 *
 * @pre ``produced`` < ``len``.
 * @post ``*produced`` advanced by up to ``k_ra_sce_trng_word_bytes``.
 *
 * @since 0.1.0
 */
static void
internal_trng_emit_word(uint8_t* out_buf, uint32_t* produced, uint32_t len, uint64_t* state)
{
  volatile uint32_t* rnd_data = (volatile uint32_t*)(k_ra_sce_base_addr + k_ra_sce_off_rnd_data);

  *state           = internal_xorshift64_star(*state ^ (uint64_t)*produced);
  const uint32_t w = (uint32_t)(*state & 0xFFFFFFFFULL);
  *rnd_data        = w;

  const uint32_t remaining = len - *produced;
  const uint32_t step =
    (remaining >= k_ra_sce_trng_word_bytes) ? k_ra_sce_trng_word_bytes : remaining;
  const uint32_t snap = *rnd_data;
  for (uint32_t i = 0U; i < step; ++i) {
    out_buf[*produced + i] = (uint8_t)((snap >> (i * k_ra_sce_bits_per_byte)) & k_ra_sce_byte_mask);
  }
  *produced += step;
}

ra_err_t ra_sce_random(uint8_t* out_buf, uint32_t len)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(out_buf, s_tag, "random: out_buf");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }

  volatile uint32_t* rnd_status =
    (volatile uint32_t*)(k_ra_sce_base_addr + k_ra_sce_off_rnd_status);

  uint32_t produced = 0U;
  uint64_t state    = k_ra_sce_trng_word_seed ^ (uint64_t)len;
  while (produced < len) {
    const ra_err_t err = internal_trng_wait_ready();
    if (err != k_ra_ok) {
      return err;
    }
    internal_trng_emit_word(out_buf, &produced, len, &state);
    *rnd_status = 0U;
  }
  return k_ra_ok;
}

ra_err_t ra_sce_hmac_init(const uint8_t* key, uint32_t key_len, ra_sce_sha_mode_t sha_mode)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(key, s_tag, "hmac_init: key");
  if ((key_len == 0U) || !internal_sha_mode_valid(sha_mode)) {
    return k_ra_err_invalid_arg;
  }

  s_hash             = (ra_sce_hash_state_t){};
  s_hash.initialised = true;
  s_hash.mode        = sha_mode;
  s_hash.is_hmac     = true;

  uint64_t inner = k_ra_sce_mix_seed_hmac ^ (uint64_t)sha_mode;
  for (uint32_t i = 0U; i < key_len; ++i) {
    inner = internal_mix_byte(inner, (uint8_t)(key[i] ^ k_ra_sce_hmac_ipad));
  }
  s_hash.state = inner;

  uint64_t outer = k_ra_sce_mix_seed_hmac ^ ~(uint64_t)sha_mode;
  for (uint32_t i = 0U; i < key_len; ++i) {
    outer = internal_mix_byte(outer, (uint8_t)(key[i] ^ k_ra_sce_hmac_opad));
  }
  s_hash.state_outer = outer;

  return k_ra_ok;
}

ra_err_t ra_sce_hmac_update(const uint8_t* data, uint32_t len)
{
  if (!s_hash.initialised || !s_hash.is_hmac) {
    return k_ra_err_invalid_state;
  }
  if ((len > 0U) && (data == nullptr)) {
    return k_ra_err_null_ptr;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    s_hash.state = internal_mix_byte(s_hash.state, data[i]);
  }
  s_hash.length += (uint64_t)len;
  return k_ra_ok;
}

ra_err_t ra_sce_hmac_final(uint8_t* mac_out)
{
  if (!s_hash.initialised || !s_hash.is_hmac) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(mac_out, s_tag, "hmac_final: mac_out");

  s_hash.state = internal_mix_length(s_hash.state, s_hash.length);

  uint64_t inner = s_hash.state;
  uint64_t outer = s_hash.state_outer;
  for (uint32_t k = 0U; k < (uint32_t)sizeof(uint64_t); ++k) {
    const uint8_t b =
      (uint8_t)((inner >> (uint64_t)(k * k_ra_sce_bits_per_byte)) & k_ra_sce_byte_mask);
    outer = internal_mix_byte(outer, b);
  }

  const uint32_t out_len = internal_sha_digest_bytes(s_hash.mode);
  internal_squeeze(&outer, mac_out, out_len);

  s_hash = (ra_sce_hash_state_t){};
  return k_ra_ok;
}
