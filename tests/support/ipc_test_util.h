/**
 * @file ipc_test_util.h
 * @brief Shared fixture for the test_ra8_ipc* suite: test constants,
 *        callback-capture state, stub callbacks, ring backing memory,
 *        the per-test prep() reset, and the default channel config
 *
 * @details Header-only (all definitions `static`) so each split
 * test_ra8_ipc* binary carries its own private copy of the fixture
 * state; the tests/CMakeLists.txt auto-glob stays free of non-test .c
 * files. Split out of test_ra8_ipc.c when the suite was divided into
 * core / irq / sem / ring binaries.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "ra8_ipc.h"
#include "ra8_isr.h"
#include "ra8_sim_mmap.h"
#include "ra8_sim_mmio.h"

/**
 * @enum ipc_test_util_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_ipc_ch_unset =
    0xFFU, /**< Poison channel id the callback record starts from, so a callback that never ran is distinguishable from one that reported channel 0. */
} ipc_test_util_uint8_const_t;

typedef enum : uint8_t {
  k_ra8_ipc_test_ch_first = 0U,   /**< RA8 ipc test channel first. */
  k_ra8_ipc_test_ch_one   = 1U,   /**< RA8 ipc test channel one.   */
  k_ra8_ipc_test_ch_mid   = 2U,   /**< RA8 ipc test channel mid.   */
  k_ra8_ipc_test_ch_last  = 3U,   /**< RA8 ipc test channel last.  */
  k_ra8_ipc_test_ch_bad   = 4U,   /**< RA8 ipc test channel bad.   */
  k_ra8_ipc_test_ch_way   = 200U, /**< RA8 ipc test channel way.   */
  k_ra8_ipc_test_unit_bad = 7U,   /**< RA8 ipc test unit bad.      */
  k_ra8_ipc_test_sem_bad  = 16U,  /**< RA8 ipc test sem bad.       */
  k_ra8_ipc_test_ring_cap = 4U,   /**< RA8 ipc test ring cap.      */
  k_ra8_ipc_test_burst    = 6U,   /**< RA8 ipc test burst.         */
} ra8_ipc_test_ch_t;

typedef enum : uint32_t {
  k_ra8_ipc_test_msg_a    = 0xDEADBEEFUL, /**< RA8 ipc test message a. */
  k_ra8_ipc_test_msg_b    = 0x12345678UL, /**< RA8 ipc test message b. */
  k_ra8_ipc_test_msg_c    = 0xCAFEBABEUL, /**< RA8 ipc test message c. */
  k_ra8_ipc_test_attr_ctx = 0xA5A5A5A5UL, /**< RA8 ipc test attr ctx.  */
  k_ra8_ipc_test_irq_ctx  = 0x5A5A5A5AUL, /**< RA8 ipc test IRQ ctx.   */
  k_ra8_ipc_test_nmi_ctx  = 0xC0FFEE00UL, /**< RA8 ipc test nmi ctx.   */
} ra8_ipc_test_const_t;

static uint32_t s_ipc_cb_count;
static uint8_t  s_ipc_cb_last_channel;
static uint32_t s_ipc_cb_last_event_mask;
static uint32_t s_ipc_cb_last_message;
static void*    s_ipc_cb_last_ctx;

static uint32_t s_ipc_irq_cb_count;
static uint8_t  s_ipc_irq_cb_last_channel;
static uint8_t  s_ipc_irq_cb_last_event;
static void*    s_ipc_irq_cb_last_ctx;

static uint32_t s_ipc_nmi_cb_count;
static uint8_t  s_ipc_nmi_cb_last_unit;
static void*    s_ipc_nmi_cb_last_ctx;

/* Backing memory for ring-buffer tests. */
static volatile uint32_t s_ring_slots[k_ra8_ipc_test_ring_cap];
static volatile uint32_t s_ring_head;
static volatile uint32_t s_ring_tail;

/**
 * @brief Stub callback used by attach + dispatch tests.
 */
static inline void stub_ipc_cb(void* ctx, uint8_t channel, uint32_t event_mask, uint32_t message)
{
  ++s_ipc_cb_count;
  s_ipc_cb_last_channel    = channel;
  s_ipc_cb_last_event_mask = event_mask;
  s_ipc_cb_last_message    = message;
  s_ipc_cb_last_ctx        = ctx;
}

static inline void stub_ipc_irq_cb(void* ctx, uint8_t channel, ra8_ipc_irq_event_id_t event_id)
{
  ++s_ipc_irq_cb_count;
  s_ipc_irq_cb_last_channel = channel;
  s_ipc_irq_cb_last_event   = (uint8_t)event_id;
  s_ipc_irq_cb_last_ctx     = ctx;
}

static inline void stub_ipc_nmi_cb(void* ctx, uint8_t unit)
{
  ++s_ipc_nmi_cb_count;
  s_ipc_nmi_cb_last_unit = unit;
  s_ipc_nmi_cb_last_ctx  = ctx;
}

/**
 * @brief Reset the sim mmap, the MMIO seam, and the callback counters.
 */
static inline void prep(void)
{
  ra8_sim_mmap_reset();
  ra8_sim_mmio_reset();
  (void)ra8_isr_init();
  s_ipc_cb_count            = 0U;
  s_ipc_cb_last_channel     = k_ipc_ch_unset;
  s_ipc_cb_last_event_mask  = 0U;
  s_ipc_cb_last_message     = 0U;
  s_ipc_cb_last_ctx         = nullptr;
  s_ipc_irq_cb_count        = 0U;
  s_ipc_irq_cb_last_channel = k_ipc_ch_unset;
  s_ipc_irq_cb_last_event   = k_ipc_ch_unset;
  s_ipc_irq_cb_last_ctx     = nullptr;
  s_ipc_nmi_cb_count        = 0U;
  s_ipc_nmi_cb_last_unit    = k_ipc_ch_unset;
  s_ipc_nmi_cb_last_ctx     = nullptr;
  s_ring_head               = 0U;
  s_ring_tail               = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_ipc_test_ring_cap; ++i) {
    s_ring_slots[i] = 0U;
  }
  /* Detach any callbacks from a previous test. */
  (void)ra8_ipc_attach_handler(nullptr, nullptr);
  (void)ra8_ipc_attach_nmi_handler(nullptr, nullptr);
}

static inline ra8_ipc_config_t make_cfg(uint8_t channel)
{
  const ra8_ipc_config_t cfg = {
    .channel      = channel,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra8_ipc_event_msg_ready | (uint32_t)k_ra8_ipc_event_irq0,
  };
  return cfg;
}
