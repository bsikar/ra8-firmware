/**
 * @file test_ra8_comic.c
 * @brief Tests for the CBZ (ZIP-of-images) comic reader (ra8_comic + ra8_comic_cbz).
 *
 * @details
 * Builds a *real* CBZ in memory with miniz's ZIP writer: four genuine
 * stb-decodable PNG page images (some STORE, some DEFLATE) placed out of order
 * and in a nested folder, plus entries the reader must skip (a text file, a macOS
 * AppleDouble fork) and one large uncompressed filler entry that dominates the
 * archive size but is never referenced. The suite opens that CBZ through
 * `ra8_comic_open` over a seek+read callback -- the #151 streaming path -- and
 * proves:
 *
 *   1. pages are enumerated in sorted-name order (reading order),
 *   2. each page's extracted bytes are byte-identical to the source PNG and
 *      decode through stb_image to the right dimensions,
 *   3. non-image / hidden entries are excluded,
 *   4. the open is bounded-RAM: the whole archive is never read (the big filler
 *      entry is untouched), and
 *   5. the open / page-read argument guards behave.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "comic_fixture.h"
#include "miniz.h"
#include "ra8_comic.h"
#include "ra8_comic_internal.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum comic_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_comic_i_31          = 31U,
  k_comic_s_arc_size_22 = 22U,
  k_comic_w_5           = 5U,
} comic_uint8_const_t;

/**
 * @enum tc_dim_t
 * @brief Fixture geometry and buffer budgets (tests are magic-number exempt).
 */
typedef enum : uint32_t {
  k_tc_img_count = 4U,           /**< Real page images in the CBZ.  */
  k_tc_page_cap  = 16U,          /**< Page-index capacity.          */
  k_tc_name_cap  = 1024U,        /**< Name-arena capacity, bytes.   */
  k_tc_filler    = 200U * 1024U, /**< Big unreferenced STORE entry. */
  k_tc_arc_cap   = 512U * 1024U, /**< Archive build buffer.         */
  k_tc_name_buf  = 64U,          /**< Per-query name buffer.        */
} tc_dim_t;

/**
 * @struct tc_img_t
 * @brief One source page image: its archive name, dimensions, and PNG bytes.
 */
typedef struct {
  const char* name;              /**< ZIP entry name.     */
  uint16_t    w;                 /**< Pixel width.        */
  uint16_t    h;                 /**< Pixel height.       */
  uint8_t     seed;              /**< Pixel-pattern seed. */
  uint8_t     png[k_cf_png_max]; /**< Encoded PNG bytes.  */
  size_t      plen;              /**< PNG byte length.    */
} tc_img_t;

/** @brief The four source page images (built once by tc_build). */
static tc_img_t s_imgs[k_tc_img_count];
/** @brief The built CBZ archive bytes. */
static uint8_t s_arc[k_tc_arc_cap];
/** @brief Length of the built archive. */
static size_t s_arc_size = 0U;
/** @brief Bytes fetched through the read callback (bounded-RAM probe). */
static uint64_t g_fetched = 0U;

/** @brief Seek+read callback over the archive; counts fetched bytes. */
static size_t tc_read(void* ctx, uint64_t off, void* buf, size_t len)
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

/** @brief Find a source image by its archive name (NULL if absent). */
static const tc_img_t* tc_find(const char* name)
{
  for (uint32_t i = 0U; i < k_tc_img_count; ++i) {
    if (strcmp(s_imgs[i].name, name) == 0) {
      return &s_imgs[i];
    }
  }
  return nullptr;
}

/** @brief Add one page image to the ZIP writer with a chosen compression level. */
static void tc_add_img(mz_zip_archive* zip, const tc_img_t* im, mz_uint level)
{
  TEST_ASSERT(mz_zip_writer_add_mem(zip, im->name, im->png, im->plen, level) == MZ_TRUE);
}

/**
 * @brief Build the four PNGs and the CBZ archive into ::s_arc.
 * @details Images are inserted scrambled and in a nested folder; the archive also
 *          carries a text file, a macOS AppleDouble fork, and a large STORE
 *          filler -- all of which the reader must skip.
 */
