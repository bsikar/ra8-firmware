/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_fake_dma.c
 * @brief Host-test DMA transfer fake implementation
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 */

#ifdef RA8_OFF_TARGET

#include "ra8_fake_dma.h"

#include <stdint.h>

#include "ra8_dma.h"
#include "ra8_dmac_regs.h"
#include "ra8_err.h"

/**
 * @enum ra8_fake_dma_bpe_t
 * @brief Bytes-per-element lookup indexed by ``ra8_dmac_width_t``.
 */
typedef enum : uint8_t {
  k_ra8_fake_dma_bpe_byte = 1U, /**< RA8 fake DMA bpe byte. */
  k_ra8_fake_dma_bpe_half = 2U, /**< RA8 fake DMA bpe half. */
  k_ra8_fake_dma_bpe_word = 4U, /**< RA8 fake DMA bpe word. */
} ra8_fake_dma_bpe_t;

/**
 * @brief Transfer-element size in bytes for a ``ra8_dmac_width_t`` value.
 */
static uint8_t internal_size_bytes(ra8_dmac_width_t width)
{
  switch (width) {
    case k_ra8_dmac_width_byte:
      return (uint8_t)k_ra8_fake_dma_bpe_byte;
    case k_ra8_dmac_width_half:
      return (uint8_t)k_ra8_fake_dma_bpe_half;
    case k_ra8_dmac_width_word:
    default:
      return (uint8_t)k_ra8_fake_dma_bpe_word;
  }
}

ra8_err_t ra8_fake_dma_memcpy(uint8_t channel)
{
  const ra8_dma_request_t* req = ra8_dma_fake_peek_request(channel);
  if (req == nullptr) {
    return k_ra8_err_invalid_arg;
  }

  const uint8_t bpe     = internal_size_bytes(req->width);
  uintptr_t     src_ptr = req->src_addr;
  uintptr_t     dst_ptr = req->dst_addr;

  for (uint32_t i = 0U; i < (uint32_t)req->count; ++i) {
    if (bpe == (uint8_t)k_ra8_fake_dma_bpe_byte) {
      *(volatile uint8_t*)dst_ptr = *(volatile const uint8_t*)src_ptr;
    } else if (bpe == (uint8_t)k_ra8_fake_dma_bpe_half) {
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
  volatile r_dmac_channel_regs_t* reg = ra8_dmac(channel);
  if (reg != nullptr) {
    reg->DMCRA = 0U;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_fake_dma_complete(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra8_dma_channel_count) {
    return k_ra8_err_invalid_arg;
  }
  ra8_dma_dispatch_complete(channel);
  return k_ra8_ok;
}

#else
/* On-target build: this translation unit is empty. */
typedef int ra8_fake_dma_placeholder_t;
#endif /* RA8_OFF_TARGET */
