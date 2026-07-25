/**
 * @file test_ra8_book_image_rect.c
 * @brief Sub-rect image addressing tests for ra8_book_src_image / _rect (#342).
 *
 * @details
 * Builds a small, self-contained `.rabook` blob in memory with three raster
 * images in the image pool. Two are 4bpp packed grayscale: a 5x3 image (odd
 * width, so a row starts on either the high or the low nibble of a pool byte) and
 * a 600x2 image (wider than one unpack span, so ::ra8_book_src_image_rect must
 * stitch several bounded ::ra8_book_src_read spans per row); each pool nibble is
 * the low 4 bits of the pixel's flat index, so a reader knows the exact byte every
 * pixel must expand to. The third is an 80x3 @ref k_ra8_book_pixfmt_gray8 image --
 * the full-resolution, continuous-tone representation the compiled `.rabook`
 * retains for zoomable content (#476) -- whose pixels take deliberately off-grid
 * values (not on the 16-level gray4 quantiser), proving the gray8 read returns the
 * source byte verbatim with no quantisation and that it crosses a paged frame
 * boundary (80 > the 64-byte fixture frame).
 *
 * The addressing contract (descriptor stride, pool base, nibble packing, odd-width
 * parity) used to be open-coded in ereader_shelf's sh_image.c; #342 moved it into
 * the library. These tests own the library-level acceptance bar: the descriptor
 * read and the sub-rect pixel read are exercised through BOTH a resident source
 * (`base + off`) and a paged source (an ::ra8_vmem cache over an ::ra8_vsource
 * object), and the two must produce byte-for-byte identical gray8, so the loupe /
 * cover / thumbnail renderers stay pixel-identical after the move.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_book.h"
#include "ra8_book_paged.h"
#include "ra8_err.h"
#include "ra8_vmem.h"
#include "ra8_vsource.h"
#include "unity_minimal.h"

/**
 * @enum ibook_dim_t
 * @brief Fixture image geometry and pool / buffer capacities (no magic numbers).
 */
typedef enum : uint16_t {
  k_img0_w      = 5U,    /**< Image 0 width (odd -> exercises nibble parity).   */
  k_img0_h      = 3U,    /**< Image 0 height.                                   */
  k_img0_bytes  = 8U,    /**< Packed bytes for image 0 (ceil(5*3 / 2)).         */
  k_img1_w      = 600U,  /**< Image 1 width (> one unpack span -> chunking).    */
  k_img1_h      = 2U,    /**< Image 1 height.                                   */
  k_img1_off    = 8U,    /**< Image 1 pool offset (== image 0 byte count).      */
  k_img1_bytes  = 600U,  /**< Packed bytes for image 1 (600 * 2 / 2).           */
  k_img2_w      = 80U,   /**< Image 2 width (gray8; > 64B frame -> paged span). */
  k_img2_h      = 3U,    /**< Image 2 height.                                   */
  k_img2_off    = 608U,  /**< Image 2 pool offset (== img1_off + img1_bytes).   */
  k_img2_bytes  = 240U,  /**< gray8 bytes for image 2 (80 * 3, 1 byte/pixel).   */
  k_img2_sub_w  = 70U,   /**< gray8 sub-rect width (> 64B frame -> paged span). */
  k_pool_cap    = 1024U, /**< Image-pool capacity (>= img2_off + 240).          */
  k_strings_cap = 32U,   /**< String-pool capacity of the fixture.              */
  k_out_cap     = 1300U, /**< gray8 output buffer (>= 600 * 2 for a full read). */
} ibook_dim_t;

/**
 * @enum ibook_nib_t
 * @brief 4bpp nibble packing constants for the fixture packer / verifier.
 */
typedef enum : uint8_t {
  k_nib_mask = 0x0FU, /**< Low-nibble value mask.                         */
  k_nib_lo   = 0x0FU, /**< Low-nibble slot mask (clears the high nibble). */
  k_nib_hi   = 0xF0U, /**< High-nibble slot mask (clears the low nibble). */
  k_nib_sh   = 4U,    /**< Nibble shift and 4->8 bit replicate amount.    */
} ibook_nib_t;

/**
 * @enum ibook_fill_t
 * @brief Output-buffer poison fills -- prove the unpack overwrites every byte.
 */
typedef enum : uint8_t {
  k_fill_poison_a = 0xAAU, /**< Poison pattern A (10101010).                  */
  k_fill_poison_b = 0x55U, /**< Poison pattern B (01010101), distinct from A. */
} ibook_fill_t;

/**
 * @enum ibook_crc_t
 * @brief CRC-32/ISO-HDLC constants (must match ra8_book_validate).
 */
