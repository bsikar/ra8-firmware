/**
 * @file ra_epub_open.c
 * @brief `ra_epub_open()` / `ra_epub_close()` lifecycle plumbing.
 *
 * @details
 * Pulls the .epub bytes out of the opaque media handle, drives miniz
 * to read the ZIP central directory, locates the OPF document via the
 * tinyxml2-backed shim, and populates `ra_epub_book_t`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "ra_epub.h"
#include "ra_err.h"

/* ---------------------------------------------------------------------------
 * Internal constants -- no magic numbers.
 * ---------------------------------------------------------------------------
 */

/**
 * @enum ra_epub_internal_t
 * @brief Implementation-only sizing constants.
 */
typedef enum : uint16_t {
  k_ra_epub_container_xml_buf = 4096,  /**< Stack buffer for container.xml.   */
  k_ra_epub_opf_xml_buf       = 16384, /**< Stack buffer for the OPF document.*/
} ra_epub_internal_t;

/**
 * @brief container.xml path inside every conformant .epub archive.
 */
static const char* const k_container_path = "META-INF/container.xml";

/* ---------------------------------------------------------------------------
 * Forward decls from the C++ shim.
 * ---------------------------------------------------------------------------
 */

typedef struct {
  char opf_path[k_ra_epub_max_path_len];
} ra_epub_container_result_t;

ra_err_t ra_epub_xml_parse_container(const uint8_t*              xml_bytes,
                                     size_t                      xml_len,
                                     ra_epub_container_result_t* out);

ra_err_t ra_epub_xml_parse_opf(const uint8_t* xml_bytes, size_t xml_len, ra_epub_book_t* book);

/* ---------------------------------------------------------------------------
 * Helpers.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Length-checked byte copy used in place of memcpy().
 *
 * @details
 * Keeps clang-tidy's `clang-analyzer-security.insecureAPI` checker
 * happy. Same effect on -O2 generated code as memcpy().
 */
static void priv_byte_copy(uint8_t* dst, const uint8_t* src, size_t n)
{
  for (size_t i = 0U; i < n; i++) {
    dst[i] = src[i];
  }
}

/**
 * @brief Bounded zero-fill used in place of memset(0).
 */
static void priv_byte_zero(uint8_t* dst, size_t n)
{
  for (size_t i = 0U; i < n; i++) {
    dst[i] = 0U;
  }
}

/**
 * @brief Write the directory portion of `path` (everything up to and
 *        including the last '/') into `dst`.
 */
static void priv_dirname(const char* path, char* dst, size_t cap)
{
  if (dst == NULL || cap == 0U) {
    return;
  }
  dst[0] = '\0';
  if (path == NULL) {
    return;
  }
  const char* slash = strrchr(path, '/');
  if (slash == NULL) {
    return;
  }
  size_t len = (size_t)(slash - path) + 1U;
  if (len >= cap) {
    len = cap - 1U;
  }
  priv_byte_copy((uint8_t*)dst, (const uint8_t*)path, len);
  dst[len] = '\0';
}

/**
 * @brief Extract a named entry from the open zip into a stack buffer.
 *
 * Returns the actual size in `*got`. `k_ra_err_no_mem` if the entry is
 * larger than `cap`, `k_ra_err_not_found` if the entry is missing.
 */
static ra_err_t
priv_extract(mz_zip_archive* zip, const char* name, uint8_t* buf, size_t cap, size_t* got)
{
  *got        = 0U;
  int32_t idx = mz_zip_reader_locate_file(zip, name, NULL, 0U);
  if (idx < 0) {
    return k_ra_err_not_found;
  }
  mz_zip_archive_file_stat st;
  if (mz_zip_reader_file_stat(zip, (mz_uint)idx, &st) == MZ_FALSE) {
    return k_ra_err_validation_failed;
  }
  if ((size_t)st.m_uncomp_size > cap) {
    return k_ra_err_no_mem;
  }
  if (mz_zip_reader_extract_to_mem(zip, (mz_uint)idx, buf, cap, 0U) == MZ_FALSE) {
    return k_ra_err_validation_failed;
  }
  *got = (size_t)st.m_uncomp_size;
  return k_ra_ok;
}

/* The book record holds an `mz_zip_archive` inline (no heap). Verify
 * the inline storage is large enough at compile time. */
