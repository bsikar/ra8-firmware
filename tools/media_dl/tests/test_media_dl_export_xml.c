/**
 * @file test_media_dl_export_xml.c
 * @brief Export XML escaping, media typing, and package-document tests.
 * @details Isolates the pure XML utility contract, the EPUB manifest media-type
 *          decisions, and the bounded package and ComicInfo document composition
 *          from archive round-trip and storage-fault cases.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mdl_export.h"
#include "mdl_export_epub_internal.h"
#include "mdl_export_internal.h"
#include "mdl_export_io_internal.h"
#include "mdl_sanitize.h"
#include "mdl_test_storage.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "test_media_dl_export_xml_internal.h"
#include "unity_minimal.h"

/** @brief Escaped XML probe capacity. */
typedef enum : uint16_t {
  k_xml_escape_bytes = 256U, /**< Complete escaped probe bytes. */
} mdl_export_xml_limit_t;

/** @brief Fixed capacities shared by the export-document vectors. */
typedef enum : uint32_t {
  k_xml_arena_bytes    = 8U * 1024U * 1024U, /**< Caller-owned exporter arena.    */
  k_xml_archive_bytes  = 256U * 1024U,       /**< Produced-archive probe bytes.   */
  k_xml_document_bytes = 8192U,              /**< Rendered document buffer bytes. */
  k_xml_document_short = 1024U,              /**< Capacity too small to render.   */
  k_xml_path_bytes     = 512U,               /**< Absolute fixture path bytes.    */
  k_xml_expect_bytes   = 320U,               /**< Expected manifest probe bytes.  */
  k_xml_cat_bytes      = 16U,                /**< Bounded accumulator probe.      */
  k_xml_page_rows      = 2U,                 /**< Direct-call page-name rows.     */
  k_xml_dir_mode       = 0755U,              /**< rwxr-xr-x fixture directory.    */
  k_xml_file_mode      = 0600U,              /**< rw------- fixture file.         */
} mdl_export_xml_bound_t;

/** @brief Named page-name table rows used by the direct writer vectors. */
typedef enum : uint8_t {
  k_xml_row_first  = 0U, /**< First direct-call page row.  */
  k_xml_row_second = 1U, /**< Second direct-call page row. */
} mdl_export_xml_row_t;

/** @brief Leading signature byte of the PNG container. */
typedef enum : uint8_t {
  k_xml_png_lead = 0x89U, /**< First byte of the eight-byte PNG signature. */
  k_xml_riff_pad = 0U,    /**< RIFF payload-length filler byte.            */
} mdl_export_xml_magic_t;

/** @brief PNG container signature prefix. */
static const uint8_t s_xml_png_head[] = {k_xml_png_lead, 'P', 'N', 'G'};

/** @brief GIF89a container signature. */
static const uint8_t s_xml_gif_head[] = {'G', 'I', 'F', '8', '9', 'a'};

/** @brief RIFF/WEBP container head with a filler payload length. */
static const uint8_t s_xml_webp_head[] = {'R',
                                          'I',
                                          'F',
                                          'F',
                                          k_xml_riff_pad,
                                          k_xml_riff_pad,
                                          k_xml_riff_pad,
                                          k_xml_riff_pad,
                                          'W',
                                          'E',
                                          'B',
                                          'P'};

/** @brief BMP container signature followed by filler. */
static const uint8_t s_xml_bmp_head[] = {'B', 'M', 'p', 'x'};

/** @brief Page bytes carrying no recognized image signature. */
static const uint8_t s_xml_opaque_head[] = {'z', 'z', 'z', 'z'};

/** @brief Caller-owned exporter arena backing every vector in this group. */
static uint8_t s_xml_arena[k_xml_arena_bytes];

/** @brief Bounded probe holding one complete produced archive. */
static uint8_t s_xml_archive[k_xml_archive_bytes];

/** @brief Bounded page-name table passed to the EPUB writer directly. */
static char s_xml_names[k_xml_page_rows][k_name_max];

/** @brief Caller workspace for one rendered package document. */
static char s_xml_opf[k_xml_document_bytes];