typedef enum : uint32_t {
  k_crc32_init           = 0xFFFFFFFFU, /**< CRC-32 initial value and final XOR-out. */
  k_crc32_poly_reflected = 0xEDB88320U, /**< The reflected CRC-32 polynomial.        */
} ibook_crc_t;

/** @brief CRC-32/ISO-HDLC over the blob body (matches ra8_book_validate). */
static uint32_t ibook_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = k_crc32_init;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ (k_crc32_poly_reflected & mask);
    }
  }
  return crc ^ k_crc32_init;
}

/**
 * @brief Self-contained `.rabook` blob: a 1-node DOM plus a 2-image pool.
 * @details Contiguous fixed-layout record so byte offsets address the same bytes
 *          a real inflated blob would; the paged backing reads these bytes.
 */
typedef struct {
  ra8_book_header_t  hdr;                    /**< Hdr.                        */
  ra8_book_chapter_t chapters[1];            /**< Chapters.                   */
  ra8_book_node_t    nodes[1];               /**< Nodes.                      */
  ra8_book_image_t   images[3];              /**< Images (2 gray4 + 1 gray8). */
  char               strings[k_strings_cap]; /**< Strings.                    */
  uint8_t            pool[k_pool_cap];       /**< Pool.                       */
} ibook_t;

/** @brief Nibble value of pixel `(px, py)` for a @p w-wide fixture image. */
static uint8_t ibook_nib(uint32_t w, uint32_t px, uint32_t py)
{
  return (uint8_t)(((py * w) + px) & (uint32_t)k_nib_mask);
}

/** @brief Gray8 value ra8_book_src_image_rect must produce for pixel `(px, py)`. */
static uint8_t ibook_expect_g8(uint32_t w, uint32_t px, uint32_t py)
{
  const uint8_t nib = ibook_nib(w, px, py);
  return (uint8_t)((nib << (uint8_t)k_nib_sh) | nib);
}

/** @brief Pack a @p w x @p h gray4 image into @p pool at @p data_off. */
static void ibook_pack(uint8_t* pool, uint32_t data_off, uint32_t w, uint32_t h)
{
  for (uint32_t py = 0U; py < h; ++py) {
    for (uint32_t px = 0U; px < w; ++px) {
      const uint32_t flat = (py * w) + px;
      const uint8_t  nib  = ibook_nib(w, px, py);
      uint8_t*       b    = &pool[data_off + (flat >> 1U)];
      if ((flat & 1U) == 0U) {
        *b = (uint8_t)((*b & (uint8_t)k_nib_lo) | (uint8_t)(nib << (uint8_t)k_nib_sh));
      } else {
        *b = (uint8_t)((*b & (uint8_t)k_nib_hi) | nib);
      }
    }
  }
}

/** @brief Compose one gray4 image descriptor row for the fixture. */
static ra8_book_image_t ibook_img(uint16_t w, uint16_t h, uint32_t data_off, uint32_t data_size)
{
  return (ra8_book_image_t){.id_off       = 0U,
                            .width        = w,
                            .height       = h,
                            .format       = (uint8_t)k_ra8_book_image_gray4,
                            .pixel_format = (uint8_t)k_ra8_book_pixfmt_gray4,
                            .data_off     = data_off,
                            .data_size    = data_size,
                            .raw_size     = data_size};
}

/** @enum ibook_g8_t @brief gray8 continuous-tone fixture generator constants. */
typedef enum : uint16_t {
  k_g8_mul  = 37U,  /**< Odd multiplier: values stride the full 0-255 range.   */
  k_g8_bias = 5U,   /**< Bias so pixel (0,0) is off the gray4 grid (5, not 0). */
  k_g8_byte = 256U, /**< gray8 modulus.                                        */
} ibook_g8_t;

/**
 * @brief Continuous-tone gray8 value of pixel `(px, py)` for a @p w-wide image.
 * @details `(flat * 37 + 5) mod 256` -- deliberately off the 16-level gray4 grid
 *          {0,17,...,255}, so a gray4 round-trip would perturb it; the gray8 read
 *          returning this exact byte proves no quantisation happened (#476).
 */
static uint8_t ibook_g8(uint32_t w, uint32_t px, uint32_t py)
{
  const uint32_t flat = (py * w) + px;
  return (uint8_t)(((flat * (uint32_t)k_g8_mul) + (uint32_t)k_g8_bias) % (uint32_t)k_g8_byte);
}

/** @brief Pack a @p w x @p h gray8 image (1 byte/pixel) into @p pool at @p data_off. */
static void ibook_pack_gray8(uint8_t* pool, uint32_t data_off, uint32_t w, uint32_t h)
{
  for (uint32_t py = 0U; py < h; ++py) {
    for (uint32_t px = 0U; px < w; ++px) {
      pool[data_off + (py * w) + px] = ibook_g8(w, px, py);
    }
  }
}

