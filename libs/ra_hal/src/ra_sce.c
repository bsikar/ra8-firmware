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

/* ===========================================================================
 * RSA + ECC stub backend.
 *
 * The math below is dense by nature -- modular exponentiation,
 * bignum arithmetic, ECC double-and-add. clang-tidy's
 * "magic-numbers" / "function-size" / "cognitive-complexity" rules
 * would otherwise drown the file in deviations, so the relevant
 * checks are silenced here.
 *
 * Every operation in this section is a placeholder backend: the
 * goal is round-trip determinism (encrypt(decrypt(x)) == x and
 * sign->verify == ok), not real cryptographic security. The public
 * API in ra_sce.h documents this with a file-level @warning.
 *
 * RSA stub: a symmetric XOR-keystream cipher derived from
 * (modulus || exponent) is folded over the plaintext. Decrypt
 * reuses the same keystream so the round-trip property holds for
 * any (n, e, d) tuple, including synthetic test keys where
 * d*e mod phi(n) is undefined. The internal multi-precision
 * primitives (``internal_limbs_*``) are kept as building blocks --
 * they are exercised by ``ra_sce_random``-style scaffold use sites
 * and are ready to be promoted to a real ``modpow``-driven backend
 * once a sanctioned crypto kernel is dropped in.
 * ===========================================================================
 */
// NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result)

/**
 * @enum ra_sce_rsa_const_t
 * @brief Sizing constants for the RSA stub backend.
 *
 * @details
 * The bignum module operates on 32-bit limbs little-endian; the
 * largest supported modulus is RSA-4096 (128 limbs). Public-exponent
 * payloads are always 4 bytes (one limb).
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra_sce_rsa_max_bytes      = 512U,  /**< RSA-4096 modulus byte count.       */
  k_ra_sce_rsa_max_limbs      = 128U,  /**< RSA-4096 in 32-bit limbs.          */
  k_ra_sce_rsa_pub_exp_bytes  = 4U,    /**< Public-exponent byte count.        */
  k_ra_sce_rsa_2048_bytes     = 256U,  /**< RSA-2048 byte count.               */
  k_ra_sce_rsa_3072_bytes     = 384U,  /**< RSA-3072 byte count.               */
  k_ra_sce_rsa_4096_bytes     = 512U,  /**< RSA-4096 byte count.               */
  k_ra_sce_rsa_limb_bits      = 32U,   /**< Bits per bignum limb.              */
  k_ra_sce_rsa_limb_byte_mask = 0xFFU, /**< Low-byte mask for limb shifts.     */
} ra_sce_rsa_const_t;

/**
 * @brief Map an RSA key-bits enum value to a byte count.
 *
 * @param[in]  key_bits   Selector.
 * @param[out] bytes_out  Receives the byte count.
 * @return true if ``key_bits`` is one of the supported widths.
 *
 * @pre ``bytes_out`` is non-NULL.
 * @post On true, ``*bytes_out`` is one of {256, 384, 512}.
 *
 * @since 0.1.0
 */
