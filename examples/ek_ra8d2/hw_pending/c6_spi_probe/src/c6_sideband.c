/**
 * @file examples/ek_ra8d2/hw_pending/c6_spi_probe/src/c6_sideband.c
 * @brief Pmod1 side-band sampling, muxed-net wire test and chip-select hunt
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Everything the probe knows about the *physical* side of the Pmod1 link
 * lives here. Two facts about the C6 image in ``coprocessor/esp32c6/`` make
 * these diagnostics possible without any working data path:
 *
 *   - It is built with ``CONFIG_ESP_SPI_DEASSERT_HS_ON_CS=y``, so the C6
 *     drops HANDSHAKE from its chip-select edge interrupt
 *     (``gpio_disable_hs_isr_handler`` in esp-hosted-mcu
 *     the C6's peripheral-side SPI driver) and re-raises it when the next
 *     transaction is queued (``spi_post_setup_cb``). Merely *asserting* a
 *     candidate chip-select therefore makes a live C6 answer on a
 *     side-band pin -- no clock, no payload required.
 *   - HANDSHAKE and DATA_READY are push-pull outputs, so the RA8 must
 *     never drive the side-band pins; they are read-only here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_probe.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"

/**
 * @var k_c6_sideband_pin
 * @brief The four Pmod1 side-band pins, in ::c6_sideband_idx_t order.
 * @details Sourced from the board layer so no EK-RA8D2 pin fact is
 * re-encoded here.
 * @note Read-only; consumed by ::c6_probe_sample_sideband.
 * @warning Never drive these pins: the C6 drives two of them push-pull.
 * @since 0.1.0
 */
static const ra8_port_pin_t k_c6_sideband_pin[k_c6_sb_count] = {
  (ra8_port_pin_t)k_ra8_board_pmod1_irq,
  (ra8_port_pin_t)k_ra8_board_pmod1_reset,
  (ra8_port_pin_t)k_ra8_board_pmod1_gpio_a,
  (ra8_port_pin_t)k_ra8_board_pmod1_gpio_b,
};

/**
 * @var k_c6_sideband_label
 * @brief Console labels for ::k_c6_sideband_pin, same order.
 * @details A fifth entry backs the "no pin resolved" case so callers never
 * index past the table.
 * @note Read-only; used only for log formatting.
 * @warning Index with ::c6_probe_sideband_name, not directly.
 * @since 0.1.0
 */
static const char* const k_c6_sideband_label[k_c6_sb_count + 1U] = {
  "P006",
  "P402",
  "P412",
  "P413",
  "unresolved",
};

/**
 * @var k_c6_wire_pin
 * @brief The five Pmod1 muxed-net candidates, in ::c6_wire_idx_t order.
 * @details Board pins only; nothing here re-encodes an EK-RA8D2 pin fact.
 * @note Read-only; consumed by ::c6_probe_wire_test and ::c6_probe_cs_hunt.
 * @warning Every entry must be an input on the C6 side, so the probe's
 *          brief drive can never contend with the peripheral.
 * @since 0.1.0
 */
static const ra8_port_pin_t k_c6_wire_pin[k_c6_wire_count] = {
  (ra8_port_pin_t)k_ra8_board_pmod1_uart_cts,
  (ra8_port_pin_t)k_ra8_board_pmod1_spi_copi,
  (ra8_port_pin_t)k_ra8_board_pmod1_spi_cipo,
  (ra8_port_pin_t)k_ra8_board_pmod1_spi_sck,
  (ra8_port_pin_t)k_ra8_board_pmod1_spi_cs,
};

/**
 * @var k_c6_wire_label
 * @brief Console labels for ::k_c6_wire_pin, same order.
 * @details A sixth entry backs the "no candidate reached the C6" case.
 * @note Read-only; used only for log formatting.
 * @warning Bounds-check before indexing; the table is one longer than
 *          ::k_c6_wire_count on purpose.
 * @since 0.1.0
 */
static const char* const k_c6_wire_label[k_c6_wire_count + 1U] = {
  "P800(J26-1 in UART mux)",
  "P801(J26-2)",
  "P802(J26-3)",
  "P803(J26-4 in SPI mux)",
  "P804(J26-1 in SPI mux)",
  "none",
};

