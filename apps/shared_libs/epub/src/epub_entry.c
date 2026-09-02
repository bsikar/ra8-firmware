/**
 * @file epub_entry.c
 * @brief Bounded-RAM streaming + windowed extraction of a single ZIP entry (#231).
 *
 * @details
 * The whole-entry accessors (`epub_load_chapter`, `epub_get_resource`,
 * ...) inflate an entry via `mz_zip_reader_extract_to_mem` into a caller buffer
 * sized to the entry's *uncompressed* size -- fine for small text/CSS/font
 * entries, but a full-page manga scan in an all-image EPUB inflates to tens of
 * megabytes and cannot be held whole on a ~10 MB working-set device.
 *
 * This TU adds two ways to read an entry without materialising it:
 *
 *   - A **forward streaming cursor** (`epub_entry_open/read/close`) over
 *     miniz's `mz_zip_reader_extract_iter_*` iterator: the caller pulls the entry
 *     in fixed-size chunks and the resident inflate state is bounded independent
 *     of the entry's size.
 *   - A **positioned read** (`epub_entry_pread`) for *stored* (uncompressed)
 *     entries, whose bytes lie contiguously in the archive so any window can be
 *     read directly off the backing -- the random-access primitive the tile
 *     source uses to page a single tile.
 *
 * Both work for a resident book (`epub_open`) and a streamed book
 * (`epub_open_streamed`); in the streamed case every compressed / stored byte
 * is fetched on demand through the book's seek+read backing.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

#include "epub_entry.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "epub.h"
#include "epub_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "epub_entry";

/* ---------------------------------------------------------------------------
 * ZIP local-file-header field layout (PKWARE APPNOTE.TXT s4.3.7). These are the
 * frozen on-disk ZIP format offsets; miniz keeps its equivalents private to
 * miniz.c, so they are re-declared here rather than reached into the SOUP.
 * ---------------------------------------------------------------------------
 */

/**
 * @enum epub_ldh_t
 * @brief Local-file-header size + the two variable-length field-length offsets.
 *
 * @details A ZIP local file header is a fixed 30-byte record followed by the
 *          file name and an extra field; the entry's stored/compressed data
 *          begins right after them. `epub_entry_pread()` reads the 30-byte
 *          header to learn those two lengths and thereby the data offset.
 */
typedef enum : uint8_t {
  k_epub_ldh_size          = 30U, /**< Fixed local-header length, bytes.       */
  k_epub_ldh_fname_len_ofs = 26U, /**< uint16 file-name length field offset.   */
  k_epub_ldh_extra_len_ofs = 28U, /**< uint16 extra-field length field offset. */
} epub_ldh_t;

/**
 * @enum epub_ldh_sig_t
 * @brief Local-file-header signature bytes (little-endian 0x04034b50).
 */
typedef enum : uint8_t {
  k_epub_ldh_sig_0 = 0x50U, /**< Signature byte 0 ('P'). */
  k_epub_ldh_sig_1 = 0x4BU, /**< Signature byte 1 ('K'). */
  k_epub_ldh_sig_2 = 0x03U, /**< Signature byte 2.       */
  k_epub_ldh_sig_3 = 0x04U, /**< Signature byte 3.       */
} epub_ldh_sig_t;

/**
 * @enum epub_le_t
 * @brief Little-endian byte shift + index constants for the header fields.
 */
typedef enum : uint8_t {
  k_epub_le_lo    = 0U, /**< Low-order byte index.  */
  k_epub_le_hi    = 1U, /**< High-order byte index. */
  k_epub_le_shift = 8U, /**< High-byte left shift.  */
} epub_le_t;

