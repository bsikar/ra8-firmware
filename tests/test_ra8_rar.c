/**
 * @file test_ra8_rar.c
 * @brief Tests for the clean-room RAR reader (ra8_rar) and the CBR comic backend.
 *
 * @details
 * Hand-crafts real RAR archives from the public block layout -- a RAR5 archive
 * (vint headers) and a RAR4 archive (fixed-field headers), each holding
 * STORE-method PNG page images built by comic_fixture.h -- then walks them two
 * ways:
 *
 *   1. directly through `ra8_rar_open` / `ra8_rar_next` (version detection, block
 *      iteration, STORE extraction, and the compressed / directory guards), and
 *   2. through the `ra8_comic` facade opened by magic, proving a `.cbr` pages,
 *      sorts, extracts, and decodes exactly like a `.cbz` -- and that a
 *      RAR-compressed member is reported unsupported rather than mis-decoded.
 *
 * The archive builders here are a legitimate STORE-only writer of the public
 * format; they vendor no `unrar` code (that license restriction is on RARLAB's
 * decompressor source, which this reader does not use).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "comic_fixture.h"
#include "ra8_comic.h"
#include "ra8_err.h"
#include "ra8_rar.h"
#include "unity_minimal.h"

/** @brief RAR main-archive-header layout used by the fixture builder. */
typedef enum : uint8_t {
  k_tr_off_high_pos_av = 7U, /**< HighPosAV + PosAV field offset.   */
  k_tr_len_high_pos_av = 6U, /**< HighPosAV + PosAV combined width. */
  k_tr_len_signature   = 5U, /**< RAR4 signature length, bytes.     */
} tr_layout_t;

/**
 * @enum rar_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_rar_b_80           = 0x80U,
  k_rar_data_off_50    = 50U,
  k_rar_h_5            = 5U,
  k_rar_i_31           = 31U,
  k_rar_out_20         = 20U,
  k_rar_out_30         = 0x30U,
  k_rar_out_73         = 0x73U,
  k_rar_out_74         = 0x74U,
  k_rar_out_7b         = 0x7BU,
  k_rar_s_arc_size_100 = 100U,
  k_rar_tr_le16_13     = 13U,
  k_rar_tr_le16_26     = 26,
  k_rar_tr_le16_5      = 5,
  k_rar_tr_le32_11     = 11,
  k_rar_tr_le32_20     = 20,
  k_rar_tr_le32_28     = 28,
  k_rar_tr_le32_7      = 7,
  k_rar_v_24           = 24,
  k_rar_v_7f           = 0x7FU,
  k_rar_val_15         = 15,
  k_rar_val_25         = 25,
  k_rar_val_7          = 7U,
} rar_uint8_const_t;

/**
 * @enum rar_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_rar_data_off_990 = 990U,
  k_rar_tr_le16_8000 = 0x8000U,
  k_rar_val_256      = 256,
} rar_uint16_const_t;

/**
 * @enum tr_dim_t
 * @brief Fixture geometry + buffer budgets (tests are magic-number exempt).
 */
typedef enum : uint32_t {
  k_tr_img_count = 4U,           /**< PNG page images in the RAR5 fixture. */
  k_tr_page_cap  = 16U,          /**< Page-index capacity.                 */
  k_tr_name_cap  = 1024U,        /**< Name-arena capacity, bytes.          */
  k_tr_filler    = 100U * 1024U, /**< Big non-image STORE member.          */
  k_tr_arc_cap   = 256U * 1024U, /**< Archive build buffer.                */
  k_tr_name_buf  = 64U,          /**< Per-query name buffer.               */
  k_tr_body_max  = 256U,         /**< RAR5 header-body scratch.            */
} tr_dim_t;

/**
 * @struct tr_img_t
 * @brief One source page image: name, dimensions, and PNG bytes.
 */
typedef struct {
  const char* name;              /**< Member name.       */
  uint16_t    w;                 /**< Pixel width.       */
  uint16_t    h;                 /**< Pixel height.      */
  uint8_t     seed;              /**< Pixel seed.        */
  uint8_t     png[k_cf_png_max]; /**< Encoded PNG bytes. */
  size_t      plen;              /**< PNG byte length.   */
} tr_img_t;

/** @brief Source page images. */
static tr_img_t s_imgs[k_tr_img_count];
/** @brief Built archive bytes. */
static uint8_t s_arc[k_tr_arc_cap];
/** @brief Built archive length. */
static size_t s_arc_size = 0U;
/** @brief Bytes fetched through the read callback (bounded-RAM probe). */
static uint64_t g_fetched = 0U;
/** @brief Large non-image member payload. */
static uint8_t s_filler[k_tr_filler];

/** @brief Seek+read callback over ::s_arc; counts fetched bytes. */
static size_t tr_read(void* ctx, uint64_t off, void* buf, size_t len)
{
  (void)ctx;
  if (off >= (uint64_t)s_arc_size) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s_arc_size - off;
  const size_t   n     = (len > (size_t)avail) ? (size_t)avail : len;
  memcpy(buf, &s_arc[off], n);
  g_fetched += (uint64_t)n;
  return n;
}