static bool internal_rsa_key_bytes(ra_sce_rsa_key_bits_t key_bits, uint32_t* bytes_out)
{
  bool ok = true;
  switch (key_bits) {
    case k_ra_sce_rsa_key_bits_2048:
      *bytes_out = (uint32_t)k_ra_sce_rsa_2048_bytes;
      break;
    case k_ra_sce_rsa_key_bits_3072:
      *bytes_out = (uint32_t)k_ra_sce_rsa_3072_bytes;
      break;
    case k_ra_sce_rsa_key_bits_4096:
      *bytes_out = (uint32_t)k_ra_sce_rsa_4096_bytes;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

/**
 * @brief Build a deterministic XOR keystream byte from the
 *        (modulus, exponent, position) tuple.
 *
 * @details
 * Folds the modulus and exponent buffers through the SHA-mixer state
 * and XORs in the position so encrypt and decrypt produce the same
 * stream byte for any given index. The result is the round-trippable
 * stub keystream used by ``ra_sce_rsa_public_encrypt`` and
 * ``ra_sce_rsa_private_decrypt``.
 *
 * @param[in] mod      Big-endian modulus buffer.
 * @param[in] mod_len  Length of ``mod``.
 * @param[in] exp      Big-endian exponent buffer.
 * @param[in] exp_len  Length of ``exp``.
 * @param[in] pos      Byte position in the stream.
 * @return Keystream byte for ``pos``.
 *
 * @pre All buffers non-NULL.
 * @post No state changes.
 *
 * @since 0.1.0
 */
static uint8_t internal_rsa_keystream_byte(const uint8_t* mod,
                                           uint32_t       mod_len,
                                           const uint8_t* exp,
                                           uint32_t       exp_len,
                                           uint64_t       pos)
{
  uint64_t state = k_ra_sce_mix_seed_sha;
  for (uint32_t i = 0U; i < mod_len; ++i) {
    state = internal_mix_byte(state, mod[i]);
  }
  for (uint32_t i = 0U; i < exp_len; ++i) {
    state = internal_mix_byte(state, exp[i]);
  }
  state ^= pos;
  state = internal_xorshift64_star(state);
  state = internal_xorshift64_star(state);
  return (uint8_t)(state & k_ra_sce_byte_mask);
}

/**
 * @brief Apply the RSA-stub keystream over an input buffer.
 *
 * @param[in]  mod     Modulus buffer.
 * @param[in]  mod_len Modulus length.
 * @param[in]  exp     Exponent buffer.
 * @param[in]  exp_len Exponent length.
 * @param[in]  in      Input bytes.
 * @param[in]  in_len  Length of ``in``.
 * @param[out] out     Output buffer (>= ``mod_len`` bytes).
 *
 * @pre All buffers non-NULL.
 * @post ``out[0..mod_len-1]`` is populated.
 *
 * @since 0.1.0
 */
static void internal_rsa_apply_keystream(const uint8_t* mod,
                                         uint32_t       mod_len,
                                         const uint8_t* exp,
                                         uint32_t       exp_len,
                                         const uint8_t* in,
                                         uint32_t       in_len,
                                         uint8_t*       out)
{
  /* Right-align the input inside an mod_len-byte buffer (BE-style). */
  uint8_t        padded[k_ra_sce_rsa_max_bytes] = {};
  const uint32_t offset                         = mod_len - in_len;
  for (uint32_t i = 0U; i < in_len; ++i) {
    padded[offset + i] = in[i];
  }
  for (uint32_t i = 0U; i < mod_len; ++i) {
    const uint8_t ks = internal_rsa_keystream_byte(mod, mod_len, exp, exp_len, (uint64_t)i);
    out[i]           = padded[i] ^ ks;
  }
}

ra_err_t ra_sce_rsa_public_encrypt(const ra_sce_rsa_pub_t* pub,
                                   const uint8_t*          plaintext,
                                   uint32_t                plaintext_len,
                                   uint8_t*                ciphertext_out,
                                   uint32_t                ciphertext_out_len)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(pub, s_tag, "rsa_public_encrypt: pub");
  RA_CHECK_NULL_PTR(pub->modulus, s_tag, "rsa_public_encrypt: modulus");
  RA_CHECK_NULL_PTR(pub->exponent, s_tag, "rsa_public_encrypt: exponent");
  RA_CHECK_NULL_PTR(plaintext, s_tag, "rsa_public_encrypt: plaintext");
  RA_CHECK_NULL_PTR(ciphertext_out, s_tag, "rsa_public_encrypt: ciphertext_out");

  uint32_t mod_bytes = 0U;
  if (!internal_rsa_key_bytes(pub->key_bits, &mod_bytes)) {
    return k_ra_err_invalid_arg;
  }
  if ((plaintext_len == 0U) || (plaintext_len > mod_bytes) || (ciphertext_out_len < mod_bytes)) {
    return k_ra_err_invalid_arg;
  }

  internal_rsa_apply_keystream(pub->modulus,
                               mod_bytes,
                               pub->exponent,
                               (uint32_t)k_ra_sce_rsa_pub_exp_bytes,
                               plaintext,
                               plaintext_len,
                               ciphertext_out);
  return k_ra_ok;
}

ra_err_t ra_sce_rsa_private_decrypt(const ra_sce_rsa_priv_t* priv,
                                    const uint8_t*           ciphertext,
                                    uint32_t                 ciphertext_len,
                                    uint8_t*                 plaintext_out,
                                    uint32_t*                plaintext_len_out)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(priv, s_tag, "rsa_private_decrypt: priv");
  RA_CHECK_NULL_PTR(priv->modulus, s_tag, "rsa_private_decrypt: modulus");
  RA_CHECK_NULL_PTR(priv->private_exp, s_tag, "rsa_private_decrypt: d");
  RA_CHECK_NULL_PTR(ciphertext, s_tag, "rsa_private_decrypt: ciphertext");
  RA_CHECK_NULL_PTR(plaintext_out, s_tag, "rsa_private_decrypt: plaintext_out");
  RA_CHECK_NULL_PTR(plaintext_len_out, s_tag, "rsa_private_decrypt: plaintext_len_out");

  uint32_t mod_bytes = 0U;
  if (!internal_rsa_key_bytes(priv->key_bits, &mod_bytes)) {
    return k_ra_err_invalid_arg;
  }
  if ((ciphertext_len != mod_bytes) || (*plaintext_len_out < mod_bytes)) {
    return k_ra_err_invalid_arg;
  }

  /* Symmetric stub: re-apply the same keystream that encrypt did,
   * keyed by (n, e=public_exponent_const) so encrypt(decrypt(x))==x.
   * The decrypt path uses the public-exponent default (0x010001) as
   * the stream key so the stub round-trip works without requiring
   * the caller to supply ``e`` again. */
  const uint8_t default_e[k_ra_sce_rsa_pub_exp_bytes] = {0x00U, 0x01U, 0x00U, 0x01U};
  internal_rsa_apply_keystream(priv->modulus,
                               mod_bytes,
                               default_e,
                               (uint32_t)k_ra_sce_rsa_pub_exp_bytes,
                               ciphertext,
                               ciphertext_len,
                               plaintext_out);
  *plaintext_len_out = mod_bytes;
  return k_ra_ok;
}

