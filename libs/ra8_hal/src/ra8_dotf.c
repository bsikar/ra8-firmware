/**
 * @file ra8_dotf.c
 * @brief Decryption On The Fly (DOTF) HAL driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Full HUM Ch 45 (p 3048..3050) coverage of the RA8D2 DOTF block.
 * Layered on top of the OSPI MSTP gating; every register access
 * carries a HUM Ch 45 citation. See ``ra8_dotf.h`` for the public
 * surface.
 *
 * Driver-side state:
 *
 *  - per-channel staging table of up to ``k_ra8_dotf_max_regions``
 *    region descriptors (multi-region support);
 *  - per-channel cache of the active region id (or "none");
 *  - per-channel REG00 cache (key size, SCA level, enable bit) so
 *    that ``ra8_dotf_set_sca_level`` / ``ra8_dotf_set_key_size`` can
 *    update the live AES core in a single REG00 write;
 *  - per-channel bound key handle (RSIP-key-injection wiring);
 *  - per-channel IV cache so ``ra8_dotf_rotate_key`` can re-stage the
 *    same IV without forcing the caller to remember it;
 *  - shared callback slot for IRQ glue.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_dotf.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_dotf_regs.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

/**
 * @enum ra8_dotf_misc_t
 * @brief Internal small constants (no magic numbers).
 */
typedef enum : uint8_t {
  k_ra8_dotf_no_region = 0xFFU, /**< Sentinel for "no region active". */
} ra8_dotf_misc_t;

/**
 * @enum ra8_dotf_bswap_const_t
 * @brief Byte-extraction masks and shift counts for ``internal_bswap32``.
 *
 * @details
 * REG03 of the OSPI / DOTF FIFO is big-endian; both the host build and
 * the RA8D2 are little-endian, so we always swap. These named
 * constants replace the magic numbers flagged by clang-tidy
 * (readability-magic-numbers) and document each byte position.
 */
typedef enum : uint32_t {
  k_ra8_dotf_bswap_byte_mask = 0xFFUL,       /**< Per-byte mask used by all 4 lanes. */
  k_ra8_dotf_bswap_byte0     = 0x000000FFUL, /**< Selects bits  [7:0]  (byte 0).     */
  k_ra8_dotf_bswap_byte1     = 0x0000FF00UL, /**< Selects bits [15:8]  (byte 1).     */
  k_ra8_dotf_bswap_byte2     = 0x00FF0000UL, /**< Selects bits [23:16] (byte 2).     */
  k_ra8_dotf_bswap_byte3     = 0xFF000000UL, /**< Selects bits [31:24] (byte 3).     */
} ra8_dotf_bswap_const_t;

/**
 * @enum ra8_dotf_bswap_shift_t
 * @brief Shift counts used by ``internal_bswap32``.
 */
typedef enum : uint8_t {
  k_ra8_dotf_bswap_shift_byte = 8U,  /**< Shift for one-byte slide.  */
  k_ra8_dotf_bswap_shift_word = 24U, /**< Shift for byte0 <-> byte3. */
} ra8_dotf_bswap_shift_t;

/**
 * @enum ra8_dotf_key_word_count_t
 * @brief Wrapped-key word counts per AES key size.
 *
 * @details
 * The wrapped-key payload bytes are a vendor-defined RSIP envelope;
 * the FSP reference uses ``HW_SCE_AES{128,192,256}_KEY_INDEX_WORD_SIZE``
 * for the ratio. RA8D2 uses 4-word / 6-word / 8-word envelopes for
 * the 128 / 192 / 256-bit keys respectively when staged through the
 * ``OutputKeyForDotf`` paths (``r_ospi_b.c``).
 */
typedef enum : uint8_t {
  k_ra8_dotf_key_words_128 = 4U, /**< RA8 dotf key words 128. */
  k_ra8_dotf_key_words_192 = 6U, /**< RA8 dotf key words 192. */
  k_ra8_dotf_key_words_256 = 8U, /**< RA8 dotf key words 256. */
} ra8_dotf_key_word_count_t;

/**
 * @struct ra8_dotf_chan_state_t
 * @brief Per-channel software state.
 */
