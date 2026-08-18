/**
 * @file ra8_board_ek_ra8d2_dualcore.h
 * @brief Where the EK-RA8D2's Cortex-M85 and Cortex-M33 meet, stated once
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * The RA8D2 is a dual-core part and its two cores have to agree on an address
 * to talk through. Nothing in the tree used to say what that address was, so
 * every dual-core application invented its own answer: seven app-private
 * headers each declared the same SRAM2 base under a different name
 * (``k_dualcore_mailbox_addr``, ``k_cpu1_pingpong_shared_addr``,
 * ``k_cache_coherency_shared_addr``, ``k_erm33_mailbox_addr``,
 * ``k_lowpower_mailbox_addr``, ``k_com33_mailbox_addr``, ``k_bg_sram_base``),
 * each re-arguing the same three facts in a hand-written paragraph. One
 * memory-map decision was spelled out in roughly nineteen places across
 * ``examples/``, and a change to it would have had to find all nineteen.
 *
 * It is a BOARD fact, and the board already acts on it: this package's
 * ``boot/system_init.c`` programmes MPU region 4 over exactly this window.
 * That translation unit now takes its numbers from here, so the boot and the
 * applications cannot drift apart.
 *
 * SELF-CONTAINED ON PURPOSE. A dual-core app's CPU1 translation unit compiles
 * against a deliberately narrow include path (``ra8_core/inc``,
 * ``ra8_hal/inc``, this directory) because a Cortex-M33 image must not reach
 * a header that drags in the logger or the pin validator. This header pulls in
 * nothing but ``<stdint.h>`` and ``ra8_err.h`` so it can be included from
 * either core's image. Only ::ra8_board_shared_ram needs the M85-side
 * translation unit; the constants alone cost no link dependency.
 *
 * THE MAP
 * -------
 * | Window                      | Base       | Length   |
 * |-----------------------------|------------|----------|
 * | CPU0 (M85) private SRAM0+1  | 0x22000000 | 1 MiB    |
 * | CPU0 <-> CPU1 shared window | 0x22100000 | 576 KiB  |
 * | CPU1 (M33) private SRAM bank| 0x22190000 | 64 KiB   |
 * | CPU1 (M33) code image, MRAM | 0x020C0000 | 256 KiB  |
 *
 * The shared window is unallocated by BOTH linker scripts -- CPU0's ends at
 * 0x22100000 and CPU1's claims only the 64 KiB bank above it -- so the same
 * physical bytes back the same addresses on both cores with no aliasing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_board_dualcore_addr_t
 * @brief Base addresses of the EK-RA8D2 dual-core memory map.
 *
 * @details
 * ``uintptr_t`` rather than ``uint32_t``: these are addresses, and the
 * host unit-test build is 64-bit, where a ``uint32_t`` enum silently truncates
 * a pointer cast.
 *
 * @invariant ::k_ra8_board_shared_ram_base is above CPU0's linker-allocated
 *            SRAM and below ::k_ra8_board_cpu1_sram_base, so neither linker
 *            script can place an object in it.
 * @invariant ::k_ra8_board_cpu1_sram_base + 64 KiB is the top of on-chip SRAM.
 *
 * @code
 * volatile my_mailbox_t* mbox =
 *   (volatile my_mailbox_t*)(uintptr_t)k_ra8_board_shared_ram_base;
 * @endcode
 *
 * @see ra8_board_shared_ram        The runtime descriptor over this window.
 * @see ra8_board_dualcore_size_t   The matching lengths.
 *
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_ra8_board_shared_ram_base = 0x22100000UL, /**< Start of SRAM2: the CPU0 <-> CPU1
                                               *   meeting window. Both linker scripts
                                               *   leave it unclaimed.               */
  k_ra8_board_cpu1_sram_base  = 0x22190000UL, /**< CPU1's private 64 KiB bank at the
                                               *   top of on-chip SRAM: .data, .bss
                                               *   and the M33 main stack.           */
  k_ra8_board_cpu1_image_base = 0x020C0000UL, /**< MRAM_CPU1: where the linked M33
                                               *   image is pinned, so one .hex spans
                                               *   both cores' code.                 */
} ra8_board_dualcore_addr_t;

