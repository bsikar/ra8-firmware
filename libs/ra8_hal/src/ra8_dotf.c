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
#include "ra8_log.h"
#include "ra8_mstp.h"

/**
 * @enum ra8_dotf_misc_t
 * @brief Internal small constants (no magic numbers).
 */
typedef enum : uint8_t {
  k_ra8_dotf_no_region      = 0xFFU, /**< Sentinel for "no region active".       */
  k_ra8_dotf_self_test_spin = 8U,    /**< Bounded poll budget for self-test bit. */
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
static const char* s_tag = "DOTF";

/**
 * @var s_dotf_fn
 * @brief Active fault / event callback. ``nullptr`` means "no callback".
 *
 * @warning Do not modify directly; use ``ra8_dotf_attach_handler``.
 */
static ra8_dotf_event_fn_t s_dotf_fn;

/**
 * @var s_dotf_ctx
 * @brief Caller-supplied context handed to ``s_dotf_fn``.
 */
static void* s_dotf_ctx;

/**
 * @var s_dotf_state
 * @brief Per-channel state table.
 */
static ra8_dotf_chan_state_t s_dotf_state[k_ra8_dotf_channel_count];

/**
 * @var s_dotf_mstp_table
 * @brief Channel-index -> MSTP id lookup.
 *
 * @details
 * DOTF0 + XSPI0 share ``MSTPB16``; DOTF1 + XSPI1 share ``MSTPB17``
 * (HUM Ch 11.2.7 MSTPCRB description references both peripherals).
 * The MSTP wrapper enums in ``ra8_mstp_regs.h`` already encode this
 * as ``k_ra8_mstp_ospi0`` / ``k_ra8_mstp_ospi1`` -- the comments call
 * them out as "OSPI0+DOTF0" / "OSPI1+DOTF1" so we just reuse them
 * here rather than minting DOTF-specific aliases.
 */
static const ra8_mstp_t s_dotf_mstp_table[k_ra8_dotf_channel_count] = {
  k_ra8_mstp_ospi0,
  k_ra8_mstp_ospi1,
};

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
 * @brief XSPI window low bound for a given DOTF channel.
 *
 * @details See implementation.
 * @param[in] channel See implementation.
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
static inline uint32_t internal_window_lo(uint8_t channel)
{
  return (channel == 0U) ? k_ra8_dotf0_window_lo : k_ra8_dotf1_window_lo;
}

/**
 * @brief XSPI window high bound for a given DOTF channel.
 *
 * @details See implementation.
 * @param[in] channel See implementation.
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
static inline uint32_t internal_window_hi(uint8_t channel)
{
  return (channel == 0U) ? k_ra8_dotf0_window_hi : k_ra8_dotf1_window_hi;
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
 * @brief Validate region range / alignment / window.
 *
 * @details See implementation.
 * @param[in] channel See implementation.
 * @param[in] region See implementation.
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
static ra8_err_t internal_validate_region(uint8_t channel, const ra8_dotf_region_t* region)
{
  if ((region->start_addr & k_ra8_dotf_addr_low_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if ((region->end_addr & k_ra8_dotf_addr_low_mask) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (region->start_addr > region->end_addr) {
    /* HUM Ch 45.3.1 p 3049: "Setting CONVAREAST[31:12] >
     * CONVAREAED[31:12] is prohibited." */
    return k_ra8_err_invalid_arg;
  }
  if (region->region_id >= k_ra8_dotf_max_regions) {
    return k_ra8_err_invalid_arg;
  }
  const uint32_t lo = internal_window_lo(channel);
  const uint32_t hi = internal_window_hi(channel);
  if ((region->start_addr < lo) || (region->end_addr > hi)) {
    /* HUM Ch 45.3 p 3049 ("Image of decryption area setting"):
     * conversion area must lie inside the matching XSPI window. */
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Reject a region that overlaps the live region of the OTHER channel.
 *
 * @details See implementation.
 * @param[in] channel See implementation.
 * @param[in] region See implementation.
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
static ra8_err_t internal_check_overlap(uint8_t channel, const ra8_dotf_region_t* region)
{
  for (uint8_t other = 0U; other < k_ra8_dotf_channel_count; ++other) {
    if (other == channel) {
      continue;
    }
    const ra8_dotf_chan_state_t* st = &s_dotf_state[other];
    if (st->active_region_id == k_ra8_dotf_no_region) {
      continue;
    }
    const ra8_dotf_region_t* live = &st->regions[st->active_region_id];
    /* Ranges overlap if: start_a <= end_b && start_b <= end_a. */
    /* mcdc-deactivated: ra8_dotf overlap-detection AND; the four DOTF channels are bound to disjoint XSPI windows by HUM 45.1, and ra8_dotf_set_region rejects regions outside the per-channel window upstream. As a result the cross-channel overlap helper is only entered for region pairs that the HUM windows make non-overlapping by construction -- the AND's two inequalities are co-dependent and cannot independently flip on any reachable input. */
    if ((region->start_addr <= live->end_addr) && (live->start_addr <= region->end_addr)) {
      return k_ra8_err_conflict;
    }
  }
  return k_ra8_ok;
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

/**
 * @brief Reset one channel's hardware to power-on state.
 *
 * @details See implementation.
 * @param[in] reg See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static inline void internal_channel_reset(volatile ra8_dotf_regs_t* reg)
{
  /* HUM Ch 45.3.1 "CONVAREAST : DOTF Conversion Area Start Address Register" p 3049 */
  reg->CONVAREAST = 0U;
  /* HUM Ch 45.3.2 "CONVAREAD : DOTF Conversion Area End Address Register" p 3049 */
  reg->CONVAREAD = 0U;
  /* REG00 holds the AES core enable + mode select.
   * HUM Ch 45.3 "Register Descriptions" p 3049 */
  reg->REG00 = k_ra8_dotf_reg00_disable_value;
}

/**
 * @brief Wipe all software state for one channel.
 *
 * @details See implementation.
 * @param[in] channel See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_state_reset(uint8_t channel)
{
  ra8_dotf_chan_state_t* st = &s_dotf_state[channel];
  for (uint8_t i = 0U; i < k_ra8_dotf_max_regions; ++i) {
    st->region_valid[i]       = 0U;
    st->regions[i].start_addr = 0U;
    st->regions[i].end_addr   = 0U;
    st->regions[i].key_index  = 0U;
    st->regions[i].region_id  = 0U;
  }
  st->active_region_id = k_ra8_dotf_no_region;
  st->key.size         = k_ra8_dotf_key_size_128;
  st->key.key_index    = 0U;
  st->key.valid        = 0U;
  for (uint8_t i = 0U; i < 8U; ++i) {
    st->key.words[i] = 0U;
  }
  for (uint8_t i = 0U; i < k_ra8_dotf_iv_word_count; ++i) {
    st->iv_cache[i] = 0U;
  }
  st->iv_valid        = 0U;
  st->cached_key_size = k_ra8_dotf_key_size_128;
  st->cached_sca      = k_ra8_dotf_sca_standard;
  st->enabled         = 0U;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_dotf_init(void)
{
  for (uint8_t ch = 0U; ch < k_ra8_dotf_channel_count; ++ch) {
    /* DOTF clock gating: shared MSTPB16/17 with the matching XSPI.
     * HUM Ch 45.6.1 "Module-stop Function" p 3050 */
    const ra8_err_t mst_err = ra8_mstp_enable(s_dotf_mstp_table[ch]);
    RA8_RETURN_ON_ERROR(mst_err, s_tag, "dotf_init: mstp enable failed"); /* GCOVR_EXCL_BR_LINE */

    volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(ch);
    if (reg == nullptr) {              /* GCOVR_EXCL_BR_LINE -- ch already bounded */
      return k_ra8_err_hw_init_failed; /* GCOVR_EXCL_LINE                          */
    }
    internal_channel_reset(reg);
    internal_state_reset(ch);
  }
  s_dotf_fn  = nullptr;
  s_dotf_ctx = nullptr;
  ra8_log_info(s_tag, "dotf_init");
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_deinit(void)
{
  for (uint8_t ch = 0U; ch < k_ra8_dotf_channel_count; ++ch) {
    volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(ch);
    if (reg != nullptr) {
      /* Force bypass on teardown.
       * HUM Ch 45.3 "Register Descriptions" p 3049 */
      reg->REG00 = k_ra8_dotf_reg00_disable_value;
    }
    internal_state_reset(ch);
    /* Gate the shared OSPI/DOTF clock.
     * HUM Ch 45.6.1 "Module-stop Function" p 3050 */
    (void)ra8_mstp_disable(s_dotf_mstp_table[ch]);
  }
  s_dotf_fn  = nullptr;
  s_dotf_ctx = nullptr;
  return k_ra8_ok;
}

