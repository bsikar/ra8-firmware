/**
 * @file test_comic.c
 * @brief Tests for the CBZ (ZIP-of-images) comic reader (comic +
 * comic_cbz).
 *
 * @details
 * Builds a *real* CBZ in memory with miniz's ZIP writer: four genuine
 * stb-decodable PNG page images (some STORE, some DEFLATE) placed out of order
 * and in a nested folder, plus entries the reader must skip (a text file, a
 * macOS AppleDouble fork) and one large uncompressed filler entry that
 * dominates the archive size but is never referenced. The suite opens that CBZ
 * through `comic_open` over a seek+read callback -- the #151 streaming path
 * -- and proves:
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

#include "comic.h"
#include "comic_fixture.h"
#include "comic_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "unity_minimal.h"

/** @brief Size of a ZIP end-of-central-directory record with no comment. */
typedef enum : uint8_t {
  k_tc_eocd_bytes = 22U, /**< Minimal EOCD record length. */
} tc_eocd_t;

/**
 * @enum comic_fixture_t
 * @brief The payload generators and their seeds, plus buffer capacities and
 * payload sizes.
 */
typedef enum : uint8_t {
  k_comic_filler_stride = 31U, /**< Stride `i * 31 + (i >> 3)`; prime, so filler
                                  stays incompressible, recognisable. */
  k_comic_truncated_arc_bytes =
    22U,                     /**< Archive shorter than its header claims; reader must fail, not
              read past the end. */
  k_comic_page_side_px = 5U, /**< Square fixture page side, in pixels; small
                                enough a whole page fits one tile. */
} comic_fixture_t;

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

/** @brief The four source page images (built once by internal_tc_build). */
static tc_img_t s_imgs[k_tc_img_count];
/** @brief The built CBZ archive bytes. */
static uint8_t s_arc[k_tc_arc_cap];
/** @brief Length of the built archive. */
static size_t s_arc_size = 0U;
/** @brief Bytes fetched through the read callback (bounded-RAM probe). */
static uint64_t s_fetched = 0U;

/**
 * @brief Seek-and-read callback over the in-memory CBZ fixture.
 * @details Copies only the requested in-range archive suffix and accounts for
 * every returned byte in ::s_fetched so the bounded-open test can distinguish
 * metadata reads from an accidental whole-archive read.
 * @param[in] ctx Unused callback context retained for the comic-reader ABI.
 * @param[in] off Zero-based archive offset at which reading starts.
 * @param[out] buf Destination receiving the available fixture bytes.
 * @param[in] len Maximum number of bytes the caller permits.
 * @return Number of bytes copied to @p buf.
 * @retval 0 @p off is at or beyond the current archive length.
 * @pre @p buf addresses at least @p len writable bytes when @p len is nonzero.
 * @pre ::s_arc_size does not exceed the capacity of ::s_arc.
 * @post The result never exceeds @p len or the bytes remaining after @p off.
 * @post ::s_fetched increases by exactly the returned byte count.
 * @note Not thread-safe because the fixture and byte counter are shared.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_tc_read(void* ctx, uint64_t off, void* buf, size_t len)
{
  (void)ctx;
  if (off >= (uint64_t)s_arc_size) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s_arc_size - off;
  const size_t   n     = (len > (size_t)avail) ? (size_t)avail : len;
  (void)memcpy(buf, &s_arc[off], n);
  s_fetched += (uint64_t)n;
  return n;
}

/**
 * @brief Find a generated source-image descriptor by archive name.
 * @details Scans the fixed fixture descriptor table in insertion order and
 * returns the first byte-exact name match.
 * @param[in] name NUL-terminated archive member name to locate.
 * @return Pointer to the matching descriptor, or NULL when no name matches.
 * @retval nullptr No generated fixture uses @p name.
 * @pre @p name is non-NULL and NUL-terminated.
 * @pre Every entry in ::s_imgs has an initialized non-NULL name.
 * @post The descriptor table and its encoded image bytes remain unchanged.
 * @post A non-NULL return points to an element of ::s_imgs.
 * @note The returned pointer remains valid for the lifetime of the process.
 * @since 0.1.0
 */