/** @brief Compose one gray8 raster descriptor (format gray4-tag, depth gray8). */
static ra8_book_image_t
ibook_img_gray8(uint16_t w, uint16_t h, uint32_t data_off, uint32_t data_size)
{
  return (ra8_book_image_t){.id_off       = 0U,
                            .width        = w,
                            .height       = h,
                            .format       = (uint8_t)k_ra8_book_image_gray4,
                            .pixel_format = (uint8_t)k_ra8_book_pixfmt_gray8,
                            .data_off     = data_off,
                            .data_size    = data_size,
                            .raw_size     = data_size};
}

/** @brief Populate a valid 2-image fixture blob (header, tables, packed pool). */
static void ibook_setup(ibook_t* b)
{
  memset(b, 0, sizeof(*b));
  memcpy(b->hdr.magic, "RABOOK1", 8);
  b->hdr.format_version = k_ra8_book_format_version;
  b->hdr.total_size     = (uint32_t)sizeof(*b);

  b->hdr.chapter_count     = 1U;
  b->hdr.chapter_off       = (uint32_t)offsetof(ibook_t, chapters);
  b->hdr.node_count        = 1U;
  b->hdr.node_off          = (uint32_t)offsetof(ibook_t, nodes);
  b->hdr.attr_count        = 0U;
  b->hdr.attr_off          = (uint32_t)offsetof(ibook_t, strings);
  b->hdr.stylesheet_count  = 0U;
  b->hdr.stylesheet_off    = (uint32_t)offsetof(ibook_t, strings);
  b->hdr.image_count       = 3U;
  b->hdr.image_off         = (uint32_t)offsetof(ibook_t, images);
  b->hdr.string_off        = (uint32_t)offsetof(ibook_t, strings);
  b->hdr.string_size       = (uint32_t)sizeof(b->strings);
  b->hdr.image_pool_off    = (uint32_t)offsetof(ibook_t, pool);
  b->hdr.image_pool_size   = (uint32_t)sizeof(b->pool);
  b->hdr.cover_image_index = k_ra8_book_nil;

  b->chapters[0].root_node = 0U;
  b->nodes[0]              = (ra8_book_node_t){.kind         = k_ra8_book_node_text,
                                               .text_off     = 0U,
                                               .first_child  = k_ra8_book_nil,
                                               .next_sibling = k_ra8_book_nil};

  b->images[0] = ibook_img((uint16_t)k_img0_w, (uint16_t)k_img0_h, 0U, (uint32_t)k_img0_bytes);
  b->images[1] = ibook_img((uint16_t)k_img1_w,
                           (uint16_t)k_img1_h,
                           (uint32_t)k_img1_off,
                           (uint32_t)k_img1_w * (uint32_t)k_img1_h / 2U);
  b->images[2] = ibook_img_gray8((uint16_t)k_img2_w,
                                 (uint16_t)k_img2_h,
                                 (uint32_t)k_img2_off,
                                 (uint32_t)k_img2_bytes);
  ibook_pack(b->pool, 0U, (uint32_t)k_img0_w, (uint32_t)k_img0_h);
  ibook_pack(b->pool, (uint32_t)k_img1_off, (uint32_t)k_img1_w, (uint32_t)k_img1_h);
  ibook_pack_gray8(b->pool, (uint32_t)k_img2_off, (uint32_t)k_img2_w, (uint32_t)k_img2_h);

  const uint8_t* body     = (const uint8_t*)b + sizeof(ra8_book_header_t);
  const uint32_t body_len = (uint32_t)(sizeof(*b) - sizeof(ra8_book_header_t));
  b->hdr.crc32            = ibook_crc32(body, body_len);
}

/* --- paged backing: read straight from the fixture blob bytes --------------- */

/** @brief The fixture blob the paged backing reads from. */
static const uint8_t* s_ibook_bytes;

/** @brief Length of ::s_ibook_bytes. */
static uint32_t s_ibook_len;

/** @brief When set, ::ibook_read fails any backing read at/after the threshold. */
static bool s_ibook_fault_armed;

/** @brief Byte offset at/after which ::ibook_read returns an error when armed. */
static uint32_t s_ibook_fault_off;

/** @brief ra8_vsource read callback: copy a byte range out of the fixture blob,
 *         or fault at/after ::s_ibook_fault_off when the fault is armed. */
static ra8_err_t ibook_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len)
{
  (void)ctx;
  if (s_ibook_fault_armed && (offset >= (uint64_t)s_ibook_fault_off)) {
    return k_ra8_err_validation_failed;
  }
  if ((offset + (uint64_t)len) > (uint64_t)s_ibook_len) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf, s_ibook_bytes + (size_t)offset, len);
  return k_ra8_ok;
}