/** @brief Caller workspace for one rendered navigation document. */
static char s_xml_navdoc[k_xml_document_bytes];

/** @brief One page fixture and the media type its manifest item must declare. */
typedef struct {
  const char*    leaf;     /**< Page file name inside the chapter directory. */
  const uint8_t* head;     /**< Signature bytes written into the page file.  */
  size_t         head_len; /**< Readable byte count at the signature.        */
  const char*    media;    /**< Media type the EPUB manifest must declare.   */
} mdl_export_xml_media_case_t;

/** @brief Archive sink that refuses exactly one recognizable archive write. */
typedef struct {
  const char* refuse;   /**< Byte run whose write must be refused.        */
  const char* expect;   /**< Byte run that must reach the sink untouched. */
  size_t      accepted; /**< Bytes accepted before the refusal.           */
  bool        refused;  /**< Whether the refused run was seen.            */
  bool        observed; /**< Whether the expected run was stored first.   */
} mdl_export_xml_sink_t;

/** @brief Page fixtures covering signature typing, suffix typing, and default. */
static const mdl_export_xml_media_case_t s_xml_media_cases[] = {
  {"a_magic.png", s_xml_png_head, sizeof(s_xml_png_head), "image/png"},
  {"b_magic.gif", s_xml_gif_head, sizeof(s_xml_gif_head), "image/gif"},
  {"c_magic.webp", s_xml_webp_head, sizeof(s_xml_webp_head), "image/webp"},
  {"d_magic.bmp", s_xml_bmp_head, sizeof(s_xml_bmp_head), "image/bmp"},
  {"e_plain.png", s_xml_opaque_head, sizeof(s_xml_opaque_head), "image/png"},
  {"f_plain.PNG", s_xml_opaque_head, sizeof(s_xml_opaque_head), "image/png"},
  {"g_plain.gif", s_xml_opaque_head, sizeof(s_xml_opaque_head), "image/gif"},
  {"h_plain.GIF", s_xml_opaque_head, sizeof(s_xml_opaque_head), "image/gif"},
  {"i_plain.webp", s_xml_opaque_head, sizeof(s_xml_opaque_head), "image/webp"},
  {"j_plain.WEBP", s_xml_opaque_head, sizeof(s_xml_opaque_head), "image/webp"},
  {"k_plain.bmp", s_xml_opaque_head, sizeof(s_xml_opaque_head), "image/bmp"},
  {"l_plain.BMP", s_xml_opaque_head, sizeof(s_xml_opaque_head), "image/bmp"},
  {"m_plain.jpg", s_xml_opaque_head, sizeof(s_xml_opaque_head), "image/jpeg"},
};