RA8_INTERNAL static const tc_img_t* internal_tc_find(const char* name)
{
  for (uint32_t i = 0U; i < k_tc_img_count; ++i) {
    if (strcmp(s_imgs[i].name, name) == 0) {
      return &s_imgs[i];
    }
  }
  return nullptr;
}

/**
 * @brief Add one generated page image to the active ZIP writer.
 * @details Forwards the fixture name and exact PNG byte span to miniz and
 * converts a writer failure into an immediate test assertion.
 * @param[in,out] zip Initialized heap-backed ZIP writer.
 * @param[in] im Generated image descriptor and encoded PNG payload.
 * @param[in] level Miniz compression level for this archive member.
 * @pre @p zip is in miniz writer mode and has not been finalized.
 * @pre @p im names a nonempty payload whose length fits its PNG array.
 * @post The writer contains one additional member named @p im->name.
 * @post The source descriptor and PNG bytes remain unchanged.
 * @note Test-only helper; a miniz error fails through unity immediately.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_tc_add_img(mz_zip_archive* zip, const tc_img_t* im, mz_uint level)
{
  TEST_ASSERT(mz_zip_writer_add_mem(zip, im->name, im->png, im->plen, level) == MZ_TRUE);
}

/**
 * @brief Build the four PNGs and the CBZ archive into ::s_arc.
 * @details Images are inserted scrambled and in a nested folder; the archive
 * also carries a text file, a macOS AppleDouble fork, and a large STORE filler
 * -- all of which the reader must skip.
 * @pre ::s_arc has capacity for the configured heap-writer ceiling.
 * @pre Every image slot has room for a generated fixture PNG.
 * @post ::s_arc_size describes a finalized CBZ held entirely in ::s_arc.
 * @post ::s_imgs contains four valid source descriptors used as extraction
 * oracles.
 * @note Rebuilds the shared fixture and is therefore not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_tc_build(void)
{
  s_imgs[0] = (tc_img_t){.name = "nested/page003.png", .w = 8U, .h = 6U, .seed = 3U};
  s_imgs[1] = (tc_img_t){.name = "page000.png", .w = 4U, .h = 4U, .seed = 0U};
  s_imgs[2] = (tc_img_t){.name = "page001.png", .w = 6U, .h = 8U, .seed = 1U};
  s_imgs[3] = (tc_img_t){.name = "page002.png",
                         .w    = k_comic_page_side_px,
                         .h    = k_comic_page_side_px,
                         .seed = 2U};
  for (uint32_t i = 0U; i < k_tc_img_count; ++i) {
    s_imgs[i].plen =
      cf_make_png(s_imgs[i].w, s_imgs[i].h, s_imgs[i].seed, s_imgs[i].png, sizeof(s_imgs[i].png));
    TEST_ASSERT(s_imgs[i].plen > 0U);
  }

  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_tc_arc_cap) == MZ_TRUE);

  /* Scrambled insertion order; two STORE, two DEFLATE, to exercise both paths.
   */
  internal_tc_add_img(&zip, &s_imgs[2], MZ_NO_COMPRESSION);      /* page001.png        */
  internal_tc_add_img(&zip, &s_imgs[0], MZ_NO_COMPRESSION);      /* nested/page003.png */
  internal_tc_add_img(&zip, &s_imgs[3], MZ_DEFAULT_COMPRESSION); /* page002.png        */
  internal_tc_add_img(&zip, &s_imgs[1], MZ_DEFAULT_COMPRESSION); /* page000.png        */

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

  /* Large unreferenced s_filler (STORE) -- dominates size, never read on open. */
  static uint8_t s_filler[k_tc_filler];
  for (size_t i = 0U; i < sizeof(s_filler); ++i) {
    s_filler[i] = (uint8_t)((i * k_comic_filler_stride) + (i >> 3U));
  }
  TEST_ASSERT(
    mz_zip_writer_add_mem(&zip, "big_filler.bin", s_filler, sizeof(s_filler), MZ_NO_COMPRESSION) ==
    MZ_TRUE);

  void*  heap = nullptr;
  size_t hsz  = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &heap, &hsz) == MZ_TRUE);
  TEST_ASSERT((heap != nullptr) && (hsz <= (size_t)k_tc_arc_cap));
  (void)memcpy(s_arc, heap, hsz);
  s_arc_size = hsz;
  mz_free(heap);
  mz_zip_writer_end(&zip);
}