/* Tiny page cache: 8 frames x 64 bytes < the fixture, so paged reads evict. */
typedef enum : uint32_t {
  k_ib_frame_bytes = 64U, /**< Ib frame bytes. */
  k_ib_frame_count = 8U,  /**< Ib frame count. */
  k_ib_buckets     = 16U, /**< Ib buckets.     */
} ibook_cache_dim_t;

static uint8_t          s_ib_frames[(size_t)k_ib_frame_count * (size_t)k_ib_frame_bytes];
static ra8_vmem_frame_t s_ib_meta[k_ib_frame_count];
static ra8_vmem_key_t   s_ib_keys[k_ib_frame_count];
static int32_t          s_ib_buckets[k_ib_buckets];

/** @brief Bind a paged book source over the shared cache + s_ibook backing. */
static void
ibook_bind_paged(ra8_book_src_t* psrc, ra8_vsource_t* vs, ra8_vsource_obj_t* objs, ra8_vmem_t* vm)
{
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vsource_init(vs, objs, 1U));
  uint32_t oid = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_vsource_add_paged(vs, ibook_read, nullptr, 0U, (uint64_t)s_ibook_len, &oid));

  ra8_vmem_cfg_t cfg = {
    .frame_mem    = s_ib_frames,
    .frame_bytes  = (uint32_t)k_ib_frame_bytes,
    .frame_count  = (uint32_t)k_ib_frame_count,
    .meta         = s_ib_meta,
    .keys         = s_ib_keys,
    .buckets      = s_ib_buckets,
    .bucket_count = (uint32_t)k_ib_buckets,
    .loader       = ra8_vsource_loader,
    .loader_ctx   = vs,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_init(vm, &cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_src_paged(psrc, vm, oid, (uint32_t)k_ib_frame_bytes, s_ibook_len));
}

/** @brief Assert the @p w x @p h window at (@p x, @p y) of a @p iw-wide image
 *         unpacked to the exact gray8 the fixture packer wrote, at @p stride. */
static void ibook_verify_rect(uint32_t       iw,
                              uint32_t       x,
                              uint32_t       y,
                              uint32_t       w,
                              uint32_t       h,
                              const uint8_t* out,
                              uint32_t       stride)
{
  for (uint32_t r = 0U; r < h; ++r) {
    for (uint32_t c = 0U; c < w; ++c) {
      TEST_ASSERT_EQ(ibook_expect_g8(iw, x + c, y + r), out[(r * stride) + c]);
    }
  }
}

/**
 * @test test_ra8_book_src_image_descriptor
 * @brief ra8_book_src_image reads a descriptor identically resident and paged.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the index guard `idx >= image_count` and
 * the two null guards are each independent single-condition checks)
 */
static void test_ra8_book_src_image_descriptor(void)
{
  TEST_BEGIN("ra8_book_src_image descriptor read");

  static ibook_t s_book;
  ibook_setup(&s_book);
  s_ibook_bytes       = (const uint8_t*)&s_book;
  s_ibook_len         = (uint32_t)sizeof(s_book);
  s_ibook_fault_armed = false;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(&s_book, sizeof(s_book)));

  ra8_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_resident(&rsrc, &s_book, (uint32_t)sizeof(s_book)));

  ra8_book_image_t img = {};
  /* Null + range guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_book_src_image(nullptr, 0U, &img));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_book_src_image(&rsrc, 0U, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_book_src_image(&rsrc, 3U, &img)); /* == image_count */

  /* Every descriptor read matches the resident inline accessor byte-for-byte. */
  const ra8_book_image_t* inl = ra8_book_images(&s_book);
  for (uint32_t i = 0U; i < s_book.hdr.image_count; ++i) {
    ra8_book_image_t got = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_image(&rsrc, i, &got));
    TEST_ASSERT_EQ(0, memcmp(&got, &inl[i], sizeof got));
  }

  /* Paged source returns the identical descriptor bytes. */
  ra8_vsource_obj_t objs[1];
  ra8_vsource_t     vs   = {};
  ra8_vmem_t        vm   = {};
  ra8_book_src_t    psrc = {};
  ibook_bind_paged(&psrc, &vs, objs, &vm);
  for (uint32_t i = 0U; i < s_book.hdr.image_count; ++i) {
    ra8_book_image_t got = {};
    TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_image(&psrc, i, &got));
    TEST_ASSERT_EQ(0, memcmp(&got, &inl[i], sizeof got));
  }

  TEST_END("ra8_book_src_image descriptor read");
}

