/**
 * @file ra8_io_stream_internal.h
 * @brief Compatibility include for the public stream-backend contract.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Retained temporarily for source compatibility with in-tree code that may have
 * included the former private implementer contract. New backends must include
 * `ra8_io_stream_backend.h`; reusable libraries must not reach into another
 * library's `src/` directory. This header declares no separate ABI.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_io_stream_backend.h"