/**
 * @test internal_test_comic_cbz_sorted_extract_decode
 * @brief Open the CBZ streamed; pages come out sorted, extract
 * byte-identically, and decode through stb_image to the right dimensions;
 * skip-entries are excluded.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the facade's guards are
 * single-condition; this is a round-trip oracle: page order, byte-equality, and
 * decoded dimensions against the source images.)
 * @details Builds a mixed STORE/DEFLATE CBZ, opens it through the callback,
 * then compares sorted names, extracted bytes, and decoded dimensions with
 * the independently generated image descriptors.
 * @pre The miniz writer and arena-backed image decoder are available.
 * @pre The fixture capacities can hold all four generated PNG pages.
 * @post Every enumerated page matches its source image byte-for-byte.
 * @post The comic handle is closed and releases its miniz arena binding.
 * @note Uses shared fixture buffers and must run serially.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_comic_cbz_sorted_extract_decode(void)
{
  TEST_BEGIN("comic cbz: sorted pages extract + decode");
  internal_tc_build();

  comic_t      c                    = {};
  comic_page_t pages[k_tc_page_cap] = {};
  char         names[k_tc_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 comic_open(&c,
                            internal_tc_read,
                            nullptr,
                            (uint64_t)s_arc_size,
                            pages,
                            (uint32_t)k_tc_page_cap,
                            names,
                            (uint32_t)sizeof(names)));
  TEST_ASSERT_EQ(k_comic_kind_cbz, comic_kind(&c));
  TEST_ASSERT_EQ(k_tc_img_count, comic_page_count(&c));

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
    TEST_ASSERT_EQ(k_ra8_ok, comic_page_info(&c, i, nb, (uint16_t)sizeof(nb), &nl, &raw, &ex));
    nb[nl] = '\0';
    TEST_ASSERT_EQ(0, strcmp(nb, k_sorted[i]));
    TEST_ASSERT_EQ(1U, ex);

    const tc_img_t* src = internal_tc_find(k_sorted[i]);
    TEST_ASSERT(src != nullptr);
    TEST_ASSERT_EQ(src->plen, raw);

    uint8_t buf[k_cf_png_max] = {};
    size_t  got               = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, comic_page_read(&c, i, buf, sizeof(buf), &got));
    TEST_ASSERT_EQ(src->plen, got);
    TEST_ASSERT_EQ(0, memcmp(buf, src->png, got));

    /* Extract -> decode to real pixels through the project's arena-backed stb.
     */
    TEST_ASSERT(cf_decode_ok(buf, got, (int)src->w, (int)src->h));
  }

  TEST_ASSERT_EQ(k_ra8_ok, comic_close(&c));
  TEST_END("comic cbz: sorted pages extract + decode");
}

