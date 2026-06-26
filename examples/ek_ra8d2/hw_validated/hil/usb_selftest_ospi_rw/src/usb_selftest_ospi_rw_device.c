/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_ospi_rw/src/usb_selftest_ospi_rw_device.c
 * @brief OSPI-flash bring-up + write-window pre-erase for the writable-OSPI self-loop
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The OSPI provisioning step of the `usb_selftest_ospi_rw` application,
 * split out of `main.c` so each translation unit stays below the 1000-line
 * file-size cap. The device-side ThreadX worker (in `main.c`) calls
 * ::ospirw_ospi_provision once, before USB attach, to bring the onboard
 * IS25LX512M up in 1S-1S-1S mode and erase the 32 KiB scratch window that
 * the host's WRITE(10) data-OUT phase later programs.
 *
 * The two J-Link debug probes that record this step's outcome
 * (``s_dbg_ospi_prov`` / ``s_dbg_ospi_id``) live here, beside their only
 * writer.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_xspi.h"
#include "usb_selftest_ospi_rw_steps.h"

#ifndef RA_SIMULATOR_MODE

/* -------------------------------------------------------------------------- */
/* J-Link probes (OSPI bring-up side) */
/* -------------------------------------------------------------------------- */

/** @brief OSPI bring-up + window pre-erase result (0 = ready, else error). */
static volatile uint32_t s_dbg_ospi_prov = (uint32_t)k_ospirw_no_mismatch;
/** @brief OSPI JEDEC id read back at boot (sanity probe). */
static volatile uint32_t s_dbg_ospi_id;

[[nodiscard]] ra_err_t ospirw_ospi_provision(void)
{
  (void)ra_board_io_expander_set_octospi_active();
  ra_err_t err = ra_board_xspi_pins_init();
  if (err != k_ra_ok) {
    s_dbg_ospi_prov = (uint32_t)err;
    return err;
  }
  err = ra_xspi_init((uint8_t)k_ospirw_instance, k_ra_xspi_lio_1s1s1s);
  if (err != k_ra_ok) {
    s_dbg_ospi_prov = (uint32_t)err;
    return err;
  }
  uint32_t id = 0U;
  (void)ra_xspi_flash_read_id((uint8_t)k_ospirw_instance, &id);
  s_dbg_ospi_id = id;
  const uint32_t windows =
    ((uint32_t)k_ospirw_sectors * (uint32_t)k_ospirw_block_size) / (uint32_t)k_ospirw_erase_sector;
  for (uint32_t e = 0U; e < windows; e++) {
    const uint32_t addr = (uint32_t)k_ospirw_offset + (e * (uint32_t)k_ospirw_erase_sector);
    err                 = ra_xspi_flash_erase_sector((uint8_t)k_ospirw_instance, addr);
    if (err != k_ra_ok) {
      s_dbg_ospi_prov = (uint32_t)err;
      return err;
    }
  }
  s_dbg_ospi_prov = 0U;
  return k_ra_ok;
}

#endif /* !RA_SIMULATOR_MODE */
