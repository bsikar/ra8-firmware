/**
 * @file media_download_format.c
 * @brief Private source-image to RBKC formatter for the media app.
 * @details Adapts bounded caller memory to the production comic and container
 *          streaming callbacks without owning a filesystem, heap, or static
 *          conversion workspace.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "media_download_format_internal.h"
#include "ra8_attributes.h"

RA8_PRIV ra8_err_t priv_media_download_memory_write_at(media_download_memory_t* memory,
                                                       uint64_t                 offset,
                                                       const uint8_t*           source,
                                                       uint32_t                 requested,
                                                       uint32_t*                written)
{
  if (written == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *written = 0U;
  if (memory == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (source == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (offset > memory->capacity) {
    return k_ra8_err_invalid_size;
  }
  if (requested > ((uint64_t)memory->capacity - offset)) {
    return k_ra8_err_invalid_size;
  }
  if (requested != 0U) {
    (void)memcpy(&memory->bytes[(size_t)offset], source, requested);
  }
  const uint32_t end = (uint32_t)offset + requested;
  if (end > memory->length) {
    memory->length = end;
  }
  *written = requested;
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_media_download_memory_read(void*     context,
                                                   uint32_t  offset,
                                                   uint8_t*  destination,
                                                   uint32_t  requested,
                                                   uint32_t* out_read)
{
  if (out_read == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_read = 0U;
  if (context == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (destination == nullptr) {
    return k_ra8_err_null_ptr;
  }
  const media_download_memory_t* memory = context;
  if (offset > memory->length) {
    return k_ra8_err_invalid_size;
  }
  if (requested > (memory->length - offset)) {
    return k_ra8_err_invalid_size;
  }
  if (requested != 0U) {
    (void)memcpy(destination, &memory->bytes[offset], requested);
  }
  *out_read = requested;
  return k_ra8_ok;
}

/**
 * @brief Adapt the comic image-spool random writer to bounded memory.
 * @details Preserves the callback's uint32 offset while delegating all range
 *          and exact-progress checks to the common memory writer.
 * @param[in,out] context Bound image spool.
 * @param[in] offset Logical image-pool offset.
 * @param[in] source Normalized image bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_written Accepted byte count.
 * @return ::priv_media_download_memory_write_at status.
 * @retval k_ra8_ok The complete image fragment was stored.
 * @pre Callback arguments satisfy ::ra8_rabook_comic_write_at_fn.
 * @pre The image spool is private and writable.
 * @post Success initializes the complete requested image span.
 * @post Failure reports zero progress.
 * @note Thin typed callback adapter.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_image_write(void*          context,
                                                   uint32_t       offset,
                                                   const uint8_t* source,
                                                   uint32_t       requested,
                                                   uint32_t*      out_written)
{
  return priv_media_download_memory_write_at(context, offset, source, requested, out_written);
}

/**
 * @brief Append one flat RABOOK1 fragment to bounded memory.
 * @details Selects the current logical end as the destination offset before
 *          delegating the exact append to the common memory writer.
 * @param[in,out] context Bound flat spool.
 * @param[in] source Ordered flat-book bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_written Accepted byte count.
 * @return ::priv_media_download_memory_write_at status.
 * @retval k_ra8_ok The complete flat fragment was appended.
 * @pre Callback arguments satisfy ::ra8_rabook_write_fn.
 * @pre The flat spool is private and writable.
 * @post Success extends the flat spool by exactly @p requested.
 * @post Failure reports zero progress.
 * @note The current logical length selects the append offset.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_flat_append(void*          context,
                                                   const uint8_t* source,
                                                   uint32_t       requested,
                                                   uint32_t*      out_written)
{
  if (context == nullptr) {
    return k_ra8_err_null_ptr;
  }
  media_download_memory_t* memory = context;
  return priv_media_download_memory_write_at(memory,
                                             memory->length,
                                             source,
                                             requested,
                                             out_written);
}

/**
 * @brief Adapt the RBKC random writer to bounded packed memory.
 * @details Preserves the writer's uint64 backfill offset while delegating the
 *          capacity and exact-progress checks to the common memory writer.
 * @param[in,out] context Bound packed destination.
 * @param[in] offset Absolute RBKC byte offset.
 * @param[in] source Container bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_written Accepted byte count.
 * @return ::priv_media_download_memory_write_at status.
 * @retval k_ra8_ok The complete container fragment was stored.
 * @pre Callback arguments satisfy ::ra8_rabook_write_at_fn.
 * @pre The packed destination is private and writable.
 * @post Success initializes the complete requested range.
 * @post Failure reports zero progress.
 * @note Supports the writer's final offset-table backfill.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_packed_write(void*          context,
                                                    uint64_t       offset,
                                                    const uint8_t* source,
                                                    uint32_t       requested,
                                                    uint32_t*      out_written)
{
  return priv_media_download_memory_write_at(context, offset, source, requested, out_written);
}

/**
 * @brief Validate one source-image formatting request.
 * @details Clears the caller's result before checking every required input,
 *          workspace span, and workspace capacity in the public contract order.
 * @param[in] source Complete encoded source image.
 * @param[in] source_size Exact encoded image extent in bytes.
 * @param[in] config Immutable conversion policy.
 * @param[in] workspace Caller-owned fixed conversion workspace.
 * @param[out] packed_size Produced RBKC byte count, cleared before validation.
 * @return Request-validation status.
 * @retval k_ra8_ok Every required pointer and extent is valid.
 * @retval k_ra8_err_null_ptr A required pointer or workspace span is absent.
 * @retval k_ra8_err_invalid_size A required input or workspace extent is zero.
 * @pre Pointer arguments may be null only for deliberate contract validation.
 * @pre Non-null workspace fields describe caller-owned writable storage.
 * @post Success guarantees the formatting pipeline can bind all fixed spans.
 * @post Failure leaves @p packed_size at zero when that pointer is non-null.
 * @note Validation performs no conversion and writes no workspace bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_validate_format_request(const uint8_t*                           source,
                                 size_t                                   source_size,
                                 const media_download_format_config_t*    config,
                                 const media_download_format_workspace_t* workspace,
                                 uint64_t*                                packed_size)
{
  if (packed_size == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *packed_size = 0U;
  if (source == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (config == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (workspace == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (workspace->image == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (workspace->flat == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (workspace->packed == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (source_size == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (workspace->image_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (workspace->flat_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (workspace->packed_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_media_download_format_rabook(const uint8_t* source,
                                                     size_t         source_size,
                                                     const media_download_format_config_t* config,
                                                     media_download_format_workspace_t* workspace,
                                                     uint64_t*                          packed_size)
{
  ra8_err_t result =
    internal_validate_format_request(source, source_size, config, workspace, packed_size);
  if (result != k_ra8_ok) {
    return result;
  }
  media_download_memory_t image  = {.bytes = workspace->image, .capacity = workspace->image_cap};
  media_download_memory_t flat   = {.bytes = workspace->flat, .capacity = workspace->flat_cap};
  media_download_memory_t packed = {.bytes = workspace->packed, .capacity = workspace->packed_cap};
  ra8_rabook_comic_t      comic  = {};
  result                         = ra8_rabook_comic_init(&comic,
                                                         &workspace->comic,
                                                         priv_media_download_memory_read,
                                                         internal_image_write,
                                                         &image,
                                                         config->max_image_edge,
                                                         config->pixel_format);
  if (result == k_ra8_ok) {
    result = ra8_rabook_comic_add_page(&comic, config->page_id, source, source_size);
  }
  uint32_t flat_size = 0U;
  if (result == k_ra8_ok) {
    result =
      ra8_rabook_comic_finish(&comic, &config->metadata, internal_flat_append, &flat, &flat_size);
  }
  if (result == k_ra8_ok) {
    if (flat_size != flat.length) {
      result = k_ra8_err_validation_failed;
    }
  }
  if (result == k_ra8_ok) {
    result = ra8_rabook_container_write(priv_media_download_memory_read,
                                        &flat,
                                        flat.length,
                                        config->chunk_bytes,
                                        internal_packed_write,
                                        &packed,
                                        &workspace->container,
                                        packed_size);
  }
  if (result == k_ra8_ok) {
    if (*packed_size != packed.length) {
      *packed_size = 0U;
      result       = k_ra8_err_validation_failed;
    }
  }
  return result;
}