/**
 * @test internal_test_comic_cbz_bounded_ram
 * @brief Opening the CBZ never reads the whole archive: the large filler entry
 * is untouched, so the bytes fetched on open are far below the file size.
 *
 * @par MC/DC:
 * (no compound decisions under test -- a single inequality over the
 * fetched-byte counter versus the unreferenced filler size.)
 * @details Resets the callback byte counter immediately before open and proves
 * that parsing the ZIP directory never fetches the large unreferenced member.
 * @pre The configured filler dominates the generated archive's metadata size.
 * @pre ::internal_tc_read is the only reader bound to the comic handle.
 * @post The open byte count is less than the filler payload length.
 * @post The successfully opened comic is closed before returning.
 * @note Measures I/O volume, not process heap usage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_comic_cbz_bounded_ram(void)
{
  TEST_BEGIN("comic cbz: bounded-RAM open (whole archive never read)");
  internal_tc_build();
  s_fetched = 0U;

  comic_t      c                    = {};
  comic_page_t pages[k_tc_page_cap] = {};
  char         names[k_tc_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 comic_open(&c,
                            internal_tc_read,
                            nullptr,
                            (uint64_t)s_arc_size,
                            pages,
                            (uint32_t)k_tc_page_cap,
                            names,
                            (uint32_t)sizeof(names)));
  /* The open walks only the ZIP tail (EOCD + central directory), never the
   * 200 KiB filler data, so far fewer bytes are fetched than the filler holds.
   */
  TEST_ASSERT(s_fetched < (uint64_t)k_tc_filler);
  TEST_ASSERT((uint64_t)k_tc_filler < (uint64_t)s_arc_size);
  TEST_ASSERT_EQ(k_ra8_ok, comic_close(&c));
  TEST_END("comic cbz: bounded-RAM open (whole archive never read)");
}

