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

/** @brief Mutable bounds for one private in-memory spool. */
typedef struct {
  uint8_t* bytes;    /**< Writable backing bytes.            */
  uint32_t capacity; /**< Maximum accessible byte extent.    */
  uint32_t length;   /**< Highest initialized byte plus one. */
} media_memory_t;

/**
 * @brief Write one exact span at an absolute memory offset.
 * @details Proves the complete range before copying and extends the logical
 *          length only after every requested byte is initialized.
 * @param[in,out] memory Private destination spool.
 * @param[in] offset Absolute destination byte offset.
 * @param[in] source Complete source span.
 * @param[in] requested Exact byte count.
 * @param[out] written Accepted byte count.
 * @return Memory-write status.
 * @retval k_ra8_ok The complete span was copied.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_size The range exceeds destination capacity.
 * @pre Non-NULL pointers span their declared extents.
 * @pre Source and destination ranges do not overlap.
 * @post Success reports @p requested and extends length monotonically.
 * @post Failure reports zero and preserves logical length.
 * @note Pure apart from caller-owned destination memory.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_write_at(media_memory_t* memory,
                                                uint64_t        offset,
                                                const uint8_t*  source,
                                                uint32_t        requested,
                                                uint32_t*       written)
{
  if ((memory == nullptr) || (source == nullptr) || (written == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *written = 0U;
  if ((offset > memory->capacity) || (requested > ((uint64_t)memory->capacity - offset))) {
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

/**
 * @brief Read one exact initialized memory range.
 * @details Rejects reads beyond logical length before copying the complete
 *          requested range to a caller destination.
 * @param[in] context Bound ::media_memory_t source.
 * @param[in] offset Absolute source offset.
 * @param[out] destination Writable destination span.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_read Copied byte count.
 * @return Memory-read status.
 * @retval k_ra8_ok The complete requested span was copied.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_size The range exceeds initialized bytes.
 * @pre Context and destination remain live for the call.
 * @pre Destination does not overlap the selected source range.
 * @post Success reports @p requested and initializes every destination byte.
 * @post Failure reports zero and leaves source state unchanged.
 * @note Satisfies both image-spool and flat-source reader callbacks.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_read(void*     context,
                                            uint32_t  offset,
                                            uint8_t*  destination,
                                            uint32_t  requested,
                                            uint32_t* out_read)
{
  if ((context == nullptr) || (destination == nullptr) || (out_read == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out_read                    = 0U;
  const media_memory_t* memory = context;
  if ((offset > memory->length) || (requested > (memory->length - offset))) {
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
 * @return ::internal_write_at status.
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
  return internal_write_at(context, offset, source, requested, out_written);
}

/**
 * @brief Append one flat RABOOK1 fragment to bounded memory.
 * @details Selects the current logical end as the destination offset before
 *          delegating the exact append to the common memory writer.
 * @param[in,out] context Bound flat spool.
 * @param[in] source Ordered flat-book bytes.
 * @param[in] requested Exact requested byte count.
 * @param[out] out_written Accepted byte count.
 * @return ::internal_write_at status.
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
  media_memory_t* memory = context;
  return internal_write_at(memory, memory->length, source, requested, out_written);
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
 * @return ::internal_write_at status.
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
  return internal_write_at(context, offset, source, requested, out_written);
}

/* See the internal header for the documented contract. */
RA8_PRIV ra8_err_t priv_media_download_format_rabook(const uint8_t* source,
                                                     size_t         source_size,
                                                     const media_download_format_config_t* config,
                                                     media_download_format_workspace_t* workspace,
                                                     uint64_t*                          packed_size)
{
  if ((source == nullptr) || (config == nullptr) || (workspace == nullptr) ||
      (packed_size == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *packed_size = 0U;
  if ((workspace->image == nullptr) || (workspace->flat == nullptr) ||
      (workspace->packed == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((source_size == 0U) || (workspace->image_cap == 0U) || (workspace->flat_cap == 0U) ||
      (workspace->packed_cap == 0U)) {
    return k_ra8_err_invalid_size;
  }
  media_memory_t     image  = {.bytes = workspace->image, .capacity = workspace->image_cap};
  media_memory_t     flat   = {.bytes = workspace->flat, .capacity = workspace->flat_cap};
  media_memory_t     packed = {.bytes = workspace->packed, .capacity = workspace->packed_cap};
  ra8_rabook_comic_t comic  = {};
  ra8_err_t          result = ra8_rabook_comic_init(&comic,
                                                    &workspace->comic,
                                                    internal_read,
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
  if ((result == k_ra8_ok) && (flat_size != flat.length)) {
    result = k_ra8_err_validation_failed;
  }
  if (result == k_ra8_ok) {
    result = ra8_rabook_container_write(internal_read,
                                        &flat,
                                        flat.length,
                                        config->chunk_bytes,
                                        internal_packed_write,
                                        &packed,
                                        &workspace->container,
                                        packed_size);
  }
  if ((result == k_ra8_ok) && (*packed_size != packed.length)) {
    *packed_size = 0U;
    result       = k_ra8_err_validation_failed;
  }
  return result;
}