/**
 * @brief Write one complete fixture byte range through a raw host descriptor.
 * @details Creates the fixture in one call and asserts every byte plus the
 *          close, so a partially written page can never reach the exporter.
 * @param[in] path Absolute host fixture path.
 * @param[in] bytes Readable fixture bytes.
 * @param[in] length Exact fixture extent.
 * @pre @p path names a file below an existing directory.
 * @pre @p bytes remains readable for @p length bytes.
 * @post Normal return leaves exactly @p length bytes at @p path.
 * @post No host descriptor remains open.
 * @note Test-only host helper; assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_xml_write_file(const char* path, const uint8_t* bytes, size_t length)
{
  const int descriptor =
    open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mode_t)k_xml_file_mode);
  TEST_ASSERT(descriptor >= 0);
  size_t offset = 0U;
  while (offset < length) {
    const ssize_t written = write(descriptor, &bytes[offset], length - offset);
    if (written > 0) {
      offset += (size_t)written;
    } else if ((written < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  TEST_ASSERT(offset == length);
  TEST_ASSERT(close(descriptor) == 0);
}

/**
 * @brief Read one complete produced artifact into a bounded probe buffer.
 * @details Reads to end of file, proves the artifact fit the probe, and closes
 *          the descriptor before returning the exact observed extent.
 * @param[in] path Absolute host artifact path.
 * @param[out] destination Bounded probe destination.
 * @param[in] capacity Writable bytes at @p destination.
 * @return Exact artifact extent in bytes.
 * @retval nonzero The complete artifact was copied into @p destination.
 * @pre @p path names a readable regular file.
 * @pre @p destination addresses @p capacity writable bytes.
 * @post Normal return means the whole artifact fit the probe.
 * @post No host descriptor remains open.
 * @note Test-only host helper; assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_xml_read_file(const char* path, uint8_t* destination, size_t capacity)
{
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  TEST_ASSERT(descriptor >= 0);
  size_t offset = 0U;
  while (offset < capacity) {
    const ssize_t got = read(descriptor, &destination[offset], capacity - offset);
    if (got > 0) {
      offset += (size_t)got;
    } else if ((got < 0) && (errno == EINTR)) {
      continue;
    } else {
      break;
    }
  }
  uint8_t tail = 0U;
  TEST_ASSERT(read(descriptor, &tail, sizeof(tail)) == 0);
  TEST_ASSERT(close(descriptor) == 0);
  TEST_ASSERT(offset > 0U);
  return offset;
}

/**
 * @brief Report whether a bounded byte probe contains an exact text run.
 * @details Scans the complete probe, so a stored archive member's text is found
 *          without treating embedded NUL bytes as a terminator.
 * @param[in] haystack Readable probe bytes.
 * @param[in] haystack_len Number of readable bytes at @p haystack.
 * @param[in] needle NUL-terminated text to locate.
 * @return Whether the complete text appears in the probe.
 * @retval true The exact byte run is present.
 * @retval false The run is absent or longer than the probe.
 * @pre @p haystack addresses @p haystack_len readable bytes.
 * @pre @p needle is non-NULL and NUL-terminated.
 * @post Neither input is modified.
 * @post The result depends only on the supplied arguments.
 * @note Pure and thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_xml_bytes_contain(const uint8_t* haystack, size_t haystack_len, const char* needle)
{
  const size_t needle_len = strlen(needle);
  if ((needle_len == 0U) || (needle_len > haystack_len)) {
    return false;
  }
  for (size_t offset = 0U; offset <= (haystack_len - needle_len); ++offset) {
    if (memcmp(&haystack[offset], needle, needle_len) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @test XML escaper replaces metacharacters and fails rather than truncating.
 * @brief Exercise the xml escape regression scenario.
 * @details Executes the xml escape scenario through production interfaces and checks its observable success, rejection, and boundary results.
 * @pre Assertions are enabled for contract verification.
 * @pre The test fixture workspace is isolated for this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_xml_escape(void)
{
  TEST_BEGIN("xml escape");
  char out[k_xml_escape_bytes];
  TEST_ASSERT(mdl_xml_escape("a&b<c>\"'", out, sizeof(out)));
  TEST_ASSERT(strcmp(out, "a&amp;b&lt;c&gt;&quot;&apos;") == 0);
  TEST_ASSERT(mdl_xml_escape("page_001.jpg", out, sizeof(out))); /* legal name kept */
  TEST_ASSERT(strcmp(out, "page_001.jpg") == 0);
  char tiny[4];
  TEST_ASSERT(!mdl_xml_escape("&&&", tiny, sizeof(tiny))); /* would not fit -> fail */
  TEST_END("xml escape");
}

/**
 * @test The bounded XML accumulator appends an exact fit and refuses one byte more.
 * @brief Exercise the epub accumulator boundary scenario.
 * @details Fills the accumulator to its last writable byte, then offers one more
 *          byte and proves the refusal leaves the previous document text intact:
 *          a truncated manifest fragment must never be reported as appended.
 * @pre Assertions are enabled for contract verification.
 * @pre The probe accumulator is exclusively owned by this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_epub_accumulator_bounds(void)
{
  TEST_BEGIN("epub accumulator bounds");
  char destination[k_xml_cat_bytes] = {};
  TEST_ASSERT(priv_mdl_epub_str_cat(destination, sizeof(destination), "<a/>"));
  TEST_ASSERT(strcmp(destination, "<a/>") == 0);
  /* 4 held + 11 appended + NUL == the exact 16-byte capacity. */
  TEST_ASSERT(priv_mdl_epub_str_cat(destination, sizeof(destination), "<bbbbbbbb/>"));
  TEST_ASSERT(strcmp(destination, "<a/><bbbbbbbb/>") == 0);
  /* One byte beyond the capacity: refuse, and leave the document unchanged. */
  TEST_ASSERT(!priv_mdl_epub_str_cat(destination, sizeof(destination), "x"));
  TEST_ASSERT(strcmp(destination, "<a/><bbbbbbbb/>") == 0);
  TEST_END("epub accumulator bounds");
}

