/**
 * @file test_ra8_comic_cbt.c
 * @brief Tests for the CBT (tar) comic backend and the wrapped (gz/xz) open.
 *
 * @details
 * Builds real tar comics block-by-block in memory and drives them through
 * the public comic facade, proving:
 *
 *   1. a bare `.cbt` opens via the magic-less tar probe: pages enumerate in
 *      sorted order, extract byte-exactly, and non-page members are skipped,
 *   2. a gzip-wrapped comic (`.cbt.gz` / `.tar.gz`, built in-memory around
 *      a tdefl DEFLATE body) opens through `ra8_comic_open_wrapped` into a
 *      caller arena,
 *   3. an XZ-wrapped comic (`.tar.xz`, the committed `k_fx_xz_cbt` fixture)
 *      opens likewise through the XZ session scratch,
 *   4. a bare container passes through the wrapped open unchanged
 *      (ZIP -> CBZ), and wrapper-in-wrapper nesting bombs (gzip-of-gzip,
 *      gzip-of-xz) are rejected as `k_ra8_err_decomp_depth`,
 *   5. the wrapped-open argument guards (alignment, arena size, NULL XZ
 *      scratch) fail closed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_comic.h"
#include "ra8_comic_internal.h"
#include "ra8_err.h"
#include "unarch_xz_fixture.h"
#include "unity_minimal.h"

/**
 * @enum comic_cbt_fixture_t
 * @brief Buffer capacities and payload sizes, plus the byte-level helpers.
 */
typedef enum : uint8_t {
  k_gzip_magic_b0   = 0x1FU, /**< First byte of the gzip magic (0x1F8B). */
  k_gzip_magic_b1   = 0x8BU, /**< Its second byte.                       */
  k_tcb_payload_len = 7U,    /**< Fixture payload length, short enough to stay
                              inside one 512-byte tar record. */
  /** Capacity of the entry-name scratch buffer. */
  k_tcb_name_cap = 64,
  /** Truncates each shifted CRC and length byte for the gzip trailer. */
  k_byte_mask = 0xFFU,
} comic_cbt_fixture_t;

/**
 * @enum comic_cbt_fixture2_t
 * @brief Buffer capacities and payload sizes.
 */
typedef enum : uint16_t {
  k_tcb_non_archive_bytes = 600U, /**< Filler past any header the sniffer reads;
                                     rejection turns on content, not length. */
} comic_cbt_fixture2_t;

/**
 * @enum comic_cbt_tcb_octal_0644_t
 * @brief Named octal mode bits used by this file.
 */
typedef enum : uint16_t {
  k_comic_cbt_tcb_octal_0644 = 0644U, /**< tar member mode: rw-r--r--. */
} comic_cbt_tcb_octal_0644_t;

/**
 * @enum tcb_dim_t
 * @brief Archive / buffer budgets (tests are magic-number exempt).
 */
typedef enum : uint32_t {
  k_tcb_arc_cap  = 64U * 1024U,  /**< Outer archive build buffer. */
  k_tcb_arena    = 64U * 1024U,  /**< Wrapped-open unwrap arena.  */
  k_tcb_scratch  = 192U * 1024U, /**< XZ session scratch.         */
  k_tcb_page_cap = 8U,           /**< Page-index capacity.        */
  k_tcb_out_cap  = 4096U,        /**< Page extraction buffer.     */
} tcb_dim_t;

/** @brief Fill byte for the "not an archive at all" rejection fixture. */
typedef enum : uint8_t {
  k_tcb_fill_non_archive = 0xA5U, /**< Matches no archive magic. */
} tcb_fill_t;

/**
 * @enum tcb_tar_layout_t
 * @brief POSIX ustar header field offsets and widths, within one 512-byte
 * block.
 *
 * @details
 * Offsets and widths are fixed by the ustar format, so they are named by the
 * FIELD they address rather than by their value. Two pairs share a value but
 * not a role: 100 is both the name width and the mode offset, and 155 is both
 * the prefix width and the offset of the checksum field's trailing space.
 */