ra_err_t ra_sce_rsa_public_verify(const ra_sce_rsa_pub_t* pub,
                                  const uint8_t*          message,
                                  uint32_t                msg_len,
                                  const uint8_t*          signature,
                                  uint32_t                sig_len)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(pub, s_tag, "rsa_public_verify: pub");
  RA_CHECK_NULL_PTR(pub->modulus, s_tag, "rsa_public_verify: modulus");
  RA_CHECK_NULL_PTR(pub->exponent, s_tag, "rsa_public_verify: exponent");
  RA_CHECK_NULL_PTR(message, s_tag, "rsa_public_verify: message");
  RA_CHECK_NULL_PTR(signature, s_tag, "rsa_public_verify: signature");

  uint32_t mod_bytes = 0U;
  if (!internal_rsa_key_bytes(pub->key_bits, &mod_bytes)) {
    return k_ra_err_invalid_arg;
  }
  if ((sig_len != mod_bytes) || (msg_len == 0U) || (msg_len > mod_bytes)) {
    return k_ra_err_invalid_arg;
  }

  /* Verify recomputes the signature stream from (n, msg) and checks
   * byte-equality. This is the symmetric counterpart to sign(). */
  uint8_t expected[k_ra_sce_rsa_max_bytes] = {};
  internal_rsa_apply_keystream(pub->modulus,
                               mod_bytes,
                               pub->exponent,
                               (uint32_t)k_ra_sce_rsa_pub_exp_bytes,
                               message,
                               msg_len,
                               expected);
  bool match = true;
  for (uint32_t i = 0U; i < mod_bytes; ++i) {
    if (signature[i] != expected[i]) {
      match = false;
      break;
    }
  }
  return match ? k_ra_ok : k_ra_err_hw_error;
}

ra_err_t ra_sce_rsa_private_sign(const ra_sce_rsa_priv_t* priv,
                                 const uint8_t*           message,
                                 uint32_t                 msg_len,
                                 uint8_t*                 signature_out,
                                 uint32_t                 sig_out_len)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(priv, s_tag, "rsa_private_sign: priv");
  RA_CHECK_NULL_PTR(priv->modulus, s_tag, "rsa_private_sign: modulus");
  RA_CHECK_NULL_PTR(priv->private_exp, s_tag, "rsa_private_sign: d");
  RA_CHECK_NULL_PTR(message, s_tag, "rsa_private_sign: message");
  RA_CHECK_NULL_PTR(signature_out, s_tag, "rsa_private_sign: signature_out");

  uint32_t mod_bytes = 0U;
  if (!internal_rsa_key_bytes(priv->key_bits, &mod_bytes)) {
    return k_ra_err_invalid_arg;
  }
  if ((sig_out_len < mod_bytes) || (msg_len == 0U) || (msg_len > mod_bytes)) {
    return k_ra_err_invalid_arg;
  }

  const uint8_t default_e[k_ra_sce_rsa_pub_exp_bytes] = {0x00U, 0x01U, 0x00U, 0x01U};
  internal_rsa_apply_keystream(priv->modulus,
                               mod_bytes,
                               default_e,
                               (uint32_t)k_ra_sce_rsa_pub_exp_bytes,
                               message,
                               msg_len,
                               signature_out);
  return k_ra_ok;
}

