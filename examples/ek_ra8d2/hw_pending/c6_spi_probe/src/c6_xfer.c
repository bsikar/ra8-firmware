/**
 * @file examples/ek_ra8d2/hw_pending/c6_spi_probe/src/c6_xfer.c
 * @brief One full esp-hosted SPI full-duplex transaction against the C6
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Owns the two 1600-byte transport buffers and the SCI2 Simple-SPI channel
 * that clocks them. The transfer is split after the twelve-byte header so a
 * side-band sample lands while the chip-select is still asserted -- that is
 * the window in which the C6 holds HANDSHAKE low, and it is what identifies
 * the pin. Both halves go out inside one chip-select assertion, which is
 * what the esp-hosted peripheral expects.
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
#include "ra8_sci_spi.h"
#include "ra8_spi.h"
#include "ra8_time.h"

/**
 * @var s_c6_tx
 * @brief Transmit buffer: one full esp-hosted SPI transaction.
 * @details Statically allocated (NASA Power of 10 Rule 3 -- no dynamic
 * memory) and re-stamped before every transfer.
 * @note Written only by ::internal_fill_idle_tx.
 * @warning Never write past ::k_c6_proto_buf_size.
 * @since 0.1.0
 */
static uint8_t s_c6_tx[k_c6_proto_buf_size];

/**
 * @var s_c6_rx
 * @brief Receive buffer: one full esp-hosted SPI transaction.
 * @details Statically allocated for the same reason as ::s_c6_tx.
 * @note Written only by the SCI transfer.
 * @warning Contents are meaningful only after a completed transfer.
 * @since 0.1.0
 */
static uint8_t s_c6_rx[k_c6_proto_buf_size];

/**
 * @brief Stamp the transmit buffer as an esp-hosted idle host frame.
 *
 * @details
 * The reference host does exactly this when it has nothing to send: zero
 * the whole 1600-byte buffer and set ``if_type = ESP_MAX_IF``, leaving
 * length, offset and checksum at zero (the dummy-buffer branch of
 * ``spi_transaction()`` in esp-hosted-mcu
 * ``host/drivers/transport/spi/spi_drv.c``). The C6 recognises the marker
 * and drops the frame without complaint.
 *
 * @pre ::s_c6_tx is at least ::k_c6_proto_buf_size bytes.
 * @pre No transfer is in flight.
 * @post Every byte of ::s_c6_tx outside byte zero is zero.
 * @post Byte zero carries ::k_c6_if_max in its low nibble.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_fill_idle_tx(void)
{
  for (uint16_t i = 0U; i < (uint16_t)k_c6_proto_buf_size; i++) {
    s_c6_tx[i] = 0U;
    s_c6_rx[i] = 0U;
  }
  s_c6_tx[k_c6_hdr_off_iface] = (uint8_t)k_c6_if_max;
}

/**
 * @brief Drive the chip-select and let the line settle.
 *
 * @param[in] level Level to drive: low asserts the C6's chip-select.
 *
 * @pre The chip-select was claimed by ::c6_probe_spi_pins_init.
 * @pre ``level`` is a valid ::ra8_level_t.
 * @post The chip-select holds ``level``.
 * @post At least ::k_c6_probe_cs_hold_ms of setup or hold time elapsed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void internal_cs(ra8_level_t level)
{
  (void)ra8_gpio_write((ra8_port_pin_t)k_ra8_board_pmod1_spi_cs, level);
  ra8_delay_ms((uint32_t)k_c6_probe_cs_hold_ms);
}

/**
 * @brief Decide whether one side-band sample says the C6 is ready.
 *
 * @details
 * Once HANDSHAKE has been identified the decision is that one pin. Before
 * then no single pin can be gated on, so the weaker "some pin is asserted"
 * rule stands in: the C6 raises HANDSHAKE whenever it is armed, so an
 * all-low side-band means the C6 is not ready (or nothing is wired) and
 * clocking would be pointless.
 *
 * @param[in] s      Side-band sample to judge.
 * @param[in] hs_idx Identified handshake index, or ::k_c6_sb_count when the
 *                   map is not yet resolved.
 *
 * @return ``true`` when the sample satisfies the ready condition.
 * @retval true  The gating pin (or, pre-map, any pin) read high.
 * @retval false Nothing in the sample indicates readiness, or ``s`` was NULL.
 *
 * @pre ``s`` was filled by ::c6_probe_sample_sideband.
 * @pre ``hs_idx`` is a valid index or ::k_c6_sb_count.
 * @post Neither argument is modified.
 * @post Exactly one verdict is returned.
 *
 * @note Pure function; safe from any context.
 * @since 0.1.0
 */
