/**
 * @file test_epub_open_cov.c
 * @brief Coverage-boost tests for apps/shared_libs/epub/src/epub_open.c.
 *
 * @details
 * Drives the previously-uncovered branches in epub_open.c by
 * constructing minimal in-memory ZIP / EPUB archives via the miniz
 * writer API. Each test targets a semantic error-path group:
 *
 *   Group A -- ``priv_extract`` error returns:
 *     - container entry absent from the archive;
 *     - container entry larger than the bounded extraction workspace.
 *
 *   Group B -- ``priv_parse_archive`` error returns:
 *     - missing or malformed ``container.xml``;
 *     - a container that names a missing or malformed OPF.
 *
 *   Group C -- ``epub_open`` cleanup after every Group-B failure.
 *
 *   Group D -- ``priv_dirname`` with an OPF stored at the archive root.
 *
 *   Group E -- ``priv_load_toc`` branches:
 *     - navigation item with an empty href;
 *     - fallback from the OPF-relative path to the bare archive path;
 *     - failure of both extraction attempts;
 *     - the EPUB2 NCX parser branch.
 *
 * The current source exclusions cover only predicates constrained by the
 * production call graph: stat after a successful locate, destruction of the
 * embedded ZIP object, and allocation failures for exact-size embedded
 * workspaces. This header deliberately names the predicates rather than source
 * line numbers so movement cannot turn its evidence stale.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @enum epub_open_pad_t
 * @brief Padding that pushes the container XML past the parser's buffer.
 */
typedef enum : uint16_t {
  k_epub_container_overflow_pad =
    512U, /**< Padding pushing container XML past the parser buffer, so the overflow guard fires. */
} epub_open_pad_t;

/* ---------------------------------------------------------------------------
 * Sizing constants (tests are exempt from the magic-number gate per CLAUDE.md).
 * ---------------------------------------------------------------------------
 */

/**
 * @enum cov_open_sizes_t
 * @brief Buffer capacities used across all test groups in this TU.
 */
typedef enum : uint32_t {
  k_cov_zip_buf  = 65536U, /**< Max in-memory ZIP buffer.                     */
  k_cov_pad_len  = 4200U,  /**< Padding chars to push container.xml > 4096 B. */
  k_cov_path_max = 192U,   /**< Mirror of k_epub_max_path_len.                */
} cov_open_sizes_t;

/* ---------------------------------------------------------------------------
 * Static ZIP scratch buffers -- one per test to avoid aliasing.
 * ---------------------------------------------------------------------------
 */

/**
 * @var s_zip_buf
 * @brief Scratch buffer reused across ZIP-writer tests (written by builder).
 * @note Single-threaded; not thread-safe.
 * @warning Do not read concurrently with a builder call.
 * @since 0.1.0
 */
static uint8_t s_zip_buf[k_cov_zip_buf];

/**
 * @var s_zip_size
 * @brief Number of bytes written into s_zip_buf by the most recent builder.
 * @note Set by each builder helper before the test body runs.
 * @since 0.1.0
 */
static size_t s_zip_size = 0U;

/* ---------------------------------------------------------------------------
 * Shared XML fragments.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief XML documents shared across the open-path coverage cases.
 * @details `container_good` is well-formed and points to OEBPS/content.opf.
 */