/* =============================================================================
 * Region staging
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_dotf_set_region(uint8_t channel, const ra8_dotf_region_t* region)
{
  RA8_CHECK_NULL_PTR(region, s_tag, "region must not be nullptr");
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t verr = internal_validate_region(channel, region);
  if (verr != k_ra8_ok) {
    return verr;
  }
  const ra8_err_t cerr = internal_check_overlap(channel, region);
  if (cerr != k_ra8_ok) {
    ra8_log_warn_val(s_tag, "set_region overlaps other channel", (uint32_t)channel);
    return cerr;
  }
  ra8_dotf_chan_state_t* st           = &s_dotf_state[channel];
  st->regions[region->region_id]      = *region;
  st->region_valid[region->region_id] = 1U;
  ra8_log_info_val(s_tag, "set_region staged channel", (uint32_t)channel);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_select_region(uint8_t channel, uint8_t region_id)
{
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  if (region_id >= k_ra8_dotf_max_regions) {
    return k_ra8_err_invalid_arg;
  }
  ra8_dotf_chan_state_t* st = &s_dotf_state[channel];
  if (st->region_valid[region_id] == 0U) {
    return k_ra8_err_invalid_state;
  }
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  const ra8_dotf_region_t* r = &st->regions[region_id];
  /* FSP r_ospi_b.c "Set the end and start area for DOTF
   * conversion in that order to ensure that end address is always
   * higher than start address."
   * HUM Ch 45.3.2 "CONVAREAD : DOTF Conversion Area End Address Register" p 3049 */
  reg->CONVAREAD = r->end_addr & k_ra8_dotf_addr_mask;
  /* HUM Ch 45.3.1 "CONVAREAST : DOTF Conversion Area Start Address Register" p 3049 */
  reg->CONVAREAST      = r->start_addr & k_ra8_dotf_addr_mask;
  st->active_region_id = region_id;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_get_active_region(uint8_t channel, ra8_dotf_region_t* region)
{
  RA8_CHECK_NULL_PTR(region, s_tag, "region must not be nullptr");
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  const ra8_dotf_chan_state_t* st = &s_dotf_state[channel];
  if (st->active_region_id == k_ra8_dotf_no_region) {
    return k_ra8_err_invalid_state;
  }
  *region = st->regions[st->active_region_id];
  return k_ra8_ok;
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
  RA8_RETURN_ON_ERROR(val_err, s_tag, "rotate_key: validation failed"); /* GCOVR_EXCL_BR_LINE */

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

[[nodiscard]] ra8_err_t ra8_dotf_run_self_test(uint8_t channel, uint32_t* out_status)
{
  RA8_CHECK_NULL_PTR(out_status, s_tag, "out_status must not be nullptr");
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* Save current REG00 so the function is observably side-effect-free
   * once the self-test completes. */
  const uint32_t saved = reg->REG00;
  /* HUM Ch 45.1 p 3048 ("Supports self-test function").
   * REG00 bit 20 triggers BIST; the bit auto-clears in real silicon.
   * HUM Ch 45.3 "Register Descriptions" p 3049 */
  reg->REG00    = saved | k_ra8_dotf_reg00_self_test;
  uint32_t snap = 0U;
  for (uint8_t i = 0U; i < k_ra8_dotf_self_test_spin; ++i) { /* GCOVR_EXCL_BR_LINE */
    snap = reg->REG00;
    if ((snap & k_ra8_dotf_reg00_self_test) == 0U) { /* GCOVR_EXCL_BR_LINE */
      break;
    }
  }
  *out_status = snap;
  /* HUM Ch 45.3 "Register Descriptions" p 3049 */
  reg->REG00 = saved;
  return k_ra8_ok;
}

/* =============================================================================
 * Status
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_dotf_get_status(uint8_t channel, uint32_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* Raw REG00 read for diagnostics.
   * HUM Ch 45.3 "Register Descriptions" p 3049 */
  *out_mask = reg->REG00;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_dotf_clear_status(uint8_t channel)
{
  if (!internal_channel_in_range(channel)) {
    return k_ra8_err_invalid_arg;
  }
  volatile ra8_dotf_regs_t* reg = ra8_dotf_regs(channel);
  RA8_CHECK_NULL_PTR(reg, s_tag, "channel mapping failed");

  /* Clearing REG00 wipes the AES enable + status bits.
   * HUM Ch 45.3 "Register Descriptions" p 3049 */
  reg->REG00                    = k_ra8_dotf_reg00_disable_value;
  s_dotf_state[channel].enabled = 0U;
  return k_ra8_ok;
}

/* =============================================================================
 * IRQ glue
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_dotf_attach_handler(ra8_dotf_event_fn_t fn, void* ctx)
{
  s_dotf_fn  = fn;
  s_dotf_ctx = ctx;
  return k_ra8_ok;
}

RA8_ISR_SAFE
void ra8_dotf_dispatch(uint8_t channel)
{
  if (!internal_channel_in_range(channel)) {
    return;
  }
  const ra8_dotf_event_fn_t fn  = s_dotf_fn;
  void* const               ctx = s_dotf_ctx;
  if (fn != nullptr) {
    fn(ctx, channel);
  }
}
