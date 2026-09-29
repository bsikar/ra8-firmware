/**
 * @file ra8_imgdec_dims.c
 * @brief The shared container geometry probe behind the image-decoder seam (#768).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @details
 * The sibling of `ra8_imgdec_sniff.c`. The sniff says which container the
 * bytes hold; this file says how big that container claims to be, and both
 * answer without linking a decoder.
 *
 * One geometry probe existed in the tree before this, `jof_probe_dims()` in
 * `apps/shared_libs/jof`, and three callers reach up into the JOF producer
 * to use it while producing no JOF at all: `mdl_export_jof.c`,
 * `comic_tiles.c` and the host `jof_worker.c`. It also answers only the
 * producer's three formats, so a GIF or a BMP that the sniff recognises has
 * no size anyone can read. Both problems are the missing seam, so the probe
 * belongs here, beneath its callers, covering what the sniff covers.
 *
 * Every read is at a fixed offset except JPEG, whose size lives behind a
 * marker walk. Nothing here validates a payload: a declared geometry is the
 * container's claim, range-checked against ::k_ra8_imgdec_dim_max and
 * otherwise taken verbatim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_imgdec.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/* =============================================================================
 * Header vocabulary
 * =============================================================================
 */

/**
 * @enum ra8_imgdec_dims_png_t
 * @brief Offsets and lengths inside a PNG's leading IHDR chunk.
 *
 * @details IHDR is mandated first by the specification, so the geometry sits
 * at constant offsets: signature (8), chunk length (4), chunk type (4), then
 * width and height as big-endian 32-bit fields.
 */
typedef enum : uint8_t {
  k_png_type_off  = 12U, /**< Offset of the four-byte chunk type tag. */
  k_png_width_off = 16U, /**< Offset of the big-endian width field.   */
  k_png_min_bytes = 24U, /**< Bytes needed to reach past the height.  */
} ra8_imgdec_dims_png_t;

/**
 * @enum ra8_imgdec_dims_gif_t
 * @brief Offsets inside a GIF's logical screen descriptor.
 */
typedef enum : uint8_t {
  k_gif_width_off = 6U,  /**< Offset of the little-endian canvas width. */
  k_gif_min_bytes = 10U, /**< Bytes needed to reach past the height.    */
} ra8_imgdec_dims_gif_t;

/**
 * @enum ra8_imgdec_dims_bmp_t
 * @brief Offsets inside a BMP file header and its DIB header.
 *
 * @details Two DIB shapes are in circulation. BITMAPCOREHEADER declares a
 * size of 12 and stores 16-bit dimensions; every later header (INFO, V4, V5)
 * declares 40 or more and stores signed 32-bit ones.
 */
typedef enum : uint8_t {
  k_bmp_dibsize_off  = 14U, /**< Offset of the DIB header's own size.     */
  k_bmp_dims_off     = 18U, /**< Offset of the first dimension field.     */
  k_bmp_core_size    = 12U, /**< DIB size that means 16-bit dimensions.   */
  k_bmp_core_bytes   = 22U, /**< Bytes needed for the core shape.         */
  k_bmp_info_bytes   = 26U, /**< Bytes needed for the 32-bit shape.       */
  k_bmp_core_hgt_off = 20U, /**< Offset of the core shape's height field. */
  k_bmp_info_hgt_off = 22U, /**< Offset of the info shape's height field. */
} ra8_imgdec_dims_bmp_t;

/**
 * @enum ra8_imgdec_dims_webp_t
 * @brief Offsets inside a RIFF/WEBP container and its first chunk.
 *
 * @details The RIFF header is 12 bytes, then a four-byte chunk tag and a
 * four-byte chunk size, so every payload begins at offset 20.
 */
typedef enum : uint8_t {
  k_webp_chunk_off    = 12U,   /**< First chunk's fourCC.           */
  k_webp_payload_off  = 20U,   /**< That chunk's payload.           */
  k_webp_lossy_sz_off = 6U,    /**< Width, inside a `VP8 ` payload. */
  k_webp_lossy_bytes  = 10U,   /**< Payload bytes `VP8 ` needs.     */
  k_webp_lossless_sig = 0x2FU, /**< First byte of a `VP8L` payload. */
  k_webp_lossless_len = 5U,    /**< Payload bytes `VP8L` needs.     */
  k_webp_ext_wid_off  = 4U,    /**< Canvas width, in `VP8X`.        */
  k_webp_ext_hgt_off  = 7U,    /**< Canvas height, three bytes on.  */
  k_webp_ext_bytes    = 10U,   /**< Payload bytes `VP8X` needs.     */
} ra8_imgdec_dims_webp_t;

