/**
 * @file ra8_io_blockdev_usbmsc.c
 * @brief USB mass-storage block-device backend -- a hosted device as 512-byte
 *        logical blocks.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Implements ::ra8_io_blockdev_iface by forwarding reads and writes to the
 * `ra8_usb_hmsc` class layer, which issues SCSI READ(10) / WRITE(10) /
 * READ CAPACITY(10) over Bulk-Only Transport. The class layer owns the attached
 * device, so the only per-device state is the LUN this handle addresses. USB
 * mass storage offers no host erase primitive and the class layer buffers
 * nothing, so both optional vtable slots are NULL. This file touches no raw
 * MMIO -- the HUM citations live in the `ra8_usb_hmsc` driver and the USB HAL
 * beneath it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_io_blockdev_usbmsc.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_backend.h"
#include "ra8_usb_hmsc.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_io_blockdev_usbmsc";

/**
 * @enum ra8_io_usbmsc_const_t
 * @brief USB-MSC backend layout constants.
 *
 * @details
 * A hosted mass-storage device exposes no erase granularity to the host, so the
 * fabric reports the smallest legal unit -- one logical block -- exactly as the
 * SD backends do.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_io_usbmsc_erase_unit_blocks = 1, /**< One logical block per erase unit. */
} ra8_io_usbmsc_const_t;