static const struct {
  const char* container_good;     /**< Valid nested rootfile locator.       */
  const char* container_root_opf; /**< Valid root-level OPF locator.        */
  const char* container_garbage;  /**< Malformed container document.        */
  const char* opf_good;           /**< Valid package manifest and spine.    */
  const char* opf_nav_empty_href; /**< Navigation item with empty href.     */
  const char* opf_nav_missing;    /**< Package without navigation metadata. */
  const char* opf_nav_bare;       /**< Bare navigation manifest fixture.    */
  const char* opf_ncx;            /**< Legacy NCX package fixture.          */
  const char* ncx_doc;            /**< Legacy NCX navigation document.      */
  const char* opf_garbage;        /**< Malformed package document.          */
  const char* chapter_xhtml;      /**< Valid XHTML chapter.                 */
  const char* nav_xhtml;          /**< Valid XHTML navigation document.     */
} s_documents = {
  .container_good = "<?xml version=\"1.0\"?>\n"
                    "<container version=\"1.0\""
                    " xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                    "  <rootfiles>\n"
                    "    <rootfile full-path=\"OEBPS/content.opf\""
                    " media-type=\"application/oebps-package+xml\"/>\n"
                    "  </rootfiles>\n"
                    "</container>\n",

  /**
 * @var s_documents.container_root_opf
 * @brief container.xml pointing to content.opf at archive root (no slash).
 * @since 0.1.0
 */
  .container_root_opf = "<?xml version=\"1.0\"?>\n"
                        "<container version=\"1.0\""
                        " xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
                        "  <rootfiles>\n"
                        "    <rootfile full-path=\"content.opf\""
                        " media-type=\"application/oebps-package+xml\"/>\n"
                        "  </rootfiles>\n"
                        "</container>\n",

  /**
 * @var s_documents.container_garbage
 * @brief Syntactically invalid container.xml rejected by the bounded XML reader.
 * @since 0.1.0
 */
  .container_garbage = "THIS IS NOT XML AT ALL <<< !! ",

  /**
 * @var s_documents.opf_good
 * @brief Minimal well-formed OPF with one spine chapter.
 * @since 0.1.0
 */
  .opf_good = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<package xmlns=\"http://www.idpf.org/2007/opf\""
              " version=\"3.0\" unique-identifier=\"id\">\n"
              "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
              "    <dc:title>Cov Book</dc:title>\n"
              "    <dc:creator>Test</dc:creator>\n"
              "    <dc:language>en</dc:language>\n"
              "    <dc:identifier id=\"id\">urn:cov:1</dc:identifier>\n"
              "  </metadata>\n"
              "  <manifest>\n"
              "    <item id=\"ch1\" href=\"ch1.xhtml\""
              " media-type=\"application/xhtml+xml\"/>\n"
              "  </manifest>\n"
              "  <spine>\n"
              "    <itemref idref=\"ch1\"/>\n"
              "  </spine>\n"
              "</package>\n",

  /**
 * @var s_documents.opf_nav_empty_href
 * @brief OPF where the nav manifest item has an empty href.
 *
 * @details The reader exposes an empty span for an empty attribute value.
 *          The parser sees nav_href != NULL but toc_path gets set to "".
 *          priv_load_toc then hits the toc_path[0]=='\0' guard (line 261).
 * @since 0.1.0
 */
  .opf_nav_empty_href = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                        "<package xmlns=\"http://www.idpf.org/2007/opf\""
                        " version=\"3.0\" unique-identifier=\"id\">\n"
                        "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                        "    <dc:title>Empty Nav</dc:title>\n"
                        "    <dc:creator>Test</dc:creator>\n"
                        "    <dc:language>en</dc:language>\n"
                        "    <dc:identifier id=\"id\">urn:cov:2</dc:identifier>\n"
                        "  </metadata>\n"
                        "  <manifest>\n"
                        "    <item id=\"nav\" href=\"\""
                        " media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
                        "    <item id=\"ch1\" href=\"ch1.xhtml\""
                        " media-type=\"application/xhtml+xml\"/>\n"
                        "  </manifest>\n"
                        "  <spine>\n"
                        "    <itemref idref=\"ch1\"/>\n"
                        "  </spine>\n"
                        "</package>\n",

  /**
 * @var s_documents.opf_nav_missing
 * @brief OPF where nav href references a file absent from the archive.
 *
 * @details Both full_path and bare toc_path will not be found in the archive,
 *          exercising line 271 (fallback attempt) and line 274 (bail-out).
 * @since 0.1.0
 */
  .opf_nav_missing = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<package xmlns=\"http://www.idpf.org/2007/opf\""
                     " version=\"3.0\" unique-identifier=\"id\">\n"
                     "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                     "    <dc:title>Missing Nav</dc:title>\n"
                     "    <dc:creator>Test</dc:creator>\n"
                     "    <dc:language>en</dc:language>\n"
                     "    <dc:identifier id=\"id\">urn:cov:3</dc:identifier>\n"
                     "  </metadata>\n"
                     "  <manifest>\n"
                     "    <item id=\"nav\" href=\"does_not_exist.xhtml\""
                     " media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
                     "    <item id=\"ch1\" href=\"ch1.xhtml\""
                     " media-type=\"application/xhtml+xml\"/>\n"
                     "  </manifest>\n"
                     "  <spine>\n"
                     "    <itemref idref=\"ch1\"/>\n"
                     "  </spine>\n"
                     "</package>\n",

  /**
 * @var s_documents.opf_nav_bare
 * @brief OPF where nav is stored at the bare archive path "nav.xhtml".
 *
 * @details The join path ("OEBPS/nav.xhtml") will not be found; the bare
 *          path ("nav.xhtml") will succeed, exercising lines 271-272.
 * @since 0.1.0
 */
  .opf_nav_bare = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<package xmlns=\"http://www.idpf.org/2007/opf\""
                  " version=\"3.0\" unique-identifier=\"id\">\n"
                  "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                  "    <dc:title>Bare Nav</dc:title>\n"
                  "    <dc:creator>Test</dc:creator>\n"
                  "    <dc:language>en</dc:language>\n"
                  "    <dc:identifier id=\"id\">urn:cov:4</dc:identifier>\n"
                  "  </metadata>\n"
                  "  <manifest>\n"
                  "    <item id=\"nav\" href=\"nav.xhtml\""
                  " media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
                  "    <item id=\"ch1\" href=\"ch1.xhtml\""
                  " media-type=\"application/xhtml+xml\"/>\n"
                  "  </manifest>\n"
                  "  <spine>\n"
                  "    <itemref idref=\"ch1\"/>\n"
                  "  </spine>\n"
                  "</package>\n",

  /**
 * @var s_documents.opf_ncx
 * @brief EPUB2-style OPF with NCX navigation (no nav properties item).
 *
 * @details Drives line 280 (the epub_xml_parse_ncx call in priv_load_toc).
 * @since 0.1.0
 */
  .opf_ncx = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<package xmlns=\"http://www.idpf.org/2007/opf\""
             " version=\"2.0\" unique-identifier=\"id\">\n"
             "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
             "    <dc:title>NCX Book</dc:title>\n"
             "    <dc:creator>Test</dc:creator>\n"
             "    <dc:language>en</dc:language>\n"
             "    <dc:identifier id=\"id\">urn:cov:5</dc:identifier>\n"
             "  </metadata>\n"
             "  <manifest>\n"
             "    <item id=\"ncx\" href=\"toc.ncx\""
             " media-type=\"application/x-dtbncx+xml\"/>\n"
             "    <item id=\"ch1\" href=\"ch1.xhtml\""
             " media-type=\"application/xhtml+xml\"/>\n"
             "  </manifest>\n"
             "  <spine toc=\"ncx\">\n"
             "    <itemref idref=\"ch1\"/>\n"
             "  </spine>\n"
             "</package>\n",

  /**
 * @var s_documents.ncx_doc
 * @brief Minimal valid EPUB2 NCX document.
 * @since 0.1.0
 */
  .ncx_doc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
             "  <head><meta name=\"dtb:uid\" content=\"urn:cov:5\"/></head>\n"
             "  <docTitle><text>NCX Book</text></docTitle>\n"
             "  <navMap>\n"
             "    <navPoint id=\"np1\" playOrder=\"1\">\n"
             "      <navLabel><text>Chapter One</text></navLabel>\n"
             "      <content src=\"ch1.xhtml\"/>\n"
             "    </navPoint>\n"
             "  </navMap>\n"
             "</ncx>\n",

  /**
 * @var s_documents.opf_garbage
 * @brief Syntactically invalid OPF (triggers OPF parse failure at line 336).
 * @since 0.1.0
 */
  .opf_garbage = "DEFINITELY NOT VALID XML <<<< ",

  /**
 * @var s_documents.chapter_xhtml
 * @brief Minimal XHTML chapter body stored in the synthetic archive.
 * @since 0.1.0
 */
  .chapter_xhtml = "<?xml version=\"1.0\"?>"
                   "<html><body><p>Hello.</p></body></html>",

  /**
 * @var s_documents.nav_xhtml
 * @brief Minimal EPUB3 navigation document.
 * @since 0.1.0
 */
  .nav_xhtml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<html xmlns=\"http://www.w3.org/1999/xhtml\""
               " xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
               "<head><title>Nav</title></head><body>\n"
               "<nav epub:type=\"toc\"><ol>\n"
               "  <li><a href=\"ch1.xhtml\">Chapter One</a></li>\n"
               "</ol></nav></body></html>\n",
};