/**
 * @brief Open the shared CBZ fixture with the standard descriptor budgets.
 * @details Binds ::internal_tc_read and the canonical page/name capacities so
 * lifecycle tests vary only the handle and presented archive length.
 * @param[out] c Comic handle populated on a successful open.
 * @param[out] pages Caller-owned page descriptor table.
 * @param[out] names Caller-owned page-name arena.
 * @param[in] size Number of bytes from ::s_arc presented to the reader.
 * @return Result reported by ::comic_open.
 * @retval k_ra8_ok The shared fixture opened successfully.
 * @pre @p c, @p pages, and @p names are non-null and independently writable.
 * @pre @p size is no greater than ::s_arc_size.
 * @post Success binds @p c only to the supplied caller-owned storage.
 * @post Failure leaves no live miniz arena owned by @p c.
 * @note The callback uses shared archive state, so calls are serialized.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_tc_open_std(comic_t* c, comic_page_t* pages, char* names, uint64_t size);

/**
 * @test internal_test_two_live_cbz_isolate_miniz_arenas
 * @brief Two CBZ readers stay live independently across close/reopen.
 *
 * @par MC/DC:
 * This lifecycle test does not claim an independence pair for a compound
 * production decision. Its vector sequence opens two valid readers, closes
 * only the first, proves the second can still read, then reopens the first and
 * proves the second can still read again. Thus argument guards see only their
 * all-valid control while the independently owned arena state is varied.
 * @details Opens two handles over the same immutable archive but distinct
 * descriptor/name storage, then alternates close, read, and reopen operations
 * to expose any accidental global miniz-arena ownership.
 * @pre The generated CBZ is valid and fits both caller-owned storage sets.
 * @pre The two comic handles start zero-initialized and distinct.
 * @post Closing or reopening the first never invalidates reads from the second.
 * @post Both handles are closed and own no arena when the test returns.
 * @note Runs serially because the read callback uses shared archive bytes.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_two_live_cbz_isolate_miniz_arenas(void)
{
  TEST_BEGIN("comic cbz: two live arenas isolate + reopen");
  internal_tc_build();
  comic_t      first                       = {};
  comic_t      second                      = {};
  comic_page_t first_pages[k_tc_page_cap]  = {};
  comic_page_t second_pages[k_tc_page_cap] = {};
  char         first_names[k_tc_name_cap]  = {};
  char         second_names[k_tc_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok, internal_tc_open_std(&first, first_pages, first_names, s_arc_size));
  TEST_ASSERT_EQ(k_ra8_ok, internal_tc_open_std(&second, second_pages, second_names, s_arc_size));
  TEST_ASSERT(first.miniz_arena.base == &first.miniz_workspace.bytes[0]);
  TEST_ASSERT(second.miniz_arena.base == &second.miniz_workspace.bytes[0]);
  TEST_ASSERT(first.miniz_arena.base != second.miniz_arena.base);

  uint8_t page[k_cf_png_max];
  size_t  got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, comic_page_read(&first, 0U, page, sizeof(page), &got));
  TEST_ASSERT(got > 0U);
  TEST_ASSERT_EQ(k_ra8_ok, comic_close(&first));
  TEST_ASSERT(first.miniz_arena.base == nullptr);
  TEST_ASSERT_EQ(k_ra8_ok, comic_page_read(&second, 1U, page, sizeof(page), &got));
  TEST_ASSERT(got > 0U);

  TEST_ASSERT_EQ(k_ra8_ok, internal_tc_open_std(&first, first_pages, first_names, s_arc_size));
  TEST_ASSERT_EQ(k_ra8_ok, comic_close(&first));
  TEST_ASSERT_EQ(k_ra8_ok, comic_page_read(&second, 2U, page, sizeof(page), &got));
  TEST_ASSERT(got > 0U);
  TEST_ASSERT_EQ(k_ra8_ok, comic_close(&second));
  TEST_END("comic cbz: two live arenas isolate + reopen");
}

/**
 * @test internal_test_comic_open_arg_guards
 * @brief `comic_open` rejects NULL args, zero size, and a non-archive
 * stream.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each guard is an independent
 * single-condition early return exercised one at a time.)
 * @details Drives each required-pointer and nonzero-size guard independently,
 * then presents ordinary junk to prove format rejection restores kind-none.
 * @pre The shared fixture can first hold a valid CBZ and then the junk vector.
 * @pre The page and name workspaces are valid for all non-null vectors.
 * @post Every invalid argument returns its documented error class.
 * @post The rejected junk stream leaves the comic kind set to none.
 * @note No handle remains open after any exercised failure path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_comic_open_arg_guards(void)
{
  TEST_BEGIN("comic: open argument guards");
  internal_tc_build();
  comic_t      c                    = {};
  comic_page_t pages[k_tc_page_cap] = {};
  char         names[k_tc_name_cap] = {};

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 comic_open(nullptr, internal_tc_read, nullptr, 1U, pages, 1U, names, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, comic_open(&c, nullptr, nullptr, 1U, pages, 1U, names, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 comic_open(&c, internal_tc_read, nullptr, 1U, nullptr, 1U, names, 1U));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 comic_open(&c, internal_tc_read, nullptr, 1U, pages, 1U, nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 comic_open(&c, internal_tc_read, nullptr, 0U, pages, 1U, names, 1U));

  /* A non-archive stream is neither ZIP nor RAR. */
  static const uint8_t k_junk[] = "definitely not a zip or rar archive header here";
  s_arc_size                    = sizeof(k_junk);
  (void)memcpy(s_arc, k_junk, sizeof(k_junk));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 comic_open(&c,
                            internal_tc_read,
                            nullptr,
                            (uint64_t)s_arc_size,
                            pages,
                            (uint32_t)k_tc_page_cap,
                            names,
                            (uint32_t)sizeof(names)));
  TEST_ASSERT_EQ(k_comic_kind_none, comic_kind(&c));
  TEST_END("comic: open argument guards");
}

