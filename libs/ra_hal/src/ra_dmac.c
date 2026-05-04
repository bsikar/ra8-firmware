/**
 * @file ra_dmac.c
 * @brief DMAC0 channel driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * FSP-aligned DMAC0 channel programming. The mapping from FSP's
 * `r_dmac_config_transfer_info` (`r_dmac.c` lines 551-682) is:
 *
 *  - DMTMD.SZ   <- cfg->width        (HUM 17.2.10 p 738)
 *  - DMTMD.MD   <- cfg->mode         (HUM 17.2.10 p 738)
 *  - DMTMD.DTS  <- cfg->repeat_area  (HUM 17.2.10 p 738)
 *  - DMTMD.DCTG <- 0 (software request -- ELC trigger lives in DELSR)
 *  - DMAMD.SM   <- cfg->src_inc ? 10b : 00b (HUM 17.2.12 p 741)
 *  - DMAMD.DM   <- cfg->dst_inc ? 10b : 00b
 *  - DMINT.DTIE <- cfg->enable_dtie  (HUM 17.2.11 p 739)
 *  - DMAST.DMST <- 1 (HUM 17.2.20 p 749 -- shared activation gate)
 *  - DMCNT.DTE  <- 1 (HUM 17.2.14 p 743)
 *
 * Intentional gaps versus FSP:
 *  - No `DELSR` programming -- ELC routing is owned by the higher-
 *    level `ra_dma` substrate / individual driver wrappers.
 *  - No `DMSRR / DMDRR / DMSBS / DMDBS` programming -- repeat-block
 *    mode is selectable via `cfg->mode` but the repeat-area buffers
 *    must be zero-extended by the caller (config struct is the
 *    minimum the project uses today).
 *  - No `DMOFR` (offset-addition mode) -- always written 0.
 *  - No 64-bit transfer width.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_dmac.h"

#include <stdint.h>

#include "ra8d2_dmac_regs.h"
#include "ra_check.h"
#include "ra_dmac_internal.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

/**
 * @brief Pure DTS-disabled predicate -- see header for full contract.
 * @details Promoted helper so the line-151 OR can be driven under MC/DC.
 * @param[in] mode_normal_val       Numeric value of @c k_ra_dmac_mode_normal.
 * @param[in] mode_repeat_block_val Numeric value of @c k_ra_dmac_mode_repeat_block.
 * @param[in] mode                  Candidate mode value.
 * @return Boolean predicate.
 * @retval true  Mode disables DTS.
 * @retval false Mode requires non-zero DTS code.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_dmac_internal_mode_disables_dts(uint32_t mode_normal_val,
                                        uint32_t mode_repeat_block_val,
                                        uint32_t mode)
{
  return (mode == mode_normal_val) || (mode == mode_repeat_block_val);
}

/**
 * @brief Pure extra-IRQ predicate -- see header for full contract.
 * @details Promoted helper so the line-246 AND can be driven under MC/DC.
 * @param[in] irq_each              Boolean: per-block IRQ enable.
 * @param[in] mode_repeat_block_val Numeric value of @c k_ra_dmac_mode_repeat_block.
 * @param[in] mode                  Candidate mode value.
 * @return Boolean predicate.
 * @retval true  Caller must OR in RPTIE | ESIE.
 * @retval false Leave RPTIE / ESIE clear.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_dmac_internal_dmint_extra_irq(bool irq_each, uint32_t mode_repeat_block_val, uint32_t mode)
{
  return irq_each && (mode != mode_repeat_block_val);
}

static const char* s_tag = "DMAC";

/* =============================================================================
 * Local helpers
 * =============================================================================
 */

