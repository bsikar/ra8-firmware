/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_comic_cbz.c
 * @brief CBZ backend: a streaming miniz ZIP reader that indexes page images.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The ZIP half of the comic facade. It drives miniz's user-read reader
 * (`mz_zip_reader_init`) off the comic's seek+read backing -- the same streaming
 * path `ra8_epub_open_streamed` uses -- so only the central directory and one
 * entry at a time are ever fetched. ::ra8_comic_cbz_open walks the central
 * directory, filters entries to decodable page images (skipping directories,
 * hidden files and AppleDouble forks), and appends each to the shared page index.
 * ::ra8_comic_cbz_extract inflates one page's encoded image (STORE or DEFLATE)
 * on demand.
 *
 * On firmware, miniz's central-directory and decompressor allocations route
 * through the zero-heap `ra8_epub_miniz_alloc` static pool (NASA Rule 3), exactly
 * as `ra8_epub` does; the host unit-test build (`RA8_OFF_TARGET`) leaves miniz
 * on its default allocator.
 *
 * @since Version 0.1.0
 */
#include <stddef.h>
#include <string.h>

#include "miniz.h"
#include "ra8_check.h"
#include "ra8_comic.h"
#include "ra8_comic_internal.h"
#include "ra8_decomp_limits.h"

#ifndef RA8_OFF_TARGET
#include "ra8_attributes.h"
#include "ra8_epub_miniz_alloc.h"
#endif

/** @brief Log tag for CBZ-backend diagnostics. */
static const char* const s_tag_cbz = "ra8_comic_cbz";

/* The comic record holds the mz_zip_archive inline (no heap); prove the opaque
 * byte buffer is large enough and aligned for miniz's reader state. */
static_assert(k_ra8_comic_zip_bytes >= sizeof(mz_zip_archive),
              "k_ra8_comic_zip_bytes too small for mz_zip_archive");
static_assert(alignof(mz_zip_archive) <= alignof(max_align_t),
              "mz_zip_archive alignment exceeds max_align_t");

/**
 * @enum cbz_limits_t
 * @brief Per-entry name scratch size for the central-directory walk.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_cbz_name_max = 256U, /**< Longest entry name indexed (bytes, incl. path). */
} cbz_limits_t;

/**
 * @brief The comic's inline `mz_zip_archive` view.
 * @param[in] c Comic carrying the inline storage.
 * @return Typed pointer to the archive within `c->zip_storage`.
 * @pre @p c is non-NULL and its storage is aligned for `mz_zip_archive`.
 * @pre The storage is zeroed before `mz_zip_reader_init`.
 * @post No state is modified (pure cast).
 * @post The result aliases `c->zip_storage`.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static mz_zip_archive* s_zip(ra8_comic_t* c)
{
  /* Reinterpret the aligned byte arena as the archive via a `void*` seam (as
   * `ra8_epub` does): a direct `uint8_t* -> mz_zip_archive*` cast would trip
   * -Wcast-align, and a one-expression `(T*)(void*)p` cast trips
   * bugprone-casting-through-void, so the `void*` is a named local. */
  void* const storage = &c->zip_storage[0];
  return (mz_zip_archive*)storage;
}

