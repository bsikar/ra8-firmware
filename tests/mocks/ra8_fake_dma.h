/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fake_dma.h
 * @brief Host-test DMA transfer fake
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Tests that exercise a DMA path need a way to simulate a
 * transfer without a real DMAC engine. ``ra8_fake_dma`` provides
 * two helpers:
 *
 *  - ``ra8_fake_dma_memcpy(ch)`` -- reads the DMAC channel's
 *    DMSAR / DMDAR / DMCRA / DMTMD / DMAMD registers and
 *    performs the requested copy directly in host RAM, as if
 *    the DMA engine had done the work.
 *
 *  - ``ra8_fake_dma_complete(ch)`` -- calls
 *    ``ra8_dma_dispatch_complete(ch)`` to fire the
 *    registered completion callback, as if the DMAC
 *    transfer-end interrupt had just arrived.
 *
 * Together, these let a test do:
 *
 * @code{.c}
 * ra8_dma_request(...);
 * ra8_fake_dma_memcpy(channel);
 * ra8_fake_dma_complete(channel);
 * // assertions on the destination buffer + callback side-effects.
 * @endcode
 *
 * ``ra8_fake_dma_memcpy`` honours the ``src_inc`` / ``dst_inc``
 * mode bits by walking the DMAMD fields; fixed-address transfers
 * read or write the same location N times.
 *
 * The helper deliberately does NOT touch ``ra8_dma``'s channel
 * state -- if a test wants the channel freed, it must also call
 * ``ra8_dma_release``. This mirrors real hardware: a completion
 * IRQ on its own does not free a channel; the driver must.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Walk a DMAC channel's registers and perform the transfer
 *        in host RAM.
 *
 * @details
 * Reads DMSAR, DMDAR, DMCRA, DMTMD, DMAMD for ``channel`` and
 * copies ``DMCRA`` elements of the width encoded in DMTMD.SZ
 * from the source to the destination. Source and destination
 * addresses are incremented per-element according to DMAMD.SM /
 * DMAMD.DM.
 *
 * @param[in] channel DMAC channel number 0..7.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Copy completed.
 * @retval k_ra8_err_invalid_arg   Channel out of range.
 *
 * @pre Test is running under ``RA8_OFF_TARGET``.
 * @pre ``ra8_dma_request`` has programmed the channel.
 *
 * @post DMCRA is zeroed (transfer complete).
 *
 * @note Thread safety: test code only, single-threaded.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fake_dma_memcpy(uint8_t channel);

/**
 * @brief Fire the registered completion callback for a channel.
 *
 * @details
 * Forwards to ``ra8_dma_dispatch_complete(channel)``.
 *
 * @param[in] channel DMAC channel number 0..7.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Callback fired (or was NULL).
 * @retval k_ra8_err_invalid_arg   Channel out of range.
 *
 * @pre Test is running under ``RA8_OFF_TARGET``.
 *
 * @post The registered ``on_complete`` callback ran exactly once.
 *
 * @note Thread safety: test code only, single-threaded.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fake_dma_complete(uint8_t channel);

#ifdef __cplusplus
}
#endif