/* ---------------------------------------------------------------------------
 * Helpers.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Borrow the book's inline `mz_zip_archive`.
 * @details See implementation.
 * @param[in] book Open book owning the inline archive storage.
 * @return Pointer to the archive (never NULL for a non-NULL @p book).
 * @pre @p book is non-NULL.
 * @pre @p book was opened (archive storage initialised).
 * @post No state mutated.
 * @post The returned pointer aliases @p book storage.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static mz_zip_archive* internal_zip(epub_book_t* book)
{
  void* const storage = &book->zip_archive_storage[0];
  return (mz_zip_archive*)storage;
}

/**
 * @brief Locate an entry by OPF-prefixed path, falling back to the bare path.
 * @details See implementation. Mirrors `internal_locate_extract()` in the chapter TU.
 * @param[in]  zip      Open archive.
 * @param[in]  book     Book supplying `opf_dir` for the prefixed attempt.
 * @param[in]  path     Entry path, OPF-relative or archive-rooted.
 * @param[out] out_idx  Receives the located file index on success.
 * @return Result code.
 * @retval k_ra8_ok            Entry located; `*out_idx` set.
 * @retval k_ra8_err_not_found Neither the prefixed nor the bare path exists.
 * @pre @p zip, @p book, @p path, @p out_idx are non-NULL.
 * @pre The archive is active.
 * @post On success `*out_idx >= 0`.
 * @post On failure `*out_idx` is unmodified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_locate(mz_zip_archive* zip, epub_book_t* book, const char* path, int32_t* out_idx)
{
  char full_path[k_epub_max_path_len];
  priv_epub_join_path(book->opf_dir, path, full_path, sizeof(full_path));
  int32_t idx = mz_zip_reader_locate_file(zip, full_path, nullptr, 0U);
  if (idx < 0) {
    idx = mz_zip_reader_locate_file(zip, path, nullptr, 0U);
  }
  if (idx < 0) {
    return k_ra8_err_not_found;
  }
  *out_idx = idx;
  return k_ra8_ok;
}

/**
 * @brief Read @p n absolute archive bytes off the book's backing (resident or streamed).
 * @details See implementation. Resident books memcpy from the resident blob; a
 *          streamed book pulls the window through its seek+read callback.
 * @param[in]  book       Open book.
 * @param[in]  archive_ofs Absolute byte offset within the archive.
 * @param[out] buf        Destination (`n` writable bytes).
 * @param[in]  n          Bytes to read.
 * @return true iff exactly @p n bytes were read.
 * @retval true  Exactly @p n bytes were copied into @p buf.
 * @retval false Short read, out-of-range offset, or no read backend.
 * @pre @p book, @p buf are non-NULL.
 * @pre @p n > 0.
 * @post On true, @p buf holds the requested window.
 * @post On false, @p buf contents are unspecified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool internal_backing_read(epub_book_t* book, uint64_t archive_ofs, uint8_t* buf, size_t n)
{
  if (book->zip_bytes != nullptr) {
    if ((archive_ofs + (uint64_t)n) > (uint64_t)book->zip_size) {
      return false; /* GCOVR_EXCL_LINE -- offsets come from a validated central-directory entry */
    }
    (void)memcpy(buf, &book->zip_bytes[(size_t)archive_ofs], n);
    return true;
  }
  if (book->stream_media.read == nullptr) {
    return false; /* GCOVR_EXCL_LINE -- a streamed book always carries a validated read callback */
  }
  const size_t got = book->stream_media.read(book->stream_media.ctx, archive_ofs, buf, n);
  return got == n;
}