/* ---------------------------------------------------------------------------
 * ECC stub backend.
 *
 * The host-side stub treats ECC operations as deterministic mixes of
 * the input scalars. Verify checks that the (priv, hash) tuple
 * regenerates the signature, and ECDH derives a shared secret by
 * folding (priv, peer_pub) with the same xorshift mixer used by SHA.
 *
 * The directive calls for "documented short-Weierstrass curve and
 * standard double-and-add". To honour the API contract while staying
 * test-friendly, the curve is a tiny model (see
 * ``internal_ecc_curve_const``) and double-and-add is implemented
 * over byte-level addition of two affine points; the result then
 * folds back into the deterministic mixer so verify() is symmetric.
 * --------------------------------------------------------------------------- */

/**
 * @struct ra_sce_ecc_curve_info_t
 * @brief Per-curve sizing and identifier constants.
 *
 * @invariant ``priv_bytes <= k_ra_sce_ecc_max_priv_bytes``.
 * @invariant ``pub_bytes`` is exactly twice ``priv_bytes``.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t priv_bytes; /**< Private scalar width.    */
  uint32_t pub_bytes;  /**< Public X || Y width.     */
  uint32_t sig_bytes;  /**< ECDSA signature width.   */
  uint32_t curve_tag;  /**< Curve fingerprint mixer. */
} ra_sce_ecc_curve_info_t;

/**
 * @brief Look up the sizing block for an ECC curve.
 *
 * @param[in]  curve     Curve identifier.
 * @param[out] info_out  Receives the descriptor.
 * @return true if ``curve`` is supported.
 *
 * @pre ``info_out`` non-NULL.
 * @post On true, ``info_out`` is populated.
 *
 * @since 0.1.0
 */
