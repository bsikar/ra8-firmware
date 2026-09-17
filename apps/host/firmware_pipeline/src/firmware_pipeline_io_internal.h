// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

/**
 * @file firmware_pipeline_io_internal.h
 * @brief Injectable C-owned file and allocation lifecycle.
 * @details Separates deterministic resource-state tests from hosted raw-descriptor adapters.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum firmware_pipeline_io_status_t
 * @brief Stable result for bounded image acquisition.
 * @details Values are process-compatible and independent of platform errno values.
 * @invariant Every operation returns one declared value.
 * @code
 * firmware_pipeline_io_status_t status = k_firmware_pipeline_io_ok;
 * @endcode
 * @see priv_firmware_pipeline_read_image
 */
typedef enum : uint8_t {
  k_firmware_pipeline_io_ok     = 0, /**< Image acquisition completed. */
  k_firmware_pipeline_io_failed = 2, /**< A resource operation failed. */
} firmware_pipeline_io_status_t;

/**
 * @struct firmware_pipeline_image_t
 * @brief C-owned bounded firmware image.
 * @details A null pointer with zero size represents a successfully read empty file.
 * @invariant `bytes` is null exactly when no allocation is owned.
 * @code
 * firmware_pipeline_image_t image = {};
 * @endcode
 * @see priv_firmware_pipeline_release_image
 */
typedef struct {
  uint8_t* bytes; /**< Owned heap bytes, or null. */
  size_t   size;  /**< Complete input length.     */
} firmware_pipeline_image_t;

/**
 * @struct firmware_pipeline_io_ops_t
 * @brief Operations required by the acquisition state machine.
 * @details Every callback receives caller-owned context; file handles remain provider-owned.
 * @invariant Every callback is non-null for a call to `priv_firmware_pipeline_read_image`.
 * @code
 * firmware_pipeline_io_ops_t ops = {};
 * @endcode
 * @see priv_firmware_pipeline_host_io
 */
typedef struct {
  void* context; /**< Opaque callback context, possibly null. */
  /**
   * Open path for binary input.
   * @param[in] context Opaque provider context.
   * @param[in] path Non-empty terminated path.
   * @return Owned file handle or null on failure.
   * @post Success transfers one handle to the state machine.
   */
  void* (*open)(void* context, const char* path);
  /**
   * Reposition input.
   * @param[in] context Opaque provider context.
   * @param[in] file Owned open handle.
   * @param[in] offset Requested offset.
   * @param[in] origin Hosted seek origin.
   * @return Zero on success; nonzero on failure.
   */
  int (*seek)(void* context, void* file, long offset, int origin);
  /**
   * Report current byte offset.
   * @param[in] context Opaque provider context.
   * @param[in] file Owned open handle.
   * @return Non-negative offset or negative failure value.
   */
  long (*tell)(void* context, void* file);
  /**
   * Allocate exactly `size` bytes.
   * @param[in] context Opaque provider context.
   * @param[in] size Nonzero requested size.
   * @return Owned allocation or null on failure.
   */
  void* (*allocate)(void* context, size_t size);
  /**
   * Read bytes.
   * @param[in] context Opaque provider context.
   * @param[out] bytes Writable allocation.
   * @param[in] size Exact requested size.
   * @param[in] file Owned open handle.
   * @return Number of bytes initialized.
   */
  size_t (*read)(void* context, void* bytes, size_t size, void* file);
  /**
   * Close one acquired handle.
   * @param[in] context Opaque provider context.
   * @param[in] file Owned open handle.
   * @return Zero on success; nonzero on failure.
   * @post Handle ownership ends regardless of result.
   */
  int (*close)(void* context, void* file);
  /**
   * Release one allocation.
   * @param[in] context Opaque provider context.
   * @param[in] bytes Owned allocation.
   * @post Allocation ownership ends exactly once.
   */
  void (*release)(void* context, void* bytes);
} firmware_pipeline_io_ops_t;

/**
 * @brief Return the immutable hosted descriptor/allocator adapter.
 * @details The returned table delegates to POSIX open/lseek/read/close plus malloc/free.
 * @return Non-null process-lifetime operation table.
 * @pre The hosted C runtime is initialized.
 * @post No resource is acquired.
 * @note Thread-safe; the returned object is immutable.
 * @since 0.1.0
 */
RA8_PRIV const firmware_pipeline_io_ops_t* priv_firmware_pipeline_host_io(void);

/**
 * @brief Read one bounded file through an injectable resource provider.
 * @details Publishes only after a complete read and successful close.
 * @param[in] ops Complete resource operation table.
 * @param[in] path Non-empty terminated path.
 * @param[in,out] out_image Writable empty destination.
 * @return Stable acquisition status.
 * @retval k_firmware_pipeline_io_ok Complete image published.
 * @retval k_firmware_pipeline_io_failed Validation or one resource transition failed.
 * @pre `out_image` is writable and owns no allocation.
 * @pre Callback behavior follows the documented raw-descriptor equivalents.
 * @post Success publishes at most one allocation.
 * @post Failure closes and releases each acquired resource exactly once and preserves output.
 * @note Thread-safe when the supplied provider is thread-safe.
 * @since 0.1.0
 */
RA8_PRIV firmware_pipeline_io_status_t
priv_firmware_pipeline_read_image(const firmware_pipeline_io_ops_t* ops,
                                  const char*                       path,
                                  firmware_pipeline_image_t*        out_image);

/**
 * @brief Release one successfully acquired image.
 * @details Delegates allocation release to the same provider and clears the representation.
 * @param[in] ops Provider that created the allocation.
 * @param[in,out] image Image to release and clear.
 * @pre `ops` is non-null and supplied the original allocation.
 * @pre `image` is non-null and describes a completed acquisition.
 * @post Any allocation is released exactly once.
 * @post The image representation becomes empty.
 * @note Thread-safe when the supplied provider is thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void priv_firmware_pipeline_release_image(const firmware_pipeline_io_ops_t* ops,
                                                   firmware_pipeline_image_t*        image);
