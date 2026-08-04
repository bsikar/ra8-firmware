/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ssie_test_util.h
 * @brief Shared fixture for the test_ra8_ssie* suite: test constants,
 *        callback-capture state, the stub dispatch callback, the
 *        per-test prep() reset, and the default controller/I2S config
 *
 * @details Header-only (all definitions `static`) so each split
 * test_ra8_ssie* binary carries its own private copy of the fixture
 * state; the tests/CMakeLists.txt auto-glob stays free of non-test .c
 * files. Split out of test_ra8_ssie.c when the suite was divided into
 * core / io / mcdc binaries.
 */

#pragma once

#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_ssie.h"

/**
 * @enum ra8_ssie_test_const_t
 * @brief Magic numbers used by the tests promoted to a typed enum.
 */
typedef enum : uint16_t {
  k_ra8_ssie_test_ch0      = 0U,      /**< RA8 ssie test ch0.         */
  k_ra8_ssie_test_ch1      = 1U,      /**< RA8 ssie test ch1.         */
  k_ra8_ssie_test_ch_bad   = 2U,      /**< RA8 ssie test channel bad. */
  k_ra8_ssie_test_ch_way   = 250U,    /**< RA8 ssie test channel way. */
  k_ra8_ssie_test_sample_a = 0xCAFEU, /**< RA8 ssie test sample a.    */
  k_ra8_ssie_test_sample_b = 0xBEEFU, /**< RA8 ssie test sample b.    */
  k_ra8_ssie_test_dma_tx   = 3U,      /**< RA8 ssie test DMA TX.      */
  k_ra8_ssie_test_dma_rx   = 4U,      /**< RA8 ssie test DMA RX.      */
  k_ra8_ssie_test_dma_bad  = 100U,    /**< RA8 ssie test DMA bad.     */
} ra8_ssie_test_const_t;

/**
 * @enum ra8_ssie_test_status_t
 * @brief Synthetic SSISR / SSIFSR values used by status tests.
 */
typedef enum : uint32_t {
  k_ra8_ssie_test_ssifsr_demo =
    (0x0AUL << 24U) | (0x05UL << 8U) | 0x00010001UL,       /**< TDC=10, RDC=5, TDE+RDF. */
  k_ra8_ssie_test_ssisr_err = 0x04000000UL | 0x10000000UL, /**< ROIRQ + TOIRQ.          */
} ra8_ssie_test_status_t;

static uint32_t s_ssie_cb_count;
static uint8_t  s_ssie_cb_last_ch;
static uint32_t s_ssie_cb_last_ssisr;
static uint8_t  s_ssie_cb_last_events;

static inline void stub_ssie_cb(void* ctx, uint8_t channel, uint8_t events, uint32_t ssisr)
{
  (void)ctx;
  ++s_ssie_cb_count;
  s_ssie_cb_last_ch     = channel;
  s_ssie_cb_last_events = events;
  s_ssie_cb_last_ssisr  = ssisr;
}

static inline void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
  s_ssie_cb_count       = 0U;
  s_ssie_cb_last_ch     = 0U;
  s_ssie_cb_last_ssisr  = 0U;
  s_ssie_cb_last_events = 0U;
}

static inline ra8_ssie_cfg_t make_controller_i2s_cfg(void)
{
  const ra8_ssie_cfg_t cfg = {
    .role          = k_ra8_ssie_role_controller,
    .format        = k_ra8_ssie_format_i2s,
    .data_word     = k_ra8_ssie_dwl_16,
    .system_word   = k_ra8_ssie_swl_32,
    .bclk_div      = k_ra8_ssie_bclk_div_4,
    .use_gpt_clk   = false,
    .long_frame    = false,
    .bckp_rising   = false,
    .lrckp_low     = false,
    .spdp_high     = false,
    .byte_swap     = false,
    .lr_continue   = false,
    .bck_idle_stop = false,
    .enable_aucke  = false,
    .tx_threshold  = 0U,
    .rx_threshold  = 0U,
  };
  return cfg;
}