typedef enum : uint16_t {
  k_tcb_tar_block        = 512U,  /**< Tar block size, bytes.                  */
  k_tcb_end_marker_bytes = 1024U, /**< End-of-archive marker: two zero blocks. */
  k_tcb_off_name         = 0U,    /**< name field offset.                      */
  k_tcb_len_name         = 100U,  /**< name field width.                       */
  k_tcb_off_mode         = 100U,  /**< mode field offset.                      */
  k_tcb_off_uid          = 108U,  /**< uid field offset.                       */
  k_tcb_off_gid          = 116U,  /**< gid field offset.                       */
  k_tcb_len_id           = 8U,    /**< mode/uid/gid field width.               */
  k_tcb_off_size         = 124U,  /**< size field offset.                      */
  k_tcb_len_size         = 12U,   /**< size field width.                       */
  k_tcb_off_mtime        = 136U,  /**< mtime field offset.                     */
  k_tcb_off_chksum       = 148U,  /**< checksum field offset.                  */
  k_tcb_len_chksum       = 8U,    /**< checksum field width.                   */
  k_tcb_chksum_digits    = 7U,    /**< Octal digits written into the checksum. */
  k_tcb_off_chksum_pad   = 155U,  /**< Trailing space of the checksum field.   */
  k_tcb_off_type         = 156U,  /**< typeflag field offset.                  */
  k_tcb_off_magic        = 257U,  /**< "ustar" magic offset.                   */
  k_tcb_off_version      = 263U,  /**< version field offset.                   */
} tcb_tar_layout_t;

/** @brief ustar magic field, 5 bytes at offset 257 (no string terminator). */
static const uint8_t s_tcb_ustar_magic[] = {'u', 's', 't', 'a', 'r'};
/** @brief ustar version field, the two ASCII digits "00" at offset 263. */
static const uint8_t s_tcb_ustar_version[] = {'0', '0'};

/** @brief The outer file under test (bare tar or wrapped member). */
static uint8_t s_arc[k_tcb_arc_cap];
/** @brief Length of the outer file. */
static size_t s_arc_len = 0U;
/** @brief Wrapped-open unwrap arena (8-aligned per the contract). */
alignas(8) static uint8_t s_arena[k_tcb_arena];
/** @brief XZ session scratch. */
alignas(8) static uint8_t s_scratch[k_tcb_scratch];
/** @brief Comic page index. */
static ra8_comic_page_t s_pages[k_tcb_page_cap];
/** @brief Comic name arena. */
static char s_names[k_tcb_name_cap];
/** @brief Page extraction buffer. */
static uint8_t s_out[k_tcb_out_cap];
/** @brief Scratch for the inner tar when building wrapped members. */
static uint8_t s_inner[k_tcb_arc_cap];

