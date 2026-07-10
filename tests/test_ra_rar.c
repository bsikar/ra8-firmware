/**
 * @file test_ra_rar.c
 * @brief Tests for the clean-room RAR reader (ra_rar) and the CBR comic backend.
 *
 * @details
 * Hand-crafts real RAR archives from the public block layout -- a RAR5 archive
 * (vint headers) and a RAR4 archive (fixed-field headers), each holding
 * STORE-method PNG page images built by comic_fixture.h -- then walks them two
 * ways:
 *
 *   1. directly through `ra_rar_open` / `ra_rar_next` (version detection, block
 *      iteration, STORE extraction, and the compressed / directory guards), and
 *   2. through the `ra_comic` facade opened by magic, proving a `.cbr` pages,
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
#include "ra_comic.h"
#include "ra_err.h"
#include "ra_rar.h"
#include "unity_minimal.h"

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
    uint8_t b = (uint8_t)(v & 0x7FU);
    v >>= 7U;
    if (v != 0U) {
      out[i] = (uint8_t)(b | 0x80U);
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
  p[3] = (uint8_t)(v >> 24);
}

/** @brief Encode a RAR4 STORE file block. */
static size_t tr4_file(uint8_t* out, const char* name, const uint8_t* data, size_t dlen)
{
  const size_t nlen = strlen(name);
  const size_t head = 32U + nlen; /* fixed fields + name (no LARGE/salt/exttime) */
  tr_le16(&out[0], 0U);           /* head_crc (unverified)                       */
  out[2] = 0x74U;                 /* type: file                                  */
  tr_le16(&out[3], 0x8000U);      /* flags: LONG (ADD_SIZE present)              */
  tr_le16(&out[5], (uint16_t)head);
  tr_le32(&out[7], (uint32_t)dlen);  /* pack_size == add_size */
  tr_le32(&out[11], (uint32_t)dlen); /* unp_size              */
  out[15] = 0U;                      /* host_os               */
  tr_le32(&out[16], 0U);             /* file_crc              */
  tr_le32(&out[20], 0U);             /* ftime                 */
  out[24] = 20U;                     /* unp_ver               */
  out[25] = 0x30U;                   /* method: store         */
  tr_le16(&out[26], (uint16_t)nlen);
  tr_le32(&out[28], 0U); /* attr */
  memcpy(&out[32], name, nlen);
  memcpy(&out[head], data, dlen);
  return head + dlen;
}

/** @brief Encode the RAR4 main archive block. */
static size_t tr4_main(uint8_t* out)
{
  tr_le16(&out[0], 0U);
  out[2] = 0x73U; /* type: main */
  tr_le16(&out[3], 0U);
  tr_le16(&out[5], 13U);  /* head_size         */
  memset(&out[7], 0, 6U); /* HighPosAV + PosAV */
  return 13U;
}

/** @brief Encode the RAR4 end-of-archive block. */
static size_t tr4_end(uint8_t* out)
{
  tr_le16(&out[0], 0U);
  out[2] = 0x7BU; /* type: end */
  tr_le16(&out[3], 0U);
  tr_le16(&out[5], 7U);
  return 7U;
}

/** @brief Build the four source PNGs (shared by both archive builders). */
static void tr_build_imgs(void)
{
  s_imgs[0] = (tr_img_t){.name = "img01.png", .w = 6U, .h = 4U, .seed = 1U};
  s_imgs[1] = (tr_img_t){.name = "img02.png", .w = 8U, .h = 5U, .seed = 2U};
  s_imgs[2] = (tr_img_t){.name = "img03.png", .w = 4U, .h = 7U, .seed = 3U};
  s_imgs[3] = (tr_img_t){.name = "sub/img04.png", .w = 5U, .h = 6U, .seed = 4U};
  for (uint32_t i = 0U; i < k_tr_img_count; ++i) {
    s_imgs[i].plen =
      cf_make_png(s_imgs[i].w, s_imgs[i].h, s_imgs[i].seed, s_imgs[i].png, sizeof(s_imgs[i].png));
    TEST_ASSERT(s_imgs[i].plen > 0U);
  }
}

/**
 * @brief Build a RAR5 archive into ::s_arc: main, big filler, four images
 *        (one compressed), a directory, end -- scrambled.
 */
