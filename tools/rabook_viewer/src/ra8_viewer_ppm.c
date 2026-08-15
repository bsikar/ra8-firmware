/**
 * @file ra8_viewer_ppm.c
 * @brief RGB565 <-> 8-bit RGB conversion and the shared binary-PPM (P6) writer.
 *
 * @details
 * Both headless proofs -- the fixed-framebuffer dump (::ra8_viewer_dump_ppm) and
 * the scroll-tile dump in the CLI -- expand the same RGB565 pixels to 8-bit RGB
 * and write the same P6 file. That conversion lives here once so the two paths
 * cannot drift, and so the field shifts / masks are named constants in one place
 * rather than literals repeated at each call site.
 *
 * The 5->8 and 6->8 bit expansions replicate the high bits into the low bits
 * (`(v << 3) | (v >> 2)`), which maps 0x1F to 0xFF exactly -- a plain shift would
 * cap white at 0xF8 and tint every dump.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include "ra8_err.h"
#include "ra8_viewer_reader.h"
#include "ra8_viewer_reader_internal.h"

#ifndef O_CLOEXEC
/** @brief No-op close-on-exec fallback for hosts lacking the flag. */
#define O_CLOEXEC (0)
#endif

/**
 * @enum ra8_viewer_rgb565_shift_t
 * @brief Field positions and channel-width conversions for RGB565 packing.
 * @details The `_drop` values narrow 8-bit channels on the way in; the `_fill`
 *          values position the replicated high bits on the way back out.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_rgb565_r_shift  = 11U, /**< Red field position in the 16-bit word.       */
  k_rgb565_g_shift  = 5U,  /**< Green field position in the 16-bit word.     */
  k_rgb565_r5_drop  = 3U,  /**< 8->5-bit reduction for red/blue.             */
  k_rgb565_g6_drop  = 2U,  /**< 8->6-bit reduction for green.                */
  k_rgb565_r5_fill  = 2U,  /**< 5->8 low-bit replication shift (red/blue).   */
  k_rgb565_g6_fill  = 4U,  /**< 6->8 low-bit replication shift (green).      */
  k_rgb565_hi_shift = 8U,  /**< High-byte position of a little-endian pixel. */
} ra8_viewer_rgb565_shift_t;

/**
 * @enum ra8_viewer_rgb565_mask_t
 * @brief Channel masks used to unpack an RGB565 word.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_rgb565_r5_mask = 0x1FU, /**< 5-bit red field mask.   */
  k_rgb565_g6_mask = 0x3FU, /**< 6-bit green field mask. */
  k_rgb565_b5_mask = 0x1FU, /**< 5-bit blue field mask.  */
} ra8_viewer_rgb565_mask_t;

/**
 * @enum ra8_viewer_ppm_t
 * @brief Layout constants of the binary portable-pixmap (P6) output.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ppm_channels = 3U, /**< Bytes per P6 pixel (R, G, B).       */
  k_ppm_idx_r    = 0U, /**< Red byte index within a P6 pixel.   */
  k_ppm_idx_g    = 1U, /**< Green byte index within a P6 pixel. */
  k_ppm_idx_b    = 2U, /**< Blue byte index within a P6 pixel.  */
} ra8_viewer_ppm_t;

/** @brief Bounded buffers used by the descriptor-backed PPM publisher. */
typedef enum : uint16_t {
  k_ppm_u32_digits    = 10U,   /**< Decimal digits in UINT32_MAX.         */
  k_ppm_decimal_radix = 10U,   /**< Decimal numeric base.                 */
  k_ppm_header_bytes  = 32U,   /**< Complete P6 header capacity.          */
  k_ppm_chunk_pixels  = 1024U, /**< Pixels converted before each write.   */
  k_ppm_file_mode     = 0666,  /**< Host output mode before process mask. */
  /** @brief Conversion buffer capacity in bytes. */
  k_ppm_chunk_bytes = k_ppm_chunk_pixels * k_ppm_channels,
} ra8_viewer_ppm_io_t;