ra8_err_t c6_probe_sideband_init(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_c6_sb_count; i++) {
    const ra8_err_t err = ra8_gpio_input_init(k_c6_sideband_pin[i], k_ra8_pull_none);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

void c6_probe_sample_sideband(c6_sideband_sample_t* out)
{
  if (out == nullptr) {
    return;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_c6_sb_count; i++) {
    ra8_level_t level = k_ra8_level_low;
    if (ra8_gpio_read(k_c6_sideband_pin[i], &level) != k_ra8_ok) {
      level = k_ra8_level_low;
    }
    out->level[i] = (level == k_ra8_level_high) ? 1U : 0U;
  }
}

const char* c6_probe_sideband_name(uint8_t idx)
{
  const uint8_t safe = (idx < (uint8_t)k_c6_sb_count) ? idx : (uint8_t)k_c6_sb_count;
  return k_c6_sideband_label[safe];
}

void c6_probe_print_sideband(const char* label, const c6_sideband_sample_t* s)
{
  if (s == nullptr) {
    return;
  }
  c6_probe_puts(label);
  for (uint8_t i = 0U; i < (uint8_t)k_c6_sb_count; i++) {
    c6_probe_puts(" ");
    c6_probe_puts(c6_probe_sideband_name(i));
    c6_probe_puts("=");
    c6_probe_put_u32((uint32_t)s->level[i]);
  }
  c6_probe_puts("\r\n");
}

void c6_probe_vote(const c6_sideband_sample_t* pre,
                   const c6_sideband_sample_t* mid,
                   const c6_sideband_sample_t* post,
                   c6_probe_stats_t*           st)
{
  if (pre == nullptr || mid == nullptr || post == nullptr || st == nullptr) {
    return;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_c6_sb_count; i++) {
    if (pre->level[i] != 0U && mid->level[i] == 0U) {
      st->hs_vote[i]++;
    }
    if (pre->level[i] != 0U && post->level[i] == 0U) {
      st->dr_vote[i]++;
    }
    if (pre->level[i] != 0U || mid->level[i] != 0U || post->level[i] != 0U) {
      st->ever_high[i] = 1U;
    }
    if (pre->level[i] == 0U || mid->level[i] == 0U || post->level[i] == 0U) {
      st->ever_low[i] = 1U;
    }
  }
}

uint8_t c6_probe_best(const uint32_t* votes, uint8_t ignore)
{
  if (votes == nullptr) {
    return (uint8_t)k_c6_sb_count;
  }
  uint8_t  best       = (uint8_t)k_c6_sb_count;
  uint32_t best_score = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_c6_sb_count; i++) {
    if (i == ignore) {
      continue;
    }
    if (votes[i] > best_score) {
      best_score = votes[i];
      best       = i;
    }
  }
  return best;
}

/**
 * @brief Drive one net to a level, release it, and sample what it settles to.
 *
 * @details
 * Claims the pin as a push-pull output at @p level, holds it long enough
 * for the net to charge, then releases the claim and re-claims it as a
 * no-pull input before sampling. A net with no termination keeps the driven
 * level on its own capacitance for far longer than the settle delay; a
 * terminated net snaps back within microseconds.
 *
 * @param[in]  pin       Board pin to exercise.
 * @param[in]  level     Level to drive before releasing.
 * @param[out] out_level Level read back after release; ignored when NULL.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok The net was driven, released and sampled.
 * @retval k_ra8_err_null_ptr ``out_level`` was NULL.
 * @retval k_ra8_err_gpio_conflict The pin is owned by another driver.
 *
 * @pre The pin is not currently routed to a peripheral.
 * @pre ``out_level`` is non-NULL.
 * @post The pin is released (unclaimed) on every success path.
 * @post ``*out_level`` is 0 or 1 on success.
 *
 * @note Not thread-safe; boot-time diagnostic only.
 * @since 0.1.0
 */
static ra8_err_t internal_kick_net(ra8_port_pin_t pin, ra8_level_t level, uint8_t* out_level)
{
  if (out_level == nullptr) {
    return k_ra8_err_null_ptr;
  }
  ra8_err_t err = ra8_gpio_output_init(pin, level);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_c6_probe_cs_hold_ms);
  err = ra8_gpio_release(pin);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gpio_input_init(pin, k_ra8_pull_none);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_c6_probe_cs_hold_ms);
  ra8_level_t sampled = k_ra8_level_low;
  err                 = ra8_gpio_read(pin, &sampled);
  if (err != k_ra8_ok) {
    (void)ra8_gpio_release(pin);
    return err;
  }
  *out_level = (sampled == k_ra8_level_high) ? 1U : 0U;
  return ra8_gpio_release(pin);
}

c6_wire_kind_t c6_probe_wire_kind(uint8_t after_high, uint8_t after_low)
{
  if (after_high != 0U && after_low == 0U) {
    return k_c6_wire_floating;
  }
  if (after_high == 0U && after_low == 0U) {
    return k_c6_wire_low_side;
  }
  if (after_high != 0U && after_low != 0U) {
    return k_c6_wire_high_side;
  }
  return k_c6_wire_odd;
}