/** @brief Find a source image by member name (NULL if absent). */
static const tr_img_t* tr_find(const char* name)
{
  for (uint32_t i = 0U; i < k_tr_img_count; ++i) {
    if (strcmp(s_imgs[i].name, name) == 0) {
      return &s_imgs[i];
    }
  }
  return nullptr;
}

/* ---- RAR5 encoders (public format; STORE-only writer) -------------------- */

/** @brief Encode one little-endian base-128 vint; return its byte length. */
static size_t tr_vint(uint8_t* out, uint64_t v)
{
  size_t i = 0U;
  for (;;) {
    uint8_t b = (uint8_t)(v & k_rar_v_7f);
    v >>= k_rar_val_7;
    if (v != 0U) {
      out[i] = (uint8_t)(b | k_rar_b_80);
      i++;
    } else {
      out[i] = b;
      i++;
      break;
    }
  }
  return i;
}

/** @brief Encode a RAR5 file/service block (STORE or, via @p cinfo, compressed). */
static size_t tr5_file(uint8_t*       out,
                       const char*    name,
                       const uint8_t* data,
                       size_t         dlen,
                       uint64_t       cinfo,
                       bool           is_dir)
{
  uint8_t        body[k_tr_body_max] = {};
  size_t         b                   = 0U;
  const bool     has_data            = (dlen > 0U);
  const uint64_t hflags              = has_data ? 0x02U : 0x00U; /* data-area present  */
  const uint64_t fflags              = is_dir ? 0x01U : 0x00U;   /* directory bit      */
  b += tr_vint(&body[b], 2U);                                    /* header type = file */
  b += tr_vint(&body[b], hflags);
  if (has_data) {
    b += tr_vint(&body[b], (uint64_t)dlen); /* data size */
  }
  b += tr_vint(&body[b], fflags);
  b += tr_vint(&body[b], (uint64_t)dlen); /* unpacked size (store: == data) */
  b += tr_vint(&body[b], 0U);             /* attributes                     */
  b += tr_vint(&body[b], cinfo);          /* compression info               */
  b += tr_vint(&body[b], 0U);             /* host os                        */
  const size_t nlen = strlen(name);
  b += tr_vint(&body[b], (uint64_t)nlen); /* name length */
  /* The RAR5 file-header name is a COUNTED field: the vint length written on
   * the line above is what the parser reads, and the name bytes that follow
   * carry no terminator. Copying strlen(name) + 1 here would push a stray NUL
   * into the next header field and corrupt the fixture, so the unterminated
   * copy is the format-correct one. */
  // NOLINTNEXTLINE(bugprone-not-null-terminated-result)
  memcpy(&body[b], name, nlen);
  b += nlen;

  size_t p = 4U; /* leave the header CRC32 as zero (reader does not verify it) */
  p += tr_vint(&out[p], (uint64_t)b);
  memcpy(&out[p], body, b);
  p += b;
  if (has_data) {
    memcpy(&out[p], data, dlen);
    p += dlen;
  }
  return p;
}

/** @brief Encode a RAR5 main/end block (@p htype 1 or 5). */
static size_t tr5_simple(uint8_t* out, uint64_t htype)
{
  uint8_t body[16] = {};
  size_t  b        = 0U;
  b += tr_vint(&body[b], htype);
  b += tr_vint(&body[b], 0U); /* header flags      */
  b += tr_vint(&body[b], 0U); /* archive/end flags */
  size_t p = 4U;
  p += tr_vint(&out[p], (uint64_t)b);
  memcpy(&out[p], body, b);
  p += b;
  return p;
}

/* ---- RAR4 encoders (public fixed-field layout; STORE-only writer) -------- */

/** @brief Write a little-endian uint16. */
static void tr_le16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

/** @brief Write a little-endian uint32. */
static void tr_le32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> k_rar_v_24);
}

/** @brief Encode a RAR4 STORE file block. */
static size_t tr4_file(uint8_t* out, const char* name, const uint8_t* data, size_t dlen)
{
  const size_t nlen = strlen(name);
  const size_t head = 32U + nlen;       /* fixed fields + name (no LARGE/salt/exttime) */
  tr_le16(&out[0], 0U);                 /* head_crc (unverified)                       */
  out[2] = k_rar_out_74;                /* type: file                                  */
  tr_le16(&out[3], k_rar_tr_le16_8000); /* flags: LONG (ADD_SIZE present)              */
  tr_le16(&out[k_rar_tr_le16_5], (uint16_t)head);
  tr_le32(&out[k_rar_tr_le32_7], (uint32_t)dlen);  /* pack_size == add_size */
  tr_le32(&out[k_rar_tr_le32_11], (uint32_t)dlen); /* unp_size              */
  out[k_rar_val_15] = 0U;                          /* host_os               */
  tr_le32(&out[16], 0U);                           /* file_crc              */
  tr_le32(&out[k_rar_tr_le32_20], 0U);             /* ftime                 */
  out[k_rar_v_24]   = k_rar_out_20;                /* unp_ver               */
  out[k_rar_val_25] = k_rar_out_30;                /* method: store         */
  tr_le16(&out[k_rar_tr_le16_26], (uint16_t)nlen);
  tr_le32(&out[k_rar_tr_le32_28], 0U); /* attr */
  memcpy(&out[32], name, nlen);
  memcpy(&out[head], data, dlen);
  return head + dlen;
}

