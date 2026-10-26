/**
 * @file fw_if_dma.h
 * @brief Architecture-neutral DMA port: channel configuration and a blocking
 * transfer behind one narrow vtable.
 * @ingroup grp_fw_dma
 *
 * @par Tag
 * [Ring 2 / Interface] {World: Any}
 *
 * @details
 * Portable code needs to move a block of samples to a codec, refill a display
 * line buffer, or copy inside memory without burning cycles, and every chip
 * spells its DMA block differently. This port carries the two intents such a
 * driver actually has -- configure a channel, run one transfer -- and leaves
 * the mechanism to a binding supplied by the chip and board.
 *
 * Channels are named in *board* numbering, dense and zero-based: a board
 * presents its wired channels as channel 0, channel 1, ... however the
 * silicon allocates them, and its binding maps them. That is the same rule
 * the clock port set for module instances and the UART, SPI and I2C ports set
 * for their lines, and how many channels exist is a board fact, so the
 * membrane does not range-check the number; the binding owns it and answers
 * ::k_ra8_err_not_found for one it does not carry.
 *
 * The channel description is deliberately small: transfer direction and data
 * width. Burst sizes, FIFO thresholds, address increments, arbitration
 * priority and link-list descriptors are chip and board policy and belong in
 * a chip-specific binding header, which the composition root may name because
 * the composition root already knows which chip it picked.
 *
 * ::fw_dma_transfer is blocking and all-or-error: on ::k_ra8_ok the whole
 * buffer moved, on any error the binding tells what it could not do. The
 * membrane checks what it can prove about the request before the binding is
 * reached: a nonzero length needs both pointers present. Alignment to the
 * configured width is part of the binding's contract, because only the
 * binding knows which bus masters a channel serves; a binding that accepts a
 * transfer it cannot carry returns ::k_ra8_err_invalid_arg rather than
 * corrupting the tail. An interrupt-driven or callback-completed transfer,
 * and scatter-gather, are not declared on speculation; they wait for a
 * first-party caller the same way the critical section does.
 *
 * Per the taxonomy in `docs/PORTS.md`, this uses (a) the caller-allocated
 * facade plus (b) a narrow ops struct the binding fills. Handles are
 * caller-owned. This interface performs no allocation and contains no chip,
 * operating-system or device header.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum fw_dma_direction_t
 * @brief Where the data flows.
 */
typedef enum : uint8_t {
  k_fw_dma_direction_none       = 0U, /**< Not a direction; rejected. */
  k_fw_dma_direction_mem_to_mem = 1U, /**< Memory to memory.          */
  k_fw_dma_direction_mem_to_per = 2U, /**< Memory to peripheral.      */
  k_fw_dma_direction_per_to_mem = 3U, /**< Peripheral to memory.      */
} fw_dma_direction_t;

/** @brief One past the last valid ::fw_dma_direction_t enumerator. */
#define K_FW_DMA_DIRECTION_COUNT 4U

/**
 * @enum fw_dma_width_t
 * @brief Unit the transfer moves.
 */
typedef enum : uint8_t {
  k_fw_dma_width_none = 0U, /**< Not a width; rejected.      */
  k_fw_dma_width_1    = 1U, /**< One byte per unit.          */
  k_fw_dma_width_2    = 2U, /**< Two bytes per unit.         */
  k_fw_dma_width_4    = 4U, /**< Four bytes per unit.        */
} fw_dma_width_t;

/** @brief One past the last valid ::fw_dma_width_t enumerator. */
#define K_FW_DMA_WIDTH_COUNT 5U

/**
 * @struct fw_dma_config_s
 * @brief Everything the port says about a channel.
 */
typedef struct fw_dma_config_s {
  fw_dma_direction_t direction; /**< Where the data flows.             */
  fw_dma_width_t     width;     /**< Bytes per unit, also the alignment. */
} fw_dma_config_t;

/**
 * @struct fw_dma_iface_s
 * @brief The narrow ops struct a chip-and-board binding fills.
 *
 * @details
 * Two entries, because the port has two intents. Each takes the binding's
 * own context as its first argument, so one binding can serve more than one
 * instance without a file-scope variable. A NULL op is a malformed binding,
 * not a declined capability, and ::fw_dma_bind rejects it.
 */
typedef struct fw_dma_iface_s {
  /** @brief Put @p chan on @p cfg's direction and width. */
  ra8_err_t (*configure)(void *ctx, uint8_t chan, fw_dma_config_t cfg);
  /** @brief Move @p len bytes from @p src to @p dst. Blocking, all-or-error. */
  ra8_err_t (*transfer)(void *ctx, uint8_t chan, const void *src, void *dst,
                        size_t len);
} fw_dma_iface_t;

/**
 * @struct fw_dma_s
 * @brief Caller-owned binding handle.
 *
 * @details
 * Populated only by ::fw_dma_bind and treated as opaque afterwards. The
 * `bound` flag is what every entry point checks: a zeroed handle is not bound,
 * so a caller that forgets to bind gets ::k_ra8_err_not_initialized rather
 * than a jump through an uninitialised function pointer.
 */
typedef struct fw_dma_s {
  const fw_dma_iface_t *iface; /**< Binding ops, never NULL once bound. */
  void                   *ctx;  /**< Binding context, may be NULL.       */
  bool                    bound; /**< Set by ::fw_dma_bind only.         */
} fw_dma_t;

/**
 * @brief Bind a handle to a chip-and-board DMA binding.
 *
 * @param[out] dma   Caller-owned storage, contents ignored on entry.
 * @param[in]  iface Ops struct; borrowed, must outlive @p dma.
 * @param[in]  ctx   Binding context, handed back to every op. May be NULL.
 * @return ::k_ra8_ok, or ::k_ra8_err_invalid_arg when @p dma or @p iface is
 *         NULL or @p iface leaves an op unset.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_dma_bind(fw_dma_t *dma, const fw_dma_iface_t *iface, void *ctx);

/**
 * @brief Put one channel on a direction and width.
 *
 * @param[in] dma_handle Bound handle.
 * @param[in] chan       Channel in board numbering.
 * @param[in] cfg        Channel description; an out-of-range field is
 *                       rejected before the binding is reached.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for an unbound handle or a
 *         malformed cfg, ::k_ra8_err_not_found when the board has no such
 *         channel, or whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_dma_configure(const fw_dma_t *dma_handle, uint8_t chan, fw_dma_config_t cfg);

/**
 * @brief Move bytes over one channel, blocking until the transfer settles.
 *
 * @details
 * All-or-error: on ::k_ra8_ok the whole buffer moved; on any error the
 * binding tells what it could not do, and the caller retries the whole
 * transfer. The length is in bytes; both pointers must be aligned to the
 * channel's configured width, which is the binding's contract. A zero-length
 * transfer is satisfied without arming the channel.
 *
 * @param[in] dma_handle Bound handle.
 * @param[in] chan       Channel in board numbering.
 * @param[in] src        Bytes to move; may be NULL only when @p len is zero.
 * @param[out] dst       Where to move them; may be NULL only when @p len is
 *                       zero.
 * @param[in] len        Bytes to move.
 * @return ::k_ra8_ok, ::k_ra8_err_invalid_arg for an unbound handle or a NULL
 *         buffer with a nonzero length, or whatever the binding reported.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
fw_dma_transfer(const fw_dma_t *dma_handle, uint8_t chan, const void *src,
                void *dst, size_t len);

#ifdef __cplusplus
}
#endif