static void tc_build(void)
{
  s_imgs[0] = (tc_img_t){.name = "nested/page003.png", .w = 8U, .h = 6U, .seed = 3U};
  s_imgs[1] = (tc_img_t){.name = "page000.png", .w = 4U, .h = 4U, .seed = 0U};
  s_imgs[2] = (tc_img_t){.name = "page001.png", .w = 6U, .h = 8U, .seed = 1U};
  s_imgs[3] = (tc_img_t){.name = "page002.png", .w = k_comic_w_5, .h = k_comic_w_5, .seed = 2U};
  for (uint32_t i = 0U; i < k_tc_img_count; ++i) {
    s_imgs[i].plen =
      cf_make_png(s_imgs[i].w, s_imgs[i].h, s_imgs[i].seed, s_imgs[i].png, sizeof(s_imgs[i].png));
    TEST_ASSERT(s_imgs[i].plen > 0U);
  }

  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_tc_arc_cap) == MZ_TRUE);

  /* Scrambled insertion order; two STORE, two DEFLATE, to exercise both paths. */
  tc_add_img(&zip, &s_imgs[2], MZ_NO_COMPRESSION);      /* page001.png        */
  tc_add_img(&zip, &s_imgs[0], MZ_NO_COMPRESSION);      /* nested/page003.png */
  tc_add_img(&zip, &s_imgs[3], MZ_DEFAULT_COMPRESSION); /* page002.png        */
  tc_add_img(&zip, &s_imgs[1], MZ_DEFAULT_COMPRESSION); /* page000.png        */

  /* Entries the reader must skip. */
  static const char* const k_txt = "not an image";
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "readme.txt", k_txt, strlen(k_txt), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "__MACOSX/._page000.png",
                                    k_txt,
                                    strlen(k_txt),
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);

  /* Large unreferenced filler (STORE) -- dominates size, never read on open. */
  static uint8_t s_filler[k_tc_filler];
  for (size_t i = 0U; i < sizeof(s_filler); ++i) {
    s_filler[i] = (uint8_t)((i * k_comic_i_31) + (i >> 3U));
  }
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "big_filler.bin", s_filler, sizeof(s_filler), MZ_NO_COMPRESSION) ==
    MZ_TRUE);

  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz <= (size_t)k_tc_arc_cap));
  memcpy(s_arc, heap, hsz);
  s_arc_size = hsz;
  mz_zip_writer_end(&zip);
}

/**
 * @test test_comic_cbz_sorted_extract_decode
 * @brief Open the CBZ streamed; pages come out sorted, extract byte-identically,
 *        and decode through stb_image to the right dimensions; skip-entries are
 *        excluded.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the facade's guards are single-condition;
 * this is a round-trip oracle: page order, byte-equality, and decoded dimensions
 * against the source images.)
 */
static void test_comic_cbz_sorted_extract_decode(void)
{
  TEST_BEGIN("comic cbz: sorted pages extract + decode");
  tc_build();

  ra8_comic_t      c                    = {};
  ra8_comic_page_t pages[k_tc_page_cap] = {};
  char             names[k_tc_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                tc_read,
                                nullptr,
                                (uint64_t)s_arc_size,
                                pages,
                                (uint32_t)k_tc_page_cap,
                                names,
                                (uint32_t)sizeof(names)));
  TEST_ASSERT_EQ(k_ra8_comic_kind_cbz, ra8_comic_kind(&c));
  TEST_ASSERT_EQ(k_tc_img_count, ra8_comic_page_count(&c));

  static const char* const k_sorted[k_tc_img_count] = {
    "nested/page003.png",
    "page000.png",
    "page001.png",
    "page002.png",
  };
  for (uint32_t i = 0U; i < k_tc_img_count; ++i) {
    char     nb[k_tc_name_buf] = {};
    uint16_t nl                = 0U;
    uint64_t raw               = 0U;
    uint8_t  ex                = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_info(&c, i, nb, (uint16_t)sizeof(nb), &nl, &raw, &ex));
    nb[nl] = '\0';
    TEST_ASSERT_EQ(0, strcmp(nb, k_sorted[i]));
    TEST_ASSERT_EQ(1U, ex);

    const tc_img_t* src = tc_find(k_sorted[i]);
    TEST_ASSERT(src != nullptr);
    TEST_ASSERT_EQ(src->plen, raw);

    uint8_t buf[k_cf_png_max] = {};
    size_t  got               = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(&c, i, buf, sizeof(buf), &got));
    TEST_ASSERT_EQ(src->plen, got);
    TEST_ASSERT_EQ(0, memcmp(buf, src->png, got));

    /* Extract -> decode to real pixels through the project's arena-backed stb. */
    TEST_ASSERT(cf_decode_ok(buf, got, (int)src->w, (int)src->h));
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("comic cbz: sorted pages extract + decode");
}