/** @brief Encode the RAR4 main archive block. */
static size_t tr4_main(uint8_t* out)
{
  tr_le16(&out[0], 0U);
  out[2] = k_rar_out_73; /* type: main */
  tr_le16(&out[3], 0U);
  tr_le16(&out[k_rar_tr_le16_5], k_rar_tr_le16_13);            /* head_size         */
  memset(&out[k_tr_off_high_pos_av], 0, k_tr_len_high_pos_av); /* HighPosAV + PosAV */
  return k_rar_tr_le16_13;
}

/** @brief Encode the RAR4 end-of-archive block. */
static size_t tr4_end(uint8_t* out)
{
  tr_le16(&out[0], 0U);
  out[2] = k_rar_out_7b; /* type: end */
  tr_le16(&out[3], 0U);
  tr_le16(&out[k_rar_tr_le16_5], k_rar_val_7);
  return k_rar_val_7;
}

/** @brief Build the four source PNGs (shared by both archive builders). */
static void tr_build_imgs(void)
{
  s_imgs[0] = (tr_img_t){.name = "img01.png", .w = 6U, .h = 4U, .seed = 1U};
  s_imgs[1] = (tr_img_t){.name = "img02.png", .w = 8U, .h = k_rar_h_5, .seed = 2U};
  s_imgs[2] = (tr_img_t){.name = "img03.png", .w = 4U, .h = k_rar_val_7, .seed = 3U};
  s_imgs[3] = (tr_img_t){.name = "sub/img04.png", .w = k_rar_h_5, .h = 6U, .seed = 4U};
  for (uint32_t i = 0U; i < k_tr_img_count; ++i) {
    s_imgs[i].plen =
      cf_make_png(s_imgs[i].w, s_imgs[i].h, s_imgs[i].seed, s_imgs[i].png, sizeof(s_imgs[i].png));
    TEST_ASSERT(s_imgs[i].plen > 0U);
  }
}

/**
 * @brief Build a RAR5 archive into ::s_arc: main, big filler, four STORE images,
 *        a directory, end -- scrambled. (Compressed-member decode is exercised in
 *        test_ra8_rar5.c, which crafts genuine RAR5 packed streams.)
 */
static void tr_build_rar5(void)
{
  tr_build_imgs();
  for (size_t i = 0U; i < sizeof(s_filler); ++i) {
    s_filler[i] = (uint8_t)((i * k_rar_i_31) + (i >> 3U));
  }
  static const uint8_t k_sig[8] = {0x52U, 0x61U, 0x72U, 0x21U, 0x1AU, 0x07U, 0x01U, 0x00U};
  size_t               p        = 0U;
  memcpy(&s_arc[p], k_sig, sizeof(k_sig));
  p += sizeof(k_sig);
  p += tr5_simple(&s_arc[p], 1U); /* main */
  p += tr5_file(&s_arc[p], "filler.dat", s_filler, sizeof(s_filler), 0U, false);
  p +=
    tr5_file(&s_arc[p], s_imgs[1].name, s_imgs[1].png, s_imgs[1].plen, 0U, false); /* img02 store */
  p +=
    tr5_file(&s_arc[p], s_imgs[0].name, s_imgs[0].png, s_imgs[0].plen, 0U, false); /* img01 store */
  p += tr5_file(&s_arc[p], "emptydir/", nullptr, 0U, 0U, true);                    /* directory */
  p +=
    tr5_file(&s_arc[p], s_imgs[2].name, s_imgs[2].png, s_imgs[2].plen, 0U, false); /* img03 store */
  p += tr5_file(&s_arc[p],
                s_imgs[3].name,
                s_imgs[3].png,
                s_imgs[3].plen,
                0U,
                false);                  /* sub/img04 store */
  p += tr5_simple(&s_arc[p], k_rar_h_5); /* end             */
  s_arc_size = p;
}

/** @brief Build a RAR4 archive into ::s_arc: main, two STORE images, end. */
static void tr_build_rar4(void)
{
  tr_build_imgs();
  static const uint8_t k_sig[7] = {0x52U, 0x61U, 0x72U, 0x21U, 0x1AU, 0x07U, 0x00U};
  size_t               p        = 0U;
  memcpy(&s_arc[p], k_sig, sizeof(k_sig));
  p += sizeof(k_sig);
  p += tr4_main(&s_arc[p]);
  p += tr4_file(&s_arc[p], s_imgs[1].name, s_imgs[1].png, s_imgs[1].plen); /* img02 */
  p += tr4_file(&s_arc[p], s_imgs[0].name, s_imgs[0].png, s_imgs[0].plen); /* img01 */
  p += tr4_end(&s_arc[p]);
  s_arc_size = p;
}

/**
 * @test test_rar_open_version_detect
 * @brief `ra8_rar_open` detects RAR5 and RAR4 signatures and rejects others.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each signature branch and each reject
 * (short, non-RAR) is an independent single-condition path.)
 */