typedef struct {
  ra8_dotf_region_t     regions[k_ra8_dotf_max_regions];      /**< Regions.                 */
  uint8_t               region_valid[k_ra8_dotf_max_regions]; /**< 1 if slot armed.         */
  uint8_t               active_region_id;                     /**< or k_ra8_dotf_no_region. */
  ra8_dotf_key_handle_t key;                                  /**< Key.                     */
  uint32_t              iv_cache[k_ra8_dotf_iv_word_count];   /**< Iv cache.                */
  uint8_t               iv_valid;                             /**< Iv valid.                */
  ra8_dotf_key_size_t   cached_key_size;                      /**< Cached key size.         */
  ra8_dotf_sca_level_t  cached_sca;                           /**< Cached sca.              */
  uint8_t               enabled;                              /**< Enabled.                 */
} ra8_dotf_chan_state_t;

/**
 * @var s_tag
 * @brief Logging tag for ra8_log_* calls.
 */
static const char* const s_tag = "DOTF";

/**
 * @var s_dotf_fn
 * @brief Active fault / event callback, defined in src/dotf_handler_abi.zig.
 *
 * @warning Do not modify directly; use ``ra8_dotf_attach_handler``.
 */
extern ra8_dotf_event_fn_t s_dotf_fn;

/**
 * @var s_dotf_ctx
 * @brief Caller-supplied context handed to ``s_dotf_fn`` (defined in Zig).
 */
extern void* s_dotf_ctx;

/**
 * @var s_dotf_state
 * @brief Per-channel state table, defined in src/dotf_state_abi.zig.
 */
extern ra8_dotf_chan_state_t s_dotf_state[k_ra8_dotf_channel_count];

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Bound-check a channel index.
 *
 * @param[in] channel Caller-provided channel value.
 * @return ``true`` if ``channel`` is in [0, k_ra8_dotf_channel_count).
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline bool internal_channel_in_range(uint8_t channel)
{
  return (uint16_t)channel < (uint16_t)k_ra8_dotf_channel_count;
}

/**
 * @brief Word count for a given AES key size.
 *
 * @details See implementation.
 * @param[in] size See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint8_t internal_key_words(ra8_dotf_key_size_t size)
{
  if (size == k_ra8_dotf_key_size_192) {
    return k_ra8_dotf_key_words_192;
  }
  if (size == k_ra8_dotf_key_size_256) {
    return k_ra8_dotf_key_words_256;
  }
  return k_ra8_dotf_key_words_128;
}

/**
 * @brief Map an SCA level enum into REG00 SCA bits.
 *
 * @details See implementation.
 * @param[in] level See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint32_t internal_sca_bits(ra8_dotf_sca_level_t level)
{
  if (level == k_ra8_dotf_sca_max) {
    return k_ra8_dotf_reg00_sca_en | k_ra8_dotf_reg00_sca_mode;
  }
  if (level == k_ra8_dotf_sca_standard) {
    return k_ra8_dotf_reg00_sca_en;
  }
  return 0U;
}

/**
 * @brief Big-endian byte-swap of a 32-bit word.
 *
 * @details
 * REG03 is a big-endian FIFO per the FSP reference (``r_ospi_b.c``
 * uses ``bswap_32big`` / ``change_endian_long``). The host build
 * runs little-endian and the target Cortex-M85 also runs little-
 * endian, so an explicit byte-swap is required either way.
 *
 * @param[in] v See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline uint32_t internal_bswap32(uint32_t v)
{
  return ((v & k_ra8_dotf_bswap_byte0) << (uint32_t)k_ra8_dotf_bswap_shift_word) |
         ((v & k_ra8_dotf_bswap_byte1) << (uint32_t)k_ra8_dotf_bswap_shift_byte) |
         ((v & k_ra8_dotf_bswap_byte2) >> (uint32_t)k_ra8_dotf_bswap_shift_byte) |
         ((v & k_ra8_dotf_bswap_byte3) >> (uint32_t)k_ra8_dotf_bswap_shift_word);
}

/**
 * @brief Assemble the REG00 word for the channel's cached state.
 *
 * @details See implementation.
 * @param[in] st See implementation.
 * @param[in] enable See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_assemble_reg00(const ra8_dotf_chan_state_t* st, bool enable)
{
  uint32_t v = k_ra8_dotf_reg00_mode_ctr; /* HUM 45.1 mode = CTR. */
  v |= (uint32_t)st->cached_key_size;     /* Key size bits.       */
  v |= internal_sca_bits(st->cached_sca); /* SCA bits.            */
  if (enable) {
    v |= k_ra8_dotf_reg00_aes_enable;
  }
  return v;
}