/**
 * @brief Translate `cfg->width` -> `DMTMD.SZ` 2-bit code.
 *
 * @details
 * Maps the driver's ``ra_dmac_width_t`` enum onto the 2-bit ``SZ``
 * field in DMTMD (HUM Ch 16.2.4 "DMTMD : DMA Transfer Mode Register",
 * p 608). Defaults to byte-width when given an out-of-range value.
 *
 * @param[in] width Transfer width enum value.
 *
 * @return DMTMD.SZ field bits (already shifted into position 0).
 * @retval k_ra_dmtmd_sz_byte 8-bit transfers.
 * @retval k_ra_dmtmd_sz_half 16-bit transfers.
 * @retval k_ra_dmtmd_sz_word 32-bit transfers.
 *
 * @pre ``width`` is a valid ``ra_dmac_width_t`` value.
 * @pre Caller is composing a fresh DMTMD value.
 * @post No state mutated.
 * @post Result fits in the DMTMD.SZ field width.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static inline uint16_t internal_sz_code(ra_dmac_width_t width)
{
  switch (width) {
    case k_ra_dmac_width_byte:
      return k_ra_dmtmd_sz_byte;
    case k_ra_dmac_width_half:
      return k_ra_dmtmd_sz_half;
    case k_ra_dmac_width_word:
      return k_ra_dmtmd_sz_word;
    default:
      return k_ra_dmtmd_sz_byte;
  }
}

/**
 * @brief Translate `cfg->mode` -> `DMTMD.MD` 2-bit code.
 *
 * @details
 * Maps the driver's ``ra_dmac_mode_t`` enum onto the 2-bit ``MD``
 * field in DMTMD (HUM Ch 16.2.4, p 608). Defaults to normal mode when
 * given an out-of-range value.
 *
 * @param[in] mode Transfer mode enum value.
 *
 * @return DMTMD.MD field bits (already shifted into position 0).
 * @retval k_ra_dmtmd_md_normal       Normal-transfer mode.
 * @retval k_ra_dmtmd_md_repeat       Repeat-transfer mode.
 * @retval k_ra_dmtmd_md_block        Block-transfer mode.
 * @retval k_ra_dmtmd_md_repeat_block Repeat-block-transfer mode.
 *
 * @pre ``mode`` is a valid ``ra_dmac_mode_t`` value.
 * @pre Caller is composing a fresh DMTMD value.
 * @post No state mutated.
 * @post Result fits in the DMTMD.MD field width.
 *
 * @note Pure function; thread-safe.
 * @since 0.1.0
 */
static inline uint16_t internal_md_code(ra_dmac_mode_t mode)
{
  switch (mode) {
    case k_ra_dmac_mode_normal:
      return k_ra_dmtmd_md_normal;
    case k_ra_dmac_mode_repeat:
      return k_ra_dmtmd_md_repeat;
    case k_ra_dmac_mode_block:
      return k_ra_dmtmd_md_block;
    case k_ra_dmac_mode_repeat_block:
      return k_ra_dmtmd_md_repeat_block;
    default:
      return k_ra_dmtmd_md_normal;
  }
}

