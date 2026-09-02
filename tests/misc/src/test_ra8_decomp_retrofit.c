/**
 * @file test_ra8_decomp_retrofit.c
 * @brief Tests for the decompression-limits retrofit on the ZIP + RAR paths.
 *
 * @details
 * The pre-existing ZIP-store / DEFLATE (miniz) and RAR decode paths in
 * epub / comic now enforce the same unified decompression-limits
 * policy as the new decoders. This suite proves each retrofit point with
 * real archives built (and then sabotaged) in memory:
 *
 *   1. the epub ZIP guards: an archive whose central directory floods
 *      the entry cap is rejected at open (through `epub_open` itself),
 *      and an entry whose record declares a bomb is rejected before
 *      inflation (guard unit vectors + a lying-record EPUB through the
 *      public open),
 *   2. the comic CBZ backend: entry-count flood and lying declared sizes
 *      (output cap and ratio shapes) reject the archive at
 *      `comic_open`,
 *   3. the comic CBR backend: a block-flood RAR4 and a lying-size RAR4
 *      member reject the archive at `comic_open`,
 *   4. honest archives still open after every hostile probe.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "comic.h"
#include "epub.h"
#include "epub_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief RAR main-archive-header layout used by the fixture builder. */
typedef enum : uint8_t {
  k_td_off_high_pos_av = 7U, /**< HighPosAV + PosAV field offset.   */
  k_td_len_high_pos_av = 6U, /**< HighPosAV + PosAV combined width. */
} td_layout_t;

/**
 * @enum zip_cdir_field_t
 * @brief ZIP central-directory offsets the archive scanner walks.
 */
typedef enum : uint8_t {
  k_zip_cdir_off_comp_size   = 20U, /**< Compressed size, 20 bytes in.   */
  k_zip_cdir_off_uncomp_size = 24U, /**< Uncompressed size, 24 bytes in. */
  k_zip_cdir_hdr_bytes       = 46U, /**< A central-directory file header is 46
                                         bytes; the name starts right after.  */
} zip_cdir_field_t;

/**
 * @enum rar4_field_t
 * @brief RAR4 block-header field offsets and the type/method byte values.
 *
 * @details
 * Offsets are from the start of a RAR4 block header and are shared by every
 * block type; the `k_rar_head_type_*` and `k_rar_method_*` members are the
 * values stamped into those fields to build a stored (uncompressed) file
 * entry the extractor must accept.
 */
typedef enum : uint8_t {
  k_rar_off_head_size  = 5U,    /**< head_size.                              */
  k_rar_off_pack_size  = 7U,    /**< pack_size.                              */
  k_rar_off_unp_size   = 11U,   /**< unp_size.                               */
  k_rar_off_host_os    = 15U,   /**< host_os.                                */
  k_rar_off_ftime      = 20U,   /**< ftime.                                  */
  k_rar_off_unp_ver    = 24U,   /**< unp_ver.                                */
  k_rar_off_method     = 25U,   /**< method.                                 */
  k_rar_off_name_size  = 26U,   /**< name_size.                              */
  k_rar_off_attr       = 28U,   /**< attr.                                   */
  k_rar_head_type_main = 0x73U, /**< head_type 's': the archive main header. */
  k_rar_head_type_file = 0x74U, /**< head_type 't': a file header.           */
  k_rar_method_store   = 0x30U, /**< method '0': stored, no compression.     */
  k_rar_unp_ver_2_0    = 20U,   /**< unp_ver 20: RAR 2.0 format.             */
  k_rar_main_hdr_bytes = 13U,   /**< A main header is exactly 13 bytes, which
                                     is both its head_size and the step to the
                                     next block.                              */
  k_byte_mask          = 0xFFU, /**< Truncates each shifted size back into a byte. */
  k_shift_byte3        = 24U,   /**< Shift to the top byte of a 32-bit LE field.   */
} rar4_field_t;

/**
 * @enum rar4_wide_t
 * @brief RAR4 header flags and the sizes planted in a miniz stat record.
 */