static void test_rar_open_version_detect(void)
{
  TEST_BEGIN("rar: signature version detection");
  ra8_rar_t rar = {};

  tr_build_rar5();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));
  TEST_ASSERT_EQ(k_ra8_rar_ver_5, rar.version);

  tr_build_rar4();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));
  TEST_ASSERT_EQ(k_ra8_rar_ver_4, rar.version);

  /* Not a RAR archive. */
  static const uint8_t k_junk[] = "PK\003\004 this is a zip, not a rar";
  memcpy(s_arc, k_junk, sizeof(k_junk));
  s_arc_size = sizeof(k_junk);
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));
  TEST_ASSERT_EQ(k_ra8_rar_ver_none, rar.version);

  /* Too short to hold a signature. */
  s_arc_size = 3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));
  /* NULL guards. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rar_open(nullptr, tr_read, nullptr, 16U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rar_open(&rar, nullptr, nullptr, 16U));
  TEST_END("rar: signature version detection");
}

/** @brief Assert page @p i has @p name, extracts byte-exactly, and decodes. */
static void tr_check_page(ra8_comic_t* c, uint32_t i, const char* name)
{
  char     nb[k_tr_name_buf] = {};
  uint16_t nl                = 0U;
  uint64_t raw               = 0U;
  uint8_t  ex                = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_info(c, i, nb, (uint16_t)sizeof(nb), &nl, &raw, &ex));
  nb[nl] = '\0';
  TEST_ASSERT_EQ(0, strcmp(nb, name));
  TEST_ASSERT_EQ(1U, ex);
  const tr_img_t* src = tr_find(name);
  TEST_ASSERT(src != nullptr);
  uint8_t buf[k_cf_png_max] = {};
  size_t  got               = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(c, i, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(src->plen, got);
  TEST_ASSERT_EQ(0, memcmp(buf, src->png, got));
  /* Extract -> decode to real pixels through the project's arena-backed stb. */
  TEST_ASSERT(cf_decode_ok(buf, got, (int)src->w, (int)src->h));
}

/**
 * @test test_comic_cbr5_pages
 * @brief A RAR5 `.cbr` opens by magic, pages sorted, every STORE page extracts +
 *        decodes, the directory is skipped.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a round-trip oracle over page order,
 * byte-equality and decoded dimensions.)
 */
static void test_comic_cbr5_pages(void)
{
  TEST_BEGIN("comic cbr5: sorted pages extract + decode");
  tr_build_rar5();
  ra8_comic_t      c                    = {};
  ra8_comic_page_t pages[k_tr_page_cap] = {};
  char             names[k_tr_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                tr_read,
                                nullptr,
                                (uint64_t)s_arc_size,
                                pages,
                                (uint32_t)k_tr_page_cap,
                                names,
                                (uint32_t)sizeof(names)));
  TEST_ASSERT_EQ(k_ra8_comic_kind_cbr, ra8_comic_kind(&c));
  /* Four image members (dir + filler excluded). */
  TEST_ASSERT_EQ(k_tr_img_count, ra8_comic_page_count(&c));

  /* Sorted: img01, img02, img03, sub/img04 -- all STORE, all decode. */
  tr_check_page(&c, 0U, "img01.png");
  tr_check_page(&c, 1U, "img02.png");
  tr_check_page(&c, 2U, "img03.png");
  tr_check_page(&c, 3U, "sub/img04.png");

  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("comic cbr5: sorted pages extract + decode");
}

/**
 * @test test_comic_cbr4_pages
 * @brief A RAR4 `.cbr` opens by magic and its STORE pages extract + decode.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a round-trip oracle over the RAR4 walk.)
 */
static void test_comic_cbr4_pages(void)
{
  TEST_BEGIN("comic cbr4: sorted pages extract + decode");
  tr_build_rar4();
  ra8_comic_t      c                    = {};
  ra8_comic_page_t pages[k_tr_page_cap] = {};
  char             names[k_tr_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                tr_read,
                                nullptr,
                                (uint64_t)s_arc_size,
                                pages,
                                (uint32_t)k_tr_page_cap,
                                names,
                                (uint32_t)sizeof(names)));
  TEST_ASSERT_EQ(k_ra8_comic_kind_cbr, ra8_comic_kind(&c));
  TEST_ASSERT_EQ(2U, ra8_comic_page_count(&c));
  tr_check_page(&c, 0U, "img01.png");
  tr_check_page(&c, 1U, "img02.png");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("comic cbr4: sorted pages extract + decode");
}

/**
 * @test test_comic_cbr_bounded_ram
 * @brief Opening a RAR5 `.cbr` reads only block headers, never member data: the
 *        big filler member's payload is untouched.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a single inequality over the fetched-byte
 * counter versus the unread filler size.)
 */
static void test_comic_cbr_bounded_ram(void)
{
  TEST_BEGIN("comic cbr: bounded-RAM open (member data never read)");
  tr_build_rar5();
  g_fetched                             = 0U;
  ra8_comic_t      c                    = {};
  ra8_comic_page_t pages[k_tr_page_cap] = {};
  char             names[k_tr_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                tr_read,
                                nullptr,
                                (uint64_t)s_arc_size,
                                pages,
                                (uint32_t)k_tr_page_cap,
                                names,
                                (uint32_t)sizeof(names)));
  TEST_ASSERT(g_fetched < (uint64_t)k_tr_filler);
  TEST_ASSERT((uint64_t)k_tr_filler < (uint64_t)s_arc_size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("comic cbr: bounded-RAM open (member data never read)");
}

