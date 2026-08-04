/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_spi_b_dma.c
 * @brief SPI_B DMA TX/RX pipes with Cortex-M85 cache coherency
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Companion translation unit to ``ra8_spi_b.c`` for the RA8D2 SPI_B
 * (Type-B SPI) driver (HUM Ch 43). Carries the DMA-pipe entry points
 * (``ra8_spi_write_dma`` / ``ra8_spi_read_dma``) that stream a byte buffer
 * to/from the SPDR FIFO front-end, together with the D-cache maintenance
 * that keeps those engine-touched buffers coherent with the Cortex-M85
 * L1 data cache:
 *
 *  - ``ra8_spi_write_dma`` cleans (flushes) the dirty cache lines covering
 *    the caller's TX buffer before arming the engine so the DMA reads the
 *    CPU-written bytes from memory.
 *  - ``ra8_spi_read_dma`` installs ::internal_spi_dma_rx_complete as the
 *    transfer-end hook so the RX buffer's cache lines are invalidated once
 *    the engine has finished writing memory, before the CPU (or the
 *    caller's callback) re-reads them.
 *
 * The per-channel register accessor ``ra8_spi()`` and the channel-count
 * constant ``k_ra8_spi_b_channel_count`` live in ``ra8_spi_regs.h``; the
 * public DMA API contract is in ``ra8_spi.h``. No other ``ra8_spi_b.c``
 * statics are referenced from here.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_cache.h"
#include "ra8_check.h"
#include "ra8_dma.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_spi.h"
#include "ra8_spi_regs.h"

static const char* s_tag = "SPI_B";

/* =============================================================================
 * DMA TX / RX
 * =============================================================================
 */

/**
 * @struct ra8_spi_dma_rx_ctx_t
 * @brief Driver-owned completion context for an RX DMA, used to bridge the
 *        cache-invalidate step onto the caller's completion callback.
 *
 * @details
 * The RX DMA writes the caller's ``out_buf`` directly to memory. With the
 * Cortex-M85 L1 D-cache enabled the CPU would otherwise read stale cached
 * lines, so the buffer must be invalidated once the transfer is known
 * complete. The substrate's only "known-complete" signal in this asynchronous
 * model is the transfer-end completion callback, so this driver installs its
 * own wrapper (::internal_spi_dma_rx_complete) as ``req.on_complete`` and
 * stashes the original caller callback / context / buffer here. The wrapper
 * invalidates the buffer first, then chains to the caller's callback.
 *
 * @invariant Indexed by SPI channel; at most one RX DMA is in flight per
 *            channel (the channel owns a single SPDR FIFO front-end).
 * @see internal_spi_dma_rx_complete
 */
typedef struct {
  void*                 rx_buf;           /**< Caller RX buffer to invalidate. */
  uint16_t              rx_len;           /**< RX byte count (pre-rounding).   */
  ra8_dma_complete_fn_t user_on_complete; /**< Caller callback (may be NULL).  */
  void*                 user_ctx;         /**< Caller callback context.        */
} ra8_spi_dma_rx_ctx_t;

/**
 * @var s_spi_dma_rx_ctx
 * @brief Per-channel RX-DMA completion bridge, indexed by SPI channel.
 *
 * @details Written by ::ra8_spi_read_dma before arming the transfer and read by
 *          ::internal_spi_dma_rx_complete from ISR context on transfer end.
 * @note Single-writer (init context) / single-reader (ISR) per channel.
 * @warning Do not modify directly; owned by the DMA RX path.
 * @since 0.1.0
 */
static ra8_spi_dma_rx_ctx_t s_spi_dma_rx_ctx[k_ra8_spi_b_channel_count];

/**
 * @brief Round a byte count up to a whole number of D-cache lines.
 *
 * @details
 * Cache maintenance by address acts on whole cache lines (32 bytes on the
 * Cortex-M85, reported by ``ra8_cache_dcache_line_bytes()``). Rounding the
 * length up to a line multiple guarantees the maintenance call covers the
 * final partial line of the buffer instead of stopping short of it. The line
 * size is a power of two, so the round-up uses a mask.
 *
 * @param[in] bytes Raw byte count to round.
 *
 * @return ``bytes`` rounded up to the next multiple of the D-cache line size.
 * @retval 0 ``bytes`` was 0.
 *
 * @pre ``bytes`` is small enough that the rounded value does not overflow.
 * @pre The reported line size is a non-zero power of two.
 * @post The result is a whole multiple of ``ra8_cache_dcache_line_bytes()``.
 * @post The result is greater than or equal to ``bytes``.
 *
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint32_t internal_round_up_to_cache_line(uint32_t bytes)
{
  if (bytes == 0U) {
    return 0U;
  }
  const uint32_t line = ra8_cache_dcache_line_bytes();
  if (line == 0U) { /* GCOVR_EXCL_BR_LINE */
    return bytes;   /* GCOVR_EXCL_LINE    */
  }
  return (bytes + (line - 1U)) & ~(line - 1U);
}