/** @brief Read the @p w x @p h window at (@p x, @p y) from both @p rsrc and
 *         @p psrc, assert it unpacks to the fixture's exact gray8 (a @p iw-wide
 *         image), and assert the resident and paged bytes are byte-identical. */
static void ibook_rect_both(const ra8_book_src_t*   rsrc,
                            const ra8_book_src_t*   psrc,
                            const ra8_book_image_t* img,
                            uint32_t                iw,
                            uint32_t                x,
                            uint32_t                y,
                            uint32_t                w,
                            uint32_t                h)
{
  static uint8_t s_out_r[k_out_cap];
  static uint8_t s_out_p[k_out_cap];
  memset(s_out_r, k_fill_poison_a, sizeof s_out_r);
  memset(s_out_p, k_fill_poison_b, sizeof s_out_p);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_image_rect(rsrc, img, x, y, w, h, s_out_r, w));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_image_rect(psrc, img, x, y, w, h, s_out_p, w));
  ibook_verify_rect(iw, x, y, w, h, s_out_r, w);
  TEST_ASSERT_EQ(0, memcmp(s_out_r, s_out_p, (size_t)w * (size_t)h));
}

/**
 * @test test_ra8_book_src_image_rect_matches
 * @brief Full, sub-rect and wide (chunked) reads unpack to the exact gray8,
 *        byte-identical between a resident and a paged source.
 *
 * @par MC/DC:
 * (no compound decisions under test here -- this test owns the byte-identity of
 * the unpack; the compound bound guards are exercised by
 * test_mcdc_ra8_book_src_image_rect_guards)
 */
static void test_ra8_book_src_image_rect_matches(void)
{
  TEST_BEGIN("ra8_book_src_image_rect unpack matches");

  static ibook_t s_book;
  ibook_setup(&s_book);
  s_ibook_bytes       = (const uint8_t*)&s_book;
  s_ibook_len         = (uint32_t)sizeof(s_book);
  s_ibook_fault_armed = false;

  ra8_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_resident(&rsrc, &s_book, (uint32_t)sizeof(s_book)));
  ra8_vsource_obj_t objs[1];
  ra8_vsource_t     vs   = {};
  ra8_vmem_t        vm   = {};
  ra8_book_src_t    psrc = {};
  ibook_bind_paged(&psrc, &vs, objs, &vm);

  ra8_book_image_t img0 = {};
  ra8_book_image_t img1 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_image(&rsrc, 0U, &img0));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_image(&rsrc, 1U, &img1));

  /* (A) Full image 0 (odd width -> both nibble parities), resident == paged. */
  ibook_rect_both(&rsrc, &psrc, &img0, k_img0_w, 0U, 0U, k_img0_w, k_img0_h);
  /* (B) Full image 1: 600 px/row forces the multi-span chunk path. */
  ibook_rect_both(&rsrc, &psrc, &img1, k_img1_w, 0U, 0U, k_img1_w, k_img1_h);

  /* (C) Sub-rect of image 0 written into a WIDER buffer (out_stride > w). */
  static uint8_t s_out[k_out_cap];
  memset(s_out, k_fill_poison_a, sizeof s_out);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_src_image_rect(&rsrc, &img0, 1U, 1U, 3U, 2U, s_out, (uint32_t)k_img0_w));
  ibook_verify_rect(k_img0_w, 1U, 1U, 3U, 2U, s_out, (uint32_t)k_img0_w);

  /* The paged cache actually paged (misses + evictions on the tiny budget). */
  uint32_t hits = 0U;
  uint32_t miss = 0U;
  uint32_t evic = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_vmem_stats(&vm, &hits, &miss, &evic));
  TEST_ASSERT(miss > 0U);
  TEST_ASSERT(evic > 0U);

  TEST_END("ra8_book_src_image_rect unpack matches");
}

/** @brief Drive the three null-pointer guards of ra8_book_src_image_rect and the
 *         merged "readable raster of a known depth" decision (see the @par MC/DC on
 *         the guards test): an SVG (Vector 1) and an unknown depth (Vector 4) are
 *         rejected; a gray8 raster (Vector 3) proceeds. The gray4 case (Vector 2)
 *         is the caller's all-false rect read. Split out so the guards test stays
 *         within the function-size budget. */