/**
 * @brief Render one package/navigation document pair into the group workspaces.
 * @details Wraps the private metadata seam with the empty manifest, spine, and
 *          navigation fragments every rejection vector shares, so each vector
 *          differs only in the metadata and the two offered capacities.
 * @param[in,out] zip Archive writer the seam may add finished documents to.
 * @param[in] meta Metadata to encode, or NULL for deterministic defaults.
 * @param[in] opf_cap Package-document capacity offered to the seam.
 * @param[in] nav_cap Navigation-document capacity offered to the seam.
 * @return Metadata rendering status.
 * @retval k_ra8_ok Both documents were rendered and added.
 * @retval other The seam rejected the metadata or the offered capacity.
 * @pre @p zip is either initialized for writing or deliberately unusable.
 * @pre The group document workspaces are exclusively owned by the caller.
 * @post No caller storage ownership is transferred.
 * @post The seam status is returned unchanged.
 * @note Test-only adapter over one private exporter seam.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_xml_add_meta(mz_zip_archive*          zip,
                                                    const mdl_export_meta_t* meta,
                                                    size_t                   opf_cap,
                                                    size_t                   nav_cap)
{
  return priv_mdl_epub_add_meta(zip,
                                "",
                                "",
                                "",
                                0U,
                                meta,
                                s_xml_opf,
                                opf_cap,
                                s_xml_navdoc,
                                nav_cap);
}

/**
 * @test Package-document rendering refuses short workspaces and invalid sources.
 * @brief Exercise the epub package-document rejection scenario.
 * @details Drives ::priv_mdl_epub_add_meta with an unusable OPF capacity, an
 *          unusable navigation capacity, absent metadata, and an unusable source
 *          URL. Every vector must be rejected before the archive writer is
 *          touched, which the untouched zeroed writer descriptor proves.
 * @pre Assertions are enabled for contract verification.
 * @pre The document workspaces are exclusively owned by this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post No archive member was added by any rejected vector.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_epub_package_document_rejects(void)
{
  TEST_BEGIN("epub package document rejects");
  mz_zip_archive    zip = {};
  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  /* The fixed package overhead alone exceeds the offered OPF workspace. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    internal_xml_add_meta(&zip, &meta, (size_t)k_xml_document_short, sizeof(s_xml_navdoc)));
  /* The OPF fits; the navigation document does not. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    internal_xml_add_meta(&zip, &meta, sizeof(s_xml_opf), (size_t)k_xml_document_short));
  /* Absent metadata resolves to deterministic defaults and is bounded alike. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_size,
    internal_xml_add_meta(&zip, nullptr, (size_t)k_xml_document_short, sizeof(s_xml_navdoc)));
  /* A non-HTTP(S) attribution URL is a metadata failure, not a size failure. */
  (void)__builtin_snprintf(meta.source_url,
                           sizeof(meta.source_url),
                           "ftp://example.invalid/chapter");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 internal_xml_add_meta(&zip, &meta, sizeof(s_xml_opf), sizeof(s_xml_navdoc)));
  TEST_ASSERT_NULL(zip.m_pState);
  TEST_ASSERT_EQ(MZ_ZIP_MODE_INVALID, zip.m_zip_mode);
  TEST_ASSERT_EQ(0U, zip.m_total_files);
  TEST_END("epub package document rejects");
}