/**
 * @enum ra8_imgdec_dims_jpeg_t
 * @brief Marker values and field offsets for the JPEG SOF walk.
 *
 * @details JPEG is the one format here with no fixed geometry offset: the
 * frame header can sit behind any number of application, comment and table
 * segments, so the only way to the size is to walk the marker chain.
 */
typedef enum : uint16_t {
  k_jpeg_pad       = 0xFFU, /**< Marker prefix, and legal as fill.        */
  k_jpeg_sof_first = 0xC0U, /**< First of the SOFn marker block.          */
  k_jpeg_sof_last  = 0xCFU, /**< Last of the SOFn marker block.           */
  k_jpeg_dht       = 0xC4U, /**< Huffman tables, not a frame header.      */
  k_jpeg_jpg       = 0xC8U, /**< Reserved, not a frame header.            */
  k_jpeg_dac       = 0xCCU, /**< Arithmetic conditioning, not a frame.    */
  k_jpeg_rst_first = 0xD0U, /**< First standalone restart marker.         */
  k_jpeg_rst_last  = 0xD7U, /**< Last standalone restart marker.          */
  k_jpeg_soi       = 0xD8U, /**< Start of image. Standalone.              */
  k_jpeg_eoi       = 0xD9U, /**< End of image. Standalone, ends the walk. */
  k_jpeg_sos       = 0xDAU, /**< Start of scan: entropy data follows.     */
  k_jpeg_tem       = 0x01U, /**< Temporary private marker. Standalone.    */
  k_jpeg_seg_min   = 2U,    /**< Smallest legal segment length field.     */
  k_jpeg_sof_hgt   = 3U,    /**< Height offset within an SOF segment.     */
  k_jpeg_sof_min   = 7U,    /**< Segment bytes an SOF size read needs.    */
} ra8_imgdec_dims_jpeg_t;

/**
 * @enum ra8_imgdec_dims_shift_t
 * @brief Bit positions used to assemble multi-byte fields.
 */
typedef enum : uint32_t {
  k_shift_8  = 8U,      /**< One byte.                                  */
  k_shift_16 = 16U,     /**< Two bytes.                                 */
  k_shift_24 = 24U,     /**< Three bytes.                               */
  k_shift_14 = 14U,     /**< Width field width in a `VP8L` bit packing. */
  k_mask_14  = 0x3FFFU, /**< Fourteen bits, the VP8 dimension width.    */
} ra8_imgdec_dims_shift_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Assemble a big-endian 32-bit field.
 *
 * @details PNG is the only big-endian container the tree reads, which is why
 * this sits here rather than in a shared byte-order header: the JOF stack's
 * little-endian helpers cannot serve it.
 */
RA8_INTERNAL static uint32_t internal_be32(const uint8_t* bytes, uint32_t offset) {
  return ((uint32_t)bytes[offset] << (uint32_t)k_shift_24) |
         ((uint32_t)bytes[offset + 1U] << (uint32_t)k_shift_16) |
         ((uint32_t)bytes[offset + 2U] << (uint32_t)k_shift_8) |
         (uint32_t)bytes[offset + 3U];
}

/** @brief Assemble a little-endian 16-bit field. */
RA8_INTERNAL static uint32_t internal_le16(const uint8_t* bytes, uint32_t offset) {
  return (uint32_t)bytes[offset] | ((uint32_t)bytes[offset + 1U] << (uint32_t)k_shift_8);
}

/** @brief Assemble a little-endian 24-bit field. */
RA8_INTERNAL static uint32_t internal_le24(const uint8_t* bytes, uint32_t offset) {
  return internal_le16(bytes, offset) | ((uint32_t)bytes[offset + 2U] << (uint32_t)k_shift_16);
}

/** @brief Assemble a little-endian 32-bit field. */
RA8_INTERNAL static uint32_t internal_le32(const uint8_t* bytes, uint32_t offset) {
  return internal_le24(bytes, offset) | ((uint32_t)bytes[offset + 3U] << (uint32_t)k_shift_24);
}

