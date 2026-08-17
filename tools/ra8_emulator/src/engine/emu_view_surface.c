/**
 * @file emu_view_surface.c
 * @brief Bounded-tile GLCDC rotation, raw-fd composition, and PPM export
 * @details Reads live framebuffer rows through the authoritative memory seam,
 * rotates them in fixed-capacity tiles into a descriptor-backed composite,
 * applies board status overlays, and exports exact PPM scanlines.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "emu_host_io_internal.h"
#include "emu_memmap.h"
#include "emu_mmio.h"
#include "emu_view.h"
#include "emu_view_surface_internal.h"
#include "emu_view_tile_internal.h"

/** @brief GLCDC layer registers decoded by the streamed renderer. */
typedef enum : uint64_t {
  k_surface_gr1_saddr = 0x4034310CUL, /**< GR[0].FLM2 framebuffer base. */
  k_surface_gr1_flm3  = 0x40343110UL, /**< GR[0].FLM3 line stride.      */
  k_surface_gr1_flm5  = 0x40343118UL, /**< GR[0].FLM5 line count.       */
  k_surface_gr1_fmt   = 0x4034311CUL, /**< GR[0].FLM6 pixel format.     */
} surface_glcdc_reg_t;

/** @brief GLCDC field and RGB conversion constants. */
typedef enum : uint32_t {
  k_surface_fmt_rgb565  = 2U,          /**< GLCDC RGB565 format code.     */
  k_surface_fmt_shift   = 28U,         /**< FORMAT[30:28] shift.          */
  k_surface_fmt_mask    = 0x7U,        /**< FORMAT mask.                  */
  k_surface_high_shift  = 16U,         /**< Stride/count high-half shift. */
  k_surface_stride_mask = 0xFFFFU,     /**< FLM3 stride mask.             */
  k_surface_lnnum_mask  = 0x7FFU,      /**< FLM5 line count mask.         */
  k_surface_rgb_r_shift = 16U,         /**< RGB888 red shift.             */
  k_surface_rgb_g_shift = 8U,          /**< RGB888 green shift.           */
  k_surface_rgb_mask    = 0x00FFFFFFU, /**< Live background color mask.   */
  k_surface_byte_mask   = 0xFFU,       /**< One RGB888 component.         */
  k_surface_r_keep      = 0xF8U,       /**< RGB565 red high bits.         */
  k_surface_g_keep      = 0xFCU,       /**< RGB565 green high bits.       */
  k_surface_r_pos       = 8U,          /**< Packed red position.          */
  k_surface_g_pos       = 3U,          /**< Packed green position.        */
  k_surface_b_drop      = 3U,          /**< Packed blue truncation.       */
  k_surface_r565_shift  = 11U,         /**< RGB565 red field shift.       */
  k_surface_g565_shift  = 5U,          /**< RGB565 green field shift.     */
  k_surface_5bit        = 0x1FU,       /**< Five-bit channel mask.        */
  k_surface_6bit        = 0x3FU,       /**< Six-bit channel mask.         */
} surface_color_t;

/** @brief Live GLCDC source layer clipped to native panel geometry. */
typedef struct {
  uint32_t address; /**< Emulated row-zero address. */
  uint32_t stride;  /**< Source row bytes.          */
  uint16_t width;   /**< Clipped source pixels.     */
  uint16_t height;  /**< Clipped source rows.       */
  bool     active;  /**< Valid RGB565 layer.        */
} surface_layer_t;