/**
 * @test test_comic_cbz_bounded_ram
 * @brief Opening the CBZ never reads the whole archive: the large filler entry is
 *        untouched, so the bytes fetched on open are far below the file size.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a single inequality over the fetched-byte
 * counter versus the unreferenced filler size.)
 */
static void test_comic_cbz_bounded_ram(void)
{
  TEST_BEGIN("comic cbz: bounded-RAM open (whole archive never read)");
  tc_build();
  g_fetched = 0U;

  ra8_comic_t      c                    = {};
  ra8_comic_page_t pages[k_tc_page_cap] = {};
  char             names[k_tc_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                tc_read,
                                nullptr,
                                (uint64_t)s_arc_size,
                                pages,
                                (uint32_t)k_tc_page_cap,
                                names,
                                (uint32_t)sizeof(names)));
  /* The open walks only the ZIP tail (EOCD + central directory), never the
   * 200 KiB filler data, so far fewer bytes are fetched than the filler holds. */
  TEST_ASSERT(g_fetched < (uint64_t)k_tc_filler);
  TEST_ASSERT((uint64_t)k_tc_filler < (uint64_t)s_arc_size);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("comic cbz: bounded-RAM open (whole archive never read)");
}

/**
 * @test test_comic_open_arg_guards
 * @brief `ra8_comic_open` rejects NULL args, zero size, and a non-archive stream.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition early return exercised one at a time.)
 */
static void test_comic_open_arg_guards(void)
{
  TEST_BEGIN("comic: open argument guards");
  tc_build();
  ra8_comic_t      c                    = {};
  ra8_comic_page_t pages[k_tc_page_cap] = {};
  char             names[k_tc_name_cap] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_comic_open(nullptr, tc_read, nullptr, 1U, pages, 1U, names, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_comic_open(&c, nullptr, nullptr, 1U, pages, 1U, names, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_comic_open(&c, tc_read, nullptr, 1U, nullptr, 1U, names, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_comic_open(&c, tc_read, nullptr, 1U, pages, 1U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_comic_open(&c, tc_read, nullptr, 0U, pages, 1U, names, 1U));

  /* A non-archive stream is neither ZIP nor RAR. */
  static const uint8_t k_junk[] = "definitely not a zip or rar archive header here";
  s_arc_size                    = sizeof(k_junk);
  memcpy(s_arc, k_junk, sizeof(k_junk));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_comic_open(&c,
                                tc_read,
                                nullptr,
                                (uint64_t)s_arc_size,
                                pages,
                                (uint32_t)k_tc_page_cap,
                                names,
                                (uint32_t)sizeof(names)));
  TEST_ASSERT_EQ(k_ra8_comic_kind_none, ra8_comic_kind(&c));
  TEST_END("comic: open argument guards");
}

/**
 * @test test_comic_page_guards
 * @brief `ra8_comic_page_read` / `_info` reject out-of-range pages and a too-small
 *        buffer; both fail after close (unopened state).
 *
 * @par MC/DC:
 * (no compound decisions under test -- independent single-condition guards, each
 * driven to its failing arm in turn.)
 */
static void test_comic_page_guards(void)
{
  TEST_BEGIN("comic: page read/info guards");
  tc_build();
  ra8_comic_t      c                    = {};
  ra8_comic_page_t pages[k_tc_page_cap] = {};
  char             names[k_tc_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                tc_read,
                                nullptr,
                                (uint64_t)s_arc_size,
                                pages,
                                (uint32_t)k_tc_page_cap,
                                names,
                                (uint32_t)sizeof(names)));

  uint8_t buf[k_cf_png_max] = {};
  size_t  got               = 0U;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, ra8_comic_page_read(&c, 99U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_comic_page_read(&c, 0U, nullptr, sizeof(buf), &got));
  /* cap smaller than the page -> no_mem. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_comic_page_read(&c, 0U, buf, 1U, &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 ra8_comic_page_info(&c, 99U, nullptr, 0U, nullptr, nullptr, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  /* After close the comic is unopened. */
  TEST_ASSERT_EQ(0U, ra8_comic_page_count(&c));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_page_read(&c, 0U, buf, sizeof(buf), &got));
  TEST_END("comic: page read/info guards");
}

