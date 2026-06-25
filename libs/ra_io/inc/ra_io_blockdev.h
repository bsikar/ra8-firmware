/**
 * @file ra_io_blockdev.h
 * @brief ra_io block-device fabric -- one LBA vtable across every storage medium.
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * A block device exposes fixed-size logical blocks (sectors) addressed by a
 * Logical Block Address (LBA). This header defines the single block-device
 * interface every storage backend implements -- SD-over-SPI, native SDHI,
 * OSPI NOR flash, MRAM, an SDRAM ramdisk, and a pure-RAM scratch device --
 * so the layers above (filesystem, cache, VFS) never name a peripheral.
 *
 * The interface generalises the `ra_fs_backend_t` seam the FAT filesystem
 * already runs on: it keeps the same 512-byte logical block but adds an
 * explicit capability query (erase granularity, program size, erase value,
 * read-only, must-erase-before-write) so the filesystem and the write-back
 * cache can do correct read-modify-write on flash media. `ra_io_blockdev_as_fs_backend()`
 * bridges any backend back to an `ra_fs_backend_t`, so existing `ra_fs` mounts
 * unchanged on top of any medium.
 *
 * ## Backend model
 *
 * The handle (::ra_io_blockdev_t) is caller-allocated -- declare one per device
 * and bind a backend into it with that backend's `_init()` helper (see
 * `ra_io_blockdev_ram.h` and friends). No dynamic allocation occurs; backends
 * keep their state in caller-provided storage. Multiple devices coexist because
 * each owns its own handle and context.
 *
 * @code
 * static uint8_t s_disk[64U * 512U];
 * static ra_io_blockdev_ram_state_t s_state;
 * ra_io_blockdev_t bd = {};
 * (void)ra_io_blockdev_ram_init(&bd, &s_state, s_disk, 64U);
 *
 * ra_fs_backend_t be = {};
 * (void)ra_io_blockdev_as_fs_backend(&bd, &be);   // FAT now runs on the ramdisk
 * @endcode
 *
 * ## Boundary with ra_vsource
 *
 * This block device is the read/write/erase storage seam at [Ring 4 / PAL]. It
 * is deliberately distinct from `ra_vsource` (`ra_vsource.h`, [Ring 2 / Core]),
 * the read-only byte-offset view that feeds the #147 page cache. The split is
 * intentional, not drift: a Ring-2 source must not depend on this Ring-4 fabric,
 * since that would invert ring ordering (see `docs/RING_AND_WORLD.md`). The
 * sanctioned bridge is the Ring-4 adapter `ra_io_blockdev_vsource.h`, which
 * exposes a bound block device as a `ra_vsource_read_fn` so it can be wired into
 * the page cache via `ra_vsource_add_paged`.
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

#include "ra_err.h"
#include "ra_fs.h"

/* =============================================================================
 * Constants
 * =============================================================================
 */

/**
 * @enum ra_io_block_size_t
 * @brief Logical block size of the fabric.
 *
 * @details
 * The fabric, like `ra_fs`, addresses storage in 512-byte logical blocks. A
 * backend whose medium has a different native sector or page size adapts to
 * this logical size internally (e.g. NOR flash buffers a program/erase unit).
 *
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra_io_block_size_bytes = 512, /**< Bytes per logical block (the only size). */
} ra_io_block_size_t;

/**
 * @enum ra_io_erase_value_t
 * @brief Byte value a block reads back as after a successful erase.
 *
 * @details
 * RAM-backed media return zero after erase; NOR flash and MRAM return all-ones.
 * The bridge to `ra_fs_backend_t` consults this so it only advertises a
 * zero-guaranteeing erase to the formatter (`ra_fs` requires erased ranges to
 * read back as `0x00`).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_io_erase_value_zero = 0x00, /**< Block reads back as 0x00 after erase.  */
  k_ra_io_erase_value_ones = 0xFF, /**< Block reads back as 0xFF after erase.  */
} ra_io_erase_value_t;

/* =============================================================================
 * Capabilities
 * =============================================================================
 */

