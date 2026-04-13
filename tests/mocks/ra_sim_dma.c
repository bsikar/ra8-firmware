/**
 * @file ra_sim_dma.c
 * @brief Host-test DMA transfer simulator implementation
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifdef RA_SIMULATOR_MODE

#include "ra_sim_dma.h"

#include <stdint.h>

#include "ra8d2_dmac_regs.h"
#include "ra_dma.h"
#include "ra_err.h"

/**
 * @enum ra_sim_dma_bpe_t
 * @brief Bytes-per-element lookup indexed by ``ra_dmac_width_t``.
 */
typedef enum : uint8_t {
  k_ra_sim_dma_bpe_byte = 1U,
  k_ra_sim_dma_bpe_half = 2U,
  k_ra_sim_dma_bpe_word = 4U,
} ra_sim_dma_bpe_t;

/**
 * @brief Transfer-element size in bytes for a ``ra_dmac_width_t`` value.
 */
static uint8_t internal_size_bytes(ra_dmac_width_t width)
{
  switch (width) {
    case k_ra_dmac_width_byte:
      return (uint8_t)k_ra_sim_dma_bpe_byte;
    case k_ra_dmac_width_half:
      return (uint8_t)k_ra_sim_dma_bpe_half;
    case k_ra_dmac_width_word:
      return (uint8_t)k_ra_sim_dma_bpe_word;
    default:
      return (uint8_t)k_ra_sim_dma_bpe_word;
  }
}

ra_err_t ra_sim_dma_memcpy(uint8_t channel)
{
  const ra_dma_request_t* req = ra_dma_sim_peek_request(channel);
  if (req == nullptr) {
    return k_ra_err_invalid_arg;
  }

  const uint8_t bpe     = internal_size_bytes(req->width);
  uintptr_t     src_ptr = req->src_addr;
  uintptr_t     dst_ptr = req->dst_addr;

  for (uint32_t i = 0U; i < (uint32_t)req->count; ++i) {
    if (bpe == (uint8_t)k_ra_sim_dma_bpe_byte) {
      *(volatile uint8_t*)dst_ptr = *(volatile const uint8_t*)src_ptr;
    } else if (bpe == (uint8_t)k_ra_sim_dma_bpe_half) {
      *(volatile uint16_t*)dst_ptr = *(volatile const uint16_t*)src_ptr;
    } else {
      *(volatile uint32_t*)dst_ptr = *(volatile const uint32_t*)src_ptr;
    }
    if (req->src_inc) {
      src_ptr += bpe;
    }
    if (req->dst_inc) {
      dst_ptr += bpe;
    }
  }

  /* Mirror the real DMAC: zero DMCRA so a second memcpy call
   * is a no-op. */
  volatile r_dmac_channel_regs_t* reg = ra_dmac(channel);
  if (reg != nullptr) {
    reg->DMCRA = 0U;
  }
  return k_ra_ok;
}

ra_err_t ra_sim_dma_complete(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra_dma_channel_count) {
    return k_ra_err_invalid_arg;
  }
  ra_dma_dispatch_complete(channel);
  return k_ra_ok;
}

#else
/* Non-simulator build: this translation unit is empty. */
typedef int ra_sim_dma_placeholder_t;
#endif /* RA_SIMULATOR_MODE */