/**
 * @brief Verify a 30-byte local header and return the entry's data offset.
 * @details See implementation. Checks the local-header signature, then adds the
 *          fixed header size plus the file-name and extra-field lengths.
 * @param[in]  hdr             30-byte local file header.
 * @param[in]  local_header_ofs Archive offset the header was read from.
 * @param[out] out_data_ofs    Receives the entry's stored-data archive offset.
 * @return true iff the signature is valid.
 * @retval true  Local-header signature matched; `*out_data_ofs` is set.
 * @retval false Signature mismatch; `*out_data_ofs` is left unmodified.
 * @pre @p hdr holds ::k_epub_ldh_size readable bytes.
 * @pre @p out_data_ofs is non-NULL.
 * @post On true `*out_data_ofs` is the stored-data offset.
 * @post On false `*out_data_ofs` is unmodified.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static bool
internal_data_offset(const uint8_t* hdr, uint64_t local_header_ofs, uint64_t* out_data_ofs)
{
  const uint8_t sig[4] = {(uint8_t)k_epub_ldh_sig_0,
                          (uint8_t)k_epub_ldh_sig_1,
                          (uint8_t)k_epub_ldh_sig_2,
                          (uint8_t)k_epub_ldh_sig_3};
  if (memcmp(hdr, sig, sizeof(sig)) != 0) {
    return false;
  }
  const uint32_t fname =
    (uint32_t)hdr[(uint8_t)k_epub_ldh_fname_len_ofs + k_epub_le_lo] |
    ((uint32_t)hdr[(uint8_t)k_epub_ldh_fname_len_ofs + k_epub_le_hi] << k_epub_le_shift);
  const uint32_t extra =
    (uint32_t)hdr[(uint8_t)k_epub_ldh_extra_len_ofs + k_epub_le_lo] |
    ((uint32_t)hdr[(uint8_t)k_epub_ldh_extra_len_ofs + k_epub_le_hi] << k_epub_le_shift);
  *out_data_ofs = local_header_ofs + (uint64_t)k_epub_ldh_size + (uint64_t)fname + (uint64_t)extra;
  return true;
}

/**
 * @brief Locate + stat an entry and start a miniz extract-iterator over it.
 * @details See implementation. Split out so `epub_entry_open` stays under the
 *          NASA-Rule-4 statement budget.
 * @param[in]  book       Open book.
 * @param[in]  path       Entry path (OPF-relative or archive-rooted).
 * @param[out] out_iter   Receives the started iterator on success.
 * @param[out] out_uncomp Receives the entry's uncompressed size.
 * @return Result code.
 * @retval k_ra8_ok                    Iterator started; outputs set.
 * @retval k_ra8_err_not_found         No entry at @p path.
 * @retval k_ra8_err_validation_failed Stat / iterator start failed.
 * @pre @p book is open; @p out_iter and @p out_uncomp are non-NULL.
 * @pre The archive is active.
 * @post On success `*out_iter != NULL`.
 * @post On failure no iterator leaks.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_open_iter(epub_book_t*                       book,
                                    const char*                        path,
                                    mz_zip_reader_extract_iter_state** out_iter,
                                    uint64_t*                          out_uncomp)
{
  mz_zip_archive* zip = internal_zip(book);
  int32_t         idx = 0;
  const ra8_err_t err = internal_locate(zip, book, path, &idx);
  if (err != k_ra8_ok) {
    return err;
  }
  mz_zip_archive_file_stat st;
  if (mz_zip_reader_file_stat(zip, (mz_uint)idx, &st) == MZ_FALSE) {
    return k_ra8_err_validation_failed; /* GCOVR_EXCL_LINE -- stat cannot fail on a located entry */
  }
  const ra8_err_t gerr = priv_epub_zip_guard_entry(&st);
  if (gerr != k_ra8_ok) {
    return gerr; /* lying header / declared bomb: reject before inflation */
  }
  mz_zip_reader_extract_iter_state* iter = mz_zip_reader_extract_iter_new(zip, (mz_uint)idx, 0U);
  if (iter == nullptr) {
    return k_ra8_err_validation_failed;
  }
  *out_iter   = iter;
  *out_uncomp = (uint64_t)st.m_uncomp_size;
  return k_ra8_ok;
}