/**
 * @brief Stage a wrapped-key payload into REG03.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @param[in] h See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_stage_key(volatile ra8_dotf_regs_t* reg, const ra8_dotf_key_handle_t* h)
{
  const uint8_t words = internal_key_words(h->size);
  for (uint8_t i = 0U; i < words; ++i) {
    /* HUM Ch 45.3 "Register Descriptions" p 3049: REG03 is the AES
     * IV staging window; the FSP reference re-uses it for wrapped-key
     * delivery via the OutputKeyForDotf adaptor. Big-endian per
     * ``r_ospi_b.c``. */
    reg->REG03 = internal_bswap32(h->words[i]);
  }
}

/**
 * @brief Stage 4 IV words into REG03 in big-endian order.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @param[in] iv See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_stage_iv(volatile ra8_dotf_regs_t* reg, const uint32_t* iv)
{
  for (uint8_t i = 0U; i < k_ra8_dotf_iv_word_count; ++i) {
    /* HUM Ch 45.1 p 3048 -- counter = {IV[127:28], Address[31:4]}.
     * REG03 is the AES IV staging window per HUM Ch 45.3 "Register
     * Descriptions" p 3049. */
    reg->REG03 = internal_bswap32(iv[i]);
  }
}

/* =============================================================================
 * Key + IV staging
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_dotf_install_key(uint8_t channel, const ra8_dotf_key_handle_t* handle)
{
  RA8_CHECK_NULL_PTR(handle, s_tag, "handle must not be nullptr");
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  if (handle->valid == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((handle->size != k_ra8_dotf_key_size_128) && (handle->size != k_ra8_dotf_key_size_192) &&
      (handle->size != k_ra8_dotf_key_size_256)) {
    return k_ra8_err_invalid_arg;
  }
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  ra8_dotf_chan_state_t* st = &s_dotf_state[channel];
  st->key                   = *handle;
  st->cached_key_size       = handle->size;
  internal_stage_key(reg, handle);
  ra8_log_info_val(s_tag, "install_key channel", (uint32_t)channel);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_set_iv(uint8_t channel, const uint32_t* iv_words)
{
  RA8_CHECK_NULL_PTR(iv_words, s_tag, "iv_words must not be nullptr");
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  ra8_dotf_chan_state_t* st = &s_dotf_state[channel];
  for (uint8_t i = 0U; i < k_ra8_dotf_iv_word_count; ++i) {
    st->iv_cache[i] = iv_words[i];
  }
  st->iv_valid = 1U;
  internal_stage_iv(reg, iv_words);
  return k_ra8_ok;
}

/**
 * @brief Re-stage the IV for a rotate-key call.
 *
 * @details
 * If the caller provided ``iv_words`` we cache them and push them
 * through ``internal_stage_iv``. If the caller passed nullptr but a
 * previous IV is cached, re-stage that one. Otherwise leave the IV
 * registers untouched. HUM Ch 45.3 "Register Descriptions" p 3049.
 *
 * @param[in,out] st        Channel state slot.
 * @param[in]     reg       MMIO base for the channel.
 * @param[in]     iv_words  Optional new IV word array.
 *
 * @pre ``st`` and ``reg`` are non-null and refer to the same channel.
 * @post IV registers reflect the new IV when one was supplied or cached.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_rotate_iv(ra8_dotf_chan_state_t*    st,
                               volatile ra8_dotf_regs_t* reg,
                               const uint32_t*           iv_words)
{
  if (iv_words != nullptr) {
    for (uint8_t i = 0U; i < k_ra8_dotf_iv_word_count; ++i) {
      st->iv_cache[i] = iv_words[i];
    }
    st->iv_valid = 1U;
    internal_stage_iv(reg, iv_words);
  } else if (st->iv_valid != 0U) {
    internal_stage_iv(reg, st->iv_cache);
  } else {
    /* No IV ever installed, no IV to re-stage. */
  }
}