static void tr_build_rar5(void)
{
  tr_build_imgs();
  for (size_t i = 0U; i < sizeof(s_filler); ++i) {
    s_filler[i] = (uint8_t)((i * 31U) + (i >> 3U));
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
  p += tr5_file(&s_arc[p],
                s_imgs[2].name,
                s_imgs[2].png,
                s_imgs[2].plen,
                0x80U,
                false); /* img03 compressed */
  p += tr5_file(&s_arc[p],
                s_imgs[3].name,
                s_imgs[3].png,
                s_imgs[3].plen,
                0U,
                false);           /* sub/img04 store */
  p += tr5_simple(&s_arc[p], 5U); /* end             */
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
 * @brief `ra_rar_open` detects RAR5 and RAR4 signatures and rejects others.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each signature branch and each reject
 * (short, non-RAR) is an independent single-condition path.)
 */
static void test_rar_open_version_detect(void)
{
  TEST_BEGIN("rar: signature version detection");
  ra_rar_t rar = {};

  tr_build_rar5();
  TEST_ASSERT_EQ(k_ra_ok, ra_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));
  TEST_ASSERT_EQ(k_ra_rar_ver_5, rar.version);

  tr_build_rar4();
  TEST_ASSERT_EQ(k_ra_ok, ra_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));
  TEST_ASSERT_EQ(k_ra_rar_ver_4, rar.version);

  /* Not a RAR archive. */
  static const uint8_t k_junk[] = "PK\003\004 this is a zip, not a rar";
  memcpy(s_arc, k_junk, sizeof(k_junk));
  s_arc_size = sizeof(k_junk);
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));
  TEST_ASSERT_EQ(k_ra_rar_ver_none, rar.version);

  /* Too short to hold a signature. */
  s_arc_size = 3U;
  TEST_ASSERT_EQ(k_ra_err_invalid_size, ra_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));
  /* NULL guards. */
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_rar_open(nullptr, tr_read, nullptr, 16U));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_rar_open(&rar, nullptr, nullptr, 16U));
  TEST_END("rar: signature version detection");
}

/** @brief Assert page @p i has @p name, extracts byte-exactly, and decodes. */
static void tr_check_page(ra_comic_t* c, uint32_t i, const char* name)
{
  char     nb[k_tr_name_buf] = {};
  uint16_t nl                = 0U;
  uint64_t raw               = 0U;
  uint8_t  ex                = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_comic_page_info(c, i, nb, (uint16_t)sizeof(nb), &nl, &raw, &ex));
  nb[nl] = '\0';
  TEST_ASSERT_EQ(0, strcmp(nb, name));
  TEST_ASSERT_EQ(1U, ex);
  const tr_img_t* src = tr_find(name);
  TEST_ASSERT(src != nullptr);
  uint8_t buf[k_cf_png_max] = {};
  size_t  got               = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_comic_page_read(c, i, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(src->plen, got);
  TEST_ASSERT_EQ(0, memcmp(buf, src->png, got));
  /* Extract -> decode to real pixels through the project's arena-backed stb. */
  TEST_ASSERT(cf_decode_ok(buf, got, (int)src->w, (int)src->h));
}

/**
 * @test test_comic_cbr5_pages
 * @brief A RAR5 `.cbr` opens by magic, pages sorted, STORE pages extract+decode,
 *        the compressed member reports unsupported, the directory is skipped.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a round-trip oracle over page order,
 * byte-equality, decoded dimensions, and the single-condition unsupported guard.)
 */