/**
 * @brief Store archive bytes, refusing exactly the configured byte run.
 * @details Stands in for the exporter's own transaction sink so one chosen
 *          archive write fails while every earlier write is stored. Reporting
 *          fewer bytes than requested is miniz's write-failure contract.
 * @param[in,out] opaque Borrowed ::mdl_export_xml_sink_t callback state.
 * @param[in] file_offset Absolute archive offset chosen by the writer.
 * @param[in] bytes Readable archive bytes.
 * @param[in] length Requested byte count.
 * @return Bytes accepted by the sink.
 * @retval 0 The write carried the refused run and was rejected.
 * @retval length The write was stored completely.
 * @pre @p opaque addresses one live configured sink.
 * @pre @p bytes addresses @p length readable bytes.
 * @post A refusal records the event and stores nothing.
 * @post An accepted write advances the accepted-byte tally exactly.
 * @note Called synchronously by one archive writer.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
internal_xml_sink_write(void* opaque, mz_uint64 file_offset, const void* bytes, size_t length)
{
  mdl_export_xml_sink_t* sink   = (mdl_export_xml_sink_t*)opaque;
  const uint8_t*         source = (const uint8_t*)bytes;
  (void)file_offset;
  if (internal_xml_bytes_contain(source, length, sink->refuse)) {
    sink->refused = true;
    return 0U;
  }
  if (internal_xml_bytes_contain(source, length, sink->expect)) {
    sink->observed = true;
  }
  sink->accepted += length;
  return length;
}

/**
 * @brief Render the package documents into an archive whose sink fails once.
 * @details Binds a caller-arena archive writer to the configured sink and runs
 *          the real metadata seam over it, so the failure appears exactly where
 *          a storage failure would appear during publication.
 * @param[in,out] sink Sink state configured with the runs to refuse and expect.
 * @return Metadata rendering status.
 * @retval k_ra8_ok Both documents were stored and the archive was finalized.
 * @retval k_ra8_fail The archive writer rejected a document or the directory.
 * @pre @p sink names a run the writer really emits, or the vector proves nothing.
 * @pre The group arena and document workspaces are exclusively owned.
 * @post The archive writer is ended and the arena cursor restored.
 * @post The sink records whether the refusal and the expected write happened.
 * @note Test-only adapter; assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_xml_add_meta_over_sink(mdl_export_xml_sink_t* sink)
{
  mz_zip_archive         zip;
  mdl_zip_allocator_t    allocator;
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, s_xml_arena, sizeof(s_xml_arena));
  priv_mdl_zip_workspace_bind(&zip, &allocator, &workspace);
  zip.m_pWrite     = internal_xml_sink_write;
  zip.m_pIO_opaque = sink;
  TEST_ASSERT(mz_zip_writer_init(&zip, 0) != MZ_FALSE);
  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  const ra8_err_t rc = internal_xml_add_meta(&zip, &meta, sizeof(s_xml_opf), sizeof(s_xml_navdoc));
  (void)mz_zip_writer_end(&zip);
  priv_mdl_zip_workspace_release(&allocator);
  return rc;
}

/**
 * @test A refused navigation member or central directory is never a success.
 * @brief Exercise the epub package write-failure scenario.
 * @details The package document is stored before the navigation document, and
 *          both are stored before the central directory. Failing each of those
 *          later writes in turn proves the metadata seam reports the failure
 *          instead of treating an archive that is missing its navigation
 *          document, or that was never finalized, as a complete publication.
 *          The expected-run flag proves the earlier member really was stored,
 *          so neither vector can pass by failing everything.
 * @pre Assertions are enabled for contract verification.
 * @pre The group arena and document workspaces are exclusively owned.
 * @post Normal return means every reached contract assertion passed.
 * @post No archive survives either vector.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_epub_package_write_failures(void)
{
  TEST_BEGIN("epub package write failures");
  /* The package document is stored; the navigation member is refused. */
  mdl_export_xml_sink_t navigation = {.refuse = "OEBPS/nav.xhtml", .expect = "OEBPS/content.opf"};
  TEST_ASSERT_EQ(k_ra8_fail, internal_xml_add_meta_over_sink(&navigation));
  TEST_ASSERT(navigation.observed);
  TEST_ASSERT(navigation.refused);
  TEST_ASSERT(navigation.accepted > 0U);
  /* Both members are stored; the central directory record is refused. */
  mdl_export_xml_sink_t directory = {.refuse = "PK\x01\x02", .expect = "OEBPS/nav.xhtml"};
  TEST_ASSERT_EQ(k_ra8_fail, internal_xml_add_meta_over_sink(&directory));
  TEST_ASSERT(directory.observed);
  TEST_ASSERT(directory.refused);
  TEST_ASSERT(directory.accepted > navigation.accepted);
  TEST_END("epub package write failures");
}

