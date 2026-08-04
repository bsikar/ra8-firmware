/**
 * @file ra8_io_blockdev_mram.h
 * @brief ra8_io block-device backend over the on-chip extra/data MRAM region.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * The MRAM backend exposes a hard-fenced window of the on-chip MRAM data
 * region as an array of 512-byte logical blocks on top of the ::ra8_io_blockdev_iface
 * fabric. It wraps the `ra8_flash` HAL driver: reads `memcpy` directly from the
 * memory-mapped MRAM array, writes route through `ra8_flash_write` (chunked into
 * the 32-byte MRAM program unit), and erases route through `ra8_flash_erase`
 * (mapping each 512-byte logical block onto sixteen 32-byte MRAM erase blocks).
 *
 * MRAM also holds the running firmware's `.text`, so this backend is
 * **dangerous**: ::ra8_io_blockdev_mram_init refuses any window that is not
 * wholly contained in the extra/data MRAM region
 * (`[k_ra8_flash_extra_start, k_ra8_flash_extra_start + k_ra8_flash_extra_size)`)
 * or that overlaps the code-MRAM region
 * (`[k_ra8_flash_code_start, k_ra8_flash_code_start + k_ra8_flash_code_size)`).
 * The fence is enforced at bind time and the per-call bounds check keeps every
 * access inside the bound window, so no operation can ever reach code MRAM.
 *
 * MRAM erases to all-ones (`0xFF`) and requires an erase before a program can
 * change a bit from 0 to 1, so the reported capabilities advertise
 * `must_erase_before_write = true` and `erase_value = 0xFF`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_io_blockdev.h"

/**
 * @struct ra8_io_blockdev_mram_state_t
 * @brief Caller-owned private state for an MRAM block device.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to ::ra8_io_blockdev_mram_init, which
 * validates the window, fills this state, and binds it into a
 * ::ra8_io_blockdev_t. Treat the fields as private. This state must out-live
 * every call made through the device.
 *
 * @invariant `base` is aligned to the MRAM erase-block size and lies inside the
 *            extra/data MRAM region.
 * @invariant `block_count * k_ra8_io_block_size_bytes` is a multiple of the MRAM
 *            erase-block size and the window does not overlap code MRAM.
 *
 * @since 0.1.0
 */
typedef struct {
  uintptr_t base;        /**< First MRAM byte address of the window (private).  */
  uint32_t  block_count; /**< Number of 512-byte logical blocks (private).      */
  bool      read_only;   /**< true => writes and erases are rejected (private). */
} ra8_io_blockdev_mram_state_t;

/**
 * @brief Bind a hard-fenced MRAM block-device backend into a caller handle.
 *
 * @details
 * Validates the requested window aggressively, records it in `state`, marks it
 * read-only per `read_only`, and points `bd` at the MRAM vtable with `state` as
 * its context. No allocation occurs; the caller owns both `bd` and `state`.
 *
 * The window `[base_addr, base_addr + block_count * 512)` must be erase-block
 * aligned, an integral number of erase blocks, wholly inside the extra/data
 * MRAM region, and must not overlap the code-MRAM `.text` region. Any violation
 * leaves `bd` and `state` untouched and returns ::k_ra8_err_invalid_arg.
 *
 * @param[out] bd          Handle to bind (zero-initialised by the caller).
 * @param[out] state       Caller-owned backend state to populate.
 * @param[in]  base_addr   First MRAM byte address of the window (erase aligned).
 * @param[in]  block_count Number of 512-byte logical blocks (>= 1).
 * @param[in]  read_only   true to reject writes and erases on this device.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Backend bound; `bd` is usable.
 * @retval k_ra8_err_null_ptr    `bd` or `state` was NULL.
 * @retval k_ra8_err_invalid_arg `block_count` was zero, the window was
 *                              misaligned, not a whole number of erase blocks,
 *                              outside the extra/data MRAM region, or overlapped
 *                              code MRAM.
 *
 * @pre `bd` and `state` out-live every call made through the device.
 * @pre `ra8_flash_init` (or `ra8_flash_open`) has been called before any write.
 * @post On success `bd` dispatches to the MRAM backend over the fenced window.
 * @post On any non-ok return `bd` and `state` are left unbound/untouched.
 *
 * @note Not thread-safe with respect to the same device.
 * @warning The bound window is the ONLY range this device can ever touch; the
 *          fence is what keeps writes and erases out of the running `.text`.
 *
 * @see ra8_io_blockdev_ram_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_io_blockdev_mram_init(ra8_io_blockdev_t*            bd,
                                                  ra8_io_blockdev_mram_state_t* state,
                                                  uintptr_t                     base_addr,
                                                  uint32_t                      block_count,
                                                  bool                          read_only);

#ifdef __cplusplus
}
#endif