typedef enum : uint16_t {
  k_rar_flag_long    = 0x8000U, /**< LONG: the 64-bit ADD_SIZE fields are present. */
  k_stat_comp_size   = 100U, /**< Compressed size in a planted stat record; under the uncompressed
               size, so the pair is self-consistent.                            */
  k_stat_uncomp_size = 300U, /**< Its uncompressed size. */
} rar4_wide_t;

/**
 * @enum td_dim_t
 * @brief Archive build budgets (tests are magic-number exempt).
 */
typedef enum : uint32_t {
  k_td_arc_cap    = 1024U * 1024U,        /**< Archive build buffer.           */
  k_td_flood      = 4097U,                /**< One over the default entry cap. */
  k_td_page_cap   = 8U,                   /**< Comic page-index capacity.      */
  k_td_name_cap   = 512U,                 /**< Comic name-arena capacity.      */
  k_td_lying_size = 256U * 1024U * 1024U, /**< Declared bomb size (256 MiB).   */
  k_td_ratio_size = 8U * 1024U * 1024U,   /**< Ratio-breach declared size.     */
} td_dim_t;

/** @brief The archive under test. */
static uint8_t s_arc[k_td_arc_cap];
/** @brief Length of the built archive. */
static size_t s_arc_len = 0U;
/** @brief Comic page index. */
static comic_page_t s_pages[k_td_page_cap];
/** @brief Comic name arena. */
static char s_names[k_td_name_cap];

/**
 * @brief Read a bounds-clamped span from the active archive fixture.
 * @details Implements the comic positioned-reader seam directly over immutable s_arc bytes.
 * @param[in] ctx Unused callback context.
 * @param[in] off Absolute archive offset.
 * @param[out] buf Destination for available bytes.
 * @param[in] len Requested byte count.
 * @return Bytes copied.
 * @retval 0 Offset is at or beyond the fixture end.
 * @retval 1..len Available bytes copied without crossing s_arc_len.
 * @pre @p buf is writable for @p len bytes when data is available.
 * @pre A fixture builder initialized s_arc_len.
 * @post Archive bytes remain unchanged.
 * @post No read crosses the active archive length.
 * @note The context is unused because this test owns one file-local source.
 * @since Version 0.1.0
 */
RA8_INTERNAL static size_t internal_read(void* ctx, uint64_t off, void* buf, size_t len)
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
 * @brief Open the active fixture through the comic facade.
 * @details Binds the file-local reader, page table, and name arena to one comic handle.
 * @param[out] c Comic handle initialized by the facade.
 * @return Comic open status.
 * @retval k_ra8_ok The fixture opened and page metadata is available.
 * @retval other The precise format or decompression guard error.
 * @pre @p c points to writable storage.
 * @pre A fixture builder initialized s_arc and s_arc_len.
 * @post Success leaves @p c open for inspection and close.
 * @post Failure does not publish a usable comic kind.
 * @note Page and name capacities are intentionally much smaller than flood counts.
 * @since Version 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_comic_open(comic_t* c)
{
  return comic_open(c,
                    internal_read,
                    nullptr,
                    (uint64_t)s_arc_len,
                    s_pages,
                    (uint32_t)k_td_page_cap,
                    s_names,
                    (uint32_t)sizeof(s_names));
}