/**
 * @test test_rar_extract_guards
 * @brief `ra8_rar_extract_stored` rejects NULL args, an unopened archive, a
 *        directory / compressed member, and a too-small buffer.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition early return, driven one at a time.)
 */
static void test_rar_extract_guards(void)
{
  TEST_BEGIN("rar: extract_stored guards");
  tr_build_rar5();
  ra8_rar_t rar = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));

  uint8_t         buf[k_cf_png_max] = {};
  size_t          got               = 0U;
  ra8_rar_entry_t store             = {.is_file   = 1U,
                                       .is_dir    = 0U,
                                       .method    = (uint8_t)k_ra8_rar_method_store,
                                       .data_off  = 0U,
                                       .pack_size = 4U,
                                       .unp_size  = 4U};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar_extract_stored(nullptr, &store, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_rar_extract_stored(&rar, nullptr, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_rar_extract_stored(&rar, &store, nullptr, sizeof(buf), &got));

  ra8_rar_entry_t dir = store;
  dir.is_dir          = 1U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rar_extract_stored(&rar, &dir, buf, sizeof(buf), &got));
  ra8_rar_entry_t comp = store;
  comp.method          = (uint8_t)k_ra8_rar_method_compressed;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rar_extract_stored(&rar, &comp, buf, sizeof(buf), &got));
  ra8_rar_entry_t big = store;
  big.unp_size        = 4U;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_rar_extract_stored(&rar, &big, buf, 1U, &got));

  ra8_rar_t unopened = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rar_extract_stored(&unopened, &store, buf, sizeof(buf), &got));
  TEST_END("rar: extract_stored guards");
}

/* ---- Malformed / edge-case coverage helpers ------------------------------ */

/** @brief RAR5 8-byte signature (shared by the edge-case block builders). */
static const uint8_t k_tr_sig5[8] = {0x52U, 0x61U, 0x72U, 0x21U, 0x1AU, 0x07U, 0x01U, 0x00U};
/** @brief RAR4 7-byte marker (shared by the edge-case block builders). */
static const uint8_t k_tr_sig4[7] = {0x52U, 0x61U, 0x72U, 0x21U, 0x1AU, 0x07U, 0x00U};

/** @brief Lay a RAR5 signature + @p block into ::s_arc and set ::s_arc_size. */
static void tr_put5(const uint8_t* block, size_t blen)
{
  memcpy(s_arc, k_tr_sig5, sizeof(k_tr_sig5));
  memcpy(&s_arc[sizeof(k_tr_sig5)], block, blen);
  s_arc_size = sizeof(k_tr_sig5) + blen;
}

/** @brief Lay a RAR4 signature + @p block into ::s_arc and set ::s_arc_size. */
static void tr_put4(const uint8_t* block, size_t blen)
{
  memcpy(s_arc, k_tr_sig4, sizeof(k_tr_sig4));
  memcpy(&s_arc[sizeof(k_tr_sig4)], block, blen);
  s_arc_size = sizeof(k_tr_sig4) + blen;
}

/** @brief Open ::s_arc at its current ::s_arc_size (asserts success). */
static ra8_rar_t tr_open_arc(void)
{
  ra8_rar_t rar = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));
  return rar;
}

/**
 * @test test_rar5_truncation_sweep
 * @brief Walking a complete RAR5 file block at every truncation length hits each
 *        block-/file-header vint's short-read leg and the mtime/CRC skip fields.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the RAR5 parser is a chain of independent
 * single-condition vint guards; the sweep drives each to its short-read arm.)
 */
static void test_rar5_truncation_sweep(void)
{
  TEST_BEGIN("rar5: header truncation sweep");
  /* A complete RAR5 STORE file block whose file flags request the optional
   * mtime + data-CRC fixed fields, so a full parse exercises those skip legs;
   * truncating the read to every length drives each vint off the end in turn. */
  static const uint8_t k_block[] = {
    0x00U, 0x00U, 0x00U, 0x00U, /* header CRC32 (unverified)     */
    0x13U,                      /* header size vint = 19         */
    0x02U,                      /* header type = file            */
    0x00U,                      /* header flags = 0              */
    0x06U,                      /* file flags = mtime | data-CRC */
    0x00U,                      /* unpacked size vint            */
    0x00U,                      /* attributes vint               */
    0x00U, 0x00U, 0x00U, 0x00U, /* mtime fixed field             */
    0x00U, 0x00U, 0x00U, 0x00U, /* data-CRC fixed field          */
    0x00U,                      /* compression info vint         */
    0x00U,                      /* host OS vint                  */
    0x03U,                      /* name size vint = 3            */
    0x61U, 0x62U, 0x63U,        /* name "abc"                    */
  };
  tr_put5(k_block, sizeof(k_block));
  const size_t full              = s_arc_size;
  char         nb[k_tr_name_buf] = {};
  for (size_t sz = sizeof(k_tr_sig5) + 1U; sz <= full; ++sz) {
    s_arc_size          = sz;
    ra8_rar_t       rar = tr_open_arc();
    ra8_rar_entry_t out = {};
    const ra8_err_t r   = ra8_rar_next(&rar, rar.first_off, nb, (uint16_t)sizeof(nb), &out);
    /* Every truncation is either a clean parse or a rejected malformed block --
     * never a crash or an out-of-band error code. */
    TEST_ASSERT(r == k_ra8_ok || r == k_ra8_err_validation_failed);
  }
  TEST_END("rar5: header truncation sweep");
}