/**
 * @struct ra_io_blockdev_caps_t
 * @brief Static properties a backend reports about its medium.
 *
 * @details
 * Queried with ::ra_io_blockdev_get_caps after a backend is bound. The cache
 * and filesystem use `erase_unit_blocks`, `program_size_bytes`, and
 * `must_erase_before_write` to coalesce writes into erase-aligned units on
 * flash media and to choose read-modify-write where the program granularity is
 * coarser than one logical block.
 *
 * @invariant `logical_block_bytes == k_ra_io_block_size_bytes`.
 * @invariant `erase_unit_blocks >= 1`.
 * @invariant `erase_value` is one of ::ra_io_erase_value_t.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t block_count;             /**< Total addressable logical blocks.      */
  uint32_t erase_unit_blocks;       /**< Erase granularity, in logical blocks.  */
  uint32_t program_size_bytes;      /**< Minimum program granularity in bytes.  */
  uint16_t logical_block_bytes;     /**< Logical block size (always 512).       */
  uint8_t  erase_value;             /**< Post-erase byte (::ra_io_erase_value_t).*/
  bool     must_erase_before_write; /**< true => program needs a prior erase.   */
  bool     read_only;               /**< true => writes/erases are rejected.    */
} ra_io_blockdev_caps_t;
/* cppcheck-suppress-end [unusedStructMember] */

/* =============================================================================
 * Interface + handle
 * =============================================================================
 */

/**
 * @struct ra_io_blockdev_iface
 * @brief Opaque to applications -- backends implement this internally and
 *        export a binding helper through their own header.
 *
 * @details
 * The concrete vtable layout lives in `src/ra_io_blockdev_internal.h`. Apps
 * never construct one; they call a backend's `_init()` helper, which binds the
 * backend's const vtable into a caller-owned ::ra_io_blockdev_t.
 *
 * @since 0.1.0
 */
typedef struct ra_io_blockdev_iface ra_io_blockdev_iface_t;

/**
 * @struct ra_io_blockdev_t
 * @brief Caller-allocated block-device handle binding a backend to its context.
 *
 * @details
 * Zero-initialise (`= {}`) and pass to a backend `_init()` helper, which fills
 * the vtable and context. Treat the fields as private; reach the device only
 * through the `ra_io_blockdev_*` functions below. The handle and the backend's
 * context must out-live every call made through them.
 *
 * @invariant `iface` is non-NULL once a backend has been bound.
 *
 * @since 0.1.0
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  const ra_io_blockdev_iface_t* iface; /**< Bound backend vtable (private).      */
  void*                         ctx;   /**< Backend-private context (private).   */
} ra_io_blockdev_t;
/* cppcheck-suppress-end [unusedStructMember] */

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Read `count` logical blocks starting at `lba` into `buf`.
 *
 * @details
 * Forwards to the bound backend's read primitive. `buf` must hold at least
 * `count * k_ra_io_block_size_bytes` bytes. Reading past the device capacity is
 * rejected by the backend, not silently truncated.
 *
 * @param[in]  bd    Bound block-device handle.
 * @param[in]  lba   First logical block address to read.
 * @param[in]  count Number of consecutive blocks to read.
 * @param[out] buf   Destination buffer (>= `count * 512` bytes).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Blocks read into `buf`.
 * @retval k_ra_err_null_ptr        `bd` or `buf` was NULL.
 * @retval k_ra_err_not_initialized No backend is bound to `bd`.
 * @retval k_ra_err_out_of_range    `lba + count` exceeds the device capacity.
 * @retval k_ra_err_invalid_arg     Backend rejected the request.
 *
 * @pre A backend has been bound into `bd`.
 * @pre `buf` is writable for `count * 512` bytes.
 * @post On success `buf[0 .. count*512)` mirrors the device contents.
 * @post On any non-ok return `buf` is left unchanged by the fabric.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_io_blockdev_read(const ra_io_blockdev_t* bd, uint32_t lba, uint32_t count, uint8_t* buf);