/**
 * @test ComicInfo defaults to zero pages and refuses an unusable destination.
 * @brief Exercise the comicinfo document scenario.
 * @details Proves the page-free generator is exactly the page-aware generator at
 *          zero pages, and that a NULL destination or zero capacity is rejected
 *          rather than partially rendered.
 * @pre Assertions are enabled for contract verification.
 * @pre The document workspaces are exclusively owned by this scenario.
 * @post Normal return means every reached contract assertion passed.
 * @post The caller receives no transferred fixture ownership.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_comicinfo_document(void)
{
  TEST_BEGIN("comicinfo document");
  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  (void)__builtin_snprintf(meta.series_title, sizeof(meta.series_title), "Bounded Series");
  TEST_ASSERT_EQ(k_ra8_ok, mdl_export_build_comicinfo(&meta, s_xml_opf, sizeof(s_xml_opf)));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_export_build_comicinfo_pages(&meta, 0U, s_xml_navdoc, sizeof(s_xml_navdoc)));
  TEST_ASSERT(strcmp(s_xml_opf, s_xml_navdoc) == 0);
  TEST_ASSERT(strstr(s_xml_opf, "<PageCount>0</PageCount>") != nullptr);
  TEST_ASSERT(strstr(s_xml_opf, "<Series>Bounded Series</Series>") != nullptr);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 mdl_export_build_comicinfo(&meta, nullptr, sizeof(s_xml_opf)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, mdl_export_build_comicinfo(&meta, s_xml_opf, 0U));
  TEST_ASSERT(strstr(s_xml_opf, "<Series>Bounded Series</Series>") != nullptr);
  TEST_END("comicinfo document");
}

/**
 * @brief Compose one media-fixture page path from the shared case table.
 * @details Builds the path in caller storage and asserts it fit, so a fixture is
 *          never created or removed under a silently truncated name.
 * @param[in] directory Chapter directory holding the media fixtures.
 * @param[in] index Case-table row identifying the page.
 * @param[out] path Caller-owned path destination.
 * @param[in] capacity Writable bytes at @p path.
 * @pre @p index addresses a readable row of ::s_xml_media_cases.
 * @pre @p path addresses @p capacity writable bytes.
 * @post Normal return leaves one complete NUL-terminated path.
 * @post No filesystem state is changed.
 * @note Test-only helper; assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_xml_media_page_path(const char* directory, size_t index, char* path, size_t capacity)
{
  const int length =
    __builtin_snprintf(path, capacity, "%s/%s", directory, s_xml_media_cases[index].leaf);
  TEST_ASSERT((length > 0) && ((size_t)length < capacity));
}

/**
 * @brief Create every media-typing page fixture in a chapter directory.
 * @details Writes each case's signature bytes, so the exporter sees pages that
 *          differ only in their leading bytes and their suffix.
 * @param[in] directory Existing chapter directory.
 * @pre @p directory exists and is writable.
 * @pre No page of the same name already exists there.
 * @post Every case row has one complete page file.
 * @post No host descriptor remains open.
 * @note Test-only helper; assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_xml_write_media_pages(const char* directory)
{
  for (size_t i = 0U; i < (sizeof(s_xml_media_cases) / sizeof(s_xml_media_cases[0])); ++i) {
    char path[k_xml_path_bytes];
    internal_xml_media_page_path(directory, i, path, sizeof(path));
    internal_xml_write_file(path, s_xml_media_cases[i].head, s_xml_media_cases[i].head_len);
  }
}

/**
 * @brief Remove every media-typing page fixture from a chapter directory.
 * @details Asserts each removal, so a leftover fixture cannot silently change
 *          the next vector or block the directory removal.
 * @param[in] directory Chapter directory holding the fixtures.
 * @pre Every case row still has its page file.
 * @pre No exporter continues to read the directory.
 * @post No case-row page file remains.
 * @post The directory itself is left in place.
 * @note Test-only helper; assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_xml_remove_media_pages(const char* directory)
{
  for (size_t i = 0U; i < (sizeof(s_xml_media_cases) / sizeof(s_xml_media_cases[0])); ++i) {
    char path[k_xml_path_bytes];
    internal_xml_media_page_path(directory, i, path, sizeof(path));
    TEST_ASSERT(unlink(path) == 0);
  }
}

/**
 * @brief Assert the manifest declares the expected media type for every page.
 * @details Checks the complete `href`/`media-type` pair rather than either half,
 *          so a page typed as some other supported image still fails.
 * @param[in] archive_len Readable bytes of the produced archive probe.
 * @pre ::s_xml_archive holds @p archive_len bytes of one produced EPUB.
 * @pre The archive members were stored without compression.
 * @post Every case row's declaration was found in the archive.
 * @post No probe or fixture state is changed.
 * @note Test-only helper; assertion failure terminates the process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_xml_assert_media_items(size_t archive_len)
{
  for (size_t i = 0U; i < (sizeof(s_xml_media_cases) / sizeof(s_xml_media_cases[0])); ++i) {
    char      expected[k_xml_expect_bytes];
    const int length = __builtin_snprintf(expected,
                                          sizeof(expected),
                                          "href=\"images/%s\" media-type=\"%s\"",
                                          s_xml_media_cases[i].leaf,
                                          s_xml_media_cases[i].media);
    TEST_ASSERT((length > 0) && ((size_t)length < sizeof(expected)));
    TEST_ASSERT(internal_xml_bytes_contain(s_xml_archive, archive_len, expected));
  }
}

/**
 * @test The EPUB manifest types pages by magic, then by suffix, and titles a
 *       chapter from its series when no chapter title exists.
 * @brief Exercise the epub manifest media-type scenario.
 * @details Packages one chapter whose pages carry PNG, GIF, WebP, and BMP
 *          signatures alongside pages that carry no signature at all. A
 *          recognized signature must decide the manifest media type; an
 *          unrecognized one must fall back to the page suffix in either case,
 *          and an unknown suffix must fall back to JPEG. Each declaration is
 *          checked as an exact `href`/`media-type` pair, so a page classified as
 *          the wrong type fails even though the archive is otherwise valid.
 * @pre Assertions are enabled for contract verification.
 * @pre The process-local storage binding is initialized.
 * @post Normal return means every reached contract assertion passed.
 * @post Every fixture page, the produced archive, and the directory are removed.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_epub_manifest_media_types(void)
{
  TEST_BEGIN("epub manifest media types");
  const char* directory = "/tmp/mdl_xml_media";
  const char* output    = "/tmp/mdl_xml_media.epub";
  (void)mkdir(directory, (mode_t)k_xml_dir_mode);
  internal_xml_write_media_pages(directory);
  mdl_export_meta_t meta;
  mdl_meta_init(&meta);
  (void)__builtin_snprintf(meta.series_title, sizeof(meta.series_title), "Long Strip Weekly");
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, s_xml_arena, sizeof(s_xml_arena));
  TEST_ASSERT_EQ(k_ra8_ok,
                 mdl_export_chapter_meta_ws(mdl_test_storage_get(),
                                            k_ra8_mdl_format_epub,
                                            directory,
                                            output,
                                            &meta,
                                            &workspace));
  const size_t archive = internal_xml_read_file(output, s_xml_archive, sizeof(s_xml_archive));
  internal_xml_assert_media_items(archive);
  /* No chapter title was supplied, so the series names the publication. */
  TEST_ASSERT(
    internal_xml_bytes_contain(s_xml_archive, archive, "<dc:title>Long Strip Weekly</dc:title>"));
  internal_xml_remove_media_pages(directory);
  TEST_ASSERT(unlink(output) == 0);
  TEST_ASSERT(rmdir(directory) == 0);
  TEST_END("epub manifest media types");
}