/**
 * @test test_rar5_block_vint_legs
 * @brief The RAR5 extra-/data-area vints, an overlong header-size vint, and a
 *        64-bit header size that wraps the next offset are all rejected.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each malformed input drives one
 * single-condition vint/overflow guard to its rejecting arm.)
 */
static void test_rar5_block_vint_legs(void)
{
  TEST_BEGIN("rar5: extra/data-size, overlong, and offset-wrap vint legs");
  char nb[k_tr_name_buf] = {};

  /* Header flags request an extra-area-size vint that is then truncated. */
  static const uint8_t k_extra[] = {0U, 0U, 0U, 0U, 0x03U, 0x02U, 0x01U};
  tr_put5(k_extra, sizeof(k_extra));
  {
    ra8_rar_t       rar = tr_open_arc();
    ra8_rar_entry_t out = {};
    TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                   ra8_rar_next(&rar, rar.first_off, nb, (uint16_t)sizeof(nb), &out));
  }

  /* Header flags request a data-size vint that is then truncated. */
  static const uint8_t k_data[] = {0U, 0U, 0U, 0U, 0x03U, 0x02U, 0x02U};
  tr_put5(k_data, sizeof(k_data));
  {
    ra8_rar_t       rar = tr_open_arc();
    ra8_rar_entry_t out = {};
    TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                   ra8_rar_next(&rar, rar.first_off, nb, (uint16_t)sizeof(nb), &out));
  }

  /* Overlong header-size vint: ten continuation bytes never terminate. */
  static const uint8_t k_overlong[] =
    {0U, 0U, 0U, 0U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U, 0x80U};
  tr_put5(k_overlong, sizeof(k_overlong));
  {
    ra8_rar_t       rar = tr_open_arc();
    ra8_rar_entry_t out = {};
    TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                   ra8_rar_next(&rar, rar.first_off, nb, (uint16_t)sizeof(nb), &out));
  }

  /* A 64-bit header size (2^64 - 14) whose value wraps the computed next offset
   * back to the block start: exercises the vint 32-bit-straddle / high-word
   * accumulation and the non-advancing-block guard. */
  uint8_t      k_wrap[16] = {};
  const size_t vlen       = tr_vint(&k_wrap[4], 0xFFFFFFFFFFFFFFF2ULL);
  TEST_ASSERT_EQ(10U, vlen);
  k_wrap[4U + vlen]        = 0x01U; /* header type = non-file */
  k_wrap[k_rar_h_5 + vlen] = 0x00U; /* header flags           */
  tr_put5(k_wrap, 6U + vlen);
  {
    ra8_rar_t       rar = tr_open_arc();
    ra8_rar_entry_t out = {};
    TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                   ra8_rar_next(&rar, rar.first_off, nb, (uint16_t)sizeof(nb), &out));
  }
  TEST_END("rar5: extra/data-size, overlong, and offset-wrap vint legs");
}

/**
 * @test test_rar4_truncation_sweep
 * @brief Walking a RAR4 STORE file block at every truncation length hits the
 *        short base-header, the truncated fixed-field, and the file-decode legs.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the RAR4 parser is a chain of independent
 * single-condition length guards; the sweep drives each to its short-read arm.)
 */
static void test_rar4_truncation_sweep(void)
{
  TEST_BEGIN("rar4: header truncation sweep");
  static uint8_t s_k_data[4] = {1U, 2U, 3U, 4U};
  const size_t   blen = tr4_file(&s_arc[sizeof(k_tr_sig4)], "a.png", s_k_data, sizeof(s_k_data));
  memcpy(s_arc, k_tr_sig4, sizeof(k_tr_sig4));
  const size_t full              = sizeof(k_tr_sig4) + blen;
  char         nb[k_tr_name_buf] = {};
  for (size_t sz = sizeof(k_tr_sig4) + 1U; sz <= full; ++sz) {
    s_arc_size          = sz;
    ra8_rar_t       rar = tr_open_arc();
    ra8_rar_entry_t out = {};
    const ra8_err_t r   = ra8_rar_next(&rar, rar.first_off, nb, (uint16_t)sizeof(nb), &out);
    TEST_ASSERT(r == k_ra8_ok || r == k_ra8_err_validation_failed);
  }
  TEST_END("rar4: header truncation sweep");
}

/**
 * @test test_rar4_badhsize_and_large
 * @brief A RAR4 block whose HEAD_SIZE is below the base length is rejected, and a
 *        LARGE (64-bit size) file block whose sizes wrap the next offset is too.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the HEAD_SIZE floor and the LARGE 64-bit
 * high-dword path are independent single-condition legs.)
 */
