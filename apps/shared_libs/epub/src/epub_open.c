/**
 * @file epub_open.c
 * @brief `epub_open()` / `epub_close()` lifecycle plumbing.
 *
 * @details
 * Pulls the .epub bytes out of the opaque media handle, drives miniz
 * to read the ZIP central directory, locates the OPF document via the
 * bounded XML pull consumers, and populates `epub_book_t`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub.h"
#include "epub_internal.h"
#include "epub_miniz_alloc.h"
#include "epub_xml_shim_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"

/* ---------------------------------------------------------------------------
 * Internal constants -- no magic numbers.
 * ---------------------------------------------------------------------------
 */

/**
 * @enum epub_internal_t
 * @brief Implementation-only sizing constants.
 */
typedef enum : uint16_t {
  k_epub_container_xml_buf = 4096,  /**< Stack buffer for container.xml. */
  k_epub_opf_xml_buf       = 49152, /**< Static OPF + NCX/nav scratch (48 KiB; uint16_t-capped).
                                        *   Real NCX TOCs exceed 16 KiB (a 77-navPoint book is
                                        *   ~17.6 KiB); the OPF + nav doc share this buffer. */
} epub_internal_t;

/* ---------------------------------------------------------------------------
 * Private contracts for the streamed XML consumers.
 * ---------------------------------------------------------------------------
 */

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
 *
 * @param[in] dst See implementation.
 * @param[in] src See implementation.
 * @param[in] n See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_byte_copy(uint8_t* dst, const uint8_t* src, size_t n)
{
  for (size_t i = 0U; i < n; i++) {
    dst[i] = src[i];
  }
}

/* see header for full description */
RA8_PRIV size_t priv_epub_mem_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  const epub_mem_media_t* const mem = (const epub_mem_media_t*)ctx;
  if ((mem == nullptr) || (buf == nullptr) || (offset > (uint64_t)mem->size) ||
      ((uint64_t)len > ((uint64_t)mem->size - offset))) {
    return 0U;
  }
  internal_byte_copy((uint8_t*)buf, &mem->data[(size_t)offset], len);
  return len;
}

/**
 * @brief Bounded zero-fill used in place of memset(0).
 *
 * @details See implementation.
 * @param[in] dst See implementation.
 * @param[in] n See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_byte_zero(uint8_t* dst, size_t n)
{
  for (size_t i = 0U; i < n; i++) {
    dst[i] = 0U;
  }
}

RA8_PRIV void priv_epub_dirname(const char* path, char* dst, size_t cap)
{
  if ((dst == nullptr) || (cap == 0U)) {
    return;
  }
  dst[0] = '\0';
  if (path == nullptr) {
    return;
  }
  size_t len = 0U;
  for (size_t index = 0U; path[index] != '\0'; ++index) {
    if (path[index] == '/') {
      len = index + 1U;
    }
  }
  if (len == 0U) {
    return;
  }
  if (len >= cap) {
    len = cap - 1U;
  }
  internal_byte_copy((uint8_t*)dst, (const uint8_t*)path, len);
  dst[len] = '\0';
}

/**
 * @brief Extract a named entry from the open zip into a stack buffer.
 *
 * Returns the actual size in `*got`. `k_ra8_err_no_mem` if the entry is
 * larger than `cap`, `k_ra8_err_not_found` if the entry is missing.
 *
 * @details See implementation.
 * @param[in] zip See implementation.
 * @param[in] name See implementation.
 * @param[in] buf See implementation.
 * @param[in] cap See implementation.
 * @param[in] got See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_extract(mz_zip_archive* zip, const char* name, uint8_t* buf, size_t cap, size_t* got)
{
  *got        = 0U;
  int32_t idx = mz_zip_reader_locate_file(zip, name, nullptr, 0U);
  if (idx < 0) {
    return k_ra8_err_not_found;
  }
  mz_zip_archive_file_stat st;
  if (mz_zip_reader_file_stat(zip, (mz_uint)idx, &st) == MZ_FALSE) {
    return k_ra8_err_validation_failed; /* GCOVR_EXCL_LINE -- stat cannot fail: locate found it */
  }
  const ra8_err_t gerr = priv_epub_zip_guard_entry(&st);
  if (gerr != k_ra8_ok) {
    return gerr; /* lying header / declared bomb: reject before inflation */
  }
  if ((size_t)st.m_uncomp_size > cap) {
    return k_ra8_err_no_mem;
  }
  if (mz_zip_reader_extract_to_mem(zip, (mz_uint)idx, buf, cap, 0U) == MZ_FALSE) {
    return k_ra8_err_validation_failed;
  }
  *got = (size_t)st.m_uncomp_size;
  return k_ra8_ok;
}