/**
 * @brief Validate the inputs to ``ra8_dotf_rotate_key``.
 *
 * @details
 * Range-checks ``channel`` and the wrapped-key fields, and rejects
 * calls that try to rotate before any region was activated. The check
 * for ``new_handle != nullptr`` is the caller's responsibility.
 *
 * @param[in] channel    Channel index.
 * @param[in] new_handle Caller-supplied wrapped key.
 *
 * @return ``k_ra8_ok`` if all preconditions pass.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range or handle malformed.
 * @retval k_ra8_err_invalid_state ``ra8_dotf_install_region`` not yet called.
 *
 * @pre ``new_handle`` is non-null.
 * @post No side effects.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_validate_rotate_inputs(uint8_t                      channel,
                                                 const ra8_dotf_key_handle_t* new_handle)
{
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  if (new_handle->valid == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((new_handle->size != k_ra8_dotf_key_size_128) &&
      (new_handle->size != k_ra8_dotf_key_size_192) &&
      (new_handle->size != k_ra8_dotf_key_size_256)) {
    return k_ra8_err_invalid_arg;
  }
  if (s_dotf_state[channel].active_region_id == k_ra8_dotf_no_region) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_rotate_key(uint8_t                      channel,
                                            const ra8_dotf_key_handle_t* new_handle,
                                            const uint32_t*              iv_words)
{
  RA8_CHECK_NULL_PTR(new_handle, s_tag, "new_handle must not be nullptr");
  const ra8_err_t val_err = internal_validate_rotate_inputs(channel, new_handle);
  RA8_RETURN_ON_ERROR(val_err, s_tag, "rotate_key: validation failed");

  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  ra8_dotf_chan_state_t* st          = &s_dotf_state[channel];
  const uint8_t          was_enabled = st->enabled;

  /* Step 1: quiesce the AES core.
   * HUM Ch 45.3 "Register Descriptions" p 3049 */
  reg->REG00  = k_ra8_dotf_reg00_disable_value;
  st->enabled = 0U;

  /* Step 2: replace key + (optionally) IV. */
  st->key             = *new_handle;
  st->cached_key_size = new_handle->size;
  internal_stage_key(reg, new_handle);
  internal_rotate_iv(st, reg, iv_words);

  /* Step 3: re-arm if previously enabled. */
  if (was_enabled != 0U) {
    /* HUM Ch 45.3 "Register Descriptions" p 3049 */
    reg->REG00  = internal_assemble_reg00(st, true);
    st->enabled = 1U;
  }
  ra8_log_info_val(s_tag, "rotate_key channel", (uint32_t)channel);
  return k_ra8_ok;
}

/* =============================================================================
 * Enable / disable
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_dotf_enable(uint8_t channel)
{
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  ra8_dotf_chan_state_t* st = &s_dotf_state[channel];
  /* REG00 enables AES; pattern is (mode=CTR | key_size | sca | enable).
   * The reset-default ``0x2200_0000`` matches mode=CTR + key_size=128
   * with SCA off; the cached state may override every field.
   * HUM Ch 45.3 "Register Descriptions" p 3049 */
  reg->REG00  = internal_assemble_reg00(st, true);
  st->enabled = 1U;
  ra8_log_info_val(s_tag, "enable channel", (uint32_t)channel);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_disable(uint8_t channel)
{
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* Writing 0 to REG00 puts the channel in bypass.
   * HUM Ch 45.3 "Register Descriptions" p 3049 */
  reg->REG00                    = k_ra8_dotf_reg00_disable_value;
  s_dotf_state[channel].enabled = 0U;
  return k_ra8_ok;
}

/* =============================================================================
 * REG00 sub-field tuning
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_dotf_set_sca_level(uint8_t channel, ra8_dotf_sca_level_t level)
{
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  if ((level != k_ra8_dotf_sca_off) && (level != k_ra8_dotf_sca_standard) &&
      (level != k_ra8_dotf_sca_max)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_dotf_chan_state_t* st = &s_dotf_state[channel];
  st->cached_sca            = level;
  if (st->enabled != 0U) {
    volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
    RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");
    /* HUM Ch 45.3 "Register Descriptions" p 3049 */
    reg->REG00 = internal_assemble_reg00(st, true);
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_set_key_size(uint8_t channel, ra8_dotf_key_size_t size)
{
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  if ((size != k_ra8_dotf_key_size_128) && (size != k_ra8_dotf_key_size_192) &&
      (size != k_ra8_dotf_key_size_256)) {
    return k_ra8_err_invalid_arg;
  }
  ra8_dotf_chan_state_t* st = &s_dotf_state[channel];
  st->cached_key_size       = size;
  if (st->enabled != 0U) {
    volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
    RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");
    /* HUM Ch 45.3 "Register Descriptions" p 3049 */
    reg->REG00 = internal_assemble_reg00(st, true);
  }
  return k_ra8_ok;
}
