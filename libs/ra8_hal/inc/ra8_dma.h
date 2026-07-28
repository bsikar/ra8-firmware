/**
 * @file ra8_dma.h
 * @brief Generic DMA transfer substrate (DMAC engine)
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Ring 3 / HAL substrate. Sits on top of ``ra8_dmac`` and exposes a
 * driver-friendly API that hides free-channel allocation and
 * completion-interrupt wiring behind a single ``ra8_dma_request``
 * call.
 *
 * Drivers call ``ra8_dma_request`` with a descriptor that says
 * "I want bytes moved from A to B when ELC event E fires". The
 * substrate:
 *
 * 1. Picks a free DMAC channel (0..7) from an internal pool.
 * 2. Programmes DMSAR / DMDAR / DMCRA / DMTMD / DMAMD on the
 * selected channel.
 * 3. Routes the ELC trigger to the channel via ``ra8_isr``.
 * 4. Registers the caller's completion handler, if any, in the
 * dispatch table so the Cortex-M85 vector-table trampoline
 * can invoke it on the DMAC transfer-end interrupt.
 * 5. Returns the channel number so the caller can cancel.
 *
 * ``ra8_dma_release`` tears a channel down: stops the channel,
 * unregisters the completion interrupt, and returns the channel
 * to the free pool.
 *
 * The DTC has its own lifecycle + request model (install a
 * vector table, enable, let activation events drive transfers);
 * callers that need DTC semantics use ``ra8_dtc_*`` directly
 * rather than going through this substrate.
 *
 * ## Threading
 *
 * Single-threaded init context only. Completion callbacks run in
 * ISR context and must not call back into ``ra8_dma_request``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_dmac.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"

/* =============================================================================
 * Types
 * =============================================================================
 */

/**
 * @enum ra8_dma_channel_count_t
 * @brief Number of channels the DMAC backend exposes.
 */
typedef enum : uint8_t {
  k_ra8_dma_channel_count = 8U, /**< RA8 DMA channel count. */
} ra8_dma_channel_count_t;

/**
 * @enum ra8_dma_invalid_channel_t
 * @brief Sentinel for "no channel allocated".
 */
typedef enum : uint8_t {
  k_ra8_dma_channel_none = 0xFFU, /**< RA8 DMA channel none. */
} ra8_dma_invalid_channel_t;

/**
 * @typedef ra8_dma_complete_fn_t
 * @brief Caller-supplied completion callback.
 *
 * @param[in] ctx Context pointer recorded at request time.
 *
 * @note Invoked from ISR context. Must return quickly and must
 * not take any ra8_dma / ra8_mstp locks.
 */
typedef void (*ra8_dma_complete_fn_t)(void* ctx);

/**
 * @struct ra8_dma_request_t
 * @brief Descriptor for a single DMA transfer.
 *
 * @details
 * Populate one of these and hand it to ``ra8_dma_request()``.
 *
 * Fields:
 * - ``src_addr`` : Source address (bus-visible).
 * - ``dst_addr`` : Destination address.
 * - ``count`` : Number of *elements* to transfer; each
 * element is ``1 << width`` bytes.
 * - ``width`` : Transfer element width from ``ra8_dmac_width_t``.
 * - ``src_inc`` : ``true`` to increment ``src_addr`` after
 * each element.
 * - ``dst_inc`` : Same, for ``dst_addr``.
 * - ``trigger`` : ELC event that triggers the next element
 * (``k_ra8_elc_event_none`` for software start).
 * - ``on_complete`` : Optional callback fired from ISR on DMAC
 * transfer-end. ``nullptr`` for no callback.
 * - ``ctx`` : Passed to ``on_complete``.
 */
/* cppcheck cannot see tests/ so it flags every ra8_dma_request_t
 * field as unused; the fields are read in ra8_dma.c and in
 * tests/mocks/ra8_fake_dma.c. */
typedef struct {
  uintptr_t             src_addr;    /**< Src address. */
  uintptr_t             dst_addr;    /**< Dst address. */
  uint16_t              count;       /**< Count.       */
  ra8_dmac_width_t      width;       /**< Width.       */
  bool                  src_inc;     /**< Src inc.     */
  bool                  dst_inc;     /**< Dst inc.     */
  ra8_elc_event_t       trigger;     /**< Trigger.     */
  ra8_dma_complete_fn_t on_complete; /**< On complete. */
  void*                 ctx;         /**< Ctx.         */
} ra8_dma_request_t;

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the DMA substrate.
 *
 * @details
 * Requests MSTPCRA.MSTPA22 via ``ra8_mstp_enable(k_ra8_mstp_dmac0_dtc0)``
 * (so the DMAC is clocked), zeroes the internal channel-allocation
 * table, and returns. Does NOT programme any channel. Callers
 * follow with ``ra8_dma_request()`` to kick off actual transfers.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Substrate ready.
 * @retval k_ra8_err_hw_init_failed ``ra8_mstp_enable`` failed.
 *
 * @pre ``ra8_mstp_init()`` has been called (or this is a first
 * request against a fresh ref-count table).
 * @pre IRQs masked or single-threaded init context.
 *
 * @post The ra8_dma channel table is all-free.
 * @post DMAC bus clock is on.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dma_init(void);

