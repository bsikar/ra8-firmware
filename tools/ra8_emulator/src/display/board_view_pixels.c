/**
 * @file board_view_pixels.c
 * @brief Bounded raw-fd RGB565 to CoreGraphics byte-range expansion
 * @details Converts arbitrary provider byte ranges from the sealed RGB565
 * surface descriptor into RGB888 using a fixed-size stack chunk, including
 * unaligned range boundaries and checked positional reads.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <string.h>
#include <sys/stat.h>

#include "board_view.h"
#include "emu_host_io_internal.h"
#include "ra8_attributes.h"

/** @brief Bounded conversion chunk and RGB565 channel geometry. */
typedef enum : size_t {
  k_view_pixel_chunk    = 128U,  /**< Fixed source/conversion chunk capacity. */
  k_rgb565_red_shift    = 11U,   /**< Red-field least-significant bit.        */
  k_rgb565_green_shift  = 5U,    /**< Green-field least-significant bit.      */
  k_rgb565_five_bit_max = 0x1FU, /**< Five-bit channel mask.                  */
  k_rgb565_six_bit_max  = 0x3FU, /**< Six-bit channel mask.                   */
} view_pixel_geometry_t;

/**
 * @brief Clamp one provider request to the bytes the surface can still supply.
 * @details Measures the sealed RGB565 descriptor, derives the virtual RGB888
 * stream length from it, and clamps @p count to what remains after @p position.
 * @param[in] fd Sealed RGB565 surface descriptor.
 * @param[in] position Provider byte offset into the virtual RGB888 stream.
 * @param[in] count Bytes the caller is willing to receive.
 * @return size_t Bytes obtainable at @p position, never more than @p count.
 * @retval 0 The descriptor is unusable, or @p position is at or past the end.
 * @pre @p fd is open and @p position is non-negative. @pre @p count is the exact
 * caller buffer capacity.
 * @post The descriptor and its file offset are unchanged. @post The result never
 * exceeds @p count.
 * @note Not thread-safe with respect to @p fd. @since 0.1.0
 */
RA8_INTERNAL static size_t internal_available_bytes(int fd, off_t position, size_t count)
{
  struct stat metadata = {};
  if ((fstat(fd, &metadata) != 0) || (metadata.st_size < 0) ||
      (((uint64_t)metadata.st_size % sizeof(uint16_t)) != 0U)) {
    return 0U;
  }
  const uint64_t pixels      = (uint64_t)metadata.st_size / sizeof(uint16_t);
  const uint64_t output_size = pixels * sizeof(uint32_t);
  if ((uint64_t)position >= output_size) {
    return 0U;
  }
  const size_t available = (size_t)(output_size - (uint64_t)position);
  return (count < available) ? count : available;
}

/**
 * @brief Expand one bounded RGB565 chunk into packed 32-bit RGB888 words.
 * @details Replicates the high channel bits into the low ones so a full-scale
 * RGB565 value expands to a full-scale RGB888 value.
 * @param[in] rgb565 Source chunk holding @p pixel_count RGB565 pixels.
 * @param[out] rgb888 Destination chunk receiving @p pixel_count packed words.
 * @param[in] pixel_count Pixels to convert, at most ::k_view_pixel_chunk.
 * @pre Both chunks span @p pixel_count elements. @pre The chunks do not overlap.
 * @post The first @p pixel_count words of @p rgb888 are initialized. @post
 * @p rgb565 is unchanged.
 * @note Not thread-safe with respect to the caller chunks. @since 0.1.0
 */
RA8_INTERNAL static void
internal_expand_chunk(const uint16_t* rgb565, uint32_t* rgb888, size_t pixel_count)
{
  for (size_t index = 0U; index < pixel_count; index++) {
    const uint32_t pixel  = rgb565[index];
    const uint32_t red5   = (pixel >> k_rgb565_red_shift) & k_rgb565_five_bit_max;
    const uint32_t green6 = (pixel >> k_rgb565_green_shift) & k_rgb565_six_bit_max;
    const uint32_t blue5  = pixel & k_rgb565_five_bit_max;
    const uint32_t red8   = (red5 << 3U) | (red5 >> 2U);
    const uint32_t green8 = (green6 << 2U) | (green6 >> 4U);
    const uint32_t blue8  = (blue5 << 3U) | (blue5 >> 2U);
    rgb888[index]         = (red8 << 16U) | (green8 << 8U) | blue8;
  }
}

size_t board_view_read_rgb888_fd(int fd, off_t position, void* buffer, size_t count)
{
  if ((fd < 0) || (position < 0) || (buffer == nullptr) || (count == 0U)) {
    return 0U;
  }
  const size_t available   = internal_available_bytes(fd, position, count);
  uint8_t*     destination = (uint8_t*)buffer;
  size_t       copied      = 0U;
  while (copied < available) {
    const uint64_t stream_offset = (uint64_t)position + copied;
    const uint64_t first_pixel   = stream_offset / sizeof(uint32_t);
    const size_t   byte_skip     = (size_t)(stream_offset % sizeof(uint32_t));
    const size_t   wanted        = available - copied;
    size_t         pixel_count   = (byte_skip + wanted + sizeof(uint32_t) - 1U) / sizeof(uint32_t);
    if (pixel_count > k_view_pixel_chunk) {
      pixel_count = k_view_pixel_chunk;
    }
    uint16_t rgb565[k_view_pixel_chunk];
    uint32_t rgb888[k_view_pixel_chunk];
    if (priv_emu_io_pread_exact(fd,
                                rgb565,
                                pixel_count * sizeof(uint16_t),
                                (off_t)(first_pixel * sizeof(uint16_t)))
          .status != k_emu_io_ok) {
      return 0U;
    }
    internal_expand_chunk(rgb565, rgb888, pixel_count);
    const size_t generated = (pixel_count * sizeof(uint32_t)) - byte_skip;
    const size_t take      = ((available - copied) < generated) ? (available - copied) : generated;
    (void)memcpy(&destination[copied], &((const uint8_t*)rgb888)[byte_skip], take);
    copied += take;
  }
  return copied;
}