static bool internal_sample_ready(const c6_sideband_sample_t* s, uint8_t hs_idx)
{
  if (s == nullptr) {
    return false;
  }
  if (hs_idx < (uint8_t)k_c6_sb_count) {
    return s->level[hs_idx] != 0U;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_c6_sb_count; i++) {
    if (s->level[i] != 0U) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Wait, with a bounded poll, for the peripheral-ready condition.
 *
 * @param[in] hs_idx Identified handshake index, or ::k_c6_sb_count when the
 *                   map is not yet resolved.
 *
 * @return ``true`` when the ready condition was observed in budget.
 * @retval true  ::internal_sample_ready accepted a sample.
 * @retval false The poll budget expired with no pin ready.
 *
 * @pre The side-band pins were configured as inputs.
 * @pre ``hs_idx`` is a valid index or ::k_c6_sb_count.
 * @post At most ::k_c6_probe_hs_poll_max samples were taken.
 * @post No pin state was modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool internal_wait_ready(uint8_t hs_idx)
{
  for (uint8_t poll = 0U; poll < (uint8_t)k_c6_probe_hs_poll_max; poll++) {
    c6_sideband_sample_t s = {};
    c6_probe_sample_sideband(&s);
    if (internal_sample_ready(&s, hs_idx)) {
      return true;
    }
    ra8_delay_ms((uint32_t)k_c6_probe_hs_poll_ms);
  }
  return false;
}

/**
 * @brief Clock one full esp-hosted transaction, sampling the side-band pins.
 *
 * @param[out] pre  Sample taken before the chip-select is asserted.
 * @param[out] mid  Sample taken mid-transfer, chip-select asserted.
 * @param[out] post Sample taken after the transfer and a settle delay.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Both halves of the frame were clocked.
 * @retval k_ra8_err_hw_timeout The SCI transmit or receive poll expired.
 * @retval k_ra8_err_null_ptr A sample pointer was NULL.
 *
 * @pre ``ra8_sci_spi_init`` succeeded for the Pmod1 channel.
 * @pre All three sample pointers are non-NULL.
 * @post On success ::s_c6_rx holds a complete received frame.
 * @post The chip-select is released whether or not the transfer succeeded.
 *
 * @note Not thread-safe; blocks for the whole frame.
 * @since 0.1.0
 */
static ra8_err_t
internal_transfer(c6_sideband_sample_t* pre, c6_sideband_sample_t* mid, c6_sideband_sample_t* post)
{
  if (pre == nullptr || mid == nullptr || post == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const uint8_t channel = (uint8_t)k_ra8_board_pmod1_sci_channel;
  internal_fill_idle_tx();

  c6_probe_sample_sideband(pre);
  internal_cs(k_ra8_level_low);
  ra8_err_t err = ra8_sci_spi_xfer(channel, s_c6_tx, s_c6_rx, (uint32_t)k_c6_proto_hdr_size);
  c6_probe_sample_sideband(mid);
  if (err == k_ra8_ok) {
    err =
      ra8_sci_spi_xfer(channel,
                       &s_c6_tx[k_c6_proto_hdr_size],
                       &s_c6_rx[k_c6_proto_hdr_size],
                       (uint32_t)((uint16_t)k_c6_proto_buf_size - (uint16_t)k_c6_proto_hdr_size));
  }
  internal_cs(k_ra8_level_high);
  ra8_delay_ms((uint32_t)k_c6_probe_settle_ms);
  c6_probe_sample_sideband(post);
  return err;
}

/**
 * @brief Decode, classify, tally and narrate the frame in the receive buffer.
 *
 * @details
 * Split out of ::internal_one_xfer so each stays inside the project's
 * function-length budget: this half owns everything that happens *after* the
 * bytes have been clocked, which is exactly the part a reader wants to follow
 * without the pin-sampling around it.
 *
 * @param[in,out] st Statistics block whose per-kind counters are incremented.
 *
 * @return The classification of the received frame.
 * @retval k_c6_frame_garbage  No esp-hosted structure, or ``st`` was NULL.
 * @retval k_c6_frame_idle     The C6 answered with its idle filler frame.
 * @retval k_c6_frame_data     A real frame arrived and its checksum verified.
 * @retval k_c6_frame_bad_csum A real-looking frame failed its checksum.
 *
 * @pre ::s_c6_rx holds a completed transaction.
 * @pre ``st`` is non-NULL and the console is up.
 * @post Exactly one counter in ``st`` grew, unless the frame was garbage.
 * @post One header line, one verdict line and (for a payload-bearing frame)
 *       one hex-dump line were printed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static c6_frame_kind_t internal_report_frame(c6_probe_stats_t* st)
{
  if (st == nullptr) {
    return k_c6_frame_garbage;
  }
  c6_hdr_t hdr = {};
  c6_probe_decode_header(s_c6_rx, &hdr);
  c6_probe_print_header(&hdr);
  const c6_frame_kind_t kind = c6_probe_classify(&hdr);
  if (kind == k_c6_frame_idle) {
    st->idle_frames++;
    c6_probe_puts("    frame: idle filler from the C6\r\n");
  } else if (kind == k_c6_frame_data) {
    st->data_frames++;
    c6_probe_puts("    frame: data, checksum ok\r\n");
    c6_probe_dump_payload(s_c6_rx, hdr.len);
  } else if (kind == k_c6_frame_bad_csum) {
    st->bad_csum_frames++;
    c6_probe_puts("    frame: header sane, checksum mismatch\r\n");
    c6_probe_dump_payload(s_c6_rx, hdr.len);
  } else {
    c6_probe_puts("    frame: no esp-hosted structure\r\n");
  }
  return kind;
}

/**
 * @brief Clock one transaction and report everything it revealed.
 *
 * @param[in]     index  One-based transaction number, for the log.
 * @param[in]     hs_idx Identified handshake index, or ::k_c6_sb_count.
 * @param[in,out] st     Statistics block to accumulate into.
 *
 * @return The classification of the received frame.
 * @retval k_c6_frame_garbage The transfer failed, was skipped, or the
 *                            received bytes carry no esp-hosted structure.
 * @retval k_c6_frame_idle    The C6 answered with its idle filler frame.
 * @retval k_c6_frame_data    A real frame arrived and its checksum verified.
 * @retval k_c6_frame_bad_csum A real-looking frame failed its checksum.
 *
 * @pre The SPI channel is initialised and the console is up.
 * @pre ``st`` is non-NULL.
 * @post ``st->attempts`` grew by one whenever a transfer was clocked.
 * @post One transaction block was printed to the console.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static c6_frame_kind_t internal_one_xfer(uint32_t index, uint8_t hs_idx, c6_probe_stats_t* st)
{
  if (st == nullptr) {
    return k_c6_frame_garbage;
  }
  c6_probe_puts("  xfer ");
  c6_probe_put_u32(index);
  c6_probe_puts("\r\n");
  if (!internal_wait_ready(hs_idx)) {
    c6_probe_puts("    skipped: no side-band pin asserted\r\n");
    return k_c6_frame_garbage;
  }

  c6_sideband_sample_t pre  = {};
  c6_sideband_sample_t mid  = {};
  c6_sideband_sample_t post = {};
  const ra8_err_t      err  = internal_transfer(&pre, &mid, &post);
  st->attempts++;
  c6_probe_print_sideband("    pre ", &pre);
  c6_probe_print_sideband("    mid ", &mid);
  c6_probe_print_sideband("    post", &post);
  c6_probe_vote(&pre, &mid, &post, st);
  if (err != k_ra8_ok) {
    c6_probe_puts("    spi transfer error\r\n");
    return k_c6_frame_garbage;
  }

  return internal_report_frame(st);
}

ra8_err_t c6_probe_spi_pins_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod1_spi_sck,
                                           k_ra8_psel_sci_async,
                                           "c6probe.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod1_spi_cipo,
                                 k_ra8_psel_sci_async,
                                 "c6probe.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod1_spi_copi,
                                 k_ra8_psel_sci_async,
                                 "c6probe.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_output_init((ra8_port_pin_t)k_ra8_board_pmod1_spi_cs, k_ra8_level_high);
}

bool c6_probe_sweep_mode(ra8_spi_mode_t    mode,
                         uint32_t          pclka_hz,
                         c6_probe_stats_t* st,
                         uint8_t*          hs_idx)
{
  if (st == nullptr || hs_idx == nullptr) {
    return false;
  }
  const ra8_sci_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_c6_probe_sck_hz,
    .pclk_hz   = pclka_hz,
    .mode      = mode,
    .lsb_first = false,
  };
  c6_probe_puts("c6_probe: spi mode=");
  c6_probe_put_u32((uint32_t)mode);
  c6_probe_puts(" sck_hz=");
  c6_probe_put_u32((uint32_t)k_c6_probe_sck_hz);
  c6_probe_puts("\r\n");
  if (ra8_sci_spi_init((uint8_t)k_ra8_board_pmod1_sci_channel, &cfg) != k_ra8_ok) {
    c6_probe_puts("c6_probe: sci spi init failed\r\n");
    return false;
  }

  bool decoded = false;
  for (uint8_t i = 0U; i < (uint8_t)k_c6_probe_xfer_per_mode; i++) {
    const c6_frame_kind_t kind = internal_one_xfer((uint32_t)i + 1U, *hs_idx, st);
    if (kind == k_c6_frame_idle || kind == k_c6_frame_data) {
      decoded = true;
    }
    *hs_idx = c6_probe_best(st->hs_vote, (uint8_t)k_c6_sb_count, (uint32_t)k_c6_probe_min_votes);
    ra8_delay_ms((uint32_t)k_c6_probe_gap_ms);
  }
  (void)ra8_sci_spi_deinit((uint8_t)k_ra8_board_pmod1_sci_channel);
  return decoded;
}