/* ---------------------------------------------------------------------------
 * ZIP builder helpers.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Finalise a heap-backed miniz archive into s_zip_buf / s_zip_size.
 *
 * @details Calls mz_zip_writer_finalize_heap_archive, copies the result to
 *          s_zip_buf, and calls mz_zip_writer_end.  Aborts the test on
 *          builder failure.
 *
 * @param[in] zip Initialised writer (not yet finalised).
 *
 * @pre zip was opened with mz_zip_writer_init_heap.
 * @pre s_zip_buf is writable for k_cov_zip_buf bytes.
 * @post s_zip_buf[0..s_zip_size-1] holds the final ZIP.
 * @post s_zip_size > 0.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_finalise(mz_zip_archive* zip)
{
  void*  heap_buf  = nullptr;
  size_t heap_size = 0U;
  TEST_ASSERT(mz_zip_writer_finalize_heap_archive(zip, &heap_buf, &heap_size) == MZ_TRUE);
  TEST_ASSERT(heap_buf != nullptr);
  TEST_ASSERT(heap_size > 0U);
  TEST_ASSERT(heap_size <= (size_t)k_cov_zip_buf);
  (void)memcpy(s_zip_buf, heap_buf, heap_size);
  s_zip_size = heap_size;
  mz_free(heap_buf);
  mz_zip_writer_end(zip);
}

/**
 * @brief Add a NUL-terminated string as a compressed archive entry.
 *
 * @param[in] zip  Open writer.
 * @param[in] name Archive entry name.
 * @param[in] text Content string (NUL-terminated).
 *
 * @pre zip and name are non-NULL; text is NUL-terminated.
 * @post Entry is appended to zip on success.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @details Implements the add str fixture operation used only by this focused test executable. @pre Fixed-capacity fixture storage required by this operation is available. @post Documented outputs contain the exercised result when the operation succeeds. */