/**
 * @brief Pack a live 0x00RRGGBB background into RGB565.
 * @details Pack a live 0x00rrggbb background into rgb565; this step is contained within the emu view surface model and uses bounded caller or module-owned storage.
 * @param[in] rgb Rgb input used by the operation.
 * @return The rgb888 to 565 result produced by the emu view surface model.
 * @retval value The operation-specific rgb888 to 565 value.
 * @pre Arguments satisfy the ranges documented for rgb888 to 565. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view surface model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_rgb888_to_565(uint32_t rgb)
{
  const uint32_t red   = (rgb >> k_surface_rgb_r_shift) & k_surface_byte_mask;
  const uint32_t green = (rgb >> k_surface_rgb_g_shift) & k_surface_byte_mask;
  const uint32_t blue  = rgb & k_surface_byte_mask;
  return (uint16_t)(((red & k_surface_r_keep) << k_surface_r_pos) |
                    ((green & k_surface_g_keep) << k_surface_g_pos) | (blue >> k_surface_b_drop));
}

/**
 * @brief True when a source base lies in one emulated RAM region.
 * @details True when a source base lies in one emulated ram region; this step is contained within the emu view surface model and uses bounded caller or module-owned storage.
 * @param[in] address Guest address involved in the operation.
 * @return The address is ram result produced by the emu view surface model.
 * @retval true The address is ram condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for address is ram. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view surface model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_address_is_ram(uint32_t address)
{
  return (((address >= (uint32_t)k_dtcm_base) && (address < (uint32_t)k_dtcm_end)) ||
          ((address >= (uint32_t)k_sram_base) && (address < (uint32_t)k_sram_end)) ||
          ((address >= (uint32_t)k_sdram_base) && (address < (uint32_t)k_sdram_end)));
}

/**
 * @brief Decode and clip the current GLCDC layer without guest-facing reads.
 * @details Decode and clip the current glcdc layer without guest-facing reads; this step is contained within the emu view surface model and uses bounded caller or module-owned storage.
 * @param[in] panel_width Panel width input used by the operation.
 * @param[in] panel_height Panel height input used by the operation.
 * @return The layer result produced by the emu view surface model.
 * @retval value The operation-specific layer value.
 * @pre Arguments satisfy the ranges documented for layer. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view surface model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static surface_layer_t internal_layer(uint16_t panel_width, uint16_t panel_height)
{
  const uint32_t address = mmio_peek((uint64_t)k_surface_gr1_saddr);
  const uint32_t format =
    (mmio_peek((uint64_t)k_surface_gr1_fmt) >> k_surface_fmt_shift) & k_surface_fmt_mask;
  const uint32_t stride =
    (mmio_peek((uint64_t)k_surface_gr1_flm3) >> k_surface_high_shift) & k_surface_stride_mask;
  const uint32_t lines =
    ((mmio_peek((uint64_t)k_surface_gr1_flm5) >> k_surface_high_shift) & k_surface_lnnum_mask) + 1U;
  const bool     active = internal_address_is_ram(address) && (format == k_surface_fmt_rgb565) &&
                          (stride >= sizeof(uint16_t));
  const uint32_t source_width = stride / sizeof(uint16_t);
  return (surface_layer_t){
    .address = address,
    .stride  = stride,
    .width   = (uint16_t)((source_width < panel_width) ? source_width : panel_width),
    .height  = (uint16_t)((lines < panel_height) ? lines : panel_height),
    .active  = active,
  };
}

/** @brief One bounded source tile inside the live GLCDC layer. */
typedef struct {
  uint16_t x;      /**< Tile origin column in the source layer. */
  uint16_t y;      /**< Tile origin row in the source layer.    */
  uint16_t width;  /**< Tile width in source pixels.            */
  uint16_t height; /**< Tile height in source rows.             */
} surface_tile_t;

/**
 * @brief Read one bounded tile of the live layer into the scratch pixels.
 * @details Copies @p tile row by row out of emulated memory through the authoritative memory seam, packing the rows tightly so the rotation stage sees a contiguous tile.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] layer Decoded and clipped live GLCDC source layer.
 * @param[in] tile Bounded tile origin and extent inside @p layer.
 * @param[out] source Scratch tile receiving the packed tile pixels.
 * @return The read tile result produced by the emu view surface model.
 * @retval true The read tile condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for read tile. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view surface model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_read_tile(uc_engine*             uc,
                                            const surface_layer_t* layer,
                                            const surface_tile_t*  tile,
                                            uint16_t*              source)
{
  for (uint16_t row = 0U; row < tile->height; row++) {
    if (emu_mem_read(uc,
                     (uint64_t)layer->address + ((uint64_t)(tile->y + row) * layer->stride) +
                       ((uint64_t)tile->x * sizeof(uint16_t)),
                     &source[(size_t)row * tile->width],
                     (size_t)tile->width * sizeof(uint16_t)) != UC_ERR_OK) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Read, rotate, and write every active GLCDC tile.
 * @details Read, rotate, and write every active glcdc tile; this step is contained within the emu view surface model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in,out] presentation Descriptor-backed presentation workspace accessed by the operation.
 * @param[in] layer Layer input used by the operation.
 * @return The write layer result produced by the emu view surface model.
 * @retval true The write layer condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for write layer. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view surface model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_write_layer(uc_engine*                    uc,
                                              emu_presentation_workspace_t* presentation,
                                              const surface_layer_t*        layer)
{
  if (!layer->active) {
    return true;
  }
  /* The borrowed scratch is a byte buffer that holds a run of RGB565 pixels, and
   * emu_presentation_open() rejects any scratch not aligned to alignof(uint16_t),
   * so the two members name one storage. The union states that aliasing instead
   * of casting a uint8_t* through void*. */
  const union {
    uint8_t*  bytes;  /**< Borrowed scratch as raw bytes.     */
    uint16_t* pixels; /**< The same storage as RGB565 pixels. */
  } scratch              = {.bytes = presentation->scratch};
  uint16_t* const source = scratch.pixels;
  for (uint16_t y0 = 0U; y0 < layer->height;) {
    const uint16_t height = ((uint32_t)layer->height - y0 < k_emu_presentation_tile_px)
                              ? (uint16_t)(layer->height - y0)
                              : (uint16_t)k_emu_presentation_tile_px;
    for (uint16_t x0 = 0U; x0 < layer->width;) {
      const uint16_t       width = ((uint32_t)layer->width - x0 < k_emu_presentation_tile_px)
                                     ? (uint16_t)(layer->width - x0)
                                     : (uint16_t)k_emu_presentation_tile_px;
      const surface_tile_t tile  = {.x = x0, .y = y0, .width = width, .height = height};
      if (!internal_read_tile(uc, layer, &tile, source)) {
        return false;
      }
      if (!priv_emu_view_tile_write(presentation, source, x0, y0, width, height)) {
        return false;
      }
      x0 = (uint16_t)(x0 + width);
    }
    y0 = (uint16_t)(y0 + height);
  }
  return true;
}