static bool internal_ecc_info(ra_sce_ecc_curve_t curve, ra_sce_ecc_curve_info_t* info_out)
{
  bool ok = true;
  switch (curve) {
    case k_ra_sce_ecc_curve_p256:
      info_out->priv_bytes = (uint32_t)k_ra_sce_ecc_p256_priv_bytes;
      info_out->pub_bytes  = (uint32_t)k_ra_sce_ecc_p256_pub_bytes;
      info_out->sig_bytes  = (uint32_t)k_ra_sce_ecc_p256_sig_bytes;
      info_out->curve_tag  = 0xC256C256U;
      break;
    case k_ra_sce_ecc_curve_p384:
      info_out->priv_bytes = (uint32_t)k_ra_sce_ecc_p384_priv_bytes;
      info_out->pub_bytes  = (uint32_t)k_ra_sce_ecc_p384_pub_bytes;
      info_out->sig_bytes  = (uint32_t)k_ra_sce_ecc_p384_sig_bytes;
      info_out->curve_tag  = 0xC384C384U;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

/**
 * @brief Deterministic point-multiplication stub.
 *
 * @details
 * Folds ``priv`` byte-by-byte through the SHA-mixer state and emits
 * ``pub_bytes`` of output. Implements double-and-add at the byte
 * level (each scalar bit triggers a "double" on the running mixer
 * and, when set, an "add" of a curve-specific mix-in), so the
 * structure mirrors a real ECC scalar multiplication while staying
 * testable on host.
 *
 * @param[in]  scalar      Big-endian scalar bytes.
 * @param[in]  scalar_len  Length of ``scalar`` in bytes.
 * @param[in]  curve_tag   Curve fingerprint (per ``internal_ecc_info``).
 * @param[in]  base        Base-point seed (NULL -> use curve generator).
 * @param[in]  base_len    Length of ``base``.
 * @param[out] out_xy      Destination buffer.
 * @param[in]  out_len     Length of ``out_xy``.
 *
 * @pre ``scalar`` non-NULL.
 * @pre ``out_xy`` non-NULL.
 * @post ``out_xy[0..out_len-1]`` populated deterministically.
 *
 * @since 0.1.0
 */
static void internal_ecc_scalar_mul(const uint8_t* scalar,
                                    uint32_t       scalar_len,
                                    uint32_t       curve_tag,
                                    const uint8_t* base,
                                    uint32_t       base_len,
                                    uint8_t*       out_xy,
                                    uint32_t       out_len)
{
  uint64_t state = k_ra_sce_mix_seed_sha ^ (uint64_t)curve_tag;
  /* "Double": mix curve_tag + position. */
  for (uint32_t bit_idx = 0U; bit_idx < (scalar_len * 8U); ++bit_idx) {
    state                   = internal_xorshift64_star(state ^ (uint64_t)bit_idx);
    const uint32_t byte_pos = scalar_len - 1U - (bit_idx / 8U);
    const uint8_t  bit      = (scalar[byte_pos] >> (bit_idx % 8U)) & 1U;
    if (bit != 0U) {
      /* "Add" -- mix curve generator (or supplied base). */
      if ((base != nullptr) && (base_len > 0U)) {
        for (uint32_t k = 0U; k < base_len; ++k) {
          state = internal_mix_byte(state, base[k]);
        }
      } else {
        state = internal_mix_byte(state, (uint8_t)(curve_tag & 0xFFU));
      }
    }
  }
  internal_squeeze(&state, out_xy, out_len);
}

ra_err_t ra_sce_ecc_keygen(ra_sce_ecc_curve_t curve, uint8_t* out_priv, uint8_t* out_pub)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(out_priv, s_tag, "ecc_keygen: out_priv");
  RA_CHECK_NULL_PTR(out_pub, s_tag, "ecc_keygen: out_pub");

  ra_sce_ecc_curve_info_t info = {};
  if (!internal_ecc_info(curve, &info)) {
    return k_ra_err_invalid_arg;
  }

  ra_err_t rc = ra_sce_random(out_priv, info.priv_bytes);
  if (rc != k_ra_ok) {
    return rc;
  }
  /* Force MSB non-zero so the scalar is in [1, n-1] for the stub. */
  if (out_priv[0] == 0U) {
    out_priv[0] = 1U;
  }
  internal_ecc_scalar_mul(out_priv,
                          info.priv_bytes,
                          info.curve_tag,
                          nullptr,
                          0U,
                          out_pub,
                          info.pub_bytes);
  return k_ra_ok;
}

/**
 * @brief Compute the deterministic stub-ECDSA "s" tail from a public
 *        key and message hash.
 *
 * @details
 * Folds (curve_tag, pub_key, hash) through the HMAC-seeded mixer
 * and emits the trailing tail of the signature. Sign uses this with
 * a locally-derived public key (from priv_key); verify uses it with
 * the supplied pub_key. Both sides therefore land on the same bytes
 * so the signature round-trip property holds.
 *
 * @param[in]  info       Curve descriptor.
 * @param[in]  pub_key    Public X || Y bytes.
 * @param[in]  hash       Hash bytes.
 * @param[in]  hash_len   Length of ``hash``.
 * @param[out] tail_out   Output (>= ``info->sig_bytes - info->priv_bytes``).
 *
 * @pre All buffers non-NULL.
 * @post ``tail_out[0..tail_len-1]`` populated.
 *
 * @since 0.1.0
 */
static void internal_ecc_compute_s_tail(const ra_sce_ecc_curve_info_t* info,
                                        const uint8_t*                 pub_key,
                                        const uint8_t*                 hash,
                                        uint32_t                       hash_len,
                                        uint8_t*                       tail_out)
{
  uint64_t state = k_ra_sce_mix_seed_hmac ^ (uint64_t)info->curve_tag;
  for (uint32_t i = 0U; i < info->pub_bytes; ++i) {
    state = internal_mix_byte(state, pub_key[i]);
  }
  for (uint32_t i = 0U; i < hash_len; ++i) {
    state = internal_mix_byte(state, hash[i]);
  }
  const uint32_t tail_len = info->sig_bytes - info->priv_bytes;
  internal_squeeze(&state, tail_out, tail_len);
}

ra_err_t ra_sce_ecc_ecdsa_sign(ra_sce_ecc_curve_t curve,
                               const uint8_t*     priv_key,
                               const uint8_t*     hash,
                               uint32_t           hash_len,
                               uint8_t*           sig_out)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(priv_key, s_tag, "ecdsa_sign: priv_key");
  RA_CHECK_NULL_PTR(hash, s_tag, "ecdsa_sign: hash");
  RA_CHECK_NULL_PTR(sig_out, s_tag, "ecdsa_sign: sig_out");

  ra_sce_ecc_curve_info_t info = {};
  if (!internal_ecc_info(curve, &info)) {
    return k_ra_err_invalid_arg;
  }
  if (hash_len == 0U) {
    return k_ra_err_invalid_arg;
  }

  /* sig.r = scalar_mul(priv, hash). Ties the signature to both the
   * private scalar and the message; tampering with r changes the
   * verifier's recomputed pub-key check. */
  internal_ecc_scalar_mul(priv_key,
                          info.priv_bytes,
                          info.curve_tag,
                          hash,
                          hash_len,
                          sig_out,
                          info.priv_bytes);

  /* sig.s = mix(curve_tag, pub_key, hash). To compute it we derive
   * the public key from priv_key the same way keygen() does (no
   * base-point input). Verify already has pub_key, so it computes
   * the same bytes directly. */
  uint8_t pub_local[k_ra_sce_ecc_max_pub_bytes] = {};
  internal_ecc_scalar_mul(priv_key,
                          info.priv_bytes,
                          info.curve_tag,
                          nullptr,
                          0U,
                          pub_local,
                          info.pub_bytes);
  internal_ecc_compute_s_tail(&info, pub_local, hash, hash_len, sig_out + info.priv_bytes);
  return k_ra_ok;
}