/**
 * @test internal_test_comic_page_guards
 * @brief `comic_page_read` / `_info` reject out-of-range pages and a
 * too-small buffer; both fail after close (unopened state).
 *
 * @par MC/DC:
 * (no compound decisions under test -- independent single-condition guards,
 * each driven to its failing arm in turn.)
 * @details Opens the canonical CBZ once, then varies the page index, output
 * pointer, capacity, and lifecycle state while preserving every other input.
 * @pre The canonical fixture opens successfully with the standard budgets.
 * @pre The page scratch buffer is large enough for a healthy extraction.
 * @post Guard failures return without reporting extracted bytes.
 * @post Closing resets page count and makes subsequent reads invalid-state.
 * @note Exercises only public page APIs after the initial open.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_comic_page_guards(void)
{
  TEST_BEGIN("comic: page read/info guards");
  internal_tc_build();
  comic_t      c                    = {};
  comic_page_t pages[k_tc_page_cap] = {};
  char         names[k_tc_name_cap] = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 comic_open(&c,
                            internal_tc_read,
                            nullptr,
                            (uint64_t)s_arc_size,
                            pages,
                            (uint32_t)k_tc_page_cap,
                            names,
                            (uint32_t)sizeof(names)));

  uint8_t buf[k_cf_png_max] = {};
  size_t  got               = 0U;
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, comic_page_read(&c, 99U, buf, sizeof(buf), &got));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, comic_page_read(&c, 0U, nullptr, sizeof(buf), &got));
  /* cap smaller than the page -> no_mem. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem, comic_page_read(&c, 0U, buf, 1U, &got));
  TEST_ASSERT_EQ(0U, got);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 comic_page_info(&c, 99U, nullptr, 0U, nullptr, nullptr, nullptr));

  TEST_ASSERT_EQ(k_ra8_ok, comic_close(&c));
  /* After close the comic is unopened. */
  TEST_ASSERT_EQ(0U, comic_page_count(&c));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, comic_page_read(&c, 0U, buf, sizeof(buf), &got));
  TEST_END("comic: page read/info guards");
}

/**
 * @test internal_test_comic_page_name_filter
 * @brief The page-name predicate accepts image extensions case-insensitively
 * and rejects non-images, hidden files, and AppleDouble forks.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the predicate is a chain of
 * single-condition checks; each accept/reject case is exercised directly.)
 * @details Covers supported extensions with mixed case and both path
 * separators, then rejects short, hidden, AppleDouble, empty, and null names.
 * @pre The private page-name seam is linked into this direct test target.
 * @pre Each supplied length matches the intended bounded name span.
 * @post Every supported image suffix is accepted independent of case.
 * @post Every hidden, metadata, malformed, or non-image vector is rejected.
 * @note Pure predicate test; it does not build or open an archive.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_comic_page_name_filter(void)
{
  TEST_BEGIN("comic: page-name filter");
  /* Accepted image extensions (case-insensitive), including nested paths. */
  TEST_ASSERT(priv_comic_is_page_name("a.png", 5U));
  TEST_ASSERT(priv_comic_is_page_name("a.JPG", 5U));
  TEST_ASSERT(priv_comic_is_page_name("a.jpeg", 6U));
  TEST_ASSERT(priv_comic_is_page_name("a.Gif", 5U));
  TEST_ASSERT(priv_comic_is_page_name("dir/sub/p.BMP", 13U));
  TEST_ASSERT(priv_comic_is_page_name("sub\\p.png", 9U)); /* backslash path separator */
  /* Rejected: non-image, no extension, hidden, AppleDouble, empty, NULL. */
  TEST_ASSERT(!priv_comic_is_page_name("a.txt", 5U));
  TEST_ASSERT(!priv_comic_is_page_name("j", 1U)); /* shorter than any image extension */
  TEST_ASSERT(!priv_comic_is_page_name("noext", 5U));
  TEST_ASSERT(!priv_comic_is_page_name(".hidden.png", 11U));
  TEST_ASSERT(!priv_comic_is_page_name("__MACOSX/._a.png", 16U));
  TEST_ASSERT(!priv_comic_is_page_name("a.png", 0U));
  TEST_ASSERT(!priv_comic_is_page_name(nullptr, 5U));
  TEST_END("comic: page-name filter");
}