/**
 * @brief RX-DMA transfer-end wrapper: invalidate the buffer, then chain.
 *
 * @details
 * Installed as ``req.on_complete`` for every ::ra8_spi_read_dma transfer. Runs
 * from the DMAC transfer-end ISR (or ``ra8_fake_dma_complete`` under
 * ``RA8_OFF_TARGET``) once the engine has finished writing ``out_buf`` to
 * memory. It invalidates the cache lines spanning ``out_buf`` (rounded up to a
 * whole number of lines) so the CPU and the caller's callback re-read the
 * engine-written bytes from memory, then invokes the caller's original
 * completion callback, if any, with its original context. The cache call is a
 * no-op when the D-cache is disabled.
 *
 * @param[in] ctx Pointer to the driver's ::ra8_spi_dma_rx_ctx_t for the channel.
 *
 * @pre ``ctx`` points at a ::ra8_spi_dma_rx_ctx_t populated by ::ra8_spi_read_dma.
 * @pre The RX DMA for the channel has completed (transfer-end signalled).
 * @post The cache lines covering ``rx_buf`` have been invalidated.
 * @post The caller's ``user_on_complete`` has been invoked exactly once if set.
 *
 * @note Invoked from ISR context; must return quickly and take no locks.
 * @see ra8_spi_read_dma
 * @since 0.1.0
 */
static void internal_spi_dma_rx_complete(void* ctx)
{
  ra8_spi_dma_rx_ctx_t* rxc = (ra8_spi_dma_rx_ctx_t*)ctx;
  if (rxc == nullptr) { /* GCOVR_EXCL_BR_LINE */
    return;             /* GCOVR_EXCL_LINE    */
  }
  ra8_cache_dcache_invalidate_by_addr(rxc->rx_buf, internal_round_up_to_cache_line(rxc->rx_len));
  if (rxc->user_on_complete != nullptr) {
    rxc->user_on_complete(rxc->user_ctx);
  }
}

ra8_err_t ra8_spi_write_dma(uint8_t               channel,
                            const uint8_t*        data,
                            uint16_t              len,
                            ra8_dma_complete_fn_t on_complete,
                            void*                 ctx,
                            uint8_t*              out_dma_channel)
{
  RA8_CHECK_NULL_PTR(data, s_tag, "spi_write_dma: data");
  RA8_CHECK_NULL_PTR(out_dma_channel, s_tag, "spi_write_dma: out_dma_channel");
  if ((channel >= k_ra8_spi_b_channel_count) || (len == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* Cache coherency: the SPI DMA reads CPU-written 'data' straight from
   * memory, so flush any dirty lines covering it before the engine starts.
   * No-op when the D-cache is disabled. */
  ra8_cache_dcache_clean_by_addr(data, internal_round_up_to_cache_line(len));
  /* Byte-wide DMA writes target the low byte of SPDR; wider-frame
   * streaming is a future wave. */
  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  ra8_dma_request_t req = {};
  req.src_addr          = (uintptr_t)data;
  req.dst_addr          = (uintptr_t)&reg->SPDR;
  req.count             = len;
  req.width             = k_ra8_dmac_width_byte;
  req.src_inc           = true;
  req.dst_inc           = false;
  req.trigger           = (ra8_elc_event_t)0;
  req.on_complete       = on_complete;
  req.ctx               = ctx;
  return ra8_dma_request(&req, out_dma_channel);
}

ra8_err_t ra8_spi_read_dma(
  uint8_t channel,
  uint8_t*
    out_buf, // NOLINT(readability-non-const-parameter) -- written by the DMAC engine via dst_addr, never through the pointer.
  uint16_t              len,
  ra8_dma_complete_fn_t on_complete,
  void*                 ctx,
  uint8_t*              out_dma_channel)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "spi_read_dma: out_buf");
  RA8_CHECK_NULL_PTR(out_dma_channel, s_tag, "spi_read_dma: out_dma_channel");
  if ((channel >= k_ra8_spi_b_channel_count) || (len == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_spi_regs_t* reg = ra8_spi(channel);
  if (reg == nullptr) {           /* GCOVR_EXCL_BR_LINE */
    return k_ra8_err_invalid_arg; /* GCOVR_EXCL_LINE    */
  }
  /* Cache coherency: the RX DMA writes out_buf straight to memory. Wrap the
   * caller's completion so the CPU invalidates those lines once the transfer
   * is known-complete -- before it (or the caller's callback) reads them.
   * The wrapper is the only known-complete hook in this asynchronous path
   * (ra8_dma_request returns immediately; completion fires from the DMAC
   * transfer-end ISR). No-op when the D-cache is disabled. */
  ra8_spi_dma_rx_ctx_t* rxc = &s_spi_dma_rx_ctx[channel];
  rxc->rx_buf               = out_buf;
  rxc->rx_len               = len;
  rxc->user_on_complete     = on_complete;
  rxc->user_ctx             = ctx;

  /* HUM Ch 43.2.2 "SPDR : SPI Data Register" p 2881 */
  ra8_dma_request_t req = {};
  req.src_addr          = (uintptr_t)&reg->SPDR;
  req.dst_addr          = (uintptr_t)out_buf;
  req.count             = len;
  req.width             = k_ra8_dmac_width_byte;
  req.src_inc           = false;
  req.dst_inc           = true;
  req.trigger           = (ra8_elc_event_t)0;
  req.on_complete       = internal_spi_dma_rx_complete;
  req.ctx               = rxc;
  return ra8_dma_request(&req, out_dma_channel);
}
