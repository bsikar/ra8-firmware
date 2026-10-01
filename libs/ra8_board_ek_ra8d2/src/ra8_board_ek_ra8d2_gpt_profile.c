/**
 * @file ra8_board_ek_ra8d2_gpt_profile.c
 * @brief Board numbering over the RA8 GPT timer and PWM adapters.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Each op translates the board index and delegates to the chip adapter with
 * a NULL context. The caps come from the adapter with only the channel count
 * replaced by the board's, so the counter width and limits stay the chip's.
 * The facades range-check the board index against that count before any op
 * here runs; the lookups still refuse an out-of-range index on their own,
 * because they are public.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_board_ek_ra8d2_gpt_profile.h"

#include <stdint.h>

#include "fw_if_pwm.h"
#include "fw_if_pwm_ra8.h"
#include "fw_if_timer.h"
#include "fw_if_timer_ra8.h"
#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_pin_validator.h"
#include "ra8_port_utils.h"

/** @brief One wired PWM output. */
typedef struct {
  uint8_t        chip;  /**< GPT channel; the output is its pin A. */
  ra8_port_pin_t pin;   /**< Package pin carrying GTIOCnA.         */
  const char*    owner; /**< Pin-validator owner name.             */
} internal_pwm_row_t;

/** @brief The GPT channels this profile hands out, named so the tables read. */
typedef enum : uint8_t {
  k_internal_gpt0 = 0U, /**< Timer 0.               */
  k_internal_gpt1 = 1U, /**< PWM 0, GTIOC1A on D6.  */
  k_internal_gpt2 = 2U, /**< PWM 1, GTIOC2A on D10. */
  k_internal_gpt3 = 3U, /**< Timer 1.               */
  k_internal_gpt4 = 4U, /**< Timer 2.               */
  k_internal_gpt5 = 5U, /**< Timer 3.               */
  k_internal_gpt6 = 6U, /**< Timer 4.               */
  k_internal_gpt7 = 7U, /**< Timer 5.               */
  k_internal_gpt8 = 8U, /**< PWM 2, GTIOC8A on D11. */
  k_internal_gpt9 = 9U, /**< Timer 6.               */
} internal_gpt_ch_t;

/** @brief Board PWM index -> chip channel and pin. UM Table 20, p 28. */
static const internal_pwm_row_t k_internal_pwm[k_ra8_board_pwm_count] = {
  {.chip = k_internal_gpt1, .pin = (ra8_port_pin_t)k_ra8_board_arduino_d6, .owner = "board.pwm.d6"},
  {.chip  = k_internal_gpt2,
   .pin   = (ra8_port_pin_t)k_ra8_board_arduino_d10,
   .owner = "board.pwm.d10"},
  {.chip  = k_internal_gpt8,
   .pin   = (ra8_port_pin_t)k_ra8_board_arduino_d11,
   .owner = "board.pwm.d11"},
};

/** @brief Board timer index -> chip channel: what the PWM rows leave free. */
static const uint8_t k_internal_timer_chip[k_ra8_board_timer_count] = {
  k_internal_gpt0,
  k_internal_gpt3,
  k_internal_gpt4,
  k_internal_gpt5,
  k_internal_gpt6,
  k_internal_gpt7,
  k_internal_gpt9,
};