/**
 * @brief miniz user-read trampoline -- forward to the comic's seek+read backing.
 * @details Bound to `mz_zip_archive::m_pRead`; miniz clamps every request to the
 *          archive size, so this only defends a caller `size` shorter than the
 *          real archive (a short read aborts the miniz read).
 * @param[in]  opaque   The comic's ::ra8_comic_stream_t (via `m_pIO_opaque`).
 * @param[in]  file_ofs Absolute byte offset within the archive.
 * @param[out] buf      Destination buffer (@p n writable bytes).
 * @param[in]  n        Bytes requested by miniz.
 * @return Bytes actually read (0 at/after EOF; `< n` aborts the read).
 * @retval 0 A NULL/invalid stream, an offset at/after EOF, or a backing read of 0.
 * @pre @p opaque is the comic's stream descriptor.
 * @pre @p buf is writable for @p n bytes.
 * @post No state outside @p buf is modified.
 * @post At most `min(n, size - file_ofs)` bytes are requested.
 * @note Not thread-safe; single-threaded reader context.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static size_t s_stream_read(void* opaque, mz_uint64 file_ofs, void* buf, size_t n)
{
  const ra8_comic_stream_t* sm = (const ra8_comic_stream_t*)opaque;
  if (sm == nullptr) {
    return 0U; /* GCOVR_EXCL_LINE -- open binds a live stream; miniz never passes NULL */
  }
  if (sm->read == nullptr) {
    return 0U; /* GCOVR_EXCL_LINE -- open validates a non-NULL reader */
  }
  if (file_ofs >= sm->size) {
    return 0U; /* GCOVR_EXCL_LINE -- miniz clamps reads within the archive size */
  }
  const uint64_t avail = sm->size - file_ofs;
  const size_t   want  = ((uint64_t)n > avail) ? (size_t)avail : n;
  return sm->read(sm->ctx, (uint64_t)file_ofs, buf, want);
}

/**
 * @brief Route miniz allocations through the static pool on firmware.
 * @details Firmware (`_sbrk` traps, NASA Rule 3) needs miniz's central-directory
 *          and inflate allocations to come from the `ra8_epub_miniz_alloc` pool;
 *          the host build (`RA8_OFF_TARGET`) leaves the default allocator.
 * @param[in,out] zip Inline archive to configure (must be zeroed first).
 * @pre @p zip addresses the comic's zeroed inline storage.
 * @pre Called before `mz_zip_reader_init`.
 * @post On firmware the archive uses the static miniz pool.
 * @post No I/O is performed.
 * @note Not thread-safe; single-threaded init context.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void s_set_alloc(mz_zip_archive* zip)
{
#ifndef RA8_OFF_TARGET
  zip->m_pAlloc        = ra8_epub_miniz_alloc;
  zip->m_pFree         = ra8_epub_miniz_free;
  zip->m_pRealloc      = ra8_epub_miniz_realloc;
  zip->m_pAlloc_opaque = nullptr;
#else
  (void)zip;
#endif
}

/**
 * @brief Index one central-directory entry if it is a decodable page image.
 * @details Skips directories, non-image / hidden names, and unreadable entries;
 *          otherwise appends the entry (with its uncompressed image length and
 *          miniz index) to the shared page index.
 * @param[in,out] c   Comic accumulating its index.
 * @param[in]     zip Initialised miniz reader.
 * @param[in]     i   Central-directory index.
 * @return ra8_err_t status (k_ra8_ok also for a skipped, non-page entry).
 * @retval k_ra8_ok               Entry indexed or intentionally skipped.
 * @retval k_ra8_err_invalid_size Page index / name arena is full.
 * @pre @p zip was initialised; @p i is a valid entry index.
 * @pre @p c has its buffers bound.
 * @post On k_ra8_ok either one page was added or nothing changed (a skip).
 * @post On error the index is unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_add_entry(ra8_comic_t* c, mz_zip_archive* zip, mz_uint i)
{
  if (mz_zip_reader_is_file_a_directory(zip, i) != MZ_FALSE) {
    return k_ra8_ok;
  }
  char nb[k_cbz_name_max] = {};
  (void)mz_zip_reader_get_filename(zip, i, nb, (mz_uint)sizeof(nb));
  nb[sizeof(nb) - 1U]  = '\0';
  const void* const z  = memchr(nb, '\0', sizeof(nb));
  const size_t      nl = (z != nullptr) ? (size_t)((const char*)z - nb) : sizeof(nb);
  if (!ra8_comic_is_page_name(nb, (uint16_t)nl)) {
    return k_ra8_ok;
  }
  mz_zip_archive_file_stat st = {};
  if (mz_zip_reader_file_stat(zip, i, &st) == MZ_FALSE) {
    return k_ra8_ok; /* GCOVR_EXCL_LINE -- stat cannot fail on an index locate already found */
  }
  /* Decompression-limits guard: a page whose central-directory record
   * declares an over-cap or bomb-ratio size rejects the whole archive
   * before any inflation (the same policy every decoder enforces). */
  const ra8_decomp_limits_t lim = ra8_decomp_limits_default();
  const ra8_err_t           derr =
    ra8_decomp_check_declared(&lim, (uint64_t)st.m_comp_size, (uint64_t)st.m_uncomp_size);
  if (derr != k_ra8_ok) {
    return derr;
  }
  return ra8_comic_page_add(c,
                            nb,
                            (uint16_t)nl,
                            (uint64_t)st.m_uncomp_size,
                            1U,
                            (uint8_t)k_ra8_rar_method_store, /* method unused by CBZ */
                            (uint32_t)i,
                            0U,
                            0U);
}