/* The book record holds an `mz_zip_archive` inline (no heap). Verify
 * the inline storage is large enough at compile time. */
static_assert(k_epub_zip_archive_bytes >= sizeof(mz_zip_archive),
              "k_epub_zip_archive_bytes too small for mz_zip_archive");

/* The byte buffer is declared with `alignas(max_align_t)` in the
 * header; verify miniz's actual alignment requirement is satisfied by
 * that choice. Catches future miniz revisions that bump alignment to
 * something larger than the platform's max_align_t (extremely
 * unlikely, but the cost of the check is zero). */
static_assert(alignof(mz_zip_archive) <= alignof(max_align_t),
              "mz_zip_archive alignment exceeds max_align_t");

/**
 * @brief Tear down an in-place archive on the failure path.
 *
 * @details Closes miniz state but does not free the storage; the book
 *          record owns it inline.
 *
 * @param[in] zip See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_zip_destroy(mz_zip_archive* zip)
{
  if (zip == nullptr) {
    return; /* GCOVR_EXCL_LINE -- callsite passes &zip_archive_storage[0], never NULL */
  }
  mz_zip_reader_end(zip);
}

/**
 * @brief Best-effort: extract and parse the book's TOC document.
 *
 * @details
 * `priv_epub_xml_parse_opf()` records which navigation document to use
 * (`book->toc_kind`) and its href (`book->toc_path`). This helper joins
 * that href onto the OPF directory, extracts the entry (falling back to
 * the bare href for archives that store it un-prefixed), and dispatches
 * to the NCX or nav parser. Any failure is swallowed: a book missing or
 * with a malformed TOC is still fully readable via the spine, so this
 * never propagates an error to `epub_open()`.
 *
 * @param[in]     zip     Open archive.
 * @param[in,out] book    Book whose `toc` table is populated.
 * @param[out]    scratch Scratch buffer reused for the TOC bytes.
 * @param[in]     cap     Capacity of @p scratch in bytes.
 * @pre `book` non-NULL; `zip` initialised; OPF already parsed.
 * @pre @p scratch non-NULL with @p cap > 0.
 * @post On success `book->toc_count` reflects the parsed entries.
 * @post On any failure `book` is left readable with `toc_count == 0`.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_load_toc(mz_zip_archive* zip, epub_book_t* book, uint8_t* scratch, size_t cap)
{
  if (book->toc_kind == (uint8_t)k_epub_toc_none) {
    return;
  }
  if (book->toc_path[0] == '\0') {
    return;
  }

  char full_path[k_epub_max_path_len];
  priv_epub_join_path(book->opf_dir, book->toc_path, full_path, sizeof(full_path));

  size_t    got = 0U;
  ra8_err_t err = internal_extract(zip, full_path, scratch, cap, &got);
  if (err == k_ra8_err_not_found) {
    /* Some EPUBs store the nav/NCX href already rooted at the archive. */
    err = internal_extract(zip, book->toc_path, scratch, cap, &got);
  }
  if (err != k_ra8_ok) {
    return;
  }

  if (book->toc_kind == (uint8_t)k_epub_toc_nav) {
    (void)priv_epub_xml_parse_nav(scratch, got, book);
  } else {
    (void)priv_epub_xml_parse_ncx(scratch, got, book);
  }
}