static void
ibook_guard_singleconds(const ra8_book_src_t* rsrc, const ra8_book_image_t* img, uint8_t* s_out)
{
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_book_src_image_rect(nullptr, img, 0U, 0U, k_img0_w, k_img0_h, s_out, k_img0_w));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_book_src_image_rect(rsrc, nullptr, 0U, 0U, k_img0_w, k_img0_h, s_out, k_img0_w));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_book_src_image_rect(rsrc, img, 0U, 0U, k_img0_w, k_img0_h, nullptr, k_img0_w));

  /* Vector 1: an SVG (non-raster) descriptor -> A true -> rejected. */
  ra8_book_image_t svg = *img;
  svg.format           = (uint8_t)k_ra8_book_image_svg;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_src_image_rect(rsrc, &svg, 0U, 0U, k_img0_w, k_img0_h, s_out, k_img0_w));

  /* Vector 3: a gray8 raster -> A false, B true, C false -> proceeds. */
  ra8_book_image_t g8 = *img;
  g8.pixel_format     = (uint8_t)k_ra8_book_pixfmt_gray8;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_book_src_image_rect(rsrc, &g8, 0U, 0U, k_img0_w, k_img0_h, s_out, k_img0_w));

  /* Vector 4: a raster with an unknown depth -> A false, B true, C true -> rejected. */
  ra8_book_image_t unk = *img;
  unk.pixel_format     = (uint8_t)(k_ra8_book_pixfmt_gray8 + 1U);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_book_src_image_rect(rsrc, &unk, 0U, 0U, k_img0_w, k_img0_h, s_out, k_img0_w));
}

/**
 * @test test_mcdc_ra8_book_src_image_rect_guards
 * @brief The two compound bound guards reject malformed rectangles.
 *
 * @par MC/DC:
 * Decision: `(w == 0U) || (h == 0U) || (out_stride < w)` (3 conditions) in
 * libs/ra8_book/src/ra8_book_paged.c@ra8_book_src_image_rect:
 * - Vector 1: w=5, h=3, stride=5 -> false (control: all three false, proceeds).
 * - Vector 2: w=0, h=3, stride=5 -> true  (varies w only        -> invalid_arg).
 * - Vector 3: w=5, h=0, stride=5 -> true  (varies h only        -> invalid_arg).
 * - Vector 4: w=5, h=3, stride=4 -> true  (varies out_stride only-> invalid_arg).
 * Vectors 1+2 / 1+3 / 1+4 each prove one condition independently drives the
 * outcome; N+1 = 4 vectors for N = 3 conditions.
 *
 * @par MC/DC:
 * Decision: `((x + w) > img->width) || ((y + h) > img->height)` (2 conditions)
 * in libs/ra8_book/src/ra8_book_paged.c@ra8_book_src_image_rect:
 * - Vector 1: x=0,w=5 / y=0,h=3 -> false (in bounds, proceeds to unpack).
 * - Vector 2: x=1,w=5 / y=0,h=3 -> true  (varies x+w only  -> out_of_range).
 * - Vector 3: x=0,w=5 / y=1,h=3 -> true  (varies y+h only  -> out_of_range).
 * Vectors 1+2 prove the width bound, 1+3 the height bound; N+1 = 3 vectors.
 *
 * @par MC/DC:
 * Decision: `(img->format != k_ra8_book_image_gray4) ||
 *           ((pf != k_ra8_book_pixfmt_gray4) && (pf != k_ra8_book_pixfmt_gray8))`
 * -- 3 conditions, `A || (B && C)` -- in
 * libs/ra8_book/src/ra8_book_paged.c@ra8_book_src_image_rect, where
 * `pf = ra8_book_image_pixfmt(img)`: the "readable raster of a known depth" guard
 * (#476), with A the SVG (non-raster) reject and B && C the unknown-depth reject:
 * - Vector 1: format=SVG          -> A true, short-circuit      -> invalid_arg.
 * - Vector 2: gray4 raster (pf=0) -> A false, B false           -> proceeds.
 * - Vector 3: gray8 raster (pf=1) -> A false, B true, C false   -> proceeds.
 * - Vector 4: raster, pf=2        -> A false, B true, C true    -> invalid_arg.
 * Vectors 1+2 isolate A, 2+4 isolate B, 3+4 isolate C; N+1 = 4 for N = 3. Vectors
 * 1, 3, 4 are driven in ibook_guard_singleconds; Vector 2 (gray4) is the all-false
 * rect read in decision 1 below.
 */