/**
 * @brief USB-MSC backend: read `count` blocks at `lba` into `buf`.
 *
 * @details
 * Rejects a run longer than one SCSI READ(10) can express, then forwards to
 * ::ra8_usb_hmsc_read10 on the bound LUN. The class layer enforces attach state,
 * LUN range and device capacity.
 *
 * @param[in]  ctx   USB-MSC backend state (as a void cookie).
 * @param[in]  lba   First logical block address.
 * @param[in]  count Number of blocks to read.
 * @param[out] buf   Destination buffer (>= `count * 512` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Blocks read into `buf`.
 * @retval k_ra8_err_null_ptr      `ctx` or `buf` was NULL.
 * @retval k_ra8_err_out_of_range  `count` exceeds the READ(10) transfer bound.
 * @retval k_ra8_err_invalid_state The class layer is down or nothing attached.
 * @retval k_ra8_err_invalid_arg   `count` was zero, or the LUN is out of range.
 * @retval k_ra8_err_hw_error      The Bulk-Only Transport exchange failed.
 *
 * @pre `ctx` is a populated USB-MSC state.
 * @pre `buf` is writable for `count * 512` bytes.
 * @post On success `buf` holds the requested device data.
 * @post On failure `buf` is left as the class layer leaves it.
 *
 * @note Blocking, polled; not safe to call from an ISR.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_usbmsc_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  const ra8_io_blockdev_usbmsc_state_t* st = (const ra8_io_blockdev_usbmsc_state_t*)ctx;
  if (count > (uint32_t)k_ra8_io_usbmsc_max_transfer_blocks) {
    return k_ra8_err_out_of_range;
  }
  return ra8_usb_hmsc_read10(st->lun, lba, (uint16_t)count, buf);
}

/**
 * @brief USB-MSC backend: write `count` blocks from `buf` at `lba`.
 *
 * @details
 * Rejects a run longer than one SCSI WRITE(10) can express, then forwards to
 * ::ra8_usb_hmsc_write10 on the bound LUN. A write-protected medium is reported
 * by the device, not predicted here.
 *
 * @param[in] ctx   USB-MSC backend state (as a void cookie).
 * @param[in] lba   First logical block address.
 * @param[in] count Number of blocks to write.
 * @param[in] buf   Source buffer (>= `count * 512` bytes).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Blocks written to the device.
 * @retval k_ra8_err_null_ptr      `ctx` or `buf` was NULL.
 * @retval k_ra8_err_out_of_range  `count` exceeds the WRITE(10) transfer bound.
 * @retval k_ra8_err_invalid_state The class layer is down or nothing attached.
 * @retval k_ra8_err_invalid_arg   `count` was zero, or the LUN is out of range.
 * @retval k_ra8_err_hw_error      The Bulk-Only Transport exchange failed.
 *
 * @pre `ctx` is a populated USB-MSC state.
 * @pre `buf` is readable for `count * 512` bytes.
 * @post On success the device blocks `[lba, lba+count)` equal `buf`.
 * @post On failure the device is left as the class layer leaves it.
 *
 * @note Blocking, polled; not safe to call from an ISR.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_usbmsc_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  const ra8_io_blockdev_usbmsc_state_t* st = (const ra8_io_blockdev_usbmsc_state_t*)ctx;
  if (count > (uint32_t)k_ra8_io_usbmsc_max_transfer_blocks) {
    return k_ra8_err_out_of_range;
  }
  return ra8_usb_hmsc_write10(st->lun, lba, (uint16_t)count, buf);
}

/**
 * @brief USB-MSC backend: report medium capabilities.
 *
 * @details
 * Issues SCSI READ CAPACITY(10) on the bound LUN and fills `out` for a
 * read-write medium with no erase concept. A device whose native block size is
 * not 512 bytes is refused: the fabric's logical block is fixed at
 * ::k_ra8_io_block_size_bytes, so reporting its block count would state a
 * capacity in a unit the layers above would read as 512-byte blocks.
 *
 * @param[in]  ctx USB-MSC backend state (as a const void cookie).
 * @param[out] out Capabilities snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                `*out` populated.
 * @retval k_ra8_err_null_ptr      `ctx` or `out` was NULL.
 * @retval k_ra8_err_not_supported The device's native block size is not 512.
 * @retval k_ra8_err_invalid_state The class layer is down or nothing attached.
 * @retval k_ra8_err_hw_error      The Bulk-Only Transport exchange failed.
 *
 * @pre `ctx` is a populated USB-MSC state.
 * @pre `out` is writable.
 * @post On success `*out` describes a read-write, no-erase USB medium.
 * @post No backend state is mutated.
 *
 * @note Blocking, polled: every call issues a SCSI command.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_usbmsc_get_caps(const void* ctx, ra8_io_blockdev_caps_t* out)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  const ra8_io_blockdev_usbmsc_state_t* st         = (const ra8_io_blockdev_usbmsc_state_t*)ctx;
  uint32_t                              blocks     = 0;
  uint32_t                              block_size = 0;
  const ra8_err_t cap = ra8_usb_hmsc_read_capacity(st->lun, &blocks, &block_size);
  if (cap != k_ra8_ok) {
    return cap;
  }
  /* GCOVR_EXCL_START -- host-unreachable: everything below runs only after a
   * completed READ CAPACITY(10), which needs a Bulk-Only Transport round trip.
   * The plain-RAM ra8_fake_mmap backing has no USB SIE model, so the CBW push
   * inside ra8_usb_hmsc always times out and this tail is bench territory --
   * the same boundary ra8_usb_hmsc.c marks on its own command tails. */
  if (block_size != (uint32_t)k_ra8_io_block_size_bytes) {
    return k_ra8_err_not_supported;
  }
  out->block_count             = blocks;
  out->erase_unit_blocks       = (uint32_t)k_ra8_io_usbmsc_erase_unit_blocks;
  out->program_size_bytes      = (uint32_t)k_ra8_io_block_size_bytes;
  out->logical_block_bytes     = (uint16_t)k_ra8_io_block_size_bytes;
  out->erase_value             = (uint8_t)k_ra8_io_erase_value_zero;
  out->must_erase_before_write = false;
  out->read_only               = false;
  return k_ra8_ok;
  /* GCOVR_EXCL_STOP */
}

/** @brief USB-MSC backend vtable. `erase`/`sync` are NULL (no host erase, no buffering). */
static const ra8_io_blockdev_iface_t s_usbmsc_iface = {
  .read     = internal_usbmsc_read,
  .write    = internal_usbmsc_write,
  .erase    = nullptr,
  .get_caps = internal_usbmsc_get_caps,
  .sync     = nullptr,
};

ra8_err_t ra8_io_blockdev_usbmsc_init(ra8_io_blockdev_t*              bd,
                                      ra8_io_blockdev_usbmsc_state_t* state,
                                      uint8_t                         lun)
{
  RA8_CHECK_NULL_PTR(bd, s_tag, "bd must not be nullptr");
  RA8_CHECK_NULL_PTR(state, s_tag, "state must not be nullptr");
  if (lun > (uint8_t)k_ra8_hmsc_max_lun) {
    return k_ra8_err_out_of_range;
  }
  state->lun = lun;
  bd->iface  = &s_usbmsc_iface;
  bd->ctx    = state;
  return k_ra8_ok;
}