RA8_INTERNAL static void internal_add_str(mz_zip_archive* zip, const char* name, const char* text)
{
  TEST_ASSERT(
    mz_zip_writer_add_mem(zip, name, (const void*)text, strlen(text), MZ_DEFAULT_COMPRESSION) ==
    MZ_TRUE);
}

/* ---------------------------------------------------------------------------
 * Group A/C -- priv_extract returns k_ra8_err_not_found (lines 181, 318, 390, 391).
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_open_no_container_xml
 * @brief ZIP missing META-INF/container.xml -> k_ra8_err_not_found.
 *
 * @details The archive contains only a "mimetype" entry.  priv_extract()
 *          fails to locate container.xml (line 181), priv_parse_archive()
 *          propagates the error (line 318), and epub_open() runs
 *          priv_zip_destroy + return (lines 390-391).
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 317 (1 condition).
 * - V1 (this test): err=not_found -> true  -> line 318.
 * - V2 (other tests with valid EPUBs): err=k_ra8_ok -> false -> continues.
 * Pair (V1,V2) gives complete MC/DC for this 1-condition decision.
 *
 * @pre s_zip_buf writable; miniz writer available.
 * @pre None.
 * @post s_zip_buf overwritten with a single-entry ZIP.
 * @post No book state modified (open fails).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_open_no_container_xml(void)
{
  TEST_BEGIN("epub_open: no container.xml -> not_found (lines 181,318,390,391)");
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_cov_zip_buf) == MZ_TRUE);
  internal_add_str(&zip, "mimetype", "application/epub+zip");
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_err_not_found, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(0, book.in_use);
  TEST_END("epub_open: no container.xml -> not_found (lines 181,318,390,391)");
}

/* ---------------------------------------------------------------------------
 * Group A/C -- priv_extract returns k_ra8_err_no_mem (lines 188, 318, 390, 391).
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_open_container_too_large
 * @brief container.xml uncomp_size > 4096 -> k_ra8_err_no_mem.
 *
 * @details The container.xml entry is padded with an XML comment to push
 *          its uncompressed size beyond k_epub_container_xml_buf (4096).
 *          priv_extract() returns no_mem (line 188), priv_parse_archive()
 *          propagates it (line 318), and epub_open() cleans up (390-391).
 *
 * @par MC/DC:
 * Decision: `if ((size_t)st.m_uncomp_size > cap)` at line 187 (1 condition).
 * - V1 (this test): uncomp_size > 4096 -> true  -> line 188 (no_mem).
 * - V2 (normal open tests): uncomp_size <= 4096 -> false -> continues.
 * Pair (V1,V2) gives complete MC/DC for this 1-condition decision.
 *
 * @pre s_zip_buf writable; k_cov_pad_len >= 100.
 * @pre None.
 * @post s_zip_buf overwritten with padded-container ZIP.
 * @post No book state modified (open fails).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_open_container_too_large(void)
{
  TEST_BEGIN("epub_open: container.xml > 4096 B -> no_mem (lines 188,318,390,391)");

  /* Build a container.xml that is > 4096 bytes uncompressed.
   * Prefix a large XML comment so the document remains parseable in
   * isolation, but the extracted size exceeds the static scratch. */
  static char s_big_container[k_cov_pad_len + k_epub_container_overflow_pad];
  size_t      pos = 0U;

  /* Opening comment with lots of padding. */
  static const char k_comment_open[]  = "<?xml version=\"1.0\"?>\n<!-- pad: ";
  static const char k_comment_close[] = " -->\n";
  const size_t      co_len            = strlen(k_comment_open);
  const size_t      cc_len            = strlen(k_comment_close);

  (void)memcpy(&s_big_container[pos], k_comment_open, co_len);
  pos += co_len;

  /* Fill padding. */
  for (size_t i = 0U; i < (size_t)k_cov_pad_len; ++i) {
    s_big_container[pos + i] = 'x';
  }
  pos += (size_t)k_cov_pad_len;

  (void)memcpy(&s_big_container[pos], k_comment_close, cc_len);
  pos += cc_len;

  /* Append the real container body. */
  const size_t body_len = strlen(s_documents.container_good);
  (void)memcpy(&s_big_container[pos], s_documents.container_good, body_len);
  pos += body_len;
  s_big_container[pos] = '\0';

  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_cov_zip_buf) == MZ_TRUE);
  TEST_ASSERT(mz_zip_writer_add_mem(&zip,
                                    "META-INF/container.xml",
                                    s_big_container,
                                    pos,
                                    MZ_DEFAULT_COMPRESSION) == MZ_TRUE);
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_err_no_mem, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(0, book.in_use);
  TEST_END("epub_open: container.xml > 4096 B -> no_mem (lines 188,318,390,391)");
}