ra8_err_t ra8_board_timer_to_chip(uint8_t index, uint8_t* out_chip)
{
  if (out_chip == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (index >= k_ra8_board_timer_count) {
    return k_ra8_err_not_found;
  }
  *out_chip = k_internal_timer_chip[index];
  return k_ra8_ok;
}

ra8_err_t ra8_board_pwm_to_chip(uint8_t index, uint8_t* out_chip)
{
  if (out_chip == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (index >= k_ra8_board_pwm_count) {
    return k_ra8_err_not_found;
  }
  *out_chip = k_internal_pwm[index].chip;
  return k_ra8_ok;
}

/* ---------------------------------------------------------------- timer -- */

/**
 * @brief Translate a board timer to the chip channel the adapter wants.
 * @param[in] ch Board timer; already range-checked by the facade.
 * @return Chip channel.
 */
static fw_timer_ch_t internal_timer_chip(fw_timer_ch_t ch)
{
  uint8_t chip = 0U;
  (void)ra8_board_timer_to_chip(ch.index, &chip);
  return (fw_timer_ch_t){.index = chip};
}

/**
 * @brief Adapter caps with the board's timer count.
 * @param[in]  ctx Unused.
 * @param[out] out Caps.
 * @return Adapter status.
 */
static ra8_err_t internal_timer_caps(void* ctx, fw_timer_caps_t* out)
{
  (void)ctx;
  const ra8_err_t err = fw_timer_ra8_iface()->get_caps(nullptr, out);
  if (err == k_ra8_ok) {
    out->channel_count = k_ra8_board_timer_count;
  }
  return err;
}

/** @brief Delegate open. @return Adapter status. */
static ra8_err_t
internal_timer_open(void* ctx, fw_timer_ch_t ch, fw_timer_mode_t mode, uint32_t period)
{
  (void)ctx;
  return fw_timer_ra8_iface()->open(nullptr, internal_timer_chip(ch), mode, period);
}

/** @brief Delegate close. @return Adapter status. */
static ra8_err_t internal_timer_close(void* ctx, fw_timer_ch_t ch)
{
  (void)ctx;
  return fw_timer_ra8_iface()->close(nullptr, internal_timer_chip(ch));
}

/** @brief Delegate start. @return Adapter status. */
static ra8_err_t internal_timer_start(void* ctx, fw_timer_ch_t ch)
{
  (void)ctx;
  return fw_timer_ra8_iface()->start(nullptr, internal_timer_chip(ch));
}

/** @brief Delegate stop. @return Adapter status. */
static ra8_err_t internal_timer_stop(void* ctx, fw_timer_ch_t ch)
{
  (void)ctx;
  return fw_timer_ra8_iface()->stop(nullptr, internal_timer_chip(ch));
}

/** @brief Delegate read. @return Adapter status. */
static ra8_err_t internal_timer_read(void* ctx, fw_timer_ch_t ch, uint32_t* out_counts)
{
  (void)ctx;
  return fw_timer_ra8_iface()->read(nullptr, internal_timer_chip(ch), out_counts);
}

/** @brief Delegate set_period. @return Adapter status. */
static ra8_err_t internal_timer_set_period(void* ctx, fw_timer_ch_t ch, uint32_t period)
{
  (void)ctx;
  return fw_timer_ra8_iface()->set_period(nullptr, internal_timer_chip(ch), period);
}

/** @brief Delegate capture_read. @return Adapter status. */
static ra8_err_t internal_timer_capture(void* ctx, fw_timer_ch_t ch, uint32_t* out_counts)
{
  (void)ctx;
  return fw_timer_ra8_iface()->capture_read(nullptr, internal_timer_chip(ch), out_counts);
}

/** @brief Delegate take_wrap. @return Adapter status. */
static ra8_err_t internal_timer_take_wrap(void* ctx, fw_timer_ch_t ch, bool* out_wrapped)
{
  (void)ctx;
  return fw_timer_ra8_iface()->take_wrap(nullptr, internal_timer_chip(ch), out_wrapped);
}

static const fw_timer_iface_t k_internal_timer_iface = {
  .get_caps     = internal_timer_caps,
  .open         = internal_timer_open,
  .close        = internal_timer_close,
  .start        = internal_timer_start,
  .stop         = internal_timer_stop,
  .read         = internal_timer_read,
  .set_period   = internal_timer_set_period,
  .capture_read = internal_timer_capture,
  .take_wrap    = internal_timer_take_wrap,
};

/* ------------------------------------------------------------------ pwm -- */

/**
 * @brief Translate a board PWM output to the chip channel.
 * @param[in] ch Board output; already range-checked by the facade.
 * @return Chip channel.
 */
static fw_pwm_ch_t internal_pwm_chip(fw_pwm_ch_t ch)
{
  return (fw_pwm_ch_t){.index = k_internal_pwm[ch.index].chip};
}

/**
 * @brief Adapter caps with the board's output count.
 * @param[in]  ctx Unused.
 * @param[out] out Caps.
 * @return Adapter status.
 */
static ra8_err_t internal_pwm_caps(void* ctx, fw_pwm_caps_t* out)
{
  (void)ctx;
  const ra8_err_t err = fw_pwm_ra8_iface()->get_caps(nullptr, out);
  if (err == k_ra8_ok) {
    out->channel_count = k_ra8_board_pwm_count;
  }
  return err;
}

/**
 * @brief Route and claim the pin, then open the chip output.
 * @details The pin goes first so a pin held elsewhere fails before the GPT
 *          channel is touched; an adapter failure gives the pin back.
 * @return Pin-routing status, else adapter status.
 */
static ra8_err_t
internal_pwm_open(void* ctx, fw_pwm_ch_t ch, uint32_t period, fw_pwm_polarity_t pol)
{
  (void)ctx;
  const internal_pwm_row_t* row = &k_internal_pwm[ch.index];
  const ra8_err_t routed        = ra8_pfs_route_peripheral(row->pin, k_ra8_psel_gpt0, row->owner);
  if (routed != k_ra8_ok) {
    return routed;
  }
  const ra8_err_t err = fw_pwm_ra8_iface()->open(nullptr, internal_pwm_chip(ch), period, pol);
  if (err != k_ra8_ok) {
    (void)ra8_pin_validator_release(row->pin);
  }
  return err;
}

/**
 * @brief Close the chip output, then release the pin claim.
 * @return Adapter status; the pin is released only when the close worked.
 */
static ra8_err_t internal_pwm_close(void* ctx, fw_pwm_ch_t ch)
{
  (void)ctx;
  const ra8_err_t err = fw_pwm_ra8_iface()->close(nullptr, internal_pwm_chip(ch));
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pin_validator_release(k_internal_pwm[ch.index].pin);
}

/** @brief Delegate start. @return Adapter status. */
static ra8_err_t internal_pwm_start(void* ctx, fw_pwm_ch_t ch)
{
  (void)ctx;
  return fw_pwm_ra8_iface()->start(nullptr, internal_pwm_chip(ch));
}

/** @brief Delegate stop. @return Adapter status. */
static ra8_err_t internal_pwm_stop(void* ctx, fw_pwm_ch_t ch)
{
  (void)ctx;
  return fw_pwm_ra8_iface()->stop(nullptr, internal_pwm_chip(ch));
}

/** @brief Delegate set_period. @return Adapter status. */
static ra8_err_t internal_pwm_set_period(void* ctx, fw_pwm_ch_t ch, uint32_t period)
{
  (void)ctx;
  return fw_pwm_ra8_iface()->set_period(nullptr, internal_pwm_chip(ch), period);
}

/** @brief Delegate set_duty. @return Adapter status. */
static ra8_err_t internal_pwm_set_duty(void* ctx, fw_pwm_ch_t ch, fw_pwm_duty_t duty)
{
  (void)ctx;
  return fw_pwm_ra8_iface()->set_duty(nullptr, internal_pwm_chip(ch), duty);
}

static const fw_pwm_iface_t k_internal_pwm_iface = {
  .get_caps   = internal_pwm_caps,
  .open       = internal_pwm_open,
  .close      = internal_pwm_close,
  .start      = internal_pwm_start,
  .stop       = internal_pwm_stop,
  .set_period = internal_pwm_set_period,
  .set_duty   = internal_pwm_set_duty,
};

/* ------------------------------------------------------------- handles -- */

ra8_err_t ra8_board_timer_profile_bind(fw_timer_t* tmr)
{
  return fw_timer_bind(tmr, &k_internal_timer_iface, nullptr);
}

ra8_err_t ra8_board_pwm_profile_bind(fw_pwm_t* pwm)
{
  return fw_pwm_bind(pwm, &k_internal_pwm_iface, nullptr);
}

/** @brief The board's one timer handle. */
static fw_timer_t internal_board_timer = {};

/** @brief The board's one PWM handle. */
static fw_pwm_t internal_board_pwm = {};

const fw_timer_t* ra8_board_timer(void)
{
  if (!internal_board_timer.bound) {
    /* Cannot fail: a file-static handle and a fully populated ops struct
     * whose caps are the adapter's constants. See ra8_board_clock(). */
    (void)ra8_board_timer_profile_bind(&internal_board_timer);
  }
  return &internal_board_timer;
}

const fw_pwm_t* ra8_board_pwm(void)
{
  if (!internal_board_pwm.bound) {
    /* Cannot fail, for the same reason as ra8_board_timer(). */
    (void)ra8_board_pwm_profile_bind(&internal_board_pwm);
  }
  return &internal_board_pwm;
}