/**
 * @brief Seek-and-read callback over the current outer archive fixture.
 * @details Clips every request to ::s_arc_len and copies only the available
 * suffix, matching the short-read behavior expected by the comic facade.
 * @param[in] ctx Unused callback context retained for the reader ABI.
 * @param[in] off Zero-based byte offset into ::s_arc.
 * @param[out] buf Destination receiving the requested archive bytes.
 * @param[in] len Maximum number of bytes the caller permits.
 * @return Number of bytes copied into @p buf.
 * @retval 0 @p off is at or beyond the current fixture length.
 * @pre @p buf is writable for @p len bytes when @p len is nonzero.
 * @pre ::s_arc_len is no greater than the capacity of ::s_arc.
 * @post The return value does not exceed @p len or the available suffix.
 * @post The archive fixture and callback context remain unchanged.
 * @note Not thread-safe because the backing fixture is shared.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_tcb_read(void* ctx, uint64_t off, void* buf, size_t len)
{
  (void)ctx;
  if (off >= (uint64_t)s_arc_len) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s_arc_len - off;
  const size_t   n     = (len > (size_t)avail) ? (size_t)avail : len;
  memcpy(buf, &s_arc[off], n);
  return n;
}

/**
 * @brief Encode a value as a NUL-terminated tar octal field.
 * @details Clears the complete fixed-width field, then writes least-significant
 * octal digits right-to-left while preserving the final NUL terminator.
 * @param[out] f Tar header field receiving the encoded digits.
 * @param[in] len Width of @p f including the trailing terminator.
 * @param[in] v Unsigned value to encode in octal.
 * @pre @p f addresses at least @p len writable bytes.
 * @pre @p len is at least two bytes and can represent @p v.
 * @post @p f contains a right-aligned octal representation terminated by NUL.
 * @post Bytes before the first emitted digit are zero-filled.
 * @note Pure with respect to all state outside @p f.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_tcb_octal(uint8_t* f, size_t len, uint64_t v)
{
  memset(f, 0, len);
  size_t i = len - 2U;
  do {
    f[i] = (uint8_t)('0' + (v % 8U));
    v /= 8U;
    if (i == 0U) {
      break;
    }
    i -= 1U;
  } while (true);
}

/**
 * @brief Append one complete ustar member to an in-memory archive.
 * @details Builds the fixed header, computes its checksum with the checksum
 * field treated as spaces, appends the optional payload, and zero-pads to the
 * next 512-byte record.
 * @param[in,out] dst Archive buffer receiving the header and payload.
 * @param[in] off Current aligned end offset within @p dst.
 * @param[in] name Bounded member name copied into the ustar name field.
 * @param[in] type Ustar typeflag byte for this member.
 * @param[in] data Optional member payload; null only when @p n is zero.
 * @param[in] n Number of payload bytes to append.
 * @return Offset immediately after the padded member.
 * @retval k_tcb_tar_block A header-only member appended at offset zero.
 * @pre @p dst has room for one header plus @p n rounded to a tar block.
 * @pre @p off is block-aligned and @p name fits the fixed name field.
 * @post The emitted header checksum matches every byte in its header block.
 * @post Bytes between the payload end and returned offset are zero-filled.
 * @note Test fixture builder; it performs no bounds discovery itself.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_tcb_tar_add(uint8_t*    dst,
                                                size_t      off,
                                                const char* name,
                                                uint8_t     type,
                                                const void* data,
                                                size_t      n)
{
  uint8_t* b = &dst[off];
  memset(b, 0, (size_t)k_tcb_tar_block);
  strncpy((char*)&b[k_tcb_off_name], name, (size_t)k_tcb_len_name);
  internal_tcb_octal(&b[k_tcb_off_mode], k_tcb_len_id, k_comic_cbt_tcb_octal_0644);
  internal_tcb_octal(&b[k_tcb_off_uid], k_tcb_len_id, 0U);
  internal_tcb_octal(&b[k_tcb_off_gid], k_tcb_len_id, 0U);
  internal_tcb_octal(&b[k_tcb_off_size], k_tcb_len_size, (uint64_t)n);
  internal_tcb_octal(&b[k_tcb_off_mtime], k_tcb_len_size, 0U);
  b[k_tcb_off_type] = type;
  memcpy(&b[k_tcb_off_magic], s_tcb_ustar_magic, sizeof(s_tcb_ustar_magic));
  memcpy(&b[k_tcb_off_version], s_tcb_ustar_version, sizeof(s_tcb_ustar_version));
  memset(&b[k_tcb_off_chksum], (int)' ', k_tcb_len_chksum);
  uint32_t sum = 0U;
  for (uint32_t i = 0U; i < k_tcb_tar_block; ++i) {
    sum += b[i];
  }
  internal_tcb_octal(&b[k_tcb_off_chksum], k_tcb_chksum_digits, (uint64_t)sum);
  b[k_tcb_off_chksum_pad] = (uint8_t)' ';
  off += k_tcb_tar_block;
  if (n > 0U) {
    memcpy(&dst[off], data, n);
    const size_t padded = ((n + 511U) / 512U) * 512U;
    memset(&dst[off + n], 0, padded - n);
    off += padded;
  }
  return off;
}

/**
 * @brief Build the canonical two-page tar comic into caller storage.
 * @details Emits a directory, two deliberately unsorted PNG members, one
 * skipped text member, and the required two zero-block end marker.
 * @param[out] dst Buffer receiving the complete bare-tar comic.
 * @return Exact byte length of the generated archive.
 * @retval 0 Never returned for the canonical fixture.
 * @pre @p dst has at least ::k_tcb_arc_cap writable bytes.
 * @pre ::internal_tcb_tar_add accepts every canonical bounded member.
 * @post @p dst contains two page members named page1.png and page2.png.
 * @post The returned length includes both zero end-marker blocks.
 * @note Uses only deterministic literals and produces byte-stable output.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_tcb_build_tar(uint8_t* dst)
{
  const char p1[] = "PAGE-ONE";
  const char p2[] = "PAGE-TWO";
  size_t     off  = 0U;
  off             = internal_tcb_tar_add(dst, off, "dir/", (uint8_t)'5', nullptr, 0U);
  off             = internal_tcb_tar_add(dst, off, "page2.png", (uint8_t)'0', p2, sizeof(p2) - 1U);
  off = internal_tcb_tar_add(dst, off, "notes.txt", (uint8_t)'0', "skip me", k_tcb_payload_len);
  off = internal_tcb_tar_add(dst, off, "page1.png", (uint8_t)'0', p1, sizeof(p1) - 1U);
  memset(&dst[off], 0, (size_t)k_tcb_end_marker_bytes);
  return off + (size_t)k_tcb_end_marker_bytes;
}

/**
 * @brief Wrap a bounded payload as a gzip member in ::s_arc.
 * @details Emits the fixed gzip header, raw DEFLATE body, little-endian CRC32,
 * and original-size trailer used by the wrapped-open integration tests.
 * @param[in] payload Uncompressed bytes to place in the gzip member.
 * @param[in] n Number of readable bytes at @p payload.
 * @pre @p payload is non-null when @p n is nonzero.
 * @pre The compressed body and eight-byte trailer fit in ::s_arc.
 * @post ::s_arc contains one complete gzip member for the exact input bytes.
 * @post ::s_arc_len names the complete header, body, and trailer length.
 * @note Overwrites the shared outer fixture and is not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_tcb_gzip_wrap(const uint8_t* payload, size_t n)
{
  size_t off   = 0U;
  s_arc[off++] = k_gzip_magic_b0;
  s_arc[off++] = k_gzip_magic_b1;
  s_arc[off++] = 8U;
  s_arc[off++] = 0U;
  memset(&s_arc[off], 0, 6U);
  off += 6U;
  const size_t comp = tdefl_compress_mem_to_mem(&s_arc[off],
                                                sizeof(s_arc) - off - 8U,
                                                payload,
                                                n,
                                                (int)TDEFL_DEFAULT_MAX_PROBES);
  TEST_ASSERT(comp > 0U);
  off += comp;
  const uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, payload, n);
  for (uint32_t i = 0U; i < 4U; ++i) {
    s_arc[off + i]      = (uint8_t)((crc >> (8U * i)) & k_byte_mask);
    s_arc[off + 4U + i] = (uint8_t)(((uint32_t)n >> (8U * i)) & k_byte_mask);
  }
  s_arc_len = off + 8U;
}

/**
 * @brief Open ::s_arc through the wrapped facade with canonical workspaces.
 * @details Binds the shared page table, name arena, unwrap arena, and XZ
 * scratch so tests vary only the wrapper bytes and destination handle.
 * @param[out] c Comic handle populated on successful wrapped open.
 * @return Result from ::ra8_comic_open_wrapped.
 * @retval k_ra8_ok The wrapped or passthrough archive opened successfully.
 * @pre @p c is non-null, zeroed or closed, and writable.
 * @pre ::s_arc_len describes initialized bytes within ::s_arc.
 * @post Success binds @p c to the shared descriptor and name storage.
 * @post Failure leaves @p c without a live comic kind.
 * @note Shared workspaces require serialized test execution.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tcb_open_wrapped(ra8_comic_t* c)
{
  return ra8_comic_open_wrapped(c,
                                internal_tcb_read,
                                nullptr,
                                (uint64_t)s_arc_len,
                                s_pages,
                                (uint32_t)k_tcb_page_cap,
                                s_names,
                                (uint32_t)sizeof(s_names),
                                s_arena,
                                sizeof(s_arena),
                                s_scratch,
                                (uint32_t)sizeof(s_scratch));
}

/**
 * @brief Assert and close a successfully opened canonical two-page comic.
 * @details Verifies backend kind, sorted page name, both byte-exact payloads,
 * and successful close using the shared extraction buffer.
 * @param[in,out] c Open comic handle containing the canonical fixture.
 * @param[in] kind Backend kind expected for the tested wrapper path.
 * @pre @p c is open and exposes exactly the canonical two pages.
 * @pre ::s_out is large enough for either eight-byte page payload.
 * @post Both pages have been extracted and compared with their literals.
 * @post @p c is closed before the helper returns.
 * @note Unity assertions abort the current test on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_tcb_assert_two_pages(ra8_comic_t* c, ra8_comic_kind_t kind)
{
  TEST_ASSERT_EQ(kind, ra8_comic_kind(c));
  TEST_ASSERT_EQ(2U, ra8_comic_page_count(c));
  char     nb[k_tcb_name_cap] = {};
  uint16_t nl                 = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_info(c, 0U, nb, sizeof(nb), &nl, nullptr, nullptr));
  TEST_ASSERT_EQ(0, memcmp(nb, "page1.png", 9U)); /* sorted: page1 first */
  size_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(c, 0U, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(8U, got);
  TEST_ASSERT_EQ(0, memcmp(s_out, "PAGE-ONE", 8U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_page_read(c, 1U, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(8U, got);
  TEST_ASSERT_EQ(0, memcmp(s_out, "PAGE-TWO", 8U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(c));
}

/**
 * @test internal_test_cbt_bare_tar_open
 * @brief A bare `.cbt` opens via the tar probe: sorted pages, exact bytes.
 *
 * @par MC/DC:
 * (no compound decisions under test -- the facade detect and CBT backend
 * are single-condition dispatch chains.)
 * @details Builds a real tar archive in memory, verifies sorted extraction
 * through the public facade, then replaces it with non-archive bytes to prove
 * every backend rejects the fallback probe.
 * @pre The shared archive and page/name workspaces are available.
 * @pre The canonical tar builder fits within ::s_arc.
 * @post The valid tar's two payloads were extracted byte-exactly and closed.
 * @post The non-archive vector returns not-supported without a live handle.
 * @note Runs serially because all fixture storage is shared.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cbt_bare_tar_open(void)
{
  TEST_BEGIN("cbt: bare tar comic opens + extracts");
  s_arc_len     = internal_tcb_build_tar(s_arc);
  ra8_comic_t c = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_comic_open(&c,
                                internal_tcb_read,
                                nullptr,
                                (uint64_t)s_arc_len,
                                s_pages,
                                (uint32_t)k_tcb_page_cap,
                                s_names,
                                (uint32_t)sizeof(s_names)));
  internal_tcb_assert_two_pages(&c, k_ra8_comic_kind_cbt);

  /* Non-archive bytes are still cleanly refused by every backend. */
  memset(s_arc, k_tcb_fill_non_archive, (size_t)k_tcb_non_archive_bytes);
  s_arc_len = k_tcb_non_archive_bytes;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 ra8_comic_open(&c,
                                internal_tcb_read,
                                nullptr,
                                (uint64_t)s_arc_len,
                                s_pages,
                                (uint32_t)k_tcb_page_cap,
                                s_names,
                                (uint32_t)sizeof(s_names)));
  TEST_END("cbt: bare tar comic opens + extracts");
}

/**
 * @test internal_test_cbt_wrapped_gzip_and_xz
 * @brief `.tar.gz` and `.tar.xz` open through the wrapped facade.
 *
 * @par MC/DC:
 * Decision: `!is_gzip && !is_xz` in `ra8_comic_open_wrapped` (2 conditions)
 * - Vector 1: plain ZIP  -> true  (T,T: passthrough)
 * - Vector 2: gzip magic -> false (F,-)
 * - Vector 3: xz magic   -> false (T,F)
 * Vectors 1+2 prove the gzip leg, 1+3 the xz leg: minimal MC/DC.
 * @details Exercises gzip unwrap, committed XZ unwrap, missing-XZ-scratch
 * rejection, and the no-wrapper ZIP passthrough leg with real containers.
 * @pre The unwrap arena and XZ scratch satisfy their documented alignment.
 * @pre The committed XZ fixture and generated tar fit the shared buffers.
 * @post Gzip and XZ inputs both yield the canonical two-page CBT and close.
 * @post A bare ZIP passes through as CBZ, while absent XZ scratch fails closed.
 * @note Miniz writer allocations are confined to the test fixture setup.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cbt_wrapped_gzip_and_xz(void)
{
  TEST_BEGIN("cbt: gzip- and xz-wrapped comics open");
  ra8_comic_t c = {};

  /* .tar.gz: gzip member around the canonical tar. */
  const size_t tar_len = internal_tcb_build_tar(s_inner);
  internal_tcb_gzip_wrap(s_inner, tar_len);
  TEST_ASSERT_EQ(k_ra8_ok, internal_tcb_open_wrapped(&c));
  internal_tcb_assert_two_pages(&c, k_ra8_comic_kind_cbt);

  /* .tar.xz: the committed fixture (same two pages). */
  memcpy(s_arc, k_fx_xz_cbt, sizeof(k_fx_xz_cbt));
  s_arc_len = sizeof(k_fx_xz_cbt);
  TEST_ASSERT_EQ(k_ra8_ok, internal_tcb_open_wrapped(&c));
  internal_tcb_assert_two_pages(&c, k_ra8_comic_kind_cbt);

  /* An XZ file with no XZ scratch provided: rejected, not crashed. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_comic_open_wrapped(&c,
                                        internal_tcb_read,
                                        nullptr,
                                        (uint64_t)s_arc_len,
                                        s_pages,
                                        (uint32_t)k_tcb_page_cap,
                                        s_names,
                                        (uint32_t)sizeof(s_names),
                                        s_arena,
                                        sizeof(s_arena),
                                        nullptr,
                                        0U));

  /* Passthrough: a bare ZIP through the wrapped open lands in CBZ. */
  mz_zip_archive zip = {};
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, 0U) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip, "page1.png", "PAGE-ONE", 8U, MZ_NO_COMPRESSION) ==
              MZ_TRUE);
  void*  zbuf = nullptr;
  size_t zlen = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &zbuf, &zlen) == MZ_TRUE);
  memcpy(s_arc, zbuf, zlen);
  s_arc_len = zlen;
  mz_free(zbuf);
  (void)mz_zip_writer_end(&zip);
  TEST_ASSERT_EQ(k_ra8_ok, internal_tcb_open_wrapped(&c));
  TEST_ASSERT_EQ(k_ra8_comic_kind_cbz, ra8_comic_kind(&c));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_comic_close(&c));
  TEST_END("cbt: gzip- and xz-wrapped comics open");
}