/* ---------------------------------------------------------------------------
 * Group B/C -- invalid container.xml XML (lines 324, 390, 391).
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_open_invalid_container_xml
 * @brief container.xml is not valid XML -> k_ra8_err_validation_failed.
 *
 * @details epub_xml_parse_container() returns validation_failed;
 *          priv_parse_archive propagates it at line 324; epub_open
 *          cleans up at lines 390-391.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 323 (1 condition).
 * - V1 (this test): err=validation_failed -> true  -> line 324.
 * - V2 (valid-container tests): err=k_ra8_ok -> false -> continues.
 * Pair (V1,V2) gives complete MC/DC for this 1-condition decision.
 *
 * @pre s_zip_buf writable.
 * @pre None.
 * @post s_zip_buf overwritten.
 * @post No book state modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_open_invalid_container_xml(void)
{
  TEST_BEGIN("epub_open: invalid container.xml XML -> validation_failed (lines 324,390,391)");
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_cov_zip_buf) == MZ_TRUE);
  internal_add_str(&zip, "META-INF/container.xml", s_documents.container_garbage);
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(0, book.in_use);
  TEST_END("epub_open: invalid container.xml XML -> validation_failed (lines 324,390,391)");
}

/* ---------------------------------------------------------------------------
 * Group B/C -- OPF missing (lines 181, 330, 390, 391).
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_open_opf_missing
 * @brief container.xml points to OEBPS/content.opf but that entry is absent.
 *
 * @details priv_extract(OEBPS/content.opf) returns not_found (line 181);
 *          priv_parse_archive propagates it at line 330; epub_open
 *          cleans up at lines 390-391.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 329 (1 condition).
 * - V1 (this test): err=not_found -> true  -> line 330.
 * - V2 (valid-OPF tests): err=k_ra8_ok -> false -> continues.
 * Pair (V1,V2) gives complete MC/DC for this 1-condition decision.
 *
 * @pre s_zip_buf writable.
 * @pre None.
 * @post s_zip_buf overwritten.
 * @post No book state modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_open_opf_missing(void)
{
  TEST_BEGIN("epub_open: OPF missing -> not_found (lines 181,330,390,391)");
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_cov_zip_buf) == MZ_TRUE);
  internal_add_str(&zip, "META-INF/container.xml", s_documents.container_good);
  /* Deliberately do NOT add OEBPS/content.opf. */
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_err_not_found, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(0, book.in_use);
  TEST_END("epub_open: OPF missing -> not_found (lines 181,330,390,391)");
}