static void test_mcdc_ra8_book_src_image_rect_guards(void)
{
  TEST_BEGIN("ra8_book_src_image_rect bound guards (MC/DC)");

  static ibook_t s_book;
  ibook_setup(&s_book);
  s_ibook_bytes       = (const uint8_t*)&s_book;
  s_ibook_len         = (uint32_t)sizeof(s_book);
  s_ibook_fault_armed = false;

  ra8_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_resident(&rsrc, &s_book, (uint32_t)sizeof(s_book)));
  ra8_book_image_t img = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_image(&rsrc, 0U, &img));

  static uint8_t s_out[k_out_cap];

  ibook_guard_singleconds(&rsrc, &img, s_out);

  /* Decision 1: (w==0) || (h==0) || (out_stride<w). */
  TEST_ASSERT_EQ(k_ra8_ok, /* V1: all false */
                 ra8_book_src_image_rect(&rsrc, &img, 0U, 0U, k_img0_w, k_img0_h, s_out, k_img0_w));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, /* V2: w==0 */
                 ra8_book_src_image_rect(&rsrc, &img, 0U, 0U, 0U, k_img0_h, s_out, k_img0_w));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, /* V3: h==0 */
                 ra8_book_src_image_rect(&rsrc, &img, 0U, 0U, k_img0_w, 0U, s_out, k_img0_w));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, /* V4: out_stride < w */
                 ra8_book_src_image_rect(&rsrc,
                                         &img,
                                         0U,
                                         0U,
                                         k_img0_w,
                                         k_img0_h,
                                         s_out,
                                         (uint32_t)k_img0_w - 1U));

  /* Decision 2: ((x+w) > width) || ((y+h) > height). */
  TEST_ASSERT_EQ(k_ra8_ok, /* V1: both in bounds (same call as above proves false) */
                 ra8_book_src_image_rect(&rsrc, &img, 0U, 0U, k_img0_w, k_img0_h, s_out, k_img0_w));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, /* V2: x+w > width */
                 ra8_book_src_image_rect(&rsrc, &img, 1U, 0U, k_img0_w, k_img0_h, s_out, k_img0_w));
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, /* V3: y+h > height */
                 ra8_book_src_image_rect(&rsrc, &img, 0U, 1U, k_img0_w, k_img0_h, s_out, k_img0_w));

  TEST_END("ra8_book_src_image_rect bound guards (MC/DC)");
}

/** @brief Assert the @p w x @p h gray8 window at (@p x, @p y) of a @p iw-wide
 *         image read back the exact source byte the fixture wrote, at @p stride. */
static void ibook_verify_rect_g8(uint32_t       iw,
                                 uint32_t       x,
                                 uint32_t       y,
                                 uint32_t       w,
                                 uint32_t       h,
                                 const uint8_t* out,
                                 uint32_t       stride)
{
  for (uint32_t r = 0U; r < h; ++r) {
    for (uint32_t c = 0U; c < w; ++c) {
      TEST_ASSERT_EQ(ibook_g8(iw, x + c, y + r), out[(r * stride) + c]);
    }
  }
}

/**
 * @test test_ra8_book_src_image_rect_gray8
 * @brief A gray8 raster reads back its full-resolution, continuous-tone source
 *        bytes verbatim -- resident == paged -- with no quantisation (#476).
 *
 * @details The core #476 acceptance at the library level: the compiled `.rabook`
 *          retains full-resolution gray8 for zoomable content, and the sub-rect
 *          reader serves it 1 byte/pixel with no gray4 nibble-pack in the path.
 *          Image 2 is 80x3 (wider than the 64-byte paged frame, so a row crosses a
 *          frame boundary) and its pixels are off the 16-level gray4 grid, so a
 *          verbatim read-back is proof the depth was preserved, not quantised.
 *
 * @par MC/DC:
 * (no compound decisions under test here -- this owns the gray8 byte-fidelity of
 * the read; the depth-dispatch guard is covered by
 * test_mcdc_ra8_book_src_image_rect_guards)
 */