/** @brief Assemble a big-endian 16-bit field. */
RA8_INTERNAL static uint32_t internal_be16(const uint8_t* bytes, uint32_t offset) {
  return ((uint32_t)bytes[offset] << (uint32_t)k_shift_8) | (uint32_t)bytes[offset + 1U];
}

/**
 * @brief Magnitude of a field that BMP stores as a signed 32-bit value.
 *
 * @details A negative height is not an error: it declares a top-down row
 * order. The geometry is the same either way, so the sign is dropped here
 * rather than refused, and the row order stays a decoder's business.
 */
RA8_INTERNAL static uint32_t internal_abs32(uint32_t raw) {
  const int32_t signed_value = (int32_t)raw;
  if (signed_value < 0) {
    return (uint32_t)(-(int64_t)signed_value);
  }
  return raw;
}

/** @brief True when @p bytes holds @p need readable bytes. */
RA8_INTERNAL static bool internal_have(uint32_t byte_count, uint32_t need) {
  return byte_count >= need;
}

/** @brief Compare four bytes at @p offset against an ASCII fourCC. */
RA8_INTERNAL static bool internal_fourcc(const uint8_t* bytes,
                                         uint32_t       byte_count,
                                         uint32_t       offset,
                                         const char*    tag) {
  const uint32_t len = 4U;
  if (!internal_have(byte_count, offset + len)) {
    return false;
  }
  for (uint32_t i = 0U; i < len; ++i) {
    if (bytes[offset + i] != (uint8_t)tag[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Read a PNG's IHDR geometry.
 *
 * @details The chunk type is checked, not assumed. `jof_produce.c`'s copy
 * reads offsets 16 and 20 on the strength of the signature alone; a PNG
 * whose first chunk is not IHDR is malformed, but reading a foreign chunk's
 * bytes as a size is a worse answer than refusing.
 */
RA8_INTERNAL static ra8_err_t internal_png(const uint8_t*     bytes,
                                           uint32_t           byte_count,
                                           ra8_imgdec_geom_t* out) {
  if (!internal_have(byte_count, (uint32_t)k_png_min_bytes)) {
    return k_ra8_err_not_supported; /* IHDR truncated */
  }
  if (!internal_fourcc(bytes, byte_count, (uint32_t)k_png_type_off, "IHDR")) {
    return k_ra8_err_not_supported; /* first chunk is not IHDR */
  }
  out->width_px  = internal_be32(bytes, (uint32_t)k_png_width_off);
  out->height_px = internal_be32(bytes, (uint32_t)k_png_width_off + 4U);
  return k_ra8_ok;
}

/** @brief Read a GIF's logical screen descriptor. */
RA8_INTERNAL static ra8_err_t internal_gif(const uint8_t*     bytes,
                                           uint32_t           byte_count,
                                           ra8_imgdec_geom_t* out) {
  if (!internal_have(byte_count, (uint32_t)k_gif_min_bytes)) {
    return k_ra8_err_not_supported; /* screen descriptor truncated */
  }
  out->width_px  = internal_le16(bytes, (uint32_t)k_gif_width_off);
  out->height_px = internal_le16(bytes, (uint32_t)k_gif_width_off + 2U);
  return k_ra8_ok;
}

/** @brief Read a BMP's DIB header, in either the core or the info shape. */
RA8_INTERNAL static ra8_err_t internal_bmp(const uint8_t*     bytes,
                                           uint32_t           byte_count,
                                           ra8_imgdec_geom_t* out) {
  if (!internal_have(byte_count, (uint32_t)k_bmp_dibsize_off + 4U)) {
    return k_ra8_err_not_supported; /* no DIB header at all */
  }
  const uint32_t dib_size = internal_le32(bytes, (uint32_t)k_bmp_dibsize_off);

  if (dib_size == (uint32_t)k_bmp_core_size) {
    if (!internal_have(byte_count, (uint32_t)k_bmp_core_bytes)) {
      return k_ra8_err_not_supported; /* core header truncated */
    }
    out->width_px  = internal_le16(bytes, (uint32_t)k_bmp_dims_off);
    out->height_px = internal_le16(bytes, (uint32_t)k_bmp_core_hgt_off);
    return k_ra8_ok;
  }

  if (!internal_have(byte_count, (uint32_t)k_bmp_info_bytes)) {
    return k_ra8_err_not_supported; /* info header truncated */
  }
  out->width_px  = internal_abs32(internal_le32(bytes, (uint32_t)k_bmp_dims_off));
  out->height_px = internal_abs32(internal_le32(bytes, (uint32_t)k_bmp_info_hgt_off));
  return k_ra8_ok;
}

/**
 * @brief Read a WebP's canvas size from whichever VP8 chunk opens it.
 *
 * @details Three encodings, all of them 14-bit fields stored differently:
 * `VP8 ` keeps them after a three-byte start code, `VP8L` packs them as
 * width-1 and height-1 into one little-endian word, and `VP8X` publishes a
 * 24-bit canvas minus one. Anything else opening a RIFF/WEBP (an `ALPH` or
 * an `ANIM` first, both legal only after a `VP8X`) is refused rather than
 * guessed at.
 */
RA8_INTERNAL static ra8_err_t internal_webp(const uint8_t*     bytes,
                                            uint32_t           byte_count,
                                            ra8_imgdec_geom_t* out) {
  const uint32_t payload = (uint32_t)k_webp_payload_off;

  if (internal_fourcc(bytes, byte_count, (uint32_t)k_webp_chunk_off, "VP8 ")) {
    if (!internal_have(byte_count, payload + (uint32_t)k_webp_lossy_bytes)) {
      return k_ra8_err_not_supported; /* frame header truncated */
    }
    const uint32_t at = payload + (uint32_t)k_webp_lossy_sz_off;
    out->width_px     = internal_le16(bytes, at) & (uint32_t)k_mask_14;
    out->height_px    = internal_le16(bytes, at + 2U) & (uint32_t)k_mask_14;
    return k_ra8_ok;
  }

  if (internal_fourcc(bytes, byte_count, (uint32_t)k_webp_chunk_off, "VP8L")) {
    if (!internal_have(byte_count, payload + (uint32_t)k_webp_lossless_len)) {
      return k_ra8_err_not_supported; /* stream header truncated */
    }
    if (bytes[payload] != (uint8_t)k_webp_lossless_sig) {
      return k_ra8_err_not_supported; /* not a lossless stream after all */
    }
    const uint32_t packed = internal_le32(bytes, payload + 1U);
    out->width_px         = (packed & (uint32_t)k_mask_14) + 1U;
    out->height_px        = ((packed >> (uint32_t)k_shift_14) & (uint32_t)k_mask_14) + 1U;
    return k_ra8_ok;
  }

  if (internal_fourcc(bytes, byte_count, (uint32_t)k_webp_chunk_off, "VP8X")) {
    if (!internal_have(byte_count, payload + (uint32_t)k_webp_ext_bytes)) {
      return k_ra8_err_not_supported; /* extended header truncated */
    }
    out->width_px  = internal_le24(bytes, payload + (uint32_t)k_webp_ext_wid_off) + 1U;
    out->height_px = internal_le24(bytes, payload + (uint32_t)k_webp_ext_hgt_off) + 1U;
    return k_ra8_ok;
  }

  return k_ra8_err_not_supported; /* recognised container, unreadable first chunk */
}

/**
 * @brief True for the SOFn markers that actually carry a frame header.
 *
 * @details The 0xC0..0xCF block is not all frames: DHT, JPG and DAC live
 * inside it and carry tables, not a size. Treating the whole block as SOF
 * reads a Huffman table as a geometry.
 */
RA8_INTERNAL static bool internal_is_sof(uint32_t marker) {
  if ((marker < (uint32_t)k_jpeg_sof_first) || (marker > (uint32_t)k_jpeg_sof_last)) {
    return false;
  }
  return (marker != (uint32_t)k_jpeg_dht) && (marker != (uint32_t)k_jpeg_jpg) &&
         (marker != (uint32_t)k_jpeg_dac);
}

/** @brief True for markers that stand alone, with no length field after them. */
RA8_INTERNAL static bool internal_is_standalone(uint32_t marker) {
  const bool restart =
      (marker >= (uint32_t)k_jpeg_rst_first) && (marker <= (uint32_t)k_jpeg_rst_last);
  return restart || (marker == (uint32_t)k_jpeg_soi) || (marker == (uint32_t)k_jpeg_eoi) ||
         (marker == (uint32_t)k_jpeg_tem);
}

/**
 * @brief Walk a JPEG's marker chain to the first frame header.
 *
 * @details The walk stops at SOS: past it the bytes are entropy-coded and a
 * 0xFF there is data, not a marker. A progressive JPEG's first SOF still
 * precedes its first SOS, so stopping there loses nothing.
 */
RA8_INTERNAL static ra8_err_t internal_jpeg(const uint8_t*     bytes,
                                            uint32_t           byte_count,
                                            ra8_imgdec_geom_t* out) {
  uint32_t at = 2U; /* past SOI */

  while (internal_have(byte_count, at + 2U)) {
    if (bytes[at] != (uint8_t)k_jpeg_pad) {
      return k_ra8_err_not_supported; /* desynchronised: not a marker boundary */
    }
    uint32_t marker = bytes[at + 1U];
    at += 2U;

    /* 0xFF is legal fill before a marker; skip any run of it. */
    while ((marker == (uint32_t)k_jpeg_pad) && internal_have(byte_count, at + 1U)) {
      marker = bytes[at];
      at += 1U;
    }

    if (marker == (uint32_t)k_jpeg_sos) {
      return k_ra8_err_not_supported; /* entropy data reached with no frame header */
    }
    if (internal_is_standalone(marker)) {
      if (marker == (uint32_t)k_jpeg_eoi) {
        return k_ra8_err_not_supported; /* image ended with no frame header */
      }
      continue;
    }

    if (!internal_have(byte_count, at + 2U)) {
      return k_ra8_err_not_supported; /* length field truncated */
    }
    const uint32_t seg_len = internal_be16(bytes, at);
    if (seg_len < (uint32_t)k_jpeg_seg_min) {
      return k_ra8_err_not_supported; /* malformed segment length */
    }

    if (internal_is_sof(marker)) {
      if ((seg_len < (uint32_t)k_jpeg_sof_min) ||
          !internal_have(byte_count, at + (uint32_t)k_jpeg_sof_min)) {
        return k_ra8_err_not_supported; /* frame header truncated */
      }
      out->height_px = internal_be16(bytes, at + (uint32_t)k_jpeg_sof_hgt);
      out->width_px  = internal_be16(bytes, at + (uint32_t)k_jpeg_sof_hgt + 2U);
      return k_ra8_ok;
    }

    at += seg_len;
  }

  return k_ra8_err_not_supported; /* ran out of bytes before any frame header */
}

/* =============================================================================
 * Public API
 * =============================================================================
 */

ra8_err_t ra8_imgdec_dims(const uint8_t* bytes, uint32_t byte_count, ra8_imgdec_geom_t* out) {
  if ((bytes == nullptr) || (out == nullptr)) {
    if (out != nullptr) {
      out->format    = k_ra8_imgdec_format_none;
      out->width_px  = 0U;
      out->height_px = 0U;
    }
    return k_ra8_err_null_ptr;
  }

  out->format    = k_ra8_imgdec_format_none;
  out->width_px  = 0U;
  out->height_px = 0U;

  ra8_imgdec_format_t found = k_ra8_imgdec_format_none;
  const ra8_err_t     snf   = ra8_imgdec_sniff(bytes, byte_count, &found);
  if (snf != k_ra8_ok) {
    return snf;
  }

  ra8_imgdec_geom_t geom = {.format = found, .width_px = 0U, .height_px = 0U};
  ra8_err_t         rc   = k_ra8_err_not_supported;

  switch (found) {
    case k_ra8_imgdec_format_png:
      rc = internal_png(bytes, byte_count, &geom);
      break;
    case k_ra8_imgdec_format_jpeg:
      rc = internal_jpeg(bytes, byte_count, &geom);
      break;
    case k_ra8_imgdec_format_webp:
      rc = internal_webp(bytes, byte_count, &geom);
      break;
    case k_ra8_imgdec_format_gif:
      rc = internal_gif(bytes, byte_count, &geom);
      break;
    case k_ra8_imgdec_format_bmp:
      rc = internal_bmp(bytes, byte_count, &geom);
      break;
    case k_ra8_imgdec_format_none:
    case k_ra8_imgdec_format_tga:
    default:
      rc = k_ra8_err_not_supported; /* no signature keys a TGA geometry read */
      break;
  }

  if (rc != k_ra8_ok) {
    return rc;
  }

  if ((geom.width_px == 0U) || (geom.height_px == 0U) ||
      (geom.width_px > (uint32_t)k_ra8_imgdec_dim_max) ||
      (geom.height_px > (uint32_t)k_ra8_imgdec_dim_max)) {
    return k_ra8_err_invalid_size;
  }

  *out = geom;
  return k_ra8_ok;
}