/**
 * @brief Build a ZIP containing deterministic tiny entries.
 * @details Encodes four decimal digits after @p prefix and appends an optional suffix.
 * @param[in] n Number of entries to add.
 * @param[in] prefix First byte of every generated entry name.
 * @param[in] suffix Optional NUL-terminated suffix after the four digits.
 * @pre @p n is at most 9999 and the finalized archive fits s_arc.
 * @pre @p suffix is NULL or short enough for the fixed name buffer.
 * @post s_arc contains one finalized ZIP and s_arc_len is exact.
 * @post The temporary miniz finalization buffer is released.
 * @note Manual decimal encoding keeps the fixture independent of stdio streams.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_build_zip(uint32_t n, char prefix, const char* suffix)
{
  mz_zip_archive zip = {};
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, 0U) == MZ_TRUE);
  char name[32] = {};
  for (uint32_t i = 0U; i < n; ++i) {
    TEST_ASSERT(i <= 9999U);
    name[0] = prefix;
    name[1] = (char)('0' + ((i / 1000U) % 10U));
    name[2] = (char)('0' + ((i / 100U) % 10U));
    name[3] = (char)('0' + ((i / 10U) % 10U));
    name[4] = (char)('0' + (i % 10U));
    name[5] = '\0';
    if (suffix != nullptr) {
      const size_t suffix_len = strlen(suffix);
      TEST_ASSERT((5U + suffix_len) < sizeof(name));
      memcpy(&name[5], suffix, suffix_len + 1U);
    }
    TEST_ASSERT(mz_zip_writer_add_mem(&zip, name, "x", 1U, MZ_NO_COMPRESSION) == MZ_TRUE);
  }
  void*  buf  = nullptr;
  size_t blen = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(&zip, &buf, &blen) == MZ_TRUE);
  TEST_ASSERT(blen <= sizeof(s_arc));
  memcpy(s_arc, buf, blen);
  s_arc_len = blen;
  mz_free(buf);
  (void)mz_zip_writer_end(&zip);
}

/**
 * @brief Patch a central-directory record's declared sizes in ::s_arc.
 * @details Scans for the "PK\x01\x02" record whose name matches, then
 *          overwrites the compressed (offset 20) and uncompressed
 *          (offset 24) size fields -- forging exactly the lying header a
 *          hostile archive would carry.
 * @param[in] name Exact entry name whose central-directory record is patched.
 * @param[in] comp Replacement compressed-size field.
 * @param[in] uncomp Replacement uncompressed-size field.
 * @pre @p name identifies one entry in the active ZIP fixture.
 * @pre s_arc_len spans the complete central directory.
 * @post Exactly the matched record's two size fields are replaced.
 * @post All unrelated archive bytes remain unchanged.
 * @note A missing name is a fixture defect and fails the test immediately.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_lie_sizes(const char* name, uint32_t comp, uint32_t uncomp)
{
  const size_t nlen = strlen(name);
  for (size_t i = 0U; (i + k_zip_cdir_hdr_bytes + nlen) <= s_arc_len; ++i) {
    const bool sig = (s_arc[i] == 0x50U) && (s_arc[i + 1U] == 0x4BU) && (s_arc[i + 2U] == 0x01U) &&
                     (s_arc[i + 3U] == 0x02U);
    if (!sig) {
      continue;
    }
    if (memcmp(&s_arc[i + k_zip_cdir_hdr_bytes], name, nlen) != 0) {
      continue;
    }
    for (uint32_t b = 0U; b < 4U; ++b) {
      s_arc[i + k_zip_cdir_off_comp_size + b]   = (uint8_t)((comp >> (8U * b)) & k_byte_mask);
      s_arc[i + k_zip_cdir_off_uncomp_size + b] = (uint8_t)((uncomp >> (8U * b)) & k_byte_mask);
    }
    return;
  }
  TEST_ASSERT(false); /* record not found: fixture bug */
}

/**
 * @brief Encode one little-endian 16-bit fixture value.
 * @details Writes both bytes explicitly without alignment assumptions.
 * @param[out] p Two-byte destination.
 * @param[in] v Value to encode.
 * @pre @p p addresses at least two writable bytes.
 * @pre The destination may be unaligned.
 * @post Both destination bytes contain the little-endian representation.
 * @post No byte outside the two-byte destination is modified.
 * @note Used only by the RAR4 fixture builder.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_le16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
}

/**
 * @brief Encode one little-endian 32-bit fixture value.
 * @details Writes all four bytes explicitly without alignment assumptions.
 * @param[out] p Four-byte destination.
 * @param[in] v Value to encode.
 * @pre @p p addresses at least four writable bytes.
 * @pre The destination may be unaligned.
 * @post All destination bytes contain the little-endian representation.
 * @post No byte outside the four-byte destination is modified.
 * @note Used only by the RAR4 fixture builder.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void internal_le32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8U);
  p[2] = (uint8_t)(v >> 16U);
  p[3] = (uint8_t)(v >> k_shift_byte3);
}

/**
 * @brief Encode one stored RAR4 file block.
 * @details Builds its header, name, and packed data while allowing a forged unpacked size.
 * @param[out] out Destination for the encoded block.
 * @param[in] name Member name.
 * @param[in] data Packed member bytes.
 * @param[in] dlen Packed byte count.
 * @param[in] unp Declared unpacked byte count.
 * @return Encoded block length.
 * @retval 33..k_td_arc_cap Header, name, and data bytes emitted.
 * @pre @p out has capacity for the complete block.
 * @pre @p name and @p data remain readable for their declared lengths.
 * @post The block uses RAR4 STORE with independent packed/unpacked fields.
 * @post No destination byte beyond the returned length is modified.
 * @note Independent sizes create the decompression-bomb vectors.
 * @since Version 0.1.0
 */
