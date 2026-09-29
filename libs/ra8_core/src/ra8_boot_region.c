/**
 * @file ra8_boot_region.c
 * @brief Startup zero-fill for linker regions the reset handler cannot reach
 * @ingroup grp_core
 *
 * @details
 * See ra8_boot_region.h for why `.sdram_data` needs a second, later fill pass:
 * the section is `NOLOAD` in external SDRAM, so the reset handler's `.bss`
 * loop (which walks `g_ra8_ls_sbss` to `g_ra8_ls_ebss`, both in SRAM) never
 * covers it, and the window itself does not answer until the SDRAM controller
 * is up.
 *
 * On the host test build (`RA8_OFF_TARGET`) there is no linker script and
 * therefore no `g_ra8_ls_ssdram` / `g_ra8_ls_esdram`, so the section bounds
 * come from a file-static stand-in window exposed through
 * ::ra8_boot_test_sdram_window. That mirrors how ra8_crashlog.c drops its
 * `.noinit` section attribute off target.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_boot_region.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"

/** @brief Module log tag. */
static const char* const s_tag = "boot_region";

#ifdef RA8_OFF_TARGET
/** @brief Sizes for the host stand-in `.sdram_data` window. */
typedef enum : uint8_t {
  k_ra8_boot_test_window_bytes = 64U, /**< Stand-in window size in bytes. */
} ra8_boot_test_window_size_t;

/** @brief Host stand-in for the `.sdram_data` section (see the file note). */
static uint8_t s_ra8_boot_test_window[k_ra8_boot_test_window_bytes];
#else
/* Defined by every linker script in the tree around the `.sdram_data` output
 * section (for example libs/ra8_board_ek_ra8d2/ld/linker_script.ld:314). The
 * project convention is that linker symbols carry the `g_ra8_ls_` prefix so
 * they stay out of the reserved leading-underscore namespace. */
extern uint8_t g_ra8_ls_ssdram; /**< Start of `.sdram_data` in SDRAM. */
extern uint8_t g_ra8_ls_esdram; /**< End of `.sdram_data` in SDRAM.   */
#endif /* RA8_OFF_TARGET */

ra8_err_t ra8_boot_zero_region(void* start, const void* end)
{
  /* Below RA8_LOG_LEVEL error the log macros collapse to ((void)0), which
   * leaves the tag unreferenced under -Werror=unused-const-variable. */
  (void)s_tag;

  RA8_CHECK_NULL_PTR(start, s_tag, "start must not be nullptr");
  RA8_CHECK_NULL_PTR(end, s_tag, "end must not be nullptr");

  uint8_t* const       first = (uint8_t*)start;
  const uint8_t* const last  = (const uint8_t*)end;
  if (last < first) {
    ra8_log_error(s_tag, "zero_region: end precedes start");
    return k_ra8_err_invalid_arg;
  }

  const size_t bytes = (size_t)(last - first);
  for (size_t i = 0U; i < bytes; i++) {
    first[i] = 0U;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_boot_zero_sdram_bss(void)
{
#ifdef RA8_OFF_TARGET
  uint8_t* const window = s_ra8_boot_test_window;
  return ra8_boot_zero_region(window, &window[k_ra8_boot_test_window_bytes]);
#else
  return ra8_boot_zero_region(&g_ra8_ls_ssdram, &g_ra8_ls_esdram);
#endif
}

#ifdef RA8_OFF_TARGET
uint8_t* ra8_boot_test_sdram_window(size_t* out_bytes)
{
  RA8_ASSERT(out_bytes != nullptr, "out_bytes must not be nullptr");
  *out_bytes = (size_t)k_ra8_boot_test_window_bytes;
  return s_ra8_boot_test_window;
}
#endif /* RA8_OFF_TARGET */