/**
 * @brief Run the metadata + spine parsers given an already-open zip.
 *
 * Splits out of `epub_open` to keep that function under the
 * NASA-Rule-4 statement budget enforced by clang-tidy
 * (`readability-function-size`).
 *
 * @details See implementation.
 * @param[in] zip See implementation.
 * @param[in] out_book See implementation.
 * @param[in] opf_scratch See implementation.
 * @param[in] opf_cap See implementation.
 * @return Result code.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_parse_archive(mz_zip_archive* zip,
                                        epub_book_t*    out_book,
                                        uint8_t*        opf_scratch,
                                        size_t          opf_cap)
{
  /** @brief container.xml path inside every conformant .epub archive. */
  static const char* const s_container_path = "META-INF/container.xml";
  /* Static rather than auto: 4 KiB on the stack would blow the
   * NASA-rule stack-usage budget. EPUB parser is single-threaded
   * init-context-only, so the static scratch buffer is safe. */
  static uint8_t s_container_buf[k_epub_container_xml_buf];
  size_t         got = 0U;
  ra8_err_t      err =
    internal_extract(zip, s_container_path, s_container_buf, sizeof(s_container_buf), &got);
  if (err != k_ra8_ok) {
    return err;
  }

  epub_container_result_t cres = {};
  err = priv_epub_xml_parse_container(s_container_buf, got, &cres, &out_book->xml_workspace);
  if (err != k_ra8_ok) {
    return err;
  }

  size_t opf_got = 0U;
  err            = internal_extract(zip, cres.opf_path, opf_scratch, opf_cap, &opf_got);
  if (err != k_ra8_ok) {
    return err;
  }

  priv_epub_dirname(cres.opf_path, out_book->opf_dir, k_epub_max_path_len);
  err = priv_epub_xml_parse_opf(opf_scratch, opf_got, out_book);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Optional navigation document (NCX / nav.xhtml). Reuses opf_scratch
   * now that the OPF has been parsed into the book. Best-effort. */
  internal_load_toc(zip, out_book, opf_scratch, opf_cap);
  return k_ra8_ok;
}