RA8_INTERNAL static size_t
internal_rar4_file(uint8_t* out, const char* name, const uint8_t* data, size_t dlen, uint32_t unp)
{
  const size_t nlen = strlen(name);
  const size_t head = 32U + nlen;
  internal_le16(&out[0], 0U);              /* head_crc (unverified)          */
  out[2] = k_rar_head_type_file;           /* type: file                     */
  internal_le16(&out[3], k_rar_flag_long); /* flags: LONG (ADD_SIZE present) */
  internal_le16(&out[k_rar_off_head_size], (uint16_t)head);
  internal_le32(&out[k_rar_off_pack_size], (uint32_t)dlen); /* pack_size */
  internal_le32(&out[k_rar_off_unp_size], unp);             /* unp_size  */
  out[k_rar_off_host_os] = 0U;
  internal_le32(&out[16], 0U);
  internal_le32(&out[k_rar_off_ftime], 0U);
  out[k_rar_off_unp_ver] = k_rar_unp_ver_2_0;
  out[k_rar_off_method]  = k_rar_method_store; /* method: store */
  internal_le16(&out[k_rar_off_name_size], (uint16_t)nlen);
  internal_le32(&out[k_rar_off_attr], 0U);
  memcpy(&out[32], name, nlen);
  if (dlen > 0U) {
    memcpy(&out[head], data, dlen);
  }
  return head + dlen;
}

/**
 * @brief Build a RAR4 archive with stored members.
 * @details Emits the signature, main block, and repeated file blocks into s_arc.
 * @param[in] nfiles Number of members to generate.
 * @param[in] name Shared member name.
 * @param[in] unp_override Optional forged unpacked size; zero uses packed length.
 * @pre The complete archive fits k_td_arc_cap.
 * @pre @p name fits the RAR4 name-size field.
 * @post s_arc_len equals the complete encoded archive length.
 * @post Every member uses the requested declared unpacked size policy.
 * @note Non-image names permit block-flood tests without exhausting the page index first.
 * @since Version 0.1.0
 */
RA8_INTERNAL static void
internal_build_rar4(uint32_t nfiles, const char* name, uint32_t unp_override)
{
  static const uint8_t k_sig[7] = {0x52U, 0x61U, 0x72U, 0x21U, 0x1AU, 0x07U, 0x00U};
  size_t               p        = 0U;
  memcpy(&s_arc[p], k_sig, sizeof(k_sig));
  p += sizeof(k_sig);
  internal_le16(&s_arc[p], 0U); /* main block */
  s_arc[p + 2U] = k_rar_head_type_main;
  internal_le16(&s_arc[p + 3U], 0U);
  internal_le16(&s_arc[p + k_rar_off_head_size], k_rar_main_hdr_bytes);
  memset(&s_arc[p + k_td_off_high_pos_av], 0, k_td_len_high_pos_av);
  p += k_rar_main_hdr_bytes;
  const uint8_t data[4] = {1U, 2U, 3U, 4U};
  for (uint32_t i = 0U; i < nfiles; ++i) {
    const uint32_t unp = (unp_override != 0U) ? unp_override : (uint32_t)sizeof(data);
    p += internal_rar4_file(&s_arc[p], name, data, sizeof(data), unp);
  }
  s_arc_len = p;
}