/**
 * @brief Resolve a stored entry's archive data offset + uncompressed size.
 * @details See implementation. Split out so `epub_entry_pread` stays under the
 *          NASA-Rule-4 statement budget. Rejects DEFLATE entries.
 * @param[in]  book         Open book.
 * @param[in]  path         Entry path (OPF-relative or archive-rooted).
 * @param[out] out_data_ofs Receives the entry's stored-data archive offset.
 * @param[out] out_uncomp   Receives the entry's uncompressed size.
 * @return Result code.
 * @retval k_ra8_ok                    Offset + size resolved.
 * @retval k_ra8_err_not_found         No entry at @p path.
 * @retval k_ra8_err_not_supported     The entry is DEFLATE-compressed.
 * @retval k_ra8_err_validation_failed Stat / local-header read failed.
 * @pre @p book is open; @p out_data_ofs and @p out_uncomp are non-NULL.
 * @pre The archive is active.
 * @post On success `*out_data_ofs` addresses the entry's stored bytes.
 * @post On failure the outputs are unspecified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_stored_data_offset(epub_book_t* book,
                                             const char*  path,
                                             uint64_t*    out_data_ofs,
                                             uint64_t*    out_uncomp)
{
  mz_zip_archive* zip = internal_zip(book);
  int32_t         idx = 0;
  const ra8_err_t err = internal_locate(zip, book, path, &idx);
  if (err != k_ra8_ok) {
    return err;
  }
  mz_zip_archive_file_stat st;
  if (mz_zip_reader_file_stat(zip, (mz_uint)idx, &st) == MZ_FALSE) {
    return k_ra8_err_validation_failed; /* GCOVR_EXCL_LINE -- stat cannot fail on a located entry */
  }
  const ra8_err_t gerr = priv_epub_zip_guard_entry(&st);
  if (gerr != k_ra8_ok) {
    return gerr; /* lying header / declared bomb: reject before the copy */
  }
  if (st.m_method != 0U) {
    return k_ra8_err_not_supported; /* DEFLATE -> use the forward cursor */
  }
  uint8_t hdr[k_epub_ldh_size] = {};
  if (!internal_backing_read(book, (uint64_t)st.m_local_header_ofs, hdr, sizeof(hdr))) {
    return k_ra8_err_validation_failed; /* GCOVR_EXCL_LINE -- header bytes are within the archive */
  }
  if (!internal_data_offset(hdr, (uint64_t)st.m_local_header_ofs, out_data_ofs)) {
    return k_ra8_err_validation_failed;
  }
  *out_uncomp = (uint64_t)st.m_uncomp_size;
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * Public API.
 * ---------------------------------------------------------------------------
 */

ra8_err_t epub_entry_open(epub_book_t*         book,
                          const char*          path,
                          epub_entry_reader_t* out_reader,
                          uint64_t*            out_size)
{
  RA8_CHECK_NULL_PTR(book, s_tag, "book must not be nullptr");
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(out_reader, s_tag, "out_reader must not be nullptr");
  (void)memset(out_reader, 0, sizeof(*out_reader));
  if (priv_epub_book_not_ready(book->in_use, book->zip_archive_active)) {
    return k_ra8_err_not_initialized;
  }

  mz_zip_reader_extract_iter_state* iter   = nullptr;
  uint64_t                          uncomp = 0U;
  const ra8_err_t                   err    = internal_open_iter(book, path, &iter, &uncomp);
  if (err != k_ra8_ok) {
    return err;
  }
  out_reader->iter  = iter;
  out_reader->book  = book;
  out_reader->total = uncomp;
  out_reader->done  = (uncomp == 0U) ? 1U : 0U;
  if (out_size != nullptr) {
    *out_size = uncomp;
  }
  return k_ra8_ok;
}

