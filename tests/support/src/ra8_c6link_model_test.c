/**
 * @file ra8_c6link_model_test.c
 * @brief Shared implementation of the bounded C6 link model-test fixture.
 *
 * @details Owns deterministic mock transport state and shared assertions used by
 * the split generic and media-download C6 link test translation units.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>

#include "ra8_c6_model.h"
#include "ra8_c6link_model_test_internal.h"
#include "unity_minimal.h"

/** @enum internal_fixture_const_t @brief Fixed storage bounds for the fixture. */
typedef enum : uint32_t {
  k_internal_arena_bytes = 4096U, /**< Decode arena handed to the link. */
  k_internal_event_slots = 8U,    /**< Maximum captured announcements.  */
} internal_fixture_const_t;

/** @brief Decode arena handed to the link under test. */
static uint8_t s_arena[(size_t)k_internal_arena_bytes];

/** @brief Link under test. */
static ra8_c6link_t s_link;

/** @brief Bounded announcement log, oldest first. */
static ra8_c6link_event_t s_events[k_internal_event_slots];

/** @brief Number of valid entries in ::s_events. */
static uint8_t s_event_count;

/** @brief Length of the last received 802.3 frame. */
static uint16_t s_rx_len;

/** @brief Record one announcement in the bounded fixture log. */
RA8_INTERNAL
static void internal_on_event(void* context, const ra8_c6link_event_t* event)
{
  (void)context;
  if (s_event_count < (uint8_t)k_internal_event_slots) {
    s_events[s_event_count] = *event;
    s_event_count++;
  }
}

/** @brief Record the length of one received 802.3 frame. */
RA8_INTERNAL
static void internal_on_rx(void* context, const uint8_t* frame, uint16_t length)
{
  (void)context;
  TEST_ASSERT_NOT_NULL(frame);
  s_rx_len = length;
}

RA8_PRIV void priv_c6link_test_cfg(ra8_c6link_cfg_t* config)
{
  *config = (ra8_c6link_cfg_t){};
  ra8_c6_model_bind(&config->transport);
  config->arena       = s_arena;
  config->arena_bytes = (uint32_t)sizeof(s_arena);
  config->event_cb    = internal_on_event;
  config->rx_cb       = internal_on_rx;
}

RA8_PRIV void priv_c6link_test_reset(void)
{
  ra8_c6_model_reset();
  s_event_count = 0U;
  s_rx_len      = 0U;
  s_link        = (ra8_c6link_t){};

  ra8_c6link_cfg_t config = {};
  priv_c6link_test_cfg(&config);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_c6link_open(&s_link, &config));
}

RA8_PRIV void priv_c6link_test_bringup(void)
{
  priv_c6link_test_reset();
  ra8_c6link_fw_version_t version = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_c6link_await_ready(&s_link, (uint16_t)k_ra8_c6link_announce_transfers, &version));
  TEST_ASSERT(ra8_c6_model()->caps_seen);
  TEST_ASSERT_EQ(k_c6m_chip_id, version.chip_id);
  s_event_count          = 0U;
  ra8_c6_model()->seen_n = 0U;
}

RA8_PRIV ra8_c6link_t* priv_c6link_test_link(void)
{
  return &s_link;
}

RA8_PRIV uint8_t priv_c6link_test_event_count(void)
{
  return s_event_count;
}

RA8_PRIV const ra8_c6link_event_t* priv_c6link_test_event(uint8_t index)
{
  return (index < (uint8_t)k_internal_event_slots) ? &s_events[index] : nullptr;
}

RA8_PRIV uint16_t priv_c6link_test_rx_len(void)
{
  return s_rx_len;
}