/**
 * @brief Translate `cfg->repeat_area` -> `DMTMD.DTS` 2-bit code.
 *
 * @details
 * In normal and repeat-block mode the field is forced to "none"
 * (HUM 17.2.10 p 738 -- "In normal or repeat-block transfer mode,
 * setting these bits is invalid").
 *
 * @param[in,out] area See function signature.
 * @param[in,out] mode See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline uint16_t internal_dts_code(ra_dmac_mode_t mode, ra_dmac_repeat_area_t area)
{
  if (ra_dmac_internal_mode_disables_dts((uint32_t)k_ra_dmac_mode_normal,
                                         (uint32_t)k_ra_dmac_mode_repeat_block,
                                         (uint32_t)mode)) {
    return k_ra_dmtmd_dts_none;
  }
  switch (area) {
    case k_ra_dmac_repeat_area_dest:
      return k_ra_dmtmd_dts_dest_repeat;
    case k_ra_dmac_repeat_area_src:
      return k_ra_dmtmd_dts_src_repeat;
    case k_ra_dmac_repeat_area_none:
    default:
      return k_ra_dmtmd_dts_none;
  }
}

/**
 * @brief Compose the `DMAMD` register value from the increment flags.
 *
 * @details
 * Incrementing -> `SM/DM = 10b`; clear -> `00b` (fixed). HUM 17.2.12
 * p 741. Offset-addition (`01b`) and decrement (`11b`) are not
 * exposed by `ra_dmac_config_t` today.
 *
 * @param[in,out] dst_inc See function signature.
 * @param[in,out] src_inc See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline uint16_t internal_dmamd_value(bool src_inc, bool dst_inc)
{
  uint16_t v = 0U;
  if (src_inc) {
    v |= (uint16_t)(k_ra_dmamd_addr_increment << k_ra_dmamd_sm_pos);
  }
  if (dst_inc) {
    v |= (uint16_t)(k_ra_dmamd_addr_increment << k_ra_dmamd_dm_pos);
  }
  return v;
}

/**
 * @brief Compose the `DMTMD` register value.
 *
 * @details
 * DCTG is left at 00b (software request); ELC routing is the
 * substrate's job via DELSR. SZ / MD / DTS are positioned per HUM.
 *
 * @param[in,out] cfg See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline uint16_t internal_dmtmd_value(const ra_dmac_config_t* cfg)
{
  uint16_t v = 0U;
  v |= (uint16_t)(internal_sz_code(cfg->width) << k_ra_dmtmd_sz_pos);
  v |= (uint16_t)(internal_md_code(cfg->mode) << k_ra_dmtmd_md_pos);
  v |= (uint16_t)(internal_dts_code(cfg->mode, cfg->repeat_area) << k_ra_dmtmd_dts_pos);
  return v;
}

/**
 * @brief Compose the `DMINT` register value.
 *
 * @details
 * Mirrors FSP's "if callback configured, enable DTIE; if irq_each
 * and not repeat-block, also enable RPTIE | ESIE" rule
 * (`r_dmac.c` lines 607-622). HUM 17.2.11 p 739.
 *
 * @param[in,out] cfg See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline uint8_t internal_dmint_value(const ra_dmac_config_t* cfg)
{
  uint8_t v = 0U;
  if (cfg->enable_dtie) {
    v |= k_ra_dmint_dtie_mask;
  }
  if (ra_dmac_internal_dmint_extra_irq(cfg->irq_each,
                                       (uint32_t)k_ra_dmac_mode_repeat_block,
                                       (uint32_t)cfg->mode)) {
    v |= (uint8_t)(k_ra_dmint_rptie_mask | k_ra_dmint_esie_mask);
  }
  return v;
}

/**
 * @brief Compose the `DMCRA` register value.
 *
 * @details
 * Low half is the running count; the FSP repeat / block / repeat-
 * block path also copies the same value into the high reload field
 * (`r_dmac.c` line 582). HUM 17.2.8.
 *
 * @param[in,out] cfg See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline uint32_t internal_dmcra_value(const ra_dmac_config_t* cfg)
{
  uint32_t v = (uint32_t)cfg->count;
  if (cfg->mode != k_ra_dmac_mode_normal) {
    v |= ((uint32_t)cfg->count << k_ra_dmcra_high_pos);
    v &= (k_ra_dmcra_high_mask | k_ra_dmcra_low_mask);
  }
  return v;
}

/* Validate a `cfg` descriptor before touching hardware -- see implementation for details. */
static inline ra_err_t internal_validate_cfg(const ra_dmac_config_t* cfg)
{
  if (cfg->width > k_ra_dmac_width_word) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->mode > k_ra_dmac_mode_repeat_block) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Programme every per-channel register from `cfg`.
 *
 * @details
 * Steps 2-6 of the start sequence; the global activation gate and
 * the channel-enable bit are flipped by the caller after this
 * helper returns so the function-size budget stays under the
 * NASA-Rule-4 / clang-tidy threshold.
 *
 * @param[in,out] cfg See function signature.
 * @param[in,out] reg See function signature.
 * @pre Module has been initialised.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline void internal_program_channel(volatile r_dmac_channel_regs_t* reg,
                                            const ra_dmac_config_t*         cfg)
{
  /* HUM 17.2.14 DMCNT, p 743 -- clear DTE before reprogramming. */
  reg->DMCNT = 0U;
  /* HUM 17.2.10 DMTMD, p 738. */
  reg->DMTMD = internal_dmtmd_value(cfg);
  /* HUM 17.2.12 DMAMD, p 741. */
  reg->DMAMD = internal_dmamd_value(cfg->src_inc, cfg->dst_inc);
  /* HUM 17.2.4 DMSAR p 734, 17.2.6 DMDAR p 735. */
  reg->DMSAR = cfg->src;
  reg->DMDAR = cfg->dst;
  /* HUM 17.2.8 DMCRA. */
  reg->DMCRA = internal_dmcra_value(cfg);
  /* HUM 17.2.9 DMCRB -- only meaningful in non-normal modes. */
  reg->DMCRB = (cfg->mode == k_ra_dmac_mode_normal) ? 0U : (uint32_t)cfg->block_count;
  /* HUM 17.2.13 DMOFR p 743 -- offset-addition not exposed. */
  reg->DMOFR = 0U;
  /* HUM 17.2.11 DMINT p 739. */
  reg->DMINT = internal_dmint_value(cfg);
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

/* Ra dmac start -- see implementation for details. */
ra_err_t ra_dmac_start(uint8_t channel, const ra_dmac_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra_err_t verr = internal_validate_cfg(cfg);
  if (verr != k_ra_ok) {
    return verr;
  }

  volatile r_dmac_channel_regs_t* reg = ra_dmac(channel);
  if (reg == nullptr) {
    return k_ra_err_out_of_range;
  }

  /* DMAC0 + DTC0 share MSTPA22; the ref count tracks how many DMAC
   * channels (or the DTC) currently need the bit cleared.
   * HUM Ch 11.2.6 "MSTPCRA : Module Stop Control Register A", p 443. */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_dmac0_dtc0);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "dmac_start: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  internal_program_channel(reg, cfg);

  /* HUM 17.2.20 DMAST p 749 -- the shared activation gate must be
   * set before any per-channel DTE write takes effect. FSP does the
   * same in `R_DMAC_Open` (`r_dmac.c` line 178). Idempotent: leaving
   * it set across multiple channels is the documented behaviour. */
  ra_dma_shared()->DMAST = k_ra_dmast_dmst_mask;

  /* HUM 17.2.14 DMCNT p 743 -- arm the channel. */
  reg->DMCNT = k_ra_dmcnt_dte_mask;

  ra_log_info_val(s_tag, "dmac_start ch", (uint32_t)channel);
  return k_ra_ok;
}