/* ---------------------------------------------------------------------------
 * Group B/C -- OPF invalid XML (lines 336, 390, 391).
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_open_opf_invalid_xml
 * @brief OPF entry present but not valid XML -> k_ra8_err_validation_failed.
 *
 * @details epub_xml_parse_opf() returns validation_failed; priv_parse_archive
 *          propagates it at line 336; epub_open cleans up at lines 390-391.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 335 (1 condition).
 * - V1 (this test): err=validation_failed -> true  -> line 336.
 * - V2 (valid-OPF tests): err=k_ra8_ok -> false -> continues.
 * Pair (V1,V2) gives complete MC/DC for this 1-condition decision.
 *
 * @pre s_zip_buf writable.
 * @pre None.
 * @post s_zip_buf overwritten.
 * @post No book state modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_open_opf_invalid_xml(void)
{
  TEST_BEGIN("epub_open: OPF not valid XML -> validation_failed (lines 336,390,391)");
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_cov_zip_buf) == MZ_TRUE);
  internal_add_str(&zip, "META-INF/container.xml", s_documents.container_good);
  internal_add_str(&zip, "OEBPS/content.opf", s_documents.opf_garbage);
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_err_validation_failed, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(0, book.in_use);
  TEST_END("epub_open: OPF not valid XML -> validation_failed (lines 336,390,391)");
}

/* ---------------------------------------------------------------------------
 * Group D -- priv_dirname no-slash arm (line 144).
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_open_opf_at_archive_root
 * @brief OPF path has no slash -> priv_dirname returns early (line 144).
 *
 * @details container.xml says full-path="content.opf" (archive root, no '/').
 *          priv_dirname("content.opf", dst, cap): strrchr returns NULL ->
 *          line 144 is hit; opf_dir is left as "".  epub_open succeeds.
 *
 * @par MC/DC:
 * Decision: `if (slash == nullptr)` at line 143 (1 condition).
 * - V1 (this test): slash=NULL -> true  -> line 144 (return).
 * - V2 (normal tests, e.g. "OEBPS/content.opf"): slash!=NULL -> false.
 * Pair (V1,V2) gives complete MC/DC for this 1-condition decision.
 *
 * @pre s_zip_buf writable; chapter entry added at bare name.
 * @pre None.
 * @post book.in_use == 1 on success.
 * @post epub_close returns k_ra8_ok.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_open_opf_at_archive_root(void)
{
  TEST_BEGIN("epub_open: OPF at archive root (no slash in path) -> line 144");
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_cov_zip_buf) == MZ_TRUE);
  internal_add_str(&zip, "META-INF/container.xml", s_documents.container_root_opf);
  internal_add_str(&zip, "content.opf", s_documents.opf_good);
  internal_add_str(&zip, "ch1.xhtml", s_documents.chapter_xhtml);
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(1, book.in_use);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub_open: OPF at archive root (no slash in path) -> line 144");
}

/* ---------------------------------------------------------------------------
 * Group E -- priv_load_toc: empty toc_path (line 261).
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_toc_empty_path
 * @brief Nav manifest item with href="" sets toc_path="" -> line 261.
 *
 * @details The reader exposes an empty span for an empty attribute value.
 *          epub_xml_parse_opf sees nav_href != NULL and calls
 *          copy_bounded(book->toc_path, ..., "") which writes '\0'.
 *          priv_load_toc then hits toc_path[0]=='\0' (line 261) and
 *          returns early, leaving toc_count at 0.
 *
 * @par MC/DC:
 * Decision: `if (book->toc_path[0] == '\0')` at line 260 (1 condition).
 * - V1 (this test): toc_path="" -> true  -> line 261 (return).
 * - V2 (nav tests with real path): toc_path!="" -> false -> continues.
 * Pair (V1,V2) gives complete MC/DC for this 1-condition decision.
 *
 * @pre s_zip_buf writable.
 * @pre None.
 * @post book.in_use == 1; book.toc_count == 0.
 * @post epub_close returns k_ra8_ok.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_toc_empty_path(void)
{
  TEST_BEGIN("epub_open: nav href=\"\" -> toc_path empty -> line 261");
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_cov_zip_buf) == MZ_TRUE);
  internal_add_str(&zip, "META-INF/container.xml", s_documents.container_good);
  internal_add_str(&zip, "OEBPS/content.opf", s_documents.opf_nav_empty_href);
  internal_add_str(&zip, "OEBPS/ch1.xhtml", s_documents.chapter_xhtml);
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(1, book.in_use);
  /* toc_path was empty so priv_load_toc bailed at line 261 -> toc_count == 0. */
  TEST_ASSERT_EQ(0, book.toc_count);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub_open: nav href=\"\" -> toc_path empty -> line 261");
}