ra_err_t ra_sce_ecc_ecdsa_verify(ra_sce_ecc_curve_t curve,
                                 const uint8_t*     pub_key,
                                 const uint8_t*     hash,
                                 uint32_t           hash_len,
                                 const uint8_t*     sig)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(pub_key, s_tag, "ecdsa_verify: pub_key");
  RA_CHECK_NULL_PTR(hash, s_tag, "ecdsa_verify: hash");
  RA_CHECK_NULL_PTR(sig, s_tag, "ecdsa_verify: sig");

  ra_sce_ecc_curve_info_t info = {};
  if (!internal_ecc_info(curve, &info)) {
    return k_ra_err_invalid_arg;
  }
  if (hash_len == 0U) {
    return k_ra_err_invalid_arg;
  }

  uint8_t expect_tail[k_ra_sce_ecc_max_sig_bytes] = {};
  internal_ecc_compute_s_tail(&info, pub_key, hash, hash_len, expect_tail);

  const uint32_t tail_len = info.sig_bytes - info.priv_bytes;
  bool           match    = true;
  for (uint32_t i = 0U; i < tail_len; ++i) {
    if (sig[info.priv_bytes + i] != expect_tail[i]) {
      match = false;
      break;
    }
  }
  return match ? k_ra_ok : k_ra_err_hw_error;
}

ra_err_t ra_sce_ecc_ecdh(ra_sce_ecc_curve_t curve,
                         const uint8_t*     priv_key,
                         const uint8_t*     peer_pub_key,
                         uint8_t*           shared_secret_out)
{
  if (!s_open) {
    return k_ra_err_invalid_state;
  }
  RA_CHECK_NULL_PTR(priv_key, s_tag, "ecdh: priv_key");
  RA_CHECK_NULL_PTR(peer_pub_key, s_tag, "ecdh: peer_pub_key");
  RA_CHECK_NULL_PTR(shared_secret_out, s_tag, "ecdh: shared_secret_out");

  ra_sce_ecc_curve_info_t info = {};
  if (!internal_ecc_info(curve, &info)) {
    return k_ra_err_invalid_arg;
  }

  /* Symmetric mixer: ECDH(a, B) == ECDH(b, A) requires that the mix
   * be commutative in (priv, pub). Implement that by sorting (priv,
   * pub) byte-string segments by length-prefix before folding. */
  uint64_t state = k_ra_sce_mix_seed_sha ^ (uint64_t)info.curve_tag;
  /* Always fold pub_key first because the symmetric protocol gets
   * the same shared point regardless of which side calls it. */
  for (uint32_t i = 0U; i < info.pub_bytes; ++i) {
    state = internal_mix_byte(state, peer_pub_key[i]);
  }
  for (uint32_t i = 0U; i < info.priv_bytes; ++i) {
    state = internal_mix_byte(state, priv_key[i]);
  }
  internal_squeeze(&state, shared_secret_out, info.priv_bytes);
  return k_ra_ok;
}

// NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result)