ra8_err_t ra8_comic_cbz_open(ra8_comic_t* c)
{
  RA8_CHECK_NULL_PTR(c, s_tag_cbz, "cbz open: null c");
  mz_zip_archive* zip = s_zip(c);
  (void)memset(zip, 0, sizeof(*zip));
  s_set_alloc(zip);
  zip->m_pRead      = s_stream_read;
  zip->m_pIO_opaque = &c->stream;
  if (mz_zip_reader_init(zip, (mz_uint64)c->size, 0U) == MZ_FALSE) {
    return k_ra8_err_validation_failed;
  }
  c->zip_active     = 1U;
  const mz_uint num = mz_zip_reader_get_num_files(zip);
  /* Decompression-limits guard: the many-tiny-entries bomb dies before
   * any central-directory entry is touched. */
  const ra8_decomp_limits_t lim = ra8_decomp_limits_default();
  if ((uint64_t)num > (uint64_t)lim.max_entries) {
    (void)ra8_comic_cbz_close(c);
    return k_ra8_err_decomp_entries;
  }
  for (mz_uint i = 0U; i < num; ++i) { /* bound: finite central-directory count */
    const ra8_err_t e = s_add_entry(c, zip, i);
    if (e != k_ra8_ok) {
      (void)ra8_comic_cbz_close(c);
      return e;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_comic_cbz_extract(ra8_comic_t*            c,
                                const ra8_comic_page_t* p,
                                uint8_t*                buf,
                                size_t                  cap,
                                size_t*                 got)
{
  RA8_CHECK_NULL_PTR(c, s_tag_cbz, "cbz extract: null c");
  RA8_CHECK_NULL_PTR(p, s_tag_cbz, "cbz extract: null p");
  RA8_CHECK_NULL_PTR(buf, s_tag_cbz, "cbz extract: null buf");
  RA8_CHECK_NULL_PTR(got, s_tag_cbz, "cbz extract: null got");
  *got = 0U;
  if (c->zip_active == 0U) {
    return k_ra8_err_invalid_state;
  }
  if ((uint64_t)cap < p->raw_size) {
    return k_ra8_err_no_mem;
  }
  mz_zip_archive* zip = s_zip(c);
  if (mz_zip_reader_extract_to_mem(zip, (mz_uint)p->zip_index, buf, cap, 0U) == MZ_FALSE) {
    return k_ra8_err_validation_failed;
  }
  *got = (size_t)p->raw_size;
  return k_ra8_ok;
}

ra8_err_t ra8_comic_cbz_close(ra8_comic_t* c)
{
  RA8_CHECK_NULL_PTR(c, s_tag_cbz, "cbz close: null c");
  if (c->zip_active != 0U) {
    (void)mz_zip_reader_end(s_zip(c));
    c->zip_active = 0U;
  }
  return k_ra8_ok;
}