/**
 * @brief Open the wrapped fixture archive with the shared page / name / scratch
 *        buffers, varying only the handle, reader, size and arena.
 *
 * @param[out] c         Comic handle (nullptr exercises the c guard).
 * @param[in]  read      Archive reader (nullptr exercises the read guard).
 * @param[in]  file_len  Wrapped-archive size in bytes.
 * @param[in]  arena     Working arena (nullptr / misaligned exercise guards).
 * @param[in]  arena_len Arena size in bytes.
 *
 * @return The `ra8_comic_open_wrapped` result code.
 * @retval k_ra8_ok The presented archive opened successfully.
 * @details Centralizes every invariant shared by the negative wrapped-open
 * vectors while exposing only the argument under test at each call site.
 * @pre The shared fixture buffers are populated.
 * @pre @p arena is readable and writable for @p arena_len unless testing a
 * guard.
 * @post Success binds only caller-owned shared workspaces to @p c.
 * @post Failure does not create an externally usable comic handle.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tcb_open_full(ra8_comic_t*      c,
                                                     ra8_comic_read_fn read,
                                                     uint64_t          file_len,
                                                     uint8_t*          arena,
                                                     size_t            arena_len)
{
  return ra8_comic_open_wrapped(c,
                                read,
                                nullptr,
                                file_len,
                                s_pages,
                                (uint32_t)k_tcb_page_cap,
                                s_names,
                                (uint32_t)sizeof(s_names),
                                arena,
                                arena_len,
                                s_scratch,
                                (uint32_t)sizeof(s_scratch));
}

/**
 * @brief Exercise the nesting-bomb rejections of the wrapped-open path.
 * @details Constructs gzip-of-gzip, gzip-of-XZ, and gzip-of-junk vectors and
 * verifies wrapper depth is rejected before recursive decompression proceeds.
 * @param[out] c Comic handle under test.
 * @pre The shared fixture buffers are available.
 * @pre @p c is non-null and writable across repeated failed opens.
 * @post Every nesting bomb returns its documented rejection code.
 * @post @p c remains kind-none after each rejected nested wrapper.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_tcb_bombs(ra8_comic_t* c)
{
  /* gzip-of-gzip: the classic nesting bomb shape. */
  internal_tcb_gzip_wrap((const uint8_t*)"payload", k_tcb_payload_len);
  memcpy(s_inner, s_arc, s_arc_len);
  internal_tcb_gzip_wrap(s_inner, s_arc_len);
  TEST_ASSERT_EQ(k_ra8_err_decomp_depth, internal_tcb_open_wrapped(c));
  TEST_ASSERT_EQ(k_ra8_comic_kind_none, ra8_comic_kind(c));

  /* gzip-of-xz: the other wrapper inside a wrapper. */
  internal_tcb_gzip_wrap(k_fx_xz_crc32, sizeof(k_fx_xz_crc32));
  TEST_ASSERT_EQ(k_ra8_err_decomp_depth, internal_tcb_open_wrapped(c));

  /* gzip of non-container garbage: the inner open rejects it. */
  internal_tcb_gzip_wrap((const uint8_t*)"not an archive at all, promise!!", 32U);
  TEST_ASSERT_EQ(k_ra8_err_not_supported, internal_tcb_open_wrapped(c));
}