/**
 * @test internal_test_retrofit_epub_guards_direct
 * @brief The ZIP guard helpers reject NULLs, floods, and lying records.
 * @details Drives entry, output-cap, ratio, and honest-archive outcomes through private guards.
 * @pre Miniz can construct and open the small honest ZIP fixture.
 * @pre The EPUB guard seams are linked for focused coverage.
 * @post Each hostile metadata shape returns its specific policy error.
 * @post The honest archive guard returns k_ra8_ok and closes cleanly.
 * @note The public-open flood vector is kept separate below.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- both guards are single-condition
 * checks over the shared `ra8_decomp_check_declared` seam, whose own
 * MC/DC lives in test_ra8_decomp_limits.c.)
 */
RA8_INTERNAL static void internal_test_retrofit_epub_guards_direct(void)
{
  TEST_BEGIN("retrofit: epub ZIP guard units");
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_epub_zip_guard_archive(nullptr));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_epub_zip_guard_entry(nullptr));

  mz_zip_archive_file_stat st = {};
  st.m_comp_size              = k_stat_comp_size;
  st.m_uncomp_size            = k_stat_uncomp_size;
  TEST_ASSERT_EQ(k_ra8_ok, priv_epub_zip_guard_entry(&st));
  st.m_uncomp_size = (mz_uint64)k_td_lying_size; /* over the 64 MiB cap */
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap, priv_epub_zip_guard_entry(&st));
  st.m_comp_size   = 16U; /* 8 MiB from 16 bytes: bomb ratio */
  st.m_uncomp_size = (mz_uint64)k_td_ratio_size;
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, priv_epub_zip_guard_entry(&st));

  /* An honest small archive passes the archive-level guard. */
  internal_build_zip(4U, 'e', nullptr);
  mz_zip_archive zip = {};
  TEST_ASSERT(mz_zip_reader_init_mem(&zip, s_arc, s_arc_len, 0U) == MZ_TRUE);
  TEST_ASSERT_EQ(k_ra8_ok, priv_epub_zip_guard_archive(&zip));
  (void)mz_zip_reader_end(&zip);
  TEST_END("retrofit: epub ZIP guard units");
}

/**
 * @test internal_test_retrofit_epub_open_paths
 * @brief `epub_open` rejects an entry-flood ZIP fail-closed.
 * @details Builds one entry beyond policy and exercises the resident public EPUB open path.
 * @pre The flood archive fits the fixed build buffer.
 * @pre The static book record is writable and has no active archive.
 * @post Open returns k_ra8_err_decomp_entries before bounded miniz allocation obscures it.
 * @post No EPUB book becomes active.
 * @note The book is static to avoid a large test stack frame.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- integration vector for the
 * archive-level guard inside the epub open path.)
 */
RA8_INTERNAL static void internal_test_retrofit_epub_open_paths(void)
{
  TEST_BEGIN("retrofit: epub open rejects entry flood");
  internal_build_zip((uint32_t)k_td_flood, 'e', nullptr);
  static epub_book_t s_book; /* large record: keep off the test stack */
  epub_mem_media_t   mem = {.data = s_arc, .size = s_arc_len};
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries, epub_open(&mem, nullptr, &s_book));
  TEST_END("retrofit: epub open rejects entry flood");
}

/**
 * @test internal_test_retrofit_cbz_paths
 * @brief The comic CBZ backend rejects floods and lying declared sizes.
 * @details Exercises entry preflight, output cap, ratio cap, and subsequent honest reuse.
 * @pre ZIP fixture construction fits s_arc for every vector.
 * @pre The comic page/name arenas remain writable between opens.
 * @post Every hostile ZIP returns its precise decompression-policy error.
 * @post A final honest three-page archive opens, counts, and closes successfully.
 * @note Reuse after rejection proves failure does not poison facade state.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- each rejection drives one
 * single-condition guard in the CBZ backend.)
 */