/**
 * @test An EPUB written without metadata declares defaults and marks no cover.
 * @brief Exercise the absent-metadata EPUB scenario.
 * @details Drives the EPUB writer directly with a NULL metadata pointer, which
 *          only the internal seam permits. The publication must still validate,
 *          must carry the default creator and title, and must declare no
 *          cover-image property: with no metadata there is no declared cover, so
 *          a page marked as one would be an invented fact.
 * @pre Assertions are enabled for contract verification.
 * @pre The process-local storage binding is initialized.
 * @post Normal return means every reached contract assertion passed.
 * @post Both fixture pages, the produced archive, and the directory are removed.
 * @note Test helper; an assertion failure terminates the test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_epub_without_metadata(void)
{
  TEST_BEGIN("epub without metadata");
  const char* directory = "/tmp/mdl_xml_nometa";
  const char* output    = "/tmp/mdl_xml_nometa.epub";
  (void)mkdir(directory, (mode_t)k_xml_dir_mode);
  (void)__builtin_snprintf(s_xml_names[k_xml_row_first], (size_t)k_name_max, "page_001.jpg");
  (void)__builtin_snprintf(s_xml_names[k_xml_row_second], (size_t)k_name_max, "page_002.jpg");
  for (size_t row = 0U; row < (size_t)k_xml_page_rows; ++row) {
    char path[k_xml_path_bytes];
    (void)__builtin_snprintf(path, sizeof(path), "%s/%s", directory, s_xml_names[row]);
    internal_xml_write_file(path, s_xml_opaque_head, sizeof(s_xml_opaque_head));
  }
  mdl_export_output_t stage = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    priv_mdl_export_output_begin(&stage, mdl_test_storage_get(), output, k_ra8_mdl_format_epub));
  mdl_export_workspace_t workspace;
  mdl_export_workspace_init(&workspace, s_xml_arena, sizeof(s_xml_arena));
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_mdl_export_epub(mdl_test_storage_get(),
                                      directory,
                                      s_xml_names,
                                      (size_t)k_xml_page_rows,
                                      &stage,
                                      nullptr,
                                      &workspace));
  bool published = false;
  TEST_ASSERT_EQ(k_ra8_ok, priv_mdl_export_output_commit(&stage, &workspace, &published));
  TEST_ASSERT(published);
  const size_t archive = internal_xml_read_file(output, s_xml_archive, sizeof(s_xml_archive));
  TEST_ASSERT(
    internal_xml_bytes_contain(s_xml_archive, archive, "<dc:creator>media_dl</dc:creator>"));
  TEST_ASSERT(internal_xml_bytes_contain(s_xml_archive, archive, "<dc:title>chapter</dc:title>"));
  TEST_ASSERT(internal_xml_bytes_contain(s_xml_archive, archive, "href=\"images/page_002.jpg\""));
  TEST_ASSERT(!internal_xml_bytes_contain(s_xml_archive, archive, "properties=\"cover-image\""));
  for (size_t row = 0U; row < (size_t)k_xml_page_rows; ++row) {
    char path[k_xml_path_bytes];
    (void)__builtin_snprintf(path, sizeof(path), "%s/%s", directory, s_xml_names[row]);
    TEST_ASSERT(unlink(path) == 0);
  }
  TEST_ASSERT(unlink(output) == 0);
  TEST_ASSERT(rmdir(directory) == 0);
  TEST_END("epub without metadata");
}

/**
 * @brief Run the export XML escaping and document-composition test group.
 * @pre The unity-minimal assertion process is initialized.
 * @pre The caller owns any process-wide fixture binding used by the group.
 * @post Normal return means every group assertion passed.
 * @post No fixture ownership transfers to the caller.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_export_xml_run(void)
{
  internal_test_xml_escape();
  internal_test_epub_accumulator_bounds();
  internal_test_epub_package_document_rejects();
  internal_test_epub_package_write_failures();
  internal_test_comicinfo_document();
  internal_test_epub_manifest_media_types();
  internal_test_epub_without_metadata();
}