/**
 * @test test_comic_page_name_filter
 * @brief The page-name predicate accepts image extensions case-insensitively and
 *        rejects non-images, hidden files, and AppleDouble forks.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the predicate is a chain of
 * single-condition checks; each accept/reject case is exercised directly.)
 */
static void test_comic_page_name_filter(void)
{
  TEST_BEGIN("comic: page-name filter");
  /* Accepted image extensions (case-insensitive), including nested paths. */
  TEST_ASSERT(ra8_comic_is_page_name("a.png", 5U));
  TEST_ASSERT(ra8_comic_is_page_name("a.JPG", 5U));
  TEST_ASSERT(ra8_comic_is_page_name("a.jpeg", 6U));
  TEST_ASSERT(ra8_comic_is_page_name("a.Gif", 5U));
  TEST_ASSERT(ra8_comic_is_page_name("dir/sub/p.BMP", 13U));
  TEST_ASSERT(ra8_comic_is_page_name("sub\\p.png", 9U)); /* backslash path separator */
  /* Rejected: non-image, no extension, hidden, AppleDouble, empty, NULL. */
  TEST_ASSERT(!ra8_comic_is_page_name("a.txt", 5U));
  TEST_ASSERT(!ra8_comic_is_page_name("j", 1U)); /* shorter than any image extension */
  TEST_ASSERT(!ra8_comic_is_page_name("noext", 5U));
  TEST_ASSERT(!ra8_comic_is_page_name(".hidden.png", 11U));
  TEST_ASSERT(!ra8_comic_is_page_name("__MACOSX/._a.png", 16U));
  TEST_ASSERT(!ra8_comic_is_page_name("a.png", 0U));
  TEST_ASSERT(!ra8_comic_is_page_name(nullptr, 5U));
  TEST_END("comic: page-name filter");
}

/**
 * @brief Open the fixture archive with the standard page/name budgets.
 * @param[out] c     Comic handle under test.
 * @param[out] pages Page-descriptor table.
 * @param[out] names Name-string buffer.
 * @param[in]  size  Archive byte length to present.
 * @return The `ra8_comic_open` result code.
 * @pre The shared archive buffer holds @p size bytes.
 * @post No state beyond @p c / @p pages / @p names is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static ra8_err_t tc_open_std(ra8_comic_t* c, ra8_comic_page_t* pages, char* names, uint64_t size)
{
  return ra8_comic_open(c,
                        tc_read,
                        nullptr,
                        size,
                        pages,
                        (uint32_t)k_tc_page_cap,
                        names,
                        (uint32_t)k_tc_name_cap);
}

/**
 * @test test_comic_facade_edges
 * @brief The facade's NULL-reader accessors, unopened page_info, zero-capacity
 *        opens, a short magic read, and the ZIP-magic discrimination legs.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each accessor guard, capacity guard, and
 * magic-byte comparison is an independent single-condition check.)
 */
/**
 * @brief Archive signature bytes fed to the facade's magic discriminator.
 *
 * @details
 * Byte arrays rather than string literals on purpose: these are raw archive
 * signatures written into a binary fixture buffer, so the trailing NUL a string
 * literal carries is not part of the data and must not be copied.
 */
/** @brief ZIP local-file-header signature plus filler to reach 8 bytes. */
static const uint8_t k_tc_sig_zip_lfh[] = {'P', 'K', 0x03, 0x04, 'x', 'x', 'x', 'x'};
/** @brief Just "PK" -- shorter than the 4 magic bytes the ZIP probe reads. */
static const uint8_t k_tc_sig_short[] = {'P', 'K'};
/** @brief Leading 'P' but not "PK": neither ZIP nor RAR. */
static const uint8_t k_tc_sig_not_pk[] = {'P', 'X', 0x03, 0x04, 'j', 'u', 'n', 'k'};
/** @brief "PK" with a 3rd byte that is neither a local header nor an EOCD. */
static const uint8_t k_tc_sig_pk_other[] = {'P', 'K', 0x01, 0x02, 'j', 'u', 'n', 'k'};
/** @brief ZIP end-of-central-directory signature ("PK\\x05\\x06"). */
static const uint8_t k_tc_sig_eocd[] = {'P', 'K', 0x05, 0x06};