uint16_t ra8_viewer_pack565(uint8_t rr, uint8_t gg, uint8_t bb)
{
  const uint16_t r5 = (uint16_t)(rr >> (uint16_t)k_rgb565_r5_drop);
  const uint16_t g6 = (uint16_t)(gg >> (uint16_t)k_rgb565_g6_drop);
  const uint16_t b5 = (uint16_t)(bb >> (uint16_t)k_rgb565_r5_drop);
  return (uint16_t)((r5 << (uint16_t)k_rgb565_r_shift) | (g6 << (uint16_t)k_rgb565_g_shift) | b5);
}

uint16_t ra8_viewer_pack565_le_pair(uint8_t lo, uint8_t hi)
{
  return (uint16_t)((uint16_t)lo | ((uint16_t)hi << (uint16_t)k_rgb565_hi_shift));
}

/**
 * @brief Expand one RGB565 word into three 8-bit channels.
 * @details High-bit replication, so a saturated field maps to 0xFF exactly.
 * @param[in]  px  Packed RGB565 pixel.
 * @param[out] rgb Receives {R, G, B} (non-NULL, ::k_ppm_channels bytes).
 * @pre @p rgb has room for ::k_ppm_channels bytes.
 * @pre @p px is a native-endian RGB565 word.
 * @post @p rgb holds the three expanded 8-bit channels.
 * @post No state other than @p rgb is mutated.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_unpack565(uint16_t px, uint8_t rgb[k_ppm_channels])
{
  const uint32_t r5 = ((uint32_t)px >> (uint32_t)k_rgb565_r_shift) & (uint32_t)k_rgb565_r5_mask;
  const uint32_t g6 = ((uint32_t)px >> (uint32_t)k_rgb565_g_shift) & (uint32_t)k_rgb565_g6_mask;
  const uint32_t b5 = (uint32_t)px & (uint32_t)k_rgb565_b5_mask;

  rgb[k_ppm_idx_r] =
    (uint8_t)((r5 << (uint32_t)k_rgb565_r5_drop) | (r5 >> (uint32_t)k_rgb565_r5_fill));
  rgb[k_ppm_idx_g] =
    (uint8_t)((g6 << (uint32_t)k_rgb565_g6_drop) | (g6 >> (uint32_t)k_rgb565_g6_fill));
  rgb[k_ppm_idx_b] =
    (uint8_t)((b5 << (uint32_t)k_rgb565_r5_drop) | (b5 >> (uint32_t)k_rgb565_r5_fill));
}

/**
 * @brief Write exactly @p length bytes to one owned host descriptor.
 * @details Advances across short writes and retries `EINTR`; every other error
 *          fails closed so a partial image is never reported as successful.
 * @param[in] fd     Open write-only host descriptor.
 * @param[in] bytes  Source bytes (non-NULL when @p length is non-zero).
 * @param[in] length Number of bytes to publish.
 * @return true only when all requested bytes were written.
 * @retval true  All @p length bytes were written.
 * @retval false A non-retryable error or zero-length write stopped progress.
 * @pre @p fd is owned by the caller and open for writing.
 * @pre @p bytes spans at least @p length readable bytes.
 * @post On true the descriptor advanced by exactly @p length bytes.
 * @post On false the descriptor may contain a bounded prefix.
 * @note Not thread-safe when callers share @p fd.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_write_all(int fd, const uint8_t* bytes, size_t length)
{
  size_t offset = 0U;
  while (offset < length) {
    const ssize_t written = write(fd, &bytes[offset], length - offset);
    if ((written < 0) && (errno == EINTR)) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    offset += (size_t)written;
  }
  return true;
}

/**
 * @brief Append one unsigned decimal value to a fixed PPM header.
 * @details Builds digits in reverse order and copies them back without locale,
 *          formatting streams, or dynamic allocation.
 * @param[out] out    Header storage with ::k_ppm_header_bytes capacity.
 * @param[in]  offset First unwritten header byte.
 * @param[in]  value  Value to append.
 * @return New first-unwritten offset.
 * @retval 0 Not returned; at least one digit is always appended.
 * @pre @p out has enough remaining room for ten decimal digits.
 * @pre @p offset is below ::k_ppm_header_bytes.
 * @post The decimal spelling of @p value occupies the appended bytes.
 * @post No terminating NUL is written or required.
 * @note Pure apart from @p out; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_append_u32(char out[k_ppm_header_bytes], size_t offset, uint32_t value)
{
  char   reverse[k_ppm_u32_digits];
  size_t digits = 0U;
  do {
    reverse[digits] = (char)('0' + (value % (uint32_t)k_ppm_decimal_radix));
    ++digits;
    value /= (uint32_t)k_ppm_decimal_radix;
  } while (value != 0U);
  while (digits > 0U) {
    --digits;
    out[offset] = reverse[digits];
    ++offset;
  }
  return offset;
}

/**
 * @brief Build the locale-independent binary-PPM header for @p width x @p height.
 * @details Appends fixed syntax and explicit unsigned decimal dimensions into
 * the caller-owned bounded header without formatting streams.
 * @param[out] out    Header storage with ::k_ppm_header_bytes capacity.
 * @param[in]  width  Non-zero image width.
 * @param[in]  height Non-zero image height.
 * @return Number of initialized header bytes.
 * @retval 0 Not returned for valid dimensions.
 * @pre @p out has ::k_ppm_header_bytes writable bytes.
 * @pre @p width and @p height are non-zero.
 * @post @p out begins with a complete P6 header of the returned length.
 * @post No terminating NUL is written or required.
 * @note Pure apart from @p out; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_ppm_header(char out[k_ppm_header_bytes], uint32_t width, uint32_t height)
{
  size_t offset = 0U;
  out[offset++] = 'P';
  out[offset++] = '6';
  out[offset++] = '\n';
  offset        = internal_append_u32(out, offset, width);
  out[offset++] = ' ';
  offset        = internal_append_u32(out, offset, height);
  out[offset++] = '\n';
  out[offset++] = '2';
  out[offset++] = '5';
  out[offset++] = '5';
  out[offset++] = '\n';
  return offset;
}

ra8_err_t ra8_viewer_write_ppm565(const uint16_t* px, uint32_t w, uint32_t h, const char* path)
{
  if ((px == nullptr) || (path == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((w == 0U) || (h == 0U)) {
    return k_ra8_err_invalid_size;
  }
  if ((size_t)w > (SIZE_MAX / (size_t)h)) {
    return k_ra8_err_invalid_size;
  }
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, k_ppm_file_mode);
  if (fd < 0) {
    return k_ra8_err_not_found;
  }
  char         header[k_ppm_header_bytes];
  const size_t header_bytes = internal_ppm_header(header, w, h);
  bool         ok           = internal_write_all(fd, (const uint8_t*)header, header_bytes);
  const size_t pixels       = (size_t)w * (size_t)h;
  uint8_t      chunk[k_ppm_chunk_bytes];
  size_t       consumed = 0U;
  while (ok && (consumed < pixels)) {
    size_t batch = pixels - consumed;
    if (batch > (size_t)k_ppm_chunk_pixels) {
      batch = (size_t)k_ppm_chunk_pixels;
    }
    for (size_t i = 0U; i < batch; ++i) {
      internal_unpack565(px[consumed + i], &chunk[i * (size_t)k_ppm_channels]);
    }
    ok = internal_write_all(fd, chunk, batch * (size_t)k_ppm_channels);
    consumed += batch;
  }
  const int closed = close(fd);
  return (ok && (closed == 0)) ? k_ra8_ok : k_ra8_err_not_found;
}

ra8_err_t ra8_viewer_dump_ppm(const ra8_viewer_reader_t* r, const char* path)
{
  if ((r == nullptr) || (path == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  return ra8_viewer_write_ppm565(r->fb,
                                 (uint32_t)k_ra8_viewer_fb_width,
                                 (uint32_t)k_ra8_viewer_fb_height,
                                 path);
}