static void test_ra8_book_src_image_rect_gray8(void)
{
  TEST_BEGIN("ra8_book_src_image_rect gray8 full-resolution read");

  static ibook_t s_book;
  ibook_setup(&s_book);
  s_ibook_bytes       = (const uint8_t*)&s_book;
  s_ibook_len         = (uint32_t)sizeof(s_book);
  s_ibook_fault_armed = false;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_validate(&s_book, sizeof(s_book)));

  ra8_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_resident(&rsrc, &s_book, (uint32_t)sizeof(s_book)));
  ra8_vsource_obj_t objs[1];
  ra8_vsource_t     vs   = {};
  ra8_vmem_t        vm   = {};
  ra8_book_src_t    psrc = {};
  ibook_bind_paged(&psrc, &vs, objs, &vm);

  ra8_book_image_t img2 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_image(&rsrc, 2U, &img2));
  TEST_ASSERT_EQ(k_ra8_book_image_gray4, img2.format); /* raster tag */
  TEST_ASSERT_EQ(k_ra8_book_pixfmt_gray8, ra8_book_image_pixfmt(&img2));
  TEST_ASSERT_EQ(k_img2_w * (uint32_t)k_img2_h, img2.raw_size); /* 1 byte/pixel */

  static uint8_t s_out_r[k_out_cap];
  static uint8_t s_out_p[k_out_cap];

  /* (A) Full image: resident and paged both return the exact source bytes. */
  memset(s_out_r, k_fill_poison_a, sizeof s_out_r);
  memset(s_out_p, k_fill_poison_b, sizeof s_out_p);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_book_src_image_rect(&rsrc, &img2, 0U, 0U, k_img2_w, k_img2_h, s_out_r, k_img2_w));
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_book_src_image_rect(&psrc, &img2, 0U, 0U, k_img2_w, k_img2_h, s_out_p, k_img2_w));
  ibook_verify_rect_g8(k_img2_w, 0U, 0U, k_img2_w, k_img2_h, s_out_r, k_img2_w);
  TEST_ASSERT_EQ(0, memcmp(s_out_r, s_out_p, (size_t)k_img2_w * (size_t)k_img2_h));

  /* (B) Continuous tone survives: a sample sits strictly between two gray4 levels
   *     {0,17,34,...}, so a gray4 quantise would have moved it. */
  const uint8_t v0 = ibook_g8(k_img2_w, 0U, 0U);
  TEST_ASSERT_EQ(v0, s_out_r[0]);
  TEST_ASSERT((v0 % 17U) != 0U); /* off the 16-level grid -> not a gray4 value */

  /* (C) Sub-rect into a wider buffer (out_stride > w), crossing a frame boundary. */
  static uint8_t s_out[k_out_cap];
  memset(s_out, k_fill_poison_a, sizeof s_out);
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_book_src_image_rect(&rsrc, &img2, 3U, 1U, k_img2_sub_w, 2U, s_out, (uint32_t)k_img2_w));
  ibook_verify_rect_g8(k_img2_w, 3U, 1U, k_img2_sub_w, 2U, s_out, (uint32_t)k_img2_w);

  TEST_END("ra8_book_src_image_rect gray8 full-resolution read");
}

/**
 * @test test_ra8_book_src_image_rect_faults
 * @brief A pool span past the blob and a paged backing fault both propagate.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the per-row `(off + n) > src->size`
 * out-of-range check and the read-fault passthrough are single-condition checks
 * in the internal priv_book_image_row helper)
 */
static void test_ra8_book_src_image_rect_faults(void)
{
  TEST_BEGIN("ra8_book_src_image_rect fault propagation");

  static ibook_t s_book;
  ibook_setup(&s_book);
  s_ibook_bytes       = (const uint8_t*)&s_book;
  s_ibook_len         = (uint32_t)sizeof(s_book);
  s_ibook_fault_armed = false;

  ra8_book_src_t rsrc = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_resident(&rsrc, &s_book, (uint32_t)sizeof(s_book)));

  static uint8_t s_out[k_out_cap];

  /* (A) A descriptor whose data_off addresses past the pool -> the per-row span
   *     check leaves the blob and returns out_of_range (resident source). */
  ra8_book_image_t past = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_image(&rsrc, 0U, &past));
  past.data_off = (uint32_t)sizeof(s_book.pool); /* pool_off + data_off == total_size */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_book_src_image_rect(&rsrc, &past, 0U, 0U, k_img0_w, 1U, s_out, k_img0_w));

  /* (B) A paged backing that faults every cold frame beyond the 100-byte header:
   *     the header (bound) and the image descriptor (read below, both while the
   *     fault is disarmed) stay resident, but the rect's cold pixel-read frame
   *     faults in the loader and the error propagates through the unpack. The
   *     loader fetches whole frames from a frame-aligned base, so the threshold
   *     is a frame boundary past the header, not the (mid-frame) pool offset. */
  ra8_vsource_obj_t objs[1];
  ra8_vsource_t     vs   = {};
  ra8_vmem_t        vm   = {};
  ra8_book_src_t    psrc = {};
  ibook_bind_paged(&psrc, &vs, objs, &vm);

  ra8_book_image_t img0 = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_book_src_image(&psrc, 0U, &img0));

  s_ibook_fault_off   = (uint32_t)k_ib_frame_bytes * 2U; /* frames 0,1 hold the header */
  s_ibook_fault_armed = true;
  const ra8_err_t e =
    ra8_book_src_image_rect(&psrc, &img0, 0U, 0U, k_img0_w, k_img0_h, s_out, k_img0_w);
  TEST_ASSERT(e != k_ra8_ok); /* the loader fault propagated through the unpack */
  s_ibook_fault_armed = false;

  TEST_END("ra8_book_src_image_rect fault propagation");
}

int32_t main(void)
{
  test_ra8_book_src_image_descriptor();
  test_ra8_book_src_image_rect_matches();
  test_mcdc_ra8_book_src_image_rect_guards();
  test_ra8_book_src_image_rect_gray8();
  test_ra8_book_src_image_rect_faults();
  (void)fprintf(stderr, "[OK ] test_ra8_book_image_rect.c\n");
  return 0;
}