/* Ra dmac stop -- see implementation for details. */
ra_err_t ra_dmac_stop(uint8_t channel)
{
  volatile r_dmac_channel_regs_t* reg = ra_dmac(channel);
  if (reg == nullptr) {
    return k_ra_err_out_of_range;
  }
  /* HUM 17.2.14 DMCNT p 743. */
  reg->DMCNT = 0U;
  /* Drop the matching MSTP reference acquired in ra_dmac_start. */
  return ra_mstp_disable(k_ra_mstp_dmac0_dtc0);
}

/* =============================================================================
 * Sweep 6 extensions: repeat / block start helpers, address-mode, callbacks
 * =============================================================================
 */

/**
 * @struct ra_dmac_cb_slot_t
 * @brief Per-channel callback slot pair (full-complete + half-complete).
 *
 * @details
 * Holds the function pointer + opaque context registered through
 * ``ra_dmac_attach_callback()`` (DTIE path) and
 * ``ra_dmac_attach_half_complete_handler()`` (RPTIE path).
 */
typedef struct {
  ra_dmac_callback_fn_t full_fn;  /**< DMINT.DTIE  user handler.  */
  void*                 full_ctx; /**< Context for ``full_fn``.   */
  ra_dmac_callback_fn_t half_fn;  /**< DMINT.RPTIE user handler.  */
  void*                 half_ctx; /**< Context for ``half_fn``.   */
} ra_dmac_cb_slot_t;

/** @brief One slot per DMAC0 channel. */
static ra_dmac_cb_slot_t s_dmac_cb_slots[k_ra_dmac_channel_count];