bool priv_emu_view_surface_build(uc_engine*                    uc,
                                 emu_presentation_workspace_t* presentation,
                                 const board_status_t*         status)
{
  if ((uc == nullptr) || (presentation == nullptr) || (presentation->fd < 0)) {
    return false;
  }
  const uint16_t background =
    internal_rgb888_to_565(mmio_peek((uint64_t)k_glcdc_bg_bgc) & k_surface_rgb_mask);
  if (!emu_presentation_fill(presentation,
                             0U,
                             0U,
                             presentation->composite_width,
                             presentation->composite_height,
                             0U) ||
      !emu_presentation_fill(presentation,
                             0U,
                             0U,
                             presentation->display_width,
                             presentation->display_height,
                             background)) {
    return false;
  }
  const surface_layer_t layer =
    internal_layer(presentation->panel_width, presentation->panel_height);
  if (!internal_write_layer(uc, presentation, &layer)) {
    return false;
  }
  board_overlay_surface_t overlay = {.context = presentation,
                                     .fill    = emu_presentation_fill,
                                     .width   = presentation->composite_width,
                                     .height  = presentation->composite_height,
                                     .ok      = true};
  return board_overlay_draw_sidebar(&overlay, presentation->display_width, status);
}

/**
 * @brief Convert and emit one bounded RGB565 chunk as PPM RGB bytes.
 * @details Convert and emit one bounded rgb565 chunk as ppm rgb bytes; this step is contained within the emu view surface model and uses bounded caller or module-owned storage.
 * @param[in] fd Open raw descriptor used for the transfer.
 * @param[in] presentation Descriptor-backed presentation workspace accessed by the operation.
 * @return The write ppm pixels result produced by the emu view surface model.
 * @retval true The write ppm pixels condition holds or completed successfully; false otherwise.
 * @pre Arguments satisfy the ranges documented for write ppm pixels. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view surface model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_write_ppm_pixels(int                                 fd,
                                                   const emu_presentation_workspace_t* presentation)
{
  enum : size_t {
    k_pixels_per_chunk = 128U, /**< Fixed conversion chunk pixel count. */
  };
  uint16_t     pixels[k_pixels_per_chunk];
  uint8_t      rgb[k_pixels_per_chunk * 3U];
  const size_t total_pixels = presentation->surface_bytes / sizeof(uint16_t);
  for (size_t base = 0U; base < total_pixels; base += k_pixels_per_chunk) {
    const size_t count =
      ((total_pixels - base) < k_pixels_per_chunk) ? (total_pixels - base) : k_pixels_per_chunk;
    if (!emu_presentation_read(presentation,
                               base * sizeof(uint16_t),
                               pixels,
                               count * sizeof(uint16_t))) {
      return false;
    }
    for (size_t index = 0U; index < count; index++) {
      const uint16_t pixel   = pixels[index];
      const uint32_t red     = (pixel >> k_surface_r565_shift) & k_surface_5bit;
      const uint32_t green   = (pixel >> k_surface_g565_shift) & k_surface_6bit;
      const uint32_t blue    = pixel & k_surface_5bit;
      rgb[(index * 3U) + 0U] = (uint8_t)((red << 3U) | (red >> 2U));
      rgb[(index * 3U) + 1U] = (uint8_t)((green << 2U) | (green >> 4U));
      rgb[(index * 3U) + 2U] = (uint8_t)((blue << 3U) | (blue >> 2U));
    }
    if (priv_emu_io_write_exact(fd, rgb, count * 3U).status != k_emu_io_ok) {
      return false;
    }
  }
  return true;
}

int write_ppm(const char* path, const emu_presentation_workspace_t* presentation)
{
  if ((path == nullptr) || (presentation == nullptr) || (presentation->fd < 0)) {
    return -1;
  }
  emu_io_txn_t transaction = {.fd = -1};
  if (priv_emu_io_txn_begin(path, &transaction).status != k_emu_io_ok) {
    return -1;
  }
  const bool ok = (priv_emu_io_filef(transaction.fd,
                                     "P6\n%u %u\n255\n",
                                     (unsigned int)presentation->composite_width,
                                     (unsigned int)presentation->composite_height)
                     .status == k_emu_io_ok) &&
                  internal_write_ppm_pixels(transaction.fd, presentation);
  if (!ok || (priv_emu_io_txn_commit(&transaction).status != k_emu_io_ok)) {
    priv_emu_io_txn_abort(&transaction);
    return -1;
  }
  return 0;
}
