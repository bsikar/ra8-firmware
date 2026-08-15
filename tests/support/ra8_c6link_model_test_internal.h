/**
 * @file ra8_c6link_model_test_internal.h
 * @brief Shared, bounded fixture seam for C6 link model tests.
 *
 * @details Declares caller-owned reset, scripted-reply, and observation helpers
 * so split tests reuse one exact transport model without hidden allocation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link.h"

/** @brief Fill a configuration accepted by ::ra8_c6link_open. */
RA8_PRIV void priv_c6link_test_cfg(ra8_c6link_cfg_t* config);

/** @brief Reset the model and open the shared bounded test link. */
RA8_PRIV void priv_c6link_test_reset(void);

/** @brief Open and complete the mandatory modelled host announcement. */
RA8_PRIV void priv_c6link_test_bringup(void);

/** @brief Borrow the link owned by the current test executable. */
RA8_PRIV ra8_c6link_t* priv_c6link_test_link(void);

/** @brief Return the number of captured announcements. */
RA8_PRIV uint8_t priv_c6link_test_event_count(void);

/** @brief Borrow one slot in the bounded announcement log. */
RA8_PRIV const ra8_c6link_event_t* priv_c6link_test_event(uint8_t index);

/** @brief Return the length of the last received 802.3 frame. */
RA8_PRIV uint16_t priv_c6link_test_rx_len(void);