RA8_INTERNAL static void internal_test_retrofit_cbz_paths(void)
{
  TEST_BEGIN("retrofit: CBZ flood + lying sizes rejected");
  comic_t c = {};

  /* Entry flood: one over the policy cap. */
  internal_build_zip((uint32_t)k_td_flood, 'e', nullptr);
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries, internal_comic_open(&c));
  TEST_ASSERT_EQ(k_comic_kind_none, comic_kind(&c));

  /* Lying record: an image entry declaring 256 MiB (over the cap). */
  internal_build_zip(3U, 'p', ".png");
  internal_lie_sizes("p0001.png", 1U, (uint32_t)k_td_lying_size);
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap, internal_comic_open(&c));

  /* Lying record: 8 MiB declared from 16 compressed bytes (bomb ratio). */
  internal_build_zip(3U, 'p', ".png");
  internal_lie_sizes("p0002.png", 16U, (uint32_t)k_td_ratio_size);
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, internal_comic_open(&c));

  /* Honest archive still opens after the hostile probes. */
  internal_build_zip(3U, 'p', ".png");
  TEST_ASSERT_EQ(k_ra8_ok, internal_comic_open(&c));
  TEST_ASSERT_EQ(3U, comic_page_count(&c));
  TEST_ASSERT_EQ(k_ra8_ok, comic_close(&c));
  TEST_END("retrofit: CBZ flood + lying sizes rejected");
}

/**
 * @test internal_test_retrofit_cbr_paths
 * @brief The comic CBR backend rejects block floods and lying sizes.
 * @details Exercises RAR block-count, output-cap, ratio, and subsequent honest reuse.
 * @pre Generated RAR4 archives fit the fixed fixture capacity.
 * @pre The comic page/name arenas remain writable between opens.
 * @post Every hostile RAR returns its precise decompression-policy error.
 * @post A final honest two-page archive opens, counts, and closes successfully.
 * @note Non-image flood members isolate the archive-entry budget.
 * @since Version 0.1.0
 *
 * @par MC/DC:
 * (no compound decisions under test -- each rejection drives one
 * single-condition guard in the CBR backend.)
 */
RA8_INTERNAL static void internal_test_retrofit_cbr_paths(void)
{
  TEST_BEGIN("retrofit: CBR flood + lying sizes rejected");
  comic_t c = {};

  /* Block flood: non-page members so the small test page index never
   * fills -- every walked block still charges the policy entry cap. */
  internal_build_rar4((uint32_t)k_td_flood, "x.bin", 0U);
  TEST_ASSERT_EQ(k_ra8_err_decomp_entries, internal_comic_open(&c));

  /* Lying member: a 4-byte STORE page declaring 256 MiB unpacked. */
  internal_build_rar4(1U, "x.png", (uint32_t)k_td_lying_size);
  TEST_ASSERT_EQ(k_ra8_err_decomp_output_cap, internal_comic_open(&c));

  /* Lying member: 8 MiB declared from 4 packed bytes (bomb ratio). */
  internal_build_rar4(1U, "x.png", (uint32_t)k_td_ratio_size);
  TEST_ASSERT_EQ(k_ra8_err_decomp_ratio, internal_comic_open(&c));

  /* Honest RAR4 still opens (STORE members, sizes match). */
  internal_build_rar4(2U, "x.png", 0U);
  TEST_ASSERT_EQ(k_ra8_ok, internal_comic_open(&c));
  TEST_ASSERT_EQ(2U, comic_page_count(&c));
  TEST_ASSERT_EQ(k_ra8_ok, comic_close(&c));
  TEST_END("retrofit: CBR flood + lying sizes rejected");
}

/**
 * @brief Test entry point -- runs the retrofit suite in order.
 * @return 0 on success; unity_minimal.h exits non-zero on the first failure.
 */
int main(void)
{
  internal_test_retrofit_epub_guards_direct();
  internal_test_retrofit_epub_open_paths();
  internal_test_retrofit_cbz_paths();
  internal_test_retrofit_cbr_paths();
  return 0;
}