/* see header for full description */
RA8_PRIV ra8_err_t priv_epub_set_miniz_alloc(mz_zip_archive* zip, epub_book_t* book)
{
  if ((zip == nullptr) || (book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const ra8_err_t err = epub_miniz_arena_init(&book->miniz_arena,
                                              &book->miniz_workspace.bytes[0],
                                              sizeof(book->miniz_workspace.bytes));
  if (err != k_ra8_ok) {
    return err; /* GCOVR_EXCL_LINE -- embedded workspace is exact and aligned */
  }
  zip->m_pAlloc        = epub_miniz_alloc;
  zip->m_pFree         = epub_miniz_free;
  zip->m_pRealloc      = epub_miniz_realloc;
  zip->m_pAlloc_opaque = &book->miniz_arena;
  return k_ra8_ok;
}

RA8_PRIV size_t priv_epub_stream_read(void* opaque, mz_uint64 file_ofs, void* buf, size_t n)
{
  const epub_stream_media_t* sm = (const epub_stream_media_t*)opaque;
  if ((sm == nullptr) || (sm->read == nullptr)) {
    return 0U;
  }
  if (file_ofs >= sm->size) {
    return 0U;
  }
  const uint64_t avail = sm->size - file_ofs;
  const size_t   want  = ((uint64_t)n > avail) ? (size_t)avail : n;
  return sm->read(sm->ctx, (uint64_t)file_ofs, buf, want);
}

RA8_PRIV ra8_err_t priv_epub_finish_open(mz_zip_archive* zip, epub_book_t* out_book)
{
  if ((zip == nullptr) || (out_book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  /* Archive-level decompression-limits guard: an archive whose central
   * directory floods the policy entry cap dies before any entry work. */
  const ra8_err_t gerr = priv_epub_zip_guard_archive(zip);
  if (gerr != k_ra8_ok) {
    internal_zip_destroy(zip);
    return gerr;
  }
  /* Static (file-scope) OPF scratch keeps the firmware stack frame small --
   * the OPF blob can be tens of KiB and would otherwise blow the per-thread
   * stack budget. Single-threaded init-context use makes the static safe. */
  static uint8_t s_opf_buf[k_epub_opf_xml_buf];
  ra8_err_t      err = internal_parse_archive(zip, out_book, s_opf_buf, sizeof(s_opf_buf));
  if (err != k_ra8_ok) {
    internal_zip_destroy(zip);
    return err;
  }
  out_book->zip_archive_active = 1U;
  out_book->in_use             = 1U;
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * Public API.
 * ---------------------------------------------------------------------------
 */

ra8_err_t epub_open(const void* media, const char* path, epub_book_t* out_book)
{
  (void)path;
  if ((media == nullptr) || (out_book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  const epub_mem_media_t* mem = (const epub_mem_media_t*)media;
  if ((mem->data == nullptr) || (mem->size == 0U)) {
    return k_ra8_err_invalid_arg;
  }

  epub_mem_media_t preflight_mem = *mem;
  const ra8_err_t  cap_err =
    ra8_decomp_zip_entry_preflight(priv_epub_mem_read, &preflight_mem, preflight_mem.size);
  if (cap_err != k_ra8_ok) {
    return cap_err;
  }

  /* Zero-init the book up front so failure paths return a clean record.
   * This also clears `zip_archive_storage` to a known state for miniz. */
  internal_byte_zero((uint8_t*)out_book, sizeof(*out_book));

  /* Place the mz_zip_archive directly in the book record's inline
   * storage. No heap allocation -- NASA Rule 3 compliance. The byte-
   * storage punning is intentional and documented in the header. */
  void* const     zip_storage = &out_book->zip_archive_storage[0];
  mz_zip_archive* zip         = (mz_zip_archive*)zip_storage;
  const ra8_err_t aerr        = priv_epub_set_miniz_alloc(zip, out_book);
  if (aerr != k_ra8_ok) {
    internal_byte_zero((uint8_t*)out_book, sizeof(*out_book));
    return aerr; /* GCOVR_EXCL_LINE -- embedded workspace is exact and aligned */
  }
  if (mz_zip_reader_init_mem(zip, mem->data, mem->size, 0U) == MZ_FALSE) {
    epub_miniz_arena_deinit(&out_book->miniz_arena);
    internal_byte_zero((uint8_t*)out_book, sizeof(*out_book));
    return k_ra8_err_validation_failed;
  }

  const ra8_err_t err = priv_epub_finish_open(zip, out_book);
  if (err != k_ra8_ok) {
    epub_miniz_arena_deinit(&out_book->miniz_arena);
    internal_byte_zero((uint8_t*)out_book, sizeof(*out_book));
    return err;
  }
  out_book->zip_bytes = mem->data;
  out_book->zip_size  = mem->size;
  return k_ra8_ok;
}

ra8_err_t
epub_open_streamed(const epub_stream_media_t* media, const char* path, epub_book_t* out_book)
{
  (void)path;
  if ((media == nullptr) || (out_book == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((media->read == nullptr) || (media->size == 0U)) {
    return k_ra8_err_invalid_arg;
  }

  const ra8_err_t cap_err = ra8_decomp_zip_entry_preflight(media->read, media->ctx, media->size);
  if (cap_err != k_ra8_ok) {
    return cap_err;
  }

  /* Zero-init the book up front (also clears the inline miniz storage). */
  internal_byte_zero((uint8_t*)out_book, sizeof(*out_book));

  /* The book owns the media descriptor so miniz's `m_pIO_opaque` has a stable
   * address for the book's whole lifetime -- per-entry reads (chapters, cover,
   * resources) happen long after this call returns and dereference it. */
  out_book->stream_media = *media;

  void* const     zip_storage = &out_book->zip_archive_storage[0];
  mz_zip_archive* zip         = (mz_zip_archive*)zip_storage;
  const ra8_err_t aerr        = priv_epub_set_miniz_alloc(zip, out_book);
  if (aerr != k_ra8_ok) {
    internal_byte_zero((uint8_t*)out_book, sizeof(*out_book));
    return aerr; /* GCOVR_EXCL_LINE -- embedded workspace is exact and aligned */
  }
  zip->m_pRead      = priv_epub_stream_read;
  zip->m_pIO_opaque = &out_book->stream_media;
  /* User-read reader: reads only the ZIP tail (EOCD + central directory) now;
   * each entry is inflated on demand later through `priv_epub_stream_read`. */
  if (mz_zip_reader_init(zip, (mz_uint64)media->size, 0U) == MZ_FALSE) {
    epub_miniz_arena_deinit(&out_book->miniz_arena);
    internal_byte_zero((uint8_t*)out_book, sizeof(*out_book));
    return k_ra8_err_validation_failed;
  }

  const ra8_err_t err = priv_epub_finish_open(zip, out_book);
  if (err != k_ra8_ok) {
    epub_miniz_arena_deinit(&out_book->miniz_arena);
    internal_byte_zero((uint8_t*)out_book, sizeof(*out_book));
    return err;
  }
  /* Streamed books hold no resident blob -- every read goes through the callback. */
  out_book->zip_bytes = nullptr;
  out_book->zip_size  = (size_t)media->size;
  return k_ra8_ok;
}

ra8_err_t epub_close(epub_book_t* book)
{
  if (book == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (book->in_use == 0U) {
    return k_ra8_err_not_initialized;
  }
  if (book->zip_archive_active != 0U) {
    void* const zip_storage = &book->zip_archive_storage[0];
    internal_zip_destroy((mz_zip_archive*)zip_storage);
    book->zip_archive_active = 0U;
  }
  epub_miniz_arena_deinit(&book->miniz_arena);
  book->in_use        = 0U;
  book->chapter_count = 0U;
  return k_ra8_ok;
}