void c6_probe_wire_test(void)
{
  static const char* const k_kind_name[] = {
    "floating(no termination)",
    "low-side(pull-down or driven low)",
    "high-side(pull-up or driven high)",
    "inconsistent",
  };
  for (uint8_t i = 0U; i < (uint8_t)k_c6_wire_count; i++) {
    uint8_t         after_high = 0U;
    uint8_t         after_low  = 0U;
    const ra8_err_t e_high     = internal_kick_net(k_c6_wire_pin[i], k_ra8_level_high, &after_high);
    const ra8_err_t e_low      = internal_kick_net(k_c6_wire_pin[i], k_ra8_level_low, &after_low);
    c6_probe_puts("c6_probe: wire ");
    c6_probe_puts(k_c6_wire_label[i]);
    if (e_high != k_ra8_ok || e_low != k_ra8_ok) {
      c6_probe_puts(" test failed\r\n");
      continue;
    }
    c6_probe_puts(" hi->");
    c6_probe_put_u32((uint32_t)after_high);
    c6_probe_puts(" lo->");
    c6_probe_put_u32((uint32_t)after_low);
    c6_probe_puts(" ");
    const c6_wire_kind_t kind = c6_probe_wire_kind(after_high, after_low);
    c6_probe_puts(k_kind_name[(uint8_t)kind]);
    c6_probe_puts("\r\n");
  }
}

/**
 * @brief Assert one candidate chip-select and report any side-band response.
 *
 * @details
 * Drives the candidate high, samples, drives it low for a settle period,
 * samples again, restores it high and releases it. A live C6 on the other
 * end answers the falling edge by dropping HANDSHAKE, so a pin that went
 * from high to low between the two samples both proves the candidate is
 * the chip-select and names HANDSHAKE.
 *
 * @param[in]     idx One-based index into ::k_c6_wire_pin.
 * @param[in,out] st  Statistics block; handshake votes accumulate here.
 *
 * @return ``true`` when at least one side-band pin dropped.
 * @retval true  The C6 answered this candidate's chip-select edge.
 * @retval false Nothing on the far end reacted, or the drive failed.
 *
 * @pre The candidate pin is unclaimed and is an input on the C6.
 * @pre ``st`` is non-NULL.
 * @post The candidate pin is released before returning.
 * @post One console line was printed for this candidate.
 *
 * @note Not thread-safe; boot-time diagnostic only.
 * @since 0.1.0
 */
static bool internal_cs_try(uint8_t idx, c6_probe_stats_t* st)
{
  const ra8_port_pin_t pin = k_c6_wire_pin[idx];
  if (ra8_gpio_output_init(pin, k_ra8_level_high) != k_ra8_ok) {
    return false;
  }
  ra8_delay_ms((uint32_t)k_c6_probe_settle_ms);
  c6_sideband_sample_t idle = {};
  c6_probe_sample_sideband(&idle);

  (void)ra8_gpio_write(pin, k_ra8_level_low);
  ra8_delay_ms((uint32_t)k_c6_probe_cs_hold_ms);
  c6_sideband_sample_t asserted = {};
  c6_probe_sample_sideband(&asserted);

  (void)ra8_gpio_write(pin, k_ra8_level_high);
  ra8_delay_ms((uint32_t)k_c6_probe_settle_ms);
  (void)ra8_gpio_release(pin);

  bool answered = false;
  for (uint8_t i = 0U; i < (uint8_t)k_c6_sb_count; i++) {
    if (idle.level[i] != 0U && asserted.level[i] == 0U) {
      st->hs_vote[i]++;
      answered = true;
    }
  }
  c6_probe_puts("c6_probe: cs-hunt ");
  c6_probe_puts(k_c6_wire_label[idx]);
  c6_probe_print_sideband(answered ? " ANSWERED, asserted" : " no response, asserted", &asserted);
  return answered;
}

uint8_t c6_probe_cs_hunt(c6_probe_stats_t* st)
{
  if (st == nullptr) {
    return (uint8_t)k_c6_wire_count;
  }
  uint8_t found = (uint8_t)k_c6_wire_count;
  for (uint8_t i = 0U; i < (uint8_t)k_c6_wire_count; i++) {
    if (internal_cs_try(i, st) && found == (uint8_t)k_c6_wire_count) {
      found = i;
    }
  }
  c6_probe_puts("c6_probe: cs-hunt result ");
  c6_probe_puts(k_c6_wire_label[found]);
  c6_probe_puts("\r\n");
  return found;
}