/* ---------------------------------------------------------------------------
 * Group E -- priv_load_toc: both extraction paths fail (lines 271, 272, 274).
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_toc_both_paths_fail
 * @brief TOC file absent from archive -> fallback also fails -> line 274.
 *
 * @details nav href="does_not_exist.xhtml" exists in the OPF but not in the
 *          archive.  priv_load_toc:
 *          1. extract("OEBPS/does_not_exist.xhtml") -> not_found.
 *          2. err == k_ra8_err_not_found -> line 271 hit.
 *          3. extract("does_not_exist.xhtml")       -> not_found.
 *          4. err != k_ra8_ok -> line 274 hit (return).
 *          epub_open still returns k_ra8_ok (TOC is best-effort).
 *
 * @par MC/DC:
 * Decision: `if (err == k_ra8_err_not_found)` at line 269 (1 condition).
 * - V1 (this test): err=not_found -> true  -> line 271 (fallback).
 * - V2 (internal_test_toc_fallback_bare_path): err=k_ra8_ok (fallback succeeds) -> line 271 not taken on second path.
 *
 * Decision: `if (err != k_ra8_ok)` at line 273 (1 condition).
 * - V1 (this test): err still not_found after fallback -> true  -> line 274.
 * - V2 (internal_test_toc_fallback_bare_path): fallback succeeds -> err=k_ra8_ok -> false.
 * Pairs give complete MC/DC for both 1-condition decisions.
 *
 * @pre s_zip_buf writable.
 * @pre None.
 * @post book.in_use == 1; book.toc_count == 0.
 * @post epub_close returns k_ra8_ok.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_toc_both_paths_fail(void)
{
  TEST_BEGIN("epub_open: TOC absent -> both extractions fail -> lines 271,272,274");
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_cov_zip_buf) == MZ_TRUE);
  internal_add_str(&zip, "META-INF/container.xml", s_documents.container_good);
  internal_add_str(&zip, "OEBPS/content.opf", s_documents.opf_nav_missing);
  internal_add_str(&zip, "OEBPS/ch1.xhtml", s_documents.chapter_xhtml);
  /* "OEBPS/does_not_exist.xhtml" and "does_not_exist.xhtml" NOT added. */
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  /* TOC is best-effort; open still succeeds. */
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(1, book.in_use);
  TEST_ASSERT_EQ(0, book.toc_count);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub_open: TOC absent -> both extractions fail -> lines 271,272,274");
}

/* ---------------------------------------------------------------------------
 * Group E -- priv_load_toc: bare-path fallback succeeds (lines 271, 272).
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_toc_fallback_bare_path
 * @brief nav stored at "nav.xhtml" (bare); join("OEBPS/","nav.xhtml") fails.
 *
 * @details nav href="nav.xhtml"; OPF is at "OEBPS/content.opf" so opf_dir=
 *          "OEBPS/".  priv_load_toc builds full_path="OEBPS/nav.xhtml".
 *          The archive stores the nav at bare "nav.xhtml" only, so:
 *          1. extract("OEBPS/nav.xhtml") -> not_found -> line 271 hit.
 *          2. extract("nav.xhtml")       -> success.
 *          3. err == k_ra8_ok -> line 274 NOT taken.
 *          epub_xml_parse_nav runs, toc_count > 0.
 *
 * @par MC/DC:
 * Decision: `if (err == k_ra8_err_not_found)` at line 269 (1 condition).
 * - V1 (internal_test_toc_both_paths_fail): both fail -- provides the true arm.
 * - V2 (this test): first extract not_found -> true -> line 271; but
 *   second extract succeeds -> err=k_ra8_ok -> line 274 NOT taken.
 * Lines 271-272 are covered by this test; line 274 is covered by the
 * other test.
 *
 * @pre s_zip_buf writable; nav stored at archive root.
 * @pre None.
 * @post book.in_use == 1; book.toc_count > 0.
 * @post epub_close returns k_ra8_ok.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_toc_fallback_bare_path(void)
{
  TEST_BEGIN(
    "epub_open: nav at bare path; OEBPS/ join fails -> fallback succeeds -> lines 271,272");
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_cov_zip_buf) == MZ_TRUE);
  internal_add_str(&zip, "META-INF/container.xml", s_documents.container_good);
  internal_add_str(&zip, "OEBPS/content.opf", s_documents.opf_nav_bare);
  internal_add_str(&zip, "OEBPS/ch1.xhtml", s_documents.chapter_xhtml);
  /* nav stored at BARE path "nav.xhtml", NOT at "OEBPS/nav.xhtml". */
  internal_add_str(&zip, "nav.xhtml", s_documents.nav_xhtml);
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(1, book.in_use);
  /* The bare-path fallback succeeded so toc_count should be 1 (one navPoint). */
  TEST_ASSERT(book.toc_count > 0U);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub_open: nav at bare path; OEBPS/ join fails -> fallback succeeds -> lines 271,272");
}