static void test_comic_facade_edges(void)
{
  TEST_BEGIN("comic: facade guards + magic discrimination");
  ra8_comic_t      c                    = {};
  ra8_comic_page_t pages[k_tc_page_cap] = {};
  char             names[k_tc_name_cap] = {};

  /* Inline accessor NULL guards (ra8_comic.h). */
  TEST_ASSERT_EQ(0U, ra8_comic_page_count(nullptr));
  TEST_ASSERT_EQ(k_ra8_comic_kind_none, ra8_comic_kind(nullptr));

  /* page_info on a never-opened comic. */
  uint16_t nl  = 0U;
  uint64_t raw = 0U;
  uint8_t  ex  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_comic_page_info(&c, 0U, nullptr, 0U, &nl, &raw, &ex));

  /* Zero page / name capacities (valid size, non-NULL buffers). */
  memcpy(s_arc, k_tc_sig_zip_lfh, sizeof(k_tc_sig_zip_lfh));
  s_arc_size = 8U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_comic_open(&c, tc_read, nullptr, 8U, pages, 0U, names, (uint32_t)sizeof(names)));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    ra8_comic_open(&c, tc_read, nullptr, 8U, pages, (uint32_t)k_tc_page_cap, names, 0U));

  /* Magic read shorter than the four bytes the ZIP test needs. */
  memcpy(s_arc, k_tc_sig_short, sizeof(k_tc_sig_short));
  s_arc_size = 2U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, tc_open_std(&c, pages, names, 2U));

  /* 'P' but not "PK" -> neither ZIP nor RAR. */
  memcpy(s_arc, k_tc_sig_not_pk, sizeof(k_tc_sig_not_pk));
  s_arc_size = 8U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, tc_open_std(&c, pages, names, 8U));

  /* "PK" but the 3rd byte is neither a local-file-header nor an EOCD marker. */
  memcpy(s_arc, k_tc_sig_pk_other, sizeof(k_tc_sig_pk_other));
  s_arc_size = 8U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, tc_open_std(&c, pages, names, 8U));

  /* Empty-archive EOCD "PK\x05\x06": the ZIP magic's EOCD leg is taken; an
   * archive with no image entries opens to no pages (or a miniz open error). */
  memset(s_arc, 0, 22U);
  memcpy(s_arc, k_tc_sig_eocd, sizeof(k_tc_sig_eocd));
  s_arc_size           = k_comic_s_arc_size_22;
  const ra8_err_t eocd = tc_open_std(&c, pages, names, 22U);
  TEST_ASSERT(eocd != k_ra8_ok);
  TEST_ASSERT_EQ(k_ra8_comic_kind_none, ra8_comic_kind(&c));
  TEST_END("comic: facade guards + magic discrimination");
}

/**
 * @test test_comic_page_add_caps
 * @brief `ra8_comic_page_add` rejects a full page index and a full name arena.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the page-count and arena-length overflow
 * guards are independent single-condition early returns.)
 */
static void test_comic_page_add_caps(void)
{
  TEST_BEGIN("comic: page index / name arena overflow guards");
  ra8_comic_page_t pg[1] = {};
  char             nm[4] = {};
  ra8_comic_t      c =
    {.pages = pg, .page_cap = 1U, .page_count = 1U, .names = nm, .names_cap = 4U, .names_len = 0U};

  /* Page index already at capacity. (args: name,len,raw,extractable,method,zip,off,pack) */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_comic_page_add(&c, "x", 1U, 0U, 1U, 0U, 0U, 0U, 0U));

  /* Name arena cannot hold the next name. */
  c.page_count = 0U;
  c.names_len  = 3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, ra8_comic_page_add(&c, "xy", 2U, 0U, 1U, 0U, 0U, 0U, 0U));
  TEST_END("comic: page index / name arena overflow guards");
}

/**
 * @brief Test entry point -- runs the CBZ comic suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int32_t main(void)
{
  test_comic_cbz_sorted_extract_decode();
  test_comic_cbz_bounded_ram();
  test_comic_open_arg_guards();
  test_comic_page_guards();
  test_comic_page_name_filter();
  test_comic_facade_edges();
  test_comic_page_add_caps();
  (void)fprintf(stderr, "[OK  ] test_ra8_comic.c\n");
  return 0;
}