ra8_err_t epub_entry_read(epub_entry_reader_t* reader, uint8_t* buf, size_t cap, size_t* got)
{
  RA8_CHECK_NULL_PTR(reader, s_tag, "reader must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  RA8_CHECK_NULL_PTR(got, s_tag, "got must not be nullptr");
  *got = 0U;
  if (reader->iter == nullptr) {
    return k_ra8_err_not_initialized;
  }
  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (reader->done != 0U) {
    return k_ra8_ok; /* clean EOF already reported; *got stays 0 */
  }

  mz_zip_reader_extract_iter_state* iter = (mz_zip_reader_extract_iter_state*)reader->iter;
  const size_t                      n    = mz_zip_reader_extract_iter_read(iter, buf, cap);
  reader->consumed += (uint64_t)n;
  *got = n;
  if (n < cap) {
    /* A short read is end-of-entry only if every byte arrived; otherwise the
     * compressed stream ended early / failed to inflate. */
    if (reader->consumed >= reader->total) {
      reader->done = 1U;
      return k_ra8_ok;
    }
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

ra8_err_t epub_entry_close(epub_entry_reader_t* reader)
{
  RA8_CHECK_NULL_PTR(reader, s_tag, "reader must not be nullptr");
  if (reader->iter == nullptr) {
    return k_ra8_ok; /* idempotent: already closed / never opened */
  }
  mz_zip_reader_extract_iter_state* iter = (mz_zip_reader_extract_iter_state*)reader->iter;
  bool                              full = false;
  if (reader->total > 0U) {
    if (reader->consumed >= reader->total) {
      full = true;
    }
  }
  const mz_bool ok = mz_zip_reader_extract_iter_free(iter);
  reader->iter     = nullptr;
  if (full) {
    if (ok == MZ_FALSE) {
      return k_ra8_err_validation_failed;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Reject any NULL `epub_entry_pread` pointer argument.
 * @details See implementation. Split out so `epub_entry_pread` stays under the
 *          NASA-Rule-4 statement budget.
 * @param[in] book Book handle.
 * @param[in] path Entry path.
 * @param[in] buf  Destination buffer.
 * @param[in] got  Output count.
 * @return Result code.
 * @retval k_ra8_ok           All four pointers are non-NULL.
 * @retval k_ra8_err_null_ptr Some pointer is NULL.
 * @pre The caller forwards its own arguments.
 * @pre No pointer is dereferenced here.
 * @post No state mutated.
 * @post Return depends solely on the inputs.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_pread_null_ok(const epub_book_t* book,
                                        const char*        path,
                                        const uint8_t*     buf,
                                        const size_t*      got)
{
  RA8_CHECK_NULL_PTR(book, s_tag, "book must not be nullptr");
  RA8_CHECK_NULL_PTR(path, s_tag, "path must not be nullptr");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  RA8_CHECK_NULL_PTR(got, s_tag, "got must not be nullptr");
  return k_ra8_ok;
}

ra8_err_t epub_entry_pread(epub_book_t* book,
                           const char*  path,
                           uint64_t     offset,
                           uint8_t*     buf,
                           size_t       len,
                           size_t*      got)
{
  const ra8_err_t nz = internal_pread_null_ok(book, path, buf, got);
  if (nz != k_ra8_ok) {
    return nz;
  }
  *got = 0U;
  if (priv_epub_book_not_ready(book->in_use, book->zip_archive_active)) {
    return k_ra8_err_not_initialized;
  }

  uint64_t        data_ofs = 0U;
  uint64_t        uncomp   = 0U;
  const ra8_err_t err      = internal_stored_data_offset(book, path, &data_ofs, &uncomp);
  if (err != k_ra8_ok) {
    return err;
  }
  if (offset >= uncomp) {
    return k_ra8_ok; /* window starts at/after EOF; *got stays 0 */
  }
  const uint64_t avail = uncomp - offset;
  const size_t   n     = ((uint64_t)len > avail) ? (size_t)avail : len;
  if (!internal_backing_read(book, data_ofs + offset, buf, n)) {
    return k_ra8_err_validation_failed; /* GCOVR_EXCL_LINE -- data window is within the archive */
  }
  *got = n;
  return k_ra8_ok;
}
