/**
 * @file ra8_io.h
 * @brief ra8_io -- unified peripheral-agnostic I/O fabric (umbrella header).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * `ra8_io` is one central I/O subsystem with swappable backends, built so an
 * application performs an operation and names the target rather than coupling
 * to a specific peripheral. Picking the destination (micro-SD, OSPI NOR flash,
 * MRAM, SDRAM, an in-RAM scratch volume, a UART/USB stream) is a one-line
 * backend choice, exactly as selecting a display backend is in `ra8_display_pal`.
 *
 * The subsystem is layered internally but ships as one coherent facade:
 *
 *       ra8_io  (one public facade: block devices, streams, files, compression)
 *     stream  |  VFS / path router  |  file ops
 *     --------+---------------------+-------------------------------
 *          filesystem-format ops  (FAT12/16/32, exFAT, future formats)
 *     ----------------------------------------------------------------
 *          page / block cache  (composes the issue #147 hierarchy)
 *     ----------------------------------------------------------------
 *          block-device vtable  (one interface, many media)
 *     ----------------------------------------------------------------
 *     SD-SPI | SDHI | OSPI-NOR | MRAM | SDRAM-ramdisk | USB-MSC | UART |
 *     USB-CDC | RAM
 *
 * This milestone ships the block-device fabric (`ra8_io_blockdev.h`): a single
 * 512-byte LBA block-device vtable with one backend per storage medium, plus a
 * bridge that exposes any backend as an `ra8_fs_backend_t` so the existing FAT
 * filesystem mounts on top of every medium unchanged. Later milestones add the
 * targetable stream/writer layer, the VFS mount table and file operations,
 * pluggable filesystem formats, the page/block cache, and compression.
 *
 * Include this single header to pull in the whole facade.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_cache.h"
#include "ra8_io_blockdev_mram.h"
#include "ra8_io_blockdev_ram.h"
#include "ra8_io_blockdev_sdhi.h"
#include "ra8_io_blockdev_sdram.h"
#include "ra8_io_blockdev_sdspi.h"
#include "ra8_io_blockdev_usbmsc.h"
#include "ra8_io_blockdev_xspi.h"
#include "ra8_io_fsfmt.h"
#include "ra8_io_log.h"
#include "ra8_io_stream.h"
#include "ra8_io_stream_blockdev.h"
#include "ra8_io_stream_ram.h"
#include "ra8_io_stream_uart.h"
#include "ra8_io_stream_usbcdc.h"
#include "ra8_io_vfs.h"

#ifdef __cplusplus
}
#endif