/* ---------------------------------------------------------------------------
 * Group E -- priv_load_toc: NCX branch (line 280).
 * ---------------------------------------------------------------------------
 */

/**
 * @test internal_test_toc_ncx_branch
 * @brief EPUB2-style NCX TOC -> priv_load_toc takes NCX branch (line 280).
 *
 * @details OPF has no properties="nav" item; the spine's toc="ncx" attribute
 *          causes epub_xml_parse_opf to set toc_kind=k_epub_toc_ncx and
 *          toc_path="toc.ncx".  priv_load_toc then dispatches to
 *          epub_xml_parse_ncx (line 280).
 *
 * @par MC/DC:
 * Decision: `if (book->toc_kind == (uint8_t)k_epub_toc_nav)` at line 277
 * (1 condition).
 * - V1 (other nav tests): toc_kind == k_epub_toc_nav -> true  -> line 278.
 * - V2 (this test): toc_kind == k_epub_toc_ncx     -> false -> line 280.
 * Pair (V1,V2) gives complete MC/DC for this 1-condition decision.
 *
 * @pre s_zip_buf writable; NCX document stored in archive.
 * @pre None.
 * @post book.in_use == 1; book.toc_count == 1.
 * @post epub_close returns k_ra8_ok.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_toc_ncx_branch(void)
{
  TEST_BEGIN("epub_open: EPUB2 NCX toc_kind -> NCX branch line 280");
  mz_zip_archive zip;
  (void)memset(&zip, 0, sizeof(zip));
  TEST_ASSERT(mz_zip_writer_init_heap(&zip, 0U, (size_t)k_cov_zip_buf) == MZ_TRUE);
  internal_add_str(&zip, "META-INF/container.xml", s_documents.container_good);
  internal_add_str(&zip, "OEBPS/content.opf", s_documents.opf_ncx);
  internal_add_str(&zip, "OEBPS/ch1.xhtml", s_documents.chapter_xhtml);
  internal_add_str(&zip, "OEBPS/toc.ncx", s_documents.ncx_doc);
  internal_finalise(&zip);

  epub_book_t            book  = {};
  const epub_mem_media_t media = {.data = s_zip_buf, .size = s_zip_size};
  TEST_ASSERT_EQ(k_ra8_ok, epub_open(&media, nullptr, &book));
  TEST_ASSERT_EQ(1, book.in_use);
  TEST_ASSERT_EQ(k_epub_toc_ncx, book.toc_kind);
  /* NCX has one navPoint -> toc_count == 1. */
  TEST_ASSERT_EQ(1, book.toc_count);
  TEST_ASSERT_EQ(k_ra8_ok, epub_close(&book));
  TEST_END("epub_open: EPUB2 NCX toc_kind -> NCX branch line 280");
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Test entry point -- runs all coverage-boost groups in order.
 *
 * @details Executes every test function defined in this TU.  Each builds a
 *          minimal in-memory ZIP, calls epub_open / epub_close, and
 *          asserts the expected return code.  No test uses SIGALRM or any
 *          timer; all assertions are deterministic.
 *
 * @return 0 on success; exit(1) is called by unity_minimal.h on first failure.
 *
 * @pre None.
 * @pre None.
 * @post All tests executed (or process exited on first failure).
 * @post stderr contains a per-test RUN/PASS log.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int main(void)
{
  /* Group A/C: priv_extract error returns (lines 181, 188, 318, 390, 391). */
  internal_test_open_no_container_xml();
  internal_test_open_container_too_large();

  /* Group B/C: priv_parse_archive error returns (lines 324, 330, 336, 390, 391). */
  internal_test_open_invalid_container_xml();
  internal_test_open_opf_missing();
  internal_test_open_opf_invalid_xml();

  /* Group D: priv_dirname no-slash arm (line 144). */
  internal_test_open_opf_at_archive_root();

  /* Group E: priv_load_toc branches (lines 261, 271, 272, 274, 280). */
  internal_test_toc_empty_path();
  internal_test_toc_both_paths_fail();
  internal_test_toc_fallback_bare_path();
  internal_test_toc_ncx_branch();

  return 0;
}
