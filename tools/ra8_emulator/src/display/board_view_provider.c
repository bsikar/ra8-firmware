/**
 * @file board_view_provider.c
 * @brief Immutable raw-fd CoreGraphics direct provider implementation
 * @details Implements descriptor encoding, bounded positional RGB565 reads,
 * CoreGraphics release ownership, and a non-Apple contract stub for the
 * immutable presentation-provider boundary.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <unistd.h>

#include "board_view.h"
#include "board_view_provider_internal.h"

#if defined(__APPLE__)

/**
 * @brief Decode the descriptor encoded in one provider info value.
 * @details Decode the descriptor encoded in one provider info value; this step is contained within the board view provider model and uses bounded caller or module-owned storage.
 * @param[in,out] info Opaque provider context supplied by the caller.
 * @return The provider descriptor result produced by the board view provider model.
 * @retval value The operation-specific provider descriptor value.
 * @pre Arguments satisfy the ranges documented for provider descriptor. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view provider model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_provider_fd(void* info)
{
  return (int)((intptr_t)info - 1);
}

/**
 * @brief Expand one arbitrary sealed RGB565 byte range for CoreGraphics.
 * @details Expand one arbitrary sealed rgb565 byte range for coregraphics; this step is contained within the board view provider model and uses bounded caller or module-owned storage.
 * @param[in,out] info Opaque provider context supplied by the caller.
 * @param[in,out] buffer Buffer read or updated by the operation.
 * @param[in] position Byte position at which processing begins.
 * @param[in] count Number of elements or bytes to process.
 * @return The provider read result produced by the board view provider model.
 * @retval value The operation-specific provider read value.
 * @pre Arguments satisfy the ranges documented for provider read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view provider model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_provider_read(void* info, void* buffer, off_t position, size_t count)
{
  return board_view_read_rgb888_fd(internal_provider_fd(info), position, buffer, count);
}

/**
 * @brief Close the immutable snapshot once CoreGraphics releases its provider.
 * @details Close the immutable snapshot once coregraphics releases its provider; this step is contained within the board view provider model and uses bounded caller or module-owned storage.
 * @param[in,out] info Opaque provider context supplied by the caller.
 * @pre Arguments satisfy the ranges documented for provider release. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board view provider model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_provider_release(void* info)
{
  (void)close(internal_provider_fd(info));
}

CGDataProviderRef priv_board_view_provider_create(int snapshot_fd, size_t provider_bytes)
{
  if ((snapshot_fd < 0) || (provider_bytes == 0U)) {
    if (snapshot_fd >= 0) {
      (void)close(snapshot_fd);
    }
    return nullptr;
  }
  const CGDataProviderDirectCallbacks callbacks = {
    .version            = 0U,
    .getBytePointer     = nullptr,
    .releaseBytePointer = nullptr,
    .getBytesAtPosition = internal_provider_read,
    .releaseInfo        = internal_provider_release,
  };
  CGDataProviderRef provider =
    CGDataProviderCreateDirect((void*)(intptr_t)(snapshot_fd + 1), provider_bytes, &callbacks);
  if (provider == nullptr) {
    (void)close(snapshot_fd);
  }
  return provider;
}
#else
CGDataProviderRef priv_board_view_provider_create(int snapshot_fd, size_t provider_bytes)
{
  (void)provider_bytes;
  if (snapshot_fd >= 0) {
    (void)close(snapshot_fd);
  }
  return nullptr;
}
#endif