/**
 * @brief Write `count` logical blocks from `buf` starting at `lba`.
 *
 * @details
 * Forwards to the bound backend's write primitive. On media that require an
 * erase before programming the backend performs the necessary read-modify-write
 * or erase internally; callers that want erase-aligned efficiency should use
 * ::ra_io_blockdev_erase plus aligned writes.
 *
 * @param[in] bd    Bound block-device handle.
 * @param[in] lba   First logical block address to write.
 * @param[in] count Number of consecutive blocks to write.
 * @param[in] buf   Source buffer (>= `count * 512` bytes).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Blocks written.
 * @retval k_ra_err_null_ptr        `bd` or `buf` was NULL.
 * @retval k_ra_err_not_initialized No backend is bound to `bd`.
 * @retval k_ra_err_out_of_range    `lba + count` exceeds the device capacity.
 * @retval k_ra_err_not_supported   The device is read-only.
 *
 * @pre A backend has been bound into `bd`.
 * @pre `buf` is readable for `count * 512` bytes.
 * @post On success the device blocks `[lba, lba+count)` equal `buf`.
 * @post On any non-ok return the device is left unchanged by the fabric.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_io_blockdev_write(const ra_io_blockdev_t* bd, uint32_t lba, uint32_t count, const uint8_t* buf);

/**
 * @brief Erase `count` blocks starting at `lba` to the medium's erase value.
 *
 * @details
 * After a successful erase the range reads back as `caps.erase_value` (zero for
 * RAM media, all-ones for NOR/MRAM). Backends whose medium has no erase concept
 * may omit the operation, in which case this returns ::k_ra_err_not_supported.
 *
 * @param[in] bd    Bound block-device handle.
 * @param[in] lba   First logical block address to erase.
 * @param[in] count Number of consecutive blocks to erase.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Range erased.
 * @retval k_ra_err_null_ptr        `bd` was NULL.
 * @retval k_ra_err_not_initialized No backend is bound to `bd`.
 * @retval k_ra_err_not_supported   The backend has no erase primitive.
 * @retval k_ra_err_out_of_range    `lba + count` exceeds the device capacity.
 *
 * @pre A backend has been bound into `bd`.
 * @pre `count` is non-zero for any observable effect.
 * @post On success the range reads back as `caps.erase_value`.
 * @post On any non-ok return the device is left unchanged by the fabric.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_io_blockdev_erase(const ra_io_blockdev_t* bd, uint32_t lba, uint32_t count);

/**
 * @brief Report the bound backend's medium capabilities.
 *
 * @param[in]  bd  Bound block-device handle.
 * @param[out] out Capabilities snapshot.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  `*out` populated.
 * @retval k_ra_err_null_ptr        `bd` or `out` was NULL.
 * @retval k_ra_err_not_initialized No backend is bound to `bd`.
 *
 * @pre A backend has been bound into `bd`.
 * @pre `out` is writable.
 * @post On success `*out` describes the bound medium.
 * @post No device or handle state is mutated.
 *
 * @note Thread-safe (pure read of immutable backend state).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_io_blockdev_get_caps(const ra_io_blockdev_t* bd,
                                               ra_io_blockdev_caps_t*  out);

/**
 * @brief Flush any backend write buffering to the medium.
 *
 * @details
 * For buffered backends (e.g. a NOR write-coalescing shim) this commits pending
 * data. Backends with no buffering treat it as a successful no-op.
 *
 * @param[in] bd Bound block-device handle.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  Pending writes committed (or none pending).
 * @retval k_ra_err_null_ptr        `bd` was NULL.
 * @retval k_ra_err_not_initialized No backend is bound to `bd`.
 *
 * @pre A backend has been bound into `bd`.
 * @pre The device is idle (no concurrent access).
 * @post On success the medium reflects every prior successful write.
 * @post No handle state is mutated.
 *
 * @note Not thread-safe with respect to the same device.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_io_blockdev_sync(const ra_io_blockdev_t* bd);

/**
 * @brief Expose a bound block device as an `ra_fs_backend_t` for `ra_fs`.
 *
 * @details
 * Fills `out` with trampolines that forward `ra_fs`'s `read_block` /
 * `write_block` / `get_capacity` / `erase_blocks` calls into this fabric, with
 * `out->ctx` pointing at `bd`. The erase trampoline advertises itself to the
 * formatter only when the medium erases to zero (`ra_fs` requires erased ranges
 * to read back as `0x00`); on all-ones flash it returns ::k_ra_err_not_supported
 * so the formatter falls back to writing zeros. After this call any existing
 * `ra_fs` API (`ra_fs_mount`, `ra_fs_format`, ...) works on the bound medium.
 *
 * @param[in]  bd  Bound block-device handle (must out-live the filesystem use).
 * @param[out] out `ra_fs_backend_t` to populate.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                  `*out` wired to `bd`.
 * @retval k_ra_err_null_ptr        `bd` or `out` was NULL.
 * @retval k_ra_err_not_initialized No backend is bound to `bd`.
 *
 * @pre A backend has been bound into `bd`.
 * @pre `out` is writable and out-lives every filesystem call.
 * @post On success `out`'s callbacks and `ctx` reference `bd`.
 * @post No device or handle state is mutated.
 *
 * @note `bd` must remain valid for the entire lifetime of the resulting mount.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_io_blockdev_as_fs_backend(const ra_io_blockdev_t* bd,
                                                    ra_fs_backend_t*        out);

#ifdef __cplusplus
}
#endif
