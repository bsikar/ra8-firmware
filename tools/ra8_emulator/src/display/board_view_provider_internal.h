/**
 * @file board_view_provider_internal.h
 * @brief Internal immutable-fd CoreGraphics provider factory
 * @details Declares the Apple provider seam that transfers ownership of a
 * sealed presentation descriptor and exposes it as lazily expanded RGB888
 * bytes without copying the complete frame into memory.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#else
/** @brief Opaque provider handle for non-Apple parse and contract checking. */
typedef void* CGDataProviderRef;
#endif

#include "ra8_attributes.h"

/**
 * @brief Transfer one sealed RGB565 descriptor into a lazy RGB888 provider.
 * @param[in] snapshot_fd Owned immutable descriptor; ownership always
 * transfers.
 * @param[in] provider_bytes Exact virtual 32-bit output length.
 * @return CoreGraphics provider, or NULL after closing @p snapshot_fd.
 * @post The provider release callback closes @p snapshot_fd exactly once.
 * @note CoreGraphics supplies every destination byte buffer; no client
 * allocation.
 * @since 0.1.0
  * @details Transfer one sealed rgb565 descriptor into a lazy rgb888 provider; this step is contained within the board view provider model and uses bounded caller or module-owned storage.
 * @retval value The operation-specific board view provider create value.
 * @pre Arguments satisfy the ranges documented for board view provider create. @pre The call executes on the emulator's single owning thread.
 * @post Ownership of caller-supplied storage is unchanged.
 */
RA8_PRIV CGDataProviderRef priv_board_view_provider_create(int snapshot_fd, size_t provider_bytes);