/**
 * @brief Open the fixture archive with the standard page/name budgets.
 * @details Centralizes the callback and capacity binding used by facade and
 * multi-handle tests while allowing the visible archive length to vary.
 * @param[out] c     Comic handle under test.
 * @param[out] pages Page-descriptor table.
 * @param[out] names Name-string buffer.
 * @param[in]  size  Archive byte length to present.
 * @return The `comic_open` result code.
 * @retval k_ra8_ok The presented archive opened and populated @p c.
 * @pre The shared archive buffer holds @p size bytes.
 * @pre @p c, @p pages, and @p names are valid caller-owned storage.
 * @post Success binds @p c to @p pages and @p names with standard capacities.
 * @post No state beyond @p c, @p pages, and @p names is modified.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_tc_open_std(comic_t* c, comic_page_t* pages, char* names, uint64_t size)
{
  return comic_open(c,
                    internal_tc_read,
                    nullptr,
                    size,
                    pages,
                    (uint32_t)k_tc_page_cap,
                    names,
                    (uint32_t)k_tc_name_cap);
}

/*
 * @brief Archive signature bytes fed to the facade's magic discriminator.
 *
 * @details
 * Byte arrays rather than string literals on purpose: these are raw archive
 * signatures written into a binary fixture buffer, so the trailing NUL a string
 * literal carries is not part of the data and must not be copied.
 */
/**
 * @test internal_test_comic_facade_edges
 * @brief The facade's NULL-reader accessors, unopened page_info, zero-capacity
 *        opens, a short magic read, and the ZIP-magic discrimination legs.
 *
 * @par MC/DC:
 * (no compound decisions under test -- each accessor guard, capacity guard, and
 * magic-byte comparison is an independent single-condition check.)
 * @details Drives null accessors, unopened page metadata, zero capacities,
 * truncated magic, near-ZIP signatures, and an empty-archive EOCD through the
 * facade without relying on a filesystem.
 * @pre ::s_arc can hold every bounded signature vector used by the test.
 * @pre The standard page and name buffers are writable and initially empty.
 * @post Each guard and magic-discriminator leg returns the expected error
 * class.
 * @post No rejected signature leaves a live comic kind or archive binding.
 * @note Uses private fixture bytes but only public facade operations.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_comic_facade_edges(void)
{
  static const uint8_t s_tc_sig_zip_lfh[]  = {'P', 'K', 0x03, 0x04, 'x', 'x', 'x', 'x'};
  static const uint8_t s_tc_sig_short[]    = {'P', 'K'};
  static const uint8_t s_tc_sig_not_pk[]   = {'P', 'X', 0x03, 0x04, 'j', 'u', 'n', 'k'};
  static const uint8_t s_tc_sig_pk_other[] = {'P', 'K', 0x01, 0x02, 'j', 'u', 'n', 'k'};
  static const uint8_t s_tc_sig_eocd[]     = {'P', 'K', 0x05, 0x06};
  TEST_BEGIN("comic: facade guards + magic discrimination");
  comic_t      c                    = {};
  comic_page_t pages[k_tc_page_cap] = {};
  char         names[k_tc_name_cap] = {};

  /* Inline accessor NULL guards (comic.h). */
  TEST_ASSERT_EQ(0U, comic_page_count(nullptr));
  TEST_ASSERT_EQ(k_comic_kind_none, comic_kind(nullptr));

  /* page_info on a never-opened comic. */
  uint16_t nl  = 0U;
  uint64_t raw = 0U;
  uint8_t  ex  = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, comic_page_info(&c, 0U, nullptr, 0U, &nl, &raw, &ex));

  /* Zero page / name capacities (valid size, non-NULL buffers). */
  (void)memcpy(s_arc, s_tc_sig_zip_lfh, sizeof(s_tc_sig_zip_lfh));
  s_arc_size = 8U;
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    comic_open(&c, internal_tc_read, nullptr, 8U, pages, 0U, names, (uint32_t)sizeof(names)));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    comic_open(&c, internal_tc_read, nullptr, 8U, pages, (uint32_t)k_tc_page_cap, names, 0U));

  /* Magic read shorter than the four bytes the ZIP test needs. */
  (void)memcpy(s_arc, s_tc_sig_short, sizeof(s_tc_sig_short));
  s_arc_size = 2U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, internal_tc_open_std(&c, pages, names, 2U));

  /* 'P' but not "PK" -> neither ZIP nor RAR. */
  (void)memcpy(s_arc, s_tc_sig_not_pk, sizeof(s_tc_sig_not_pk));
  s_arc_size = 8U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, internal_tc_open_std(&c, pages, names, 8U));

  /* "PK" but the 3rd byte is neither a local-file-header nor an EOCD marker. */
  (void)memcpy(s_arc, s_tc_sig_pk_other, sizeof(s_tc_sig_pk_other));
  s_arc_size = 8U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, internal_tc_open_std(&c, pages, names, 8U));

  /* Empty-archive EOCD "PK\x05\x06": the ZIP magic's EOCD leg is taken; an
   * archive with no image entries opens to no pages (or a miniz open error). */
  (void)memset(s_arc, 0, (size_t)k_tc_eocd_bytes);
  (void)memcpy(s_arc, s_tc_sig_eocd, sizeof(s_tc_sig_eocd));
  s_arc_size           = k_comic_truncated_arc_bytes;
  const ra8_err_t eocd = internal_tc_open_std(&c, pages, names, 22U);
  TEST_ASSERT(eocd != k_ra8_ok);
  TEST_ASSERT_EQ(k_comic_kind_none, comic_kind(&c));
  TEST_END("comic: facade guards + magic discrimination");
}