/**
 * @brief Exercise the null / size / alignment argument guards of wrapped-open.
 * @details Holds a valid gzip-wrapped tar constant while varying one handle,
 * reader, size, arena pointer, alignment, or capacity input at a time.
 * @param[out] c Comic handle under test.
 * @pre The shared fixture buffers are available.
 * @pre @p c is non-null and writable for vectors not testing the handle guard.
 * @post Every malformed argument returns its documented rejection code.
 * @post No invalid vector publishes a live comic handle or arena binding.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_tcb_arg_guards(ra8_comic_t* c)
{
  const size_t tar_len = internal_tcb_build_tar(s_inner);
  internal_tcb_gzip_wrap(s_inner, tar_len);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_tcb_open_full(nullptr,
                                        internal_tcb_read,
                                        (uint64_t)s_arc_len,
                                        s_arena,
                                        sizeof(s_arena)));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 internal_tcb_open_full(c, nullptr, (uint64_t)s_arc_len, s_arena, sizeof(s_arena)));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    internal_tcb_open_full(c, internal_tcb_read, (uint64_t)s_arc_len, nullptr, sizeof(s_arena)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_tcb_open_full(c, internal_tcb_read, 0U, s_arena, sizeof(s_arena)));
  /* Misaligned arena (the descriptor slot needs 8-byte alignment). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_tcb_open_full(c,
                                        internal_tcb_read,
                                        (uint64_t)s_arc_len,
                                        &s_arena[1],
                                        sizeof(s_arena) - 1U));
  /* Arena with no room past the descriptor slot. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 internal_tcb_open_full(c, internal_tcb_read, (uint64_t)s_arc_len, s_arena, 8U));
}

/**
 * @test internal_test_cbt_wrapped_bombs_and_guards
 * @brief Nesting bombs and bad wrapped-open arguments fail closed.
 *
 * @par MC/DC:
 * Decision: `inner_gzip || inner_xz` in `ra8_comic_open_wrapped`
 * (2 conditions)
 * - Vector 1: gzip-of-gzip -> true  (T,-: depth bomb)
 * - Vector 2: gzip-of-xz   -> true  (F,T: depth bomb)
 * - Vector 3: gzip-of-tar  -> false (F,F: honest, opens)
 * Vectors 1+3 prove the gzip leg, 2+3 the xz leg: minimal MC/DC (vector 3
 * is the honest `.tar.gz` in internal_test_cbt_wrapped_gzip_and_xz).
 * @details Sequences the independent nesting and argument guard helpers under
 * one unity case so all failures are observed against a freshly zeroed handle.
 * @pre Shared wrapper fixture buffers are writable and correctly aligned.
 * @pre The private wrapped-open seam is linked into this direct test.
 * @post All nested-wrapper shapes fail at the documented depth boundary.
 * @post All null, zero, misaligned, and undersized arguments fail closed.
 * @note The helpers deliberately reuse one handle only across failed opens.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cbt_wrapped_bombs_and_guards(void)
{
  TEST_BEGIN("cbt: nesting bombs + wrapped-open guards");
  ra8_comic_t c = {};
  internal_tcb_bombs(&c);
  internal_tcb_arg_guards(&c);
  TEST_END("cbt: nesting bombs + wrapped-open guards");
}

/**
 * @brief Both CBT entry points fail closed on a tar walker that never went
 * live.
 *
 * @details
 * `priv_comic_cbt_open` and `priv_comic_cbt_extract` must reject a comic whose
 * embedded tar walker is not live (`tar.live == 0`) rather than read
 * uninitialised walker state -- the guard that fires when a probe misclassifies
 * a container or a caller reuses a comic across a failed open.
 *
 * @par MC/DC:
 * (no compound decisions in this test -- it drives the single-condition
 * `!c->tar.live` dead-walker guard in both entry points: priv_comic_cbt_open
 * and priv_comic_cbt_extract each return k_ra8_err_invalid_state with tar.live
 * == 0 (extract also zeroing `*got` before the guard). The RA8_CHECK_NULL_PTR
 * argument checks are single-condition; no `&&` or `||` decision is reached)
 * @pre The zero-initialized comic leaves its embedded tar walker non-live.
 * @pre ::s_out is writable so extraction reaches the walker-state guard.
 * @post Both private CBT entry points return invalid-state.
 * @post The extract path resets the caller's byte count to zero before failing.
 * @note Directly targets private guards without constructing an archive.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_cbt_dead_walker_guards(void)
{
  TEST_BEGIN("cbt: open/extract reject a dead tar walker");
  ra8_comic_t c = {}; /* zero-init leaves tar.live == 0 */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, priv_comic_cbt_open(&c));
  const ra8_comic_page_t p   = {};
  size_t                 got = 1U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 priv_comic_cbt_extract(&c, &p, s_out, sizeof(s_out), &got));
  TEST_ASSERT_EQ(0U, got); /* extract zeroes the out-count before the guard */
  TEST_END("cbt: open/extract reject a dead tar walker");
}

/**
 * @brief Test entry point -- runs the CBT / wrapped-open suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int main(void)
{
  internal_test_cbt_bare_tar_open();
  internal_test_cbt_wrapped_gzip_and_xz();
  internal_test_cbt_wrapped_bombs_and_guards();
  internal_test_cbt_dead_walker_guards();
  return 0;
}
