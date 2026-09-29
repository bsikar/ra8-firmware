/**
 * @file examples/ek_ra8d2/hw_pending/devcfg_record_demo/src/main.c
 * @brief ra8_devcfg demo: commit and resolve a per-unit record through an
 *        app-owned RAM store, then probe the real unit read-only.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `ra8_devcfg` owns the versioned, CRC-32-protected per-unit record (VCOM,
 * serials, touch calibration, key identity) with a two-copy header-last commit.
 * It reaches its medium only through the injected ::ra8_devcfg_store_t seam, so
 * it was fully host-testable and had no firmware consumer at all. This app is
 * that consumer:
 *
 *   1. An app-owned RAM medium, pre-filled with 0xFF so it looks like a blank
 *      never-programmed window, is bound through the store seam. The blank
 *      resolve is checked first: ::ra8_devcfg_is_blank reports UNPROVISIONED
 *      and ::ra8_devcfg_get_vcom_mv refuses rather than inventing a default.
 *   2. A populated record is committed, re-loaded, and compared field for
 *      field, which exercises the encode, the CRC, the two-copy resolver, and
 *      the VCOM validity gate end to end.
 *   3. The production extra-MRAM store is then loaded READ-ONLY to report
 *      whether this unit is provisioned. Nothing is committed to it: that
 *      window is one-time-programmable on this silicon (HUM Ch 59.7.4.5), so
 *      an example that programmed it would burn a slot on every run.
 *
 * Observable over the SCI8 / J-Link OB VCOM console. A good run prints
 * `devcfg_record_demo: record round trip PASS` followed by one line reporting
 * the unit's own provisioning state. It lives under hw_pending because it has
 * not been captured on the bench yet; the RAM leg needs no peripheral beyond
 * the console, so ra8_emulator runs it as is.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_boot_entry.h"
#include "ra8_devcfg.h"
#include "ra8_err.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_uart.h"
#include "ra8_log.h"
#include "ra8_sci.h"

/**
 * @enum dc_const_t
 * @brief Console, medium, and record knobs (no magic numbers).
 *
 * @details Collects every literal the app uses so the magic-number gate stays
 *          silent. The RAM medium spans both copy slots, so the resolver sees
 *          the same geometry it sees on silicon.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_dc_uart_chan    = 8U,          /**< SCI8 J-Link OB console.               */
  k_dc_medium_bytes = 448U,        /**< copy1 offset + one slot pitch.        */
  k_dc_blank_byte   = 0xFFU,       /**< Never-programmed backing byte value.  */
  k_dc_vcom_mv      = 2300U,       /**< Demo VCOM magnitude (-2.30 V).        */
  k_dc_hw_rev       = 2U,          /**< Demo board revision.                  */
  k_dc_fixture_id   = 7U,          /**< Demo provisioning fixture id.         */
  k_dc_mfg_date     = 20260916U,   /**< Demo packed YYYYMMDD.                 */
  k_dc_key_id       = 0x5A5A0001U, /**< Demo key identifier, never key data.  */
  k_dc_cal_seed     = 0x11U,       /**< First byte of the demo touch blob.    */
  k_dc_cal_step     = 3U,          /**< Demo touch-blob byte step.            */
} dc_const_t;

static uint8_t s_medium[k_dc_medium_bytes]; /**< App-owned RAM backing store. */

static ra8_io_stream_t            s_uart;       /**< Console stream.       */
static ra8_io_stream_uart_state_t s_uart_state; /**< Console stream state. */

/**
 * @brief Write a NUL-terminated string to the console stream.
 *
 * @param[in] text Message to queue on SCI8.
 * @return void
 * @pre The console stream was initialised.
 * @post The text was queued on the console sink.
 * @note Errors are ignored: the console reports, it does not act.
 * @since 0.1.0
 */
static void internal_print(const char* text)
{
  (void)ra8_io_stream_puts(&s_uart, text);
}