/* Force ``cfg->mode`` then delegate to the validated start path -- see implementation for details. */
static ra_err_t
internal_start_with_mode(uint8_t channel, const ra_dmac_config_t* cfg, ra_dmac_mode_t forced_mode)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  ra_dmac_config_t local = *cfg;
  local.mode             = forced_mode;
  return ra_dmac_start(channel, &local);
}

/* Ra dmac start repeat -- see implementation for details. */
ra_err_t ra_dmac_start_repeat(uint8_t channel, const ra_dmac_config_t* cfg)
{
  return internal_start_with_mode(channel, cfg, k_ra_dmac_mode_repeat);
}

/* Ra dmac start block -- see implementation for details. */
ra_err_t ra_dmac_start_block(uint8_t channel, const ra_dmac_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (cfg->block_count == 0U) {
    return k_ra_err_invalid_arg;
  }
  return internal_start_with_mode(channel, cfg, k_ra_dmac_mode_block);
}

/* Ra dmac set address mode -- see implementation for details. */
ra_err_t ra_dmac_set_address_mode(uint8_t             channel,
                                  ra_dmac_addr_mode_t src_mode,
                                  ra_dmac_addr_mode_t dest_mode)
{
  if ((src_mode > k_ra_dmac_addr_decrement) || (dest_mode > k_ra_dmac_addr_decrement)) {
    return k_ra_err_invalid_arg;
  }
  volatile r_dmac_channel_regs_t* reg = ra_dmac(channel);
  if (reg == nullptr) {
    return k_ra_err_out_of_range;
  }
  /* HUM 17.2.12 DMAMD p 741 -- preserve DARA/SARA/DADR/SADR fields and
   * only rewrite the SM/DM 2-bit slots. */
  uint16_t v = reg->DMAMD;
  v &= (uint16_t)~(k_ra_dmamd_sm_mask | k_ra_dmamd_dm_mask);
  v |= (uint16_t)((uint16_t)src_mode << k_ra_dmamd_sm_pos);
  v |= (uint16_t)((uint16_t)dest_mode << k_ra_dmamd_dm_pos);
  reg->DMAMD = v;
  return k_ra_ok;
}

/* Ra dmac attach half complete handler -- see implementation for details. */
ra_err_t ra_dmac_attach_half_complete_handler(uint8_t channel, ra_dmac_callback_fn_t fn, void* ctx)
{
  if (channel >= k_ra_dmac_channel_count) {
    return k_ra_err_out_of_range;
  }
  s_dmac_cb_slots[channel].half_fn  = fn;
  s_dmac_cb_slots[channel].half_ctx = ctx;
  return k_ra_ok;
}

/* Ra dmac attach callback -- see implementation for details. */
ra_err_t ra_dmac_attach_callback(uint8_t channel, ra_dmac_callback_fn_t fn, void* ctx)
{
  if (channel >= k_ra_dmac_channel_count) {
    return k_ra_err_out_of_range;
  }
  s_dmac_cb_slots[channel].full_fn  = fn;
  s_dmac_cb_slots[channel].full_ctx = ctx;
  return k_ra_ok;
}

/* Ra dmac dispatch -- see implementation for details. */
void ra_dmac_dispatch(uint8_t channel)
{
  if (channel >= k_ra_dmac_channel_count) {
    return;
  }
  const ra_dmac_callback_fn_t fn  = s_dmac_cb_slots[channel].full_fn;
  void* const                 ctx = s_dmac_cb_slots[channel].full_ctx;
  if (fn != nullptr) {
    fn(ctx);
  }
}

/* Ra dmac dispatch half -- see implementation for details. */
void ra_dmac_dispatch_half(uint8_t channel)
{
  if (channel >= k_ra_dmac_channel_count) {
    return;
  }
  const ra_dmac_callback_fn_t fn  = s_dmac_cb_slots[channel].half_fn;
  void* const                 ctx = s_dmac_cb_slots[channel].half_ctx;
  if (fn != nullptr) {
    fn(ctx);
  }
}