/**
 * @enum ra8_board_dualcore_size_t
 * @brief Lengths of the windows named by ::ra8_board_dualcore_addr_t.
 *
 * @details
 * The shared window's length is the distance from its base up to CPU1's
 * private bank -- it is bounded by that bank, not by the top of SRAM, so an
 * application carving a large buffer out of it cannot silently overrun into
 * the M33's stack.
 *
 * @invariant ::k_ra8_board_shared_ram_size_bytes equals
 *            ``k_ra8_board_cpu1_sram_base - k_ra8_board_shared_ram_base``.
 *
 * @see ra8_board_dualcore_addr_t The matching bases.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_board_shared_ram_size_bytes = 0x90000UL, /**< 576 KiB, base up to CPU1's bank. */
  k_ra8_board_cpu1_sram_size_bytes  = 0x10000UL, /**< 64 KiB CPU1 private bank.        */
  k_ra8_board_cpu1_image_size_bytes = 0x40000UL, /**< 256 KiB MRAM window for the M33
                                                  *   image.                           */
} ra8_board_dualcore_size_t;

static_assert((uintptr_t)k_ra8_board_shared_ram_base < (uintptr_t)k_ra8_board_cpu1_sram_base,
              "the shared window must start below CPU1's private SRAM bank");
static_assert((uintptr_t)k_ra8_board_shared_ram_base +
                  (uintptr_t)k_ra8_board_shared_ram_size_bytes ==
                (uintptr_t)k_ra8_board_cpu1_sram_base,
              "the shared window must end exactly where CPU1's private bank begins");

/**
 * @struct ra8_board_shared_ram_t
 * @brief Runtime descriptor of the CPU0 <-> CPU1 shared window.
 *
 * @details
 * What ::ra8_board_shared_ram fills in. An application carves its mailbox out
 * of `base` rather than out of a literal it copied, and a memory-map change
 * becomes one edit here plus a rebuild.
 *
 * @invariant `base` is non-NULL and `size_bytes` is non-zero after a
 *            successful ::ra8_board_shared_ram.
 * @invariant `base` and `size_bytes` describe a window no linker script
 *            allocates into.
 *
 * @code
 * ra8_board_shared_ram_t win = {};
 * if (ra8_board_shared_ram(&win) == k_ra8_ok) {
 *   volatile my_mailbox_t* mbox = (volatile my_mailbox_t*)win.base;
 * }
 * @endcode
 *
 * @see ra8_board_shared_ram      Fills this in.
 * @see ra8_board_dualcore_addr_t The compile-time form, for a static placement.
 *
 * @since 0.1.0
 */
typedef struct {
  void*    base;          /**< Start of the shared window (::k_ra8_board_shared_ram_base). */
  uint32_t size_bytes;    /**< Its length in bytes, bounded by CPU1's private bank.        */
  bool     non_cacheable; /**< True when the boot MPU marks this window Normal
                           *   non-cacheable, so a hand-off needs no clean or
                           *   invalidate. False in a build without
                           *   ``RA8_BOOT_ENABLE_CACHE_MPU`` -- where the M85 D-cache
                           *   is not enabled either, so nothing is stale, but the
                           *   memory map itself makes no promise.                    */
} ra8_board_shared_ram_t;

/**
 * @brief Describe the window where the M85 and the M33 meet.
 *
 * @details
 * Publishes the board's answer to "where do the two cores talk", so an
 * application stops carrying its own copy of the address, the length and the
 * cacheability argument. The values are compile-time board facts; this call
 * performs no hardware access and cannot fail for any reason other than a null
 * argument.
 *
 * @param[out] out Descriptor to fill in. Must not be null. Fully overwritten
 *                 on success.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           @p out describes the shared window.
 * @retval k_ra8_err_null_ptr @p out was null.
 *
 * @pre The image is running on an EK-RA8D2 (or its emulator), whose linker
 *      scripts leave this window unallocated.
 * @pre Called from a context that may read board configuration -- any, since
 *      no register is touched.
 * @post On success `out->base` is ::k_ra8_board_shared_ram_base and
 *       `out->size_bytes` is ::k_ra8_board_shared_ram_size_bytes.
 * @post On failure no caller-visible state is modified.
 *
 * @note Thread-safe and reentrant: it reads only compile-time constants.
 * @warning The window is shared. Two applications' worth of structures cannot
 *          both start at `base`; carve from it with an explicit offset when a
 *          component needs more than the first object.
 *
 * @see ra8_board_dualcore_addr_t  The same facts as compile-time constants,
 *                                 for a statically placed object.
 * @see ra8_cpu1_release           Starting the M33 once the window is set up.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_shared_ram(ra8_board_shared_ram_t* out);

#ifdef __cplusplus
}
#endif