/**
 * @brief Read bytes out of the app-owned RAM medium.
 *
 * @param[in]  offset Byte offset into the devcfg region.
 * @param[out] dst    Destination buffer, at least `len` bytes.
 * @param[in]  len    Bytes to read.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               `dst` holds `len` bytes.
 * @retval k_ra8_err_null_ptr     `dst` was NULL.
 * @retval k_ra8_err_invalid_size The span leaves the medium.
 * @post On error `dst` is untouched.
 * @since 0.1.0
 */
static ra8_err_t internal_medium_read(uint32_t offset, uint8_t* dst, uint32_t len)
{
  if (dst == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((offset > (uint32_t)k_dc_medium_bytes) ||
      (len > ((uint32_t)k_dc_medium_bytes - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(dst, &s_medium[offset], (size_t)len);
  return k_ra8_ok;
}

/**
 * @brief Program bytes into the app-owned RAM medium.
 *
 * @param[in] offset Byte offset into the devcfg region.
 * @param[in] src    Source buffer, at least `len` bytes.
 * @param[in] len    Bytes to write.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               `len` bytes were stored.
 * @retval k_ra8_err_null_ptr     `src` was NULL.
 * @retval k_ra8_err_invalid_size The span leaves the medium.
 * @post On error the medium is untouched.
 * @since 0.1.0
 */
static ra8_err_t internal_medium_write(uint32_t offset, const uint8_t* src, uint32_t len)
{
  if (src == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if ((offset > (uint32_t)k_dc_medium_bytes) ||
      (len > ((uint32_t)k_dc_medium_bytes - offset))) {
    return k_ra8_err_invalid_size;
  }
  (void)memcpy(&s_medium[offset], src, (size_t)len);
  return k_ra8_ok;
}

static const ra8_devcfg_store_t s_ram_store = {.read  = internal_medium_read,
                                               .write = internal_medium_write};

/**
 * @brief Build the deterministic demo record.
 *
 * @param[out] out_rec Record to populate.
 * @return void
 * @post Every body field, the flag word, and the schema version are set.
 * @since 0.1.0
 */
static void internal_make_record(ra8_devcfg_record_t* out_rec)
{
  *out_rec = (ra8_devcfg_record_t){};
  (void)strncpy(out_rec->body.serial, "RA8D2-DEMO-0001", sizeof out_rec->body.serial - 1U);
  (void)strncpy(out_rec->body.panel_serial, "PANEL-0001", sizeof out_rec->body.panel_serial - 1U);
  (void)strncpy(out_rec->body.panel_lut_id, "M641", sizeof out_rec->body.panel_lut_id - 1U);
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_devcfg_touch_cal_len; i++) {
    out_rec->body.touch_cal[i] =
      (uint8_t)((uint32_t)k_dc_cal_seed + (i * (uint32_t)k_dc_cal_step));
  }
  out_rec->body.mfg_date      = (uint32_t)k_dc_mfg_date;
  out_rec->body.device_key_id = (uint32_t)k_dc_key_id;
  out_rec->body.hw_rev        = (uint16_t)k_dc_hw_rev;
  out_rec->body.fixture_id    = (uint16_t)k_dc_fixture_id;
  out_rec->body.panel_vcom_mv = (uint16_t)k_dc_vcom_mv;
  out_rec->flags              = (uint32_t)k_ra8_devcfg_flag_provisioned |
                   (uint32_t)k_ra8_devcfg_flag_vcom_valid |
                   (uint32_t)k_ra8_devcfg_flag_touch_valid;
  out_rec->schema_version = (uint16_t)k_ra8_devcfg_schema_ver;
}

/**
 * @brief Drive the blank resolve, the commit, and the resolve-back compare.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Blank resolve, commit, and compare agreed.
 * @retval k_ra8_err_invalid_state   A stage disagreed with the record written.
 * @retval other                     Propagated from load or commit.
 * @post The RAM medium holds one CRC-valid record copy on success.
 * @since 0.1.0
 */
static ra8_err_t internal_round_trip(void)
{
  (void)memset(s_medium, (int)k_dc_blank_byte, sizeof s_medium);
  ra8_devcfg_reset();
  (void)ra8_devcfg_load(&s_ram_store);
  if (!ra8_devcfg_is_blank()) {
    return k_ra8_err_invalid_state;
  }
  uint16_t refused_mv = 0U;
  if (ra8_devcfg_get_vcom_mv(&refused_mv) == k_ra8_ok) {
    return k_ra8_err_invalid_state;
  }

  ra8_devcfg_record_t rec = {};
  internal_make_record(&rec);
  const ra8_err_t commit_err = ra8_devcfg_commit(&s_ram_store, &rec);
  if (commit_err != k_ra8_ok) {
    return commit_err;
  }

  ra8_devcfg_reset();
  const ra8_err_t load_err = ra8_devcfg_load(&s_ram_store);
  if (load_err != k_ra8_ok) {
    return load_err;
  }
  if (ra8_devcfg_is_blank()) {
    return k_ra8_err_invalid_state;
  }

  uint16_t        vcom_mv  = 0U;
  const ra8_err_t vcom_err = ra8_devcfg_get_vcom_mv(&vcom_mv);
  if (vcom_err != k_ra8_ok) {
    return vcom_err;
  }
  const ra8_devcfg_body_t* body     = nullptr;
  const ra8_err_t          body_err = ra8_devcfg_get_body(&body);
  if (body_err != k_ra8_ok) {
    return body_err;
  }
  if ((vcom_mv != (uint16_t)k_dc_vcom_mv) ||
      (memcmp(body->touch_cal, rec.body.touch_cal, sizeof body->touch_cal) != 0) ||
      (memcmp(body->serial, rec.body.serial, sizeof body->serial) != 0) ||
      (body->device_key_id != (uint32_t)k_dc_key_id)) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Report this unit's own provisioning state without programming it.
 *
 * @return void
 * @post One console line describes the unit as provisioned or blank.
 * @note Read-only by design: the extra-MRAM window is one-time-programmable.
 * @since 0.1.0
 */
static void internal_probe_unit(void)
{
  ra8_devcfg_reset();
  const ra8_devcfg_store_t* store = ra8_devcfg_default_store();
  (void)ra8_devcfg_load(store);
  if (ra8_devcfg_is_blank()) {
    internal_print("devcfg_record_demo: unit UNPROVISIONED (read-only probe)\r\n");
    return;
  }
  uint16_t unit_mv = 0U;
  if (ra8_devcfg_get_vcom_mv(&unit_mv) == k_ra8_ok) {
    internal_print("devcfg_record_demo: unit provisioned, vcom ");
    (void)ra8_io_stream_put_u32(&s_uart, (uint32_t)unit_mv);
    internal_print(" mV\r\n");
    return;
  }
  internal_print("devcfg_record_demo: unit provisioned, vcom not valid\r\n");
}

/**
 * @brief Firmware entry point.
 *
 * @details Brings up the console, runs the RAM-store round trip, probes the
 *          unit read-only, and parks in an infinite loop.
 *
 * @pre SystemInit configured VTOR / FPU / priority grouping.
 * @post A PASS or FAIL verdict and one unit-state line are queued on SCI8.
 * @post Control parks in an infinite loop; the function never returns.
 * @note Single-threaded; runs to the park loop on the main stack.
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  (void)ra8_io_stream_uart_init(&s_uart, &s_uart_state, (uint8_t)k_dc_uart_chan);
  (void)ra8_io_log_attach(&s_uart);
  internal_print("devcfg_record_demo: boot\r\n");

  if (internal_round_trip() == k_ra8_ok) {
    internal_print("devcfg_record_demo: record round trip PASS\r\n");
  } else {
    internal_print("devcfg_record_demo: record round trip FAIL\r\n");
  }

  internal_probe_unit();

  (void)ra8_sci_flush((uint8_t)k_dc_uart_chan);
  while (true) {
  }
}