static void test_rar4_badhsize_and_large(void)
{
  TEST_BEGIN("rar4: sub-minimum head size + LARGE 64-bit size wrap");
  char nb[k_tr_name_buf] = {};

  /* HEAD_SIZE = 3, below the 7-byte base header. */
  static const uint8_t k_badhs[] = {0U, 0U, 0x74U, 0U, 0U, 0x03U, 0x00U, 0U, 0U, 0U, 0U, 0U, 0U};
  tr_put4(k_badhs, sizeof(k_badhs));
  {
    ra8_rar_t       rar = tr_open_arc();
    ra8_rar_entry_t out = {};
    TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                   ra8_rar_next(&rar, rar.first_off, nb, (uint16_t)sizeof(nb), &out));
  }

  /* LARGE file header (flags 0x0100): the 64-bit PACK size = 2^64 - 40 makes
   * next_off = off + 40 + pack wrap back to off, exercising the high-dword
   * merge and the non-advancing guard. */
  static const uint8_t k_large[] = {
    0U,    0U,                  /* head_crc               */
    0x74U,                      /* type = file            */
    0x00U, 0x01U,               /* flags = 0x0100 LARGE   */
    0x28U, 0x00U,               /* head_size = 40         */
    0xD8U, 0xFFU, 0xFFU, 0xFFU, /* pack low  = 0xFFFFFFD8 */
    0U,    0U,    0U,    0U,    /* unp low                */
    0U,                         /* host_os                */
    0U,    0U,    0U,    0U,    /* file_crc               */
    0U,    0U,    0U,    0U,    /* ftime                  */
    0U,                         /* unp_ver                */
    0x30U,                      /* method = store         */
    0U,    0U,                  /* name_size = 0          */
    0U,    0U,    0U,    0U,    /* attr                   */
    0xFFU, 0xFFU, 0xFFU, 0xFFU, /* high_pack = 0xFFFFFFFF */
    0U,    0U,    0U,    0U,    /* high_unp               */
  };
  tr_put4(k_large, sizeof(k_large));
  {
    ra8_rar_t       rar = tr_open_arc();
    ra8_rar_entry_t out = {};
    TEST_ASSERT_EQ(k_ra8_err_validation_failed,
                   ra8_rar_next(&rar, rar.first_off, nb, (uint16_t)sizeof(nb), &out));
  }
  TEST_END("rar4: sub-minimum head size + LARGE 64-bit size wrap");
}

/**
 * @test test_rar_name_copy_guards
 * @brief `ra8_rar_next` copies no name when the buffer is NULL or its capacity is
 *        zero, yet still decodes the file member.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the NULL-buffer and zero-capacity name
 * copy guards are independent single-condition early returns.)
 */
static void test_rar_name_copy_guards(void)
{
  TEST_BEGIN("rar: name buffer NULL / zero-capacity guards");
  static uint8_t s_k_data[4] = {1U, 2U, 3U, 4U};
  const size_t   blen = tr4_file(&s_arc[sizeof(k_tr_sig4)], "a.png", s_k_data, sizeof(s_k_data));
  memcpy(s_arc, k_tr_sig4, sizeof(k_tr_sig4));
  s_arc_size          = sizeof(k_tr_sig4) + blen;
  ra8_rar_t       rar = tr_open_arc();
  ra8_rar_entry_t out = {};

  /* NULL name buffer (capacity 0): the member still decodes, no name copied. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar_next(&rar, rar.first_off, nullptr, 0U, &out));
  TEST_ASSERT_EQ(1U, out.is_file);
  TEST_ASSERT_EQ(0U, out.name_len);

  /* Non-NULL buffer but zero capacity: still no name copied. */
  char b2[4] = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar_next(&rar, rar.first_off, b2, 0U, &out));
  TEST_ASSERT_EQ(0U, out.name_len);
  TEST_END("rar: name buffer NULL / zero-capacity guards");
}

/**
 * @test test_rar_open_next_extract_edges
 * @brief Short-signature open, `ra8_rar_next` state / offset guards, and
 *        `ra8_rar_extract_stored`'s non-file / overrun / short-read rejections.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition early return driven one at a time.)
 */
static void test_rar_open_next_extract_edges(void)
{
  TEST_BEGIN("rar: open short-sig, next guards, extract overrun / short-read");
  char            nb[k_tr_name_buf] = {};
  ra8_rar_entry_t out               = {};

  /* Backing read shorter than a signature though the declared size is not. */
  memcpy(s_arc, k_tr_sig5, (size_t)k_tr_len_signature);
  s_arc_size    = k_rar_h_5;
  ra8_rar_t rar = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_rar_open(&rar, tr_read, nullptr, 100U));

  /* next on an unopened reader. */
  ra8_rar_t unopened = {};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 ra8_rar_next(&unopened, 0U, nb, (uint16_t)sizeof(nb), &out));

  /* next with an offset at/after the archive size. */
  tr_build_rar5();
  ra8_rar_t good = tr_open_arc();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_rar_next(&good, (uint64_t)s_arc_size, nb, (uint16_t)sizeof(nb), &out));

  /* Reader whose declared size (1000) exceeds the backing bytes (100). */
  memcpy(s_arc, k_tr_sig5, sizeof(k_tr_sig5));
  s_arc_size    = k_rar_s_arc_size_100;
  ra8_rar_t big = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rar_open(&big, tr_read, nullptr, 1000U));
  uint8_t buf[k_rar_val_256] = {};
  size_t  got                = 0U;

  /* A non-file entry is unsupported. */
  ra8_rar_entry_t nonfile = {.is_file = 0U};
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_rar_extract_stored(&big, &nonfile, buf, sizeof(buf), &got));

  /* A STORE member whose data area overruns the declared archive size. */
  ra8_rar_entry_t overrun = {.is_file   = 1U,
                             .is_dir    = 0U,
                             .method    = (uint8_t)k_ra8_rar_method_store,
                             .data_off  = k_rar_data_off_990,
                             .pack_size = k_rar_out_20,
                             .unp_size  = k_rar_out_20};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rar_extract_stored(&big, &overrun, buf, sizeof(buf), &got));

  /* Within the declared size but the backing returns a short read. */
  ra8_rar_entry_t shortread = {.is_file   = 1U,
                               .is_dir    = 0U,
                               .method    = (uint8_t)k_ra8_rar_method_store,
                               .data_off  = k_rar_data_off_50,
                               .pack_size = k_rar_s_arc_size_100,
                               .unp_size  = k_rar_s_arc_size_100};
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_rar_extract_stored(&big, &shortread, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_END("rar: open short-sig, next guards, extract overrun / short-read");
}