static void test_comic_cbr5_pages(void)
{
  TEST_BEGIN("comic cbr5: sorted pages extract + decode, compressed unsupported");
  tr_build_rar5();
  ra_comic_t      c                    = {};
  ra_comic_page_t pages[k_tr_page_cap] = {};
  char            names[k_tr_name_cap] = {};
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_comic_open(&c,
                               tr_read,
                               nullptr,
                               (uint64_t)s_arc_size,
                               pages,
                               (uint32_t)k_tr_page_cap,
                               names,
                               (uint32_t)sizeof(names)));
  TEST_ASSERT_EQ(k_ra_comic_kind_cbr, ra_comic_kind(&c));
  /* Four image members (dir + filler excluded). */
  TEST_ASSERT_EQ((uint32_t)k_tr_img_count, ra_comic_page_count(&c));

  /* Sorted: img01, img02, img03(compressed), sub/img04. */
  tr_check_page(&c, 0U, "img01.png");
  tr_check_page(&c, 1U, "img02.png");
  tr_check_page(&c, 3U, "sub/img04.png");

  /* The compressed member is indexed but not decodable. */
  uint8_t  buf[k_cf_png_max] = {};
  size_t   got               = 0U;
  uint16_t nl                = 0U;
  uint64_t raw               = 0U;
  uint8_t  ex                = 0U;
  TEST_ASSERT_EQ(k_ra_ok, ra_comic_page_info(&c, 2U, nullptr, 0U, &nl, &raw, &ex));
  TEST_ASSERT_EQ(0U, ex);
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_comic_page_read(&c, 2U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(0U, (uint32_t)got);

  TEST_ASSERT_EQ(k_ra_ok, ra_comic_close(&c));
  TEST_END("comic cbr5: sorted pages extract + decode, compressed unsupported");
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
  ra_comic_t      c                    = {};
  ra_comic_page_t pages[k_tr_page_cap] = {};
  char            names[k_tr_name_cap] = {};
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_comic_open(&c,
                               tr_read,
                               nullptr,
                               (uint64_t)s_arc_size,
                               pages,
                               (uint32_t)k_tr_page_cap,
                               names,
                               (uint32_t)sizeof(names)));
  TEST_ASSERT_EQ(k_ra_comic_kind_cbr, ra_comic_kind(&c));
  TEST_ASSERT_EQ(2U, ra_comic_page_count(&c));
  tr_check_page(&c, 0U, "img01.png");
  tr_check_page(&c, 1U, "img02.png");
  TEST_ASSERT_EQ(k_ra_ok, ra_comic_close(&c));
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
  g_fetched                            = 0U;
  ra_comic_t      c                    = {};
  ra_comic_page_t pages[k_tr_page_cap] = {};
  char            names[k_tr_name_cap] = {};
  TEST_ASSERT_EQ(k_ra_ok,
                 ra_comic_open(&c,
                               tr_read,
                               nullptr,
                               (uint64_t)s_arc_size,
                               pages,
                               (uint32_t)k_tr_page_cap,
                               names,
                               (uint32_t)sizeof(names)));
  TEST_ASSERT(g_fetched < (uint64_t)k_tr_filler);
  TEST_ASSERT((uint64_t)k_tr_filler < (uint64_t)s_arc_size);
  TEST_ASSERT_EQ(k_ra_ok, ra_comic_close(&c));
  TEST_END("comic cbr: bounded-RAM open (member data never read)");
}

/**
 * @test test_rar_extract_guards
 * @brief `ra_rar_extract_stored` rejects NULL args, an unopened archive, a
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
  ra_rar_t rar = {};
  TEST_ASSERT_EQ(k_ra_ok, ra_rar_open(&rar, tr_read, nullptr, (uint64_t)s_arc_size));

  uint8_t        buf[k_cf_png_max] = {};
  size_t         got               = 0U;
  ra_rar_entry_t store             = {.is_file   = 1U,
                                      .is_dir    = 0U,
                                      .method    = (uint8_t)k_ra_rar_method_store,
                                      .data_off  = 0U,
                                      .pack_size = 4U,
                                      .unp_size  = 4U};

  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_rar_extract_stored(nullptr, &store, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr, ra_rar_extract_stored(&rar, nullptr, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra_err_null_ptr,
                 ra_rar_extract_stored(&rar, &store, nullptr, sizeof(buf), &got));

  ra_rar_entry_t dir = store;
  dir.is_dir         = 1U;
  TEST_ASSERT_EQ(k_ra_err_not_supported, ra_rar_extract_stored(&rar, &dir, buf, sizeof(buf), &got));
  ra_rar_entry_t comp = store;
  comp.method         = (uint8_t)k_ra_rar_method_compressed;
  TEST_ASSERT_EQ(k_ra_err_not_supported,
                 ra_rar_extract_stored(&rar, &comp, buf, sizeof(buf), &got));
  ra_rar_entry_t big = store;
  big.unp_size       = 4U;
  TEST_ASSERT_EQ(k_ra_err_no_mem, ra_rar_extract_stored(&rar, &big, buf, 1U, &got));

  ra_rar_t unopened = {};
  TEST_ASSERT_EQ(k_ra_err_invalid_state,
                 ra_rar_extract_stored(&unopened, &store, buf, sizeof(buf), &got));
  TEST_END("rar: extract_stored guards");
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
  (void)fprintf(stderr, "[OK  ] test_ra_rar.c\n");
  return 0;
}