/**
 * @test internal_test_comic_page_add_caps
 * @brief `priv_comic_page_add` rejects a full page index and a full name arena.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the page-count and arena-length overflow
 * guards are independent single-condition early returns.)
 * @details Constructs the smallest possible comic workspace and independently
 * saturates its page table and name arena before invoking the private add seam.
 * @pre The private page-add seam is available to this direct test.
 * @pre The one-entry descriptor and four-byte name arrays remain in scope.
 * @post A full descriptor table returns invalid-size without mutation.
 * @post An insufficient name arena returns invalid-size without adding a page.
 * @note No archive parsing or allocator-backed state is involved.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_comic_page_add_caps(void)
{
  TEST_BEGIN("comic: page index / name arena overflow guards");
  comic_page_t pg[1] = {};
  char         nm[4] = {};
  comic_t      c =
    {.pages = pg, .page_cap = 1U, .page_count = 1U, .names = nm, .names_cap = 4U, .names_len = 0U};

  /* Page index already at capacity. (args:
   * name,len,raw,extractable,method,zip,off,pack) */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_comic_page_add(&c, "x", 1U, 0U, 1U, 0U, 0U, 0U, 0U));

  /* Name arena cannot hold the next name. */
  c.page_count = 0U;
  c.names_len  = 3U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size, priv_comic_page_add(&c, "xy", 2U, 0U, 1U, 0U, 0U, 0U, 0U));
  TEST_END("comic: page index / name arena overflow guards");
}

/**
 * @brief Consume expected guard-path log bytes without touching host ITM MMIO.
 * @details Implements the injected byte-sink contract as a deliberate no-op so
 * negative tests can exercise production diagnostics safely on the host.
 * @param[in] ctx Unused logger context supplied by the test composition.
 * @param[in] byte Diagnostic byte intentionally discarded by the sink.
 * @pre The logger invokes the callback with an arbitrary context value.
 * @pre @p byte is a single already-formatted diagnostic octet.
 * @post No memory, stream, or device state is modified.
 * @post The sink returns after consuming exactly one callback byte.
 * @note Reentrant because it holds no state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)byte;
}

/**
 * @brief Test entry point -- runs the CBZ comic suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int main(void)
{
  ra8_log_set_byte_sink(internal_log_sink, nullptr);
  internal_test_comic_cbz_sorted_extract_decode();
  internal_test_comic_cbz_bounded_ram();
  internal_test_two_live_cbz_isolate_miniz_arenas();
  internal_test_comic_open_arg_guards();
  internal_test_comic_page_guards();
  internal_test_comic_page_name_filter();
  internal_test_comic_facade_edges();
  internal_test_comic_page_add_caps();
  return 0;
}