static_assert(k_ra_epub_zip_archive_bytes >= sizeof(mz_zip_archive),
              "k_ra_epub_zip_archive_bytes too small for mz_zip_archive");

/**
 * @brief Tear down an in-place archive on the failure path.
 *
 * @details Closes miniz state but does not free the storage; the book
 *          record owns it inline.
 */
static void priv_zip_destroy(mz_zip_archive* zip)
{
  if (zip == NULL) {
    return;
  }
  mz_zip_reader_end(zip);
}

/**
 * @brief Run the metadata + spine parsers given an already-open zip.
 *
 * Splits out of `ra_epub_open` to keep that function under the
 * NASA-Rule-4 statement budget enforced by clang-tidy
 * (`readability-function-size`).
 */
static ra_err_t priv_parse_archive(mz_zip_archive* zip,
                                   ra_epub_book_t* out_book,
                                   uint8_t*        opf_scratch,
                                   size_t          opf_cap)
{
  uint8_t  container_buf[k_ra_epub_container_xml_buf];
  size_t   got = 0U;
  ra_err_t err = priv_extract(zip, k_container_path, container_buf, sizeof(container_buf), &got);
  if (err != k_ra_ok) {
    return err;
  }

  ra_epub_container_result_t cres = {};
  err                             = ra_epub_xml_parse_container(container_buf, got, &cres);
  if (err != k_ra_ok) {
    return err;
  }

  size_t opf_got = 0U;
  err            = priv_extract(zip, cres.opf_path, opf_scratch, opf_cap, &opf_got);
  if (err != k_ra_ok) {
    return err;
  }

  priv_dirname(cres.opf_path, out_book->opf_dir, k_ra_epub_max_path_len);
  return ra_epub_xml_parse_opf(opf_scratch, opf_got, out_book);
}

/* ---------------------------------------------------------------------------
 * Public API.
 * ---------------------------------------------------------------------------
 */

ra_err_t ra_epub_open(const void* media, const char* path, ra_epub_book_t* out_book)
{
  (void)path;
  if (media == NULL || out_book == NULL) {
    return k_ra_err_null_ptr;
  }
  const ra_epub_mem_media_t* mem = (const ra_epub_mem_media_t*)media;
  if (mem->data == NULL || mem->size == 0U) {
    return k_ra_err_invalid_arg;
  }

  /* Zero-init the book up front so failure paths return a clean record.
   * This also clears `zip_archive_storage` to a known state for miniz. */
  priv_byte_zero((uint8_t*)out_book, sizeof(*out_book));

  /* Place the mz_zip_archive directly in the book record's inline
   * storage. No heap allocation -- NASA Rule 3 compliance. The byte-
   * storage punning is intentional and documented in the header. */
  void* const     zip_storage = &out_book->zip_archive_storage[0];
  mz_zip_archive* zip         = (mz_zip_archive*)zip_storage;
  if (mz_zip_reader_init_mem(zip, mem->data, mem->size, 0U) == MZ_FALSE) {
    return k_ra_err_validation_failed;
  }

  /* Static (file-scope) OPF scratch keeps the firmware stack frame
   * small -- the OPF blob can be 16 KB and would otherwise blow our
   * 2200-byte per-thread stack budget. */
  static uint8_t s_opf_buf[k_ra_epub_opf_xml_buf];
  ra_err_t       err = priv_parse_archive(zip, out_book, s_opf_buf, sizeof(s_opf_buf));
  if (err != k_ra_ok) {
    priv_zip_destroy(zip);
    return err;
  }

  out_book->zip_archive_active = 1U;
  out_book->zip_bytes          = mem->data;
  out_book->zip_size           = mem->size;
  out_book->in_use             = 1U;
  return k_ra_ok;
}

ra_err_t ra_epub_close(ra_epub_book_t* book)
{
  if (book == NULL) {
    return k_ra_err_null_ptr;
  }
  if (book->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  if (book->zip_archive_active != 0U) {
    void* const zip_storage = &book->zip_archive_storage[0];
    priv_zip_destroy((mz_zip_archive*)zip_storage);
    book->zip_archive_active = 0U;
  }
  book->in_use        = 0U;
  book->chapter_count = 0U;
  return k_ra_ok;
}