/**
 * @brief Open the shared RAR fixture with the standard page/name budgets.
 * @param[out] c     Comic handle under test.
 * @param[out] pages Page-descriptor table.
 * @param[out] names Name-string buffer.
 * @return The `ra8_comic_open` result code.
 * @pre The shared archive buffer holds s_arc_size bytes.
 * @post No state beyond @p c / @p pages / @p names is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static ra8_err_t tr_open(ra8_comic_t* c, ra8_comic_page_t* pages, char* names)
{
  return ra8_comic_open(c,
                        tr_read,
                        nullptr,
                        (uint64_t)s_arc_size,
                        pages,
                        (uint32_t)k_tr_page_cap,
                        names,
                        (uint32_t)k_tr_name_cap);
}

/**
 * @test test_comic_cbr_sort_and_empty
 * @brief The comic name-sort resolves shared-prefix names by length (both
 *        directions and equal), and a RAR with no image members reports
 *        not_found.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the name comparator's length tiebreak and
 * the empty-index guard are independent single-condition legs; this drives the
 * shorter / longer / equal comparison arms via a crafted prefix set.)
 */
static void test_comic_cbr_sort_and_empty(void)
{
  TEST_BEGIN("comic cbr: name-sort length tiebreak + no-image not_found");
  static const uint8_t k_one[1] = {0x00U};

  /* Members share the "a.png" prefix (one duplicated), so the insertion sort
   * compares shorter-vs-longer, longer-vs-shorter, and equal names. */
  size_t p = 0U;
  memcpy(&s_arc[p], k_tr_sig5, sizeof(k_tr_sig5));
  p += sizeof(k_tr_sig5);
  p += tr5_simple(&s_arc[p], 1U);
  p += tr5_file(&s_arc[p], "a.png.png", k_one, sizeof(k_one), 0U, false);
  p += tr5_file(&s_arc[p], "a.png", k_one, sizeof(k_one), 0U, false);
  p += tr5_file(&s_arc[p], "a.png.png.png", k_one, sizeof(k_one), 0U, false);
  p += tr5_file(&s_arc[p], "a.png.png", k_one, sizeof(k_one), 0U, false);
  p += tr5_simple(&s_arc[p], k_rar_h_5);
  s_arc_size = p;

  ra8_comic_t      c                    = {};
  ra8_comic_page_t pages[k_tr_page_cap] = {};
  char             names[k_tr_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, tr_open(&c, pages, names));
  TEST_ASSERT_EQ(4U, ra8_comic_page_count(&c));
  char     nb[k_tr_name_buf] = {};
  uint16_t nl                = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_page_info(&c, 0U, nb, (uint16_t)sizeof(nb), &nl, nullptr, nullptr));
  nb[nl] = '\0';
  TEST_ASSERT_EQ(0, strcmp(nb, "a.png")); /* shortest sorts first */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));

  /* A valid RAR whose only member is not an image -> zero pages -> not_found. */
  p = 0U;
  memcpy(&s_arc[p], k_tr_sig5, sizeof(k_tr_sig5));
  p += sizeof(k_tr_sig5);
  p += tr5_simple(&s_arc[p], 1U);
  p += tr5_file(&s_arc[p], "notes.txt", k_one, sizeof(k_one), 0U, false);
  p += tr5_simple(&s_arc[p], k_rar_h_5);
  s_arc_size = p;

  ra8_comic_t      c2                    = {};
  ra8_comic_page_t pages2[k_tr_page_cap] = {};
  char             names2[k_tr_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_err_not_found, tr_open(&c2, pages2, names2));
  TEST_END("comic cbr: name-sort length tiebreak + no-image not_found");
}

/**
 * @brief Test entry point -- runs the RAR / CBR suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int32_t main(void)
{
  test_rar_open_version_detect();
  test_comic_cbr5_pages();
  test_comic_cbr4_pages();
  test_comic_cbr_bounded_ram();
  test_rar_extract_guards();
  test_rar5_truncation_sweep();
  test_rar5_block_vint_legs();
  test_rar4_truncation_sweep();
  test_rar4_badhsize_and_large();
  test_rar_name_copy_guards();
  test_rar_open_next_extract_edges();
  test_comic_cbr_sort_and_empty();
  (void)fprintf(stderr, "[OK  ] test_ra8_rar.c\n");
  return 0;
}