/**
 * @brief Release the DMA substrate.
 *
 * @details
 * Stops every in-use channel, then drops the MSTP reference.
 * Used by tests; production code rarely tears this down.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Substrate torn down.
 * @retval k_ra8_err_hw_error ``ra8_mstp_disable`` underflow.
 *
 * @pre IRQs masked or single-threaded init context.
 * @post Every ra8_dma channel is free.
 * @post DMAC MSTP reference decremented.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dma_deinit(void);

/* =============================================================================
 * Request / release
 * =============================================================================
 */

/**
 * @brief Allocate a DMAC channel, programme it, and start the trigger wiring.
 *
 * @param[in] req Transfer descriptor. ``engine`` must be
 * ``k_ra8_dma_engine_dmac``.
 * @param[out] out_channel On success, the allocated DMAC channel
 * number in the range
 * ``[0, k_ra8_dma_channel_count)``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Transfer programmed and armed.
 * @retval k_ra8_err_null_ptr ``req`` or ``out_channel`` NULL.
 * @retval k_ra8_err_invalid_arg ``engine`` unsupported, ``count``
 * zero, or ``width`` out of range.
 * @retval k_ra8_err_no_mem All DMAC channels in use.
 * @retval k_ra8_err_hw_error Underlying ``ra8_dmac_start`` failed.
 *
 * @pre ``ra8_dma_init()`` has been called.
 * @pre IRQs masked or single-threaded init context.
 *
 * @post On success, the DMAC channel is running.
 * @post On success, the ELC trigger is wired (if ``trigger`` is
 * not ``k_ra8_elc_event_none``).
 * @post ``*out_channel`` holds a valid channel number on success.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_dma_release
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dma_request(const ra8_dma_request_t* req, uint8_t* out_channel);

/**
 * @brief Stop a channel and return it to the free pool.
 *
 * @param[in] channel Channel number returned by ``ra8_dma_request``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Channel released.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range.
 * @retval k_ra8_err_invalid_state Channel was not allocated.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller previously called ``ra8_dma_request`` for
 * ``channel``.
 *
 * @post Channel is stopped and marked free.
 * @post Any registered completion handler is unregistered.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dma_release(uint8_t channel);

/**
 * @brief Query the allocation state of a channel.
 *
 * @details
 * Diagnostic accessor used by unit tests to verify the allocator
 * state. Returns ``true`` if the channel is currently in use.
 *
 * @param[in] channel Channel number 0..k_ra8_dma_channel_count - 1.
 * @param[out] out_busy On success, ``true`` if allocated.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Query successful.
 * @retval k_ra8_err_null_ptr ``out_busy`` NULL.
 * @retval k_ra8_err_invalid_arg ``channel`` out of range.
 *
 * @pre ``out_busy`` is non-NULL.
 * @post No hardware state is modified.
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dma_channel_is_busy(uint8_t channel, bool* out_busy);

#ifdef RA8_OFF_TARGET
/**
 * @brief Host-only peek at the ``ra8_dma_request_t`` stashed for a channel.
 *
 * @details
 * The DMAC's DMSAR / DMDAR registers are 32-bit, but host-side
 * test buffers sit at 64-bit addresses. ``ra8_dma_request`` copies
 * the full ``ra8_dma_request_t`` into a off-target-only side table keyed
 * by channel so ``ra8_fake_dma_memcpy`` can walk the real
 * ``uintptr_t`` values instead of the truncated 32-bit copies in
 * the fake MMIO.
 *
 * @param[in] channel DMAC channel number 0..7.
 * @return Pointer to the stashed request, or ``nullptr`` if the
 * channel is free or out of range.
 * @retval non-NULL Pointer to the cached ``ra8_dma_request_t``.
 * @retval nullptr  ``channel`` out of range or slot not in use.
 *
 * @pre Test is running under ``RA8_OFF_TARGET``.
 * @pre ``ra8_dma_request()`` previously returned ``k_ra8_ok`` for ``channel``.
 * @post No firmware state is modified.
 * @post Returned pointer aliases the static side-table entry.
 *
 * @note Test-only; not declared on the target build.
 * @since 0.1.0
 */
const ra8_dma_request_t* ra8_dma_fake_peek_request(uint8_t channel);
#endif

/**
 * @brief Dispatch a transfer-end completion callback for one channel.
 *
 * @details
 * Called from the DMAC IRQ trampoline when a channel finishes.
 * Looks up the caller-supplied ``on_complete`` and invokes it
 * with the stored context. Tests can call this function directly
 * to simulate DMA completion without running real hardware.
 *
 * @param[in] channel Channel whose completion just fired.
 *
 * @pre Called from ISR context or (for tests) from a direct test
 * helper such as ``ra8_fake_dma_complete``.
 * @pre ``ra8_dma_init()`` previously succeeded.
 * @post On exit, the stored completion callback has been
 * invoked exactly once (if non-NULL).
 * @post No state change if ``channel`` is out of range.
 *
 * @note Thread safety: re-entrant in the sense that the handler
 * itself may enable nested interrupts; the dispatcher does
 * not take any locks.
 * @since 0.1.0
 */
void ra8_dma_dispatch_complete(uint8_t channel);

#ifdef __cplusplus
}
#endif
