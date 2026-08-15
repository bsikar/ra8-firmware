/**
 * @file ra8_comic.c
 * @brief Facade for the comic-archive reader: magic detect, page index, dispatch.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The container-agnostic half of `ra8_comic.h`. ::ra8_comic_open reads the leading
 * magic, routes a ZIP to the CBZ backend, a RAR to the CBR backend, or a tar
 * (probed by header block -- tar has no leading magic) to the CBT backend, and lets
 * each backend append its image members to the shared page index through
 * ::ra8_comic_page_add; the index is then sorted by entry name so reading order is
 * the sorted names. ::ra8_comic_page_read / ::ra8_comic_page_info / ::ra8_comic_close
 * dispatch on the detected kind.
 *
 * Every decision here is a single condition on purpose, so the facade carries no
 * MC/DC obligation of its own.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#include "ra8_comic.h"

#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_comic_internal.h"

/** @brief Log tag for comic-facade diagnostics. */
static const char* const s_tag_comic = "ra8_comic";

/**
 * @enum comic_magic_t
 * @brief ZIP magic bytes and the minimum magic-read length.
 * @details The ZIP local-file-header and empty-archive (EOCD) signatures; a RAR
 *          is delegated to ::ra8_rar_open, which owns its own signature match.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_zip_b0        = 0x50U, /**< 'P'.                                  */
  k_zip_b1        = 0x4BU, /**< 'K'.                                  */
  k_zip_lfh_b2    = 0x03U, /**< Local-file-header 3rd byte.           */
  k_zip_lfh_b3    = 0x04U, /**< Local-file-header 4th byte.           */
  k_zip_eocd_b2   = 0x05U, /**< Empty-archive EOCD 3rd byte.          */
  k_zip_eocd_b3   = 0x06U, /**< Empty-archive EOCD 4th byte.          */
  k_comic_sig_min = 4U,    /**< Bytes required to test the ZIP magic. */
} comic_magic_t;

/**
 * @enum comic_ascii_t
 * @brief Constants for branchless ASCII case folding.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ascii_upper_a = 0x41U, /**< 'A'.                                   */
  k_ascii_alpha_n = 26U,   /**< Letters in the alphabet.               */
  k_ascii_case_sh = 5U,    /**< 0x20 == 1 << 5 (upper->lower bit).     */
  k_comic_ext_max = 8U,    /**< Longest extension the filter compares. */
} comic_ascii_t;

/**
 * @brief ASCII-lowercase one byte, branchlessly (letters only).
 * @details Sets the case bit only for 'A'..'Z'; the unsigned-wrap range test is a
 *          single condition, so no other byte is altered.
 * @param[in] c Byte to fold.
 * @return @p c with the case bit set if it was an uppercase letter.
 * @retval c For any non-letter byte (unchanged).
 * @pre None.
 * @pre None.
 * @post No state is modified (pure function).
 * @post Only 'A'..'Z' are altered.
 * @note Thread-safe: pure.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint8_t internal_lower(uint8_t c)
{
  const uint8_t up =
    (uint8_t)(((uint8_t)(c - (uint8_t)k_ascii_upper_a) < (uint8_t)k_ascii_alpha_n) ? 1U : 0U);
  return (uint8_t)(c | (uint8_t)(up << (uint8_t)k_ascii_case_sh));
}

/**
 * @brief Case-insensitive test that @p name ends with @p ext.
 * @details Compares the tail of @p name against @p ext byte-for-byte through
 *          ::internal_lower. A name shorter than @p ext cannot match.
 * @param[in] name Entry name bytes.
 * @param[in] len  Length of @p name.
 * @param[in] ext  NUL-terminated extension (e.g. ".png"), <= k_comic_ext_max.
 * @return Whether @p name ends with @p ext, case-insensitively.
 * @retval true  The suffix matches.
 * @retval false The suffix differs or @p name is too short.
 * @pre @p name holds @p len readable bytes; @p ext is NUL-terminated.
 * @pre @p ext is at most ::k_comic_ext_max bytes.
 * @post No state is modified (pure read).
 * @post The result depends only on the arguments.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool internal_ends_with_ci(const char* name, uint16_t len, const char* ext)
{
  size_t elen = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_comic_ext_max; ++i) { /* bound: fixed max ext */
    if (ext[i] == '\0') {
      break;
    }
    elen = (size_t)i + 1U;
  }
  if ((size_t)len < elen) {
    return false;
  }
  const char* tail = &name[(size_t)len - elen];
  for (size_t i = 0U; i < elen; ++i) { /* bound: elen <= k_comic_ext_max */
    if (internal_lower((uint8_t)tail[i]) != internal_lower((uint8_t)ext[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Whether @p name ends in a decodable image extension.
 * @details The pure suffix test over the `stb_image` set; the hidden/base-name
 *          policy is applied separately by ::ra8_comic_is_page_name.
 * @param[in] name Entry name bytes.
 * @param[in] len  Length of @p name.
 * @return Whether @p name has a supported image extension.
 * @retval true  A decodable image extension.
 * @retval false Not a decodable image.
 * @pre @p name holds @p len readable bytes.
 * @pre @p len > 0.
 * @post No state is modified (pure read).
 * @post The result depends only on the arguments.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool internal_is_image_ext(const char* name, uint16_t len)
{
  static const char* const k_exts[] = {".jpg", ".jpeg", ".png", ".gif", ".bmp"};
  for (uint8_t i = 0U; i < (uint8_t)(sizeof(k_exts) / sizeof(k_exts[0]));
       ++i) { /* bound: fixed table */
    if (internal_ends_with_ci(name, len, k_exts[i])) {
      return true;
    }
  }
  return false;
}

bool ra8_comic_is_page_name(const char* name, uint16_t len)
{
  if (name == nullptr) {
    return false;
  }
  if (len == 0U) {
    return false;
  }
  size_t base = 0U;
  for (size_t i = 0U; i < (size_t)len; ++i) { /* bound: len */
    if (name[i] == '/') {
      base = i + 1U;
    }
    if (name[i] == '\\') {
      base = i + 1U;
    }
  }
  if (base < (size_t)len) {
    if (name[base] == '.') {
      return false; /* hidden entry or macOS AppleDouble resource fork */
    }
  }
  return internal_is_image_ext(name, len);
}

ra8_err_t ra8_comic_page_add(ra8_comic_t* c,
                             const char*  name,
                             uint16_t     name_len,
                             uint64_t     raw_size,
                             uint8_t      extractable,
                             uint8_t      method,
                             uint32_t     zip_index,
                             uint64_t     data_off,
                             uint64_t     pack_size)
{
  RA8_CHECK_NULL_PTR(c, s_tag_comic, "add: null c");
  RA8_CHECK_NULL_PTR(name, s_tag_comic, "add: null name");
  if (c->page_count >= c->page_cap) {
    return k_ra8_err_invalid_size;
  }
  if (((uint64_t)c->names_len + (uint64_t)name_len) > (uint64_t)c->names_cap) {
    return k_ra8_err_invalid_size;
  }
  ra8_comic_page_t* p = &c->pages[c->page_count];
  *p                  = (ra8_comic_page_t){};
  p->name_off         = c->names_len;
  p->name_len         = name_len;
  p->extractable      = extractable;
  p->rar_method       = method;
  p->zip_index        = zip_index;
  p->data_off         = data_off;
  p->pack_size        = pack_size;
  p->raw_size         = raw_size;
  if (name_len > 0U) {
    (void)memcpy(&c->names[c->names_len], name, (size_t)name_len);
  }
  c->names_len += (uint32_t)name_len;
  c->page_count += 1U;
  return k_ra8_ok;
}

/**
 * @brief Lexicographic compare of two entry names (byte order, length tiebreak).
 * @details Unsigned byte compare over the shorter length, then the shorter name
 *          sorts first -- the total order the page index is sorted by.
 * @param[in] a    First name bytes.
 * @param[in] alen First name length.
 * @param[in] b    Second name bytes.
 * @param[in] blen Second name length.
 * @return Negative, zero, or positive as @p a sorts before, equal to, or after @p b.
 * @retval 0 The names are byte-identical and equal length.
 * @pre @p a / @p b hold @p alen / @p blen readable bytes.
 * @pre Both length arguments are the true name lengths.
 * @post No state is modified (pure read).
 * @post The order is a total order over (bytes, length).
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static int32_t internal_name_cmp(const char* a, uint16_t alen, const char* b, uint16_t blen)
{
  const uint16_t m = (alen < blen) ? alen : blen;
  const int      c = memcmp(a, b, (size_t)m);
  if (c != 0) {
    return (c < 0) ? -1 : 1;
  }
  if (alen < blen) {
    return -1;
  }
  if (alen > blen) {
    return 1;
  }
  return 0;
}

/**
 * @brief Insertion-sort the page index by entry name (reading order).
 * @details Stable insertion sort over the resident index; page-count-bounded, no
 *          recursion. The name arena is immutable, so only the small page records
 *          move.
 * @param[in,out] c Comic whose `pages[0..page_count)` is sorted in place.
 * @pre `c->page_count <= c->page_cap` and the name arena is populated.
 * @pre Every page's `name_off`/`name_len` addresses the arena.
 * @post `pages` is non-decreasing under ::internal_name_cmp.
 * @post `page_count` is unchanged.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static void internal_sort(ra8_comic_t* c)
{
  for (uint32_t i = 1U; i < c->page_count; ++i) { /* bound: page_count <= page_cap */
    const ra8_comic_page_t key = c->pages[i];
    const char* const      kn  = &c->names[key.name_off];
    uint32_t               j   = i;
    while (j > 0U) { /* bound: j strictly decreases toward 0 */
      const ra8_comic_page_t prev = c->pages[j - 1U];
      if (internal_name_cmp(&c->names[prev.name_off], prev.name_len, kn, key.name_len) <= 0) {
        break;
      }
      c->pages[j] = prev;
      j -= 1U;
    }
    c->pages[j] = key;
  }
}

/**
 * @brief Whether the leading bytes are a ZIP local-file-header or empty EOCD.
 * @details Matches "PK" then the local-file-header (0x03 0x04) or empty-archive
 *          EOCD (0x05 0x06) pair; a RAR is detected separately via ::ra8_rar_open.
 * @param[in] s At least ::k_comic_sig_min readable bytes.
 * @return Whether @p s begins a ZIP archive.
 * @retval true  A ZIP signature.
 * @retval false Not a ZIP signature.
 * @pre @p s addresses at least ::k_comic_sig_min bytes.
 * @pre @p s is the archive start.
 * @post No state is modified (pure read).
 * @post The result depends only on the four leading bytes.
 * @note Thread-safe: pure read.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool internal_is_zip(const uint8_t* s)
{
  if (s[0] != (uint8_t)k_zip_b0) {
    return false;
  }
  if (s[1] != (uint8_t)k_zip_b1) {
    return false;
  }
  if (s[2] == (uint8_t)k_zip_lfh_b2) {
    return s[3] == (uint8_t)k_zip_lfh_b3;
  }
  if (s[2] == (uint8_t)k_zip_eocd_b2) {
    return s[3] == (uint8_t)k_zip_eocd_b3;
  }
  return false;
}

/**
 * @brief Reject any NULL required argument to ::ra8_comic_open.
 * @details Runs the mandatory null guards (reader, page index, name arena) so the
 *          entry point stays within the function-size budget; scalars may be zero.
 * @param[in] c     Reader-to-populate pointer.
 * @param[in] read  Archive byte reader.
 * @param[in] pages Caller page-index array.
 * @param[in] names Caller name arena.
 * @return ra8_err_t status.
 * @retval k_ra8_ok           Every required argument is non-NULL.
 * @retval k_ra8_err_null_ptr Some required argument was NULL.
 * @pre The caller forwards ::ra8_comic_open's pointer arguments unchanged.
 * @pre No argument is dereferenced before this returns k_ra8_ok.
 * @post On k_ra8_ok each checked pointer is safe to dereference.
 * @post On any error the reason is logged against ::s_tag_comic.
 * @note Thread-safe: reads only its pointer arguments.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_open_reject_null(const ra8_comic_t*      c,
                                           ra8_comic_read_fn       read,
                                           const ra8_comic_page_t* pages,
                                           const char*             names)
{
  RA8_CHECK_NULL_PTR(c, s_tag_comic, "open: null c");
  RA8_CHECK_NULL_PTR((const void*)read, s_tag_comic, "open: null read");
  RA8_CHECK_NULL_PTR(pages, s_tag_comic, "open: null pages");
  RA8_CHECK_NULL_PTR(names, s_tag_comic, "open: null names");
  return k_ra8_ok;
}

/**
 * @brief Detect the container and run its backend enumeration.
 * @details Reads the magic, dispatches to the CBZ or CBR backend, and leaves the
 *          page index populated (unsorted). Split from ::ra8_comic_open so the
 *          entry point stays within the function-size budget.
 * @param[in,out] c Comic with backing + buffers already bound.
 * @return ra8_err_t status.
 * @retval k_ra8_ok                A backend populated the index.
 * @retval k_ra8_err_invalid_size  A short magic read.
 * @retval k_ra8_err_not_supported Neither a ZIP nor a RAR archive.
 * @retval k_ra8_err_*             A backend error.
 * @pre `c->read` / `c->size` / buffers are bound.
 * @pre @p c was zeroed then populated by the caller.
 * @post On k_ra8_ok, `c->kind` is set and the index is filled (unsorted).
 * @post On any error `c->kind` reflects the detection outcome.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_open_detect(ra8_comic_t* c)
{
  uint8_t      sig[k_ra8_comic_magic_len] = {};
  const size_t got                        = c->read(c->ctx, 0U, sig, sizeof(sig));
  if (got < (size_t)k_comic_sig_min) {
    return k_ra8_err_invalid_size;
  }
  if (internal_is_zip(sig)) {
    c->kind        = k_ra8_comic_kind_cbz;
    c->stream.read = c->read;
    c->stream.ctx  = c->ctx;
    c->stream.size = c->size;
    return ra8_comic_cbz_open(c);
  }
  const ra8_err_t rerr = ra8_rar_open(&c->rar, c->read, c->ctx, c->size);
  if (rerr == k_ra8_ok) {
    c->kind = k_ra8_comic_kind_cbr;
    return ra8_comic_cbr_open(c);
  }
  /* tar has no leading magic bytes; the walker's open probes block 0
   * (ustar magic + checksum), so a non-tar file is rejected cheaply. */
  const ra8_err_t terr = ra8_unarch_tar_open(&c->tar, c->read, c->ctx, c->size, nullptr);
  if (terr == k_ra8_ok) {
    c->kind = k_ra8_comic_kind_cbt;
    return ra8_comic_cbt_open(c);
  }
  return k_ra8_err_not_supported;
}

ra8_err_t ra8_comic_open(ra8_comic_t*      c,
                         ra8_comic_read_fn read,
                         void*             ctx,
                         uint64_t          size,
                         ra8_comic_page_t* pages,
                         uint32_t          page_cap,
                         char*             names,
                         uint32_t          names_cap)
{
  const ra8_err_t nerr = internal_open_reject_null(c, read, pages, names);
  if (nerr != k_ra8_ok) {
    return nerr;
  }
  *c = (ra8_comic_t){};
  if (size == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (page_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (names_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  c->read      = read;
  c->ctx       = ctx;
  c->size      = size;
  c->pages     = pages;
  c->page_cap  = page_cap;
  c->names     = names;
  c->names_cap = names_cap;

  const ra8_err_t err = internal_open_detect(c);
  if (err != k_ra8_ok) {
    (void)ra8_comic_close(c);
    return err;
  }
  if (c->page_count == 0U) {
    (void)ra8_comic_close(c);
    return k_ra8_err_not_found;
  }
  internal_sort(c);
  return k_ra8_ok;
}

ra8_err_t ra8_comic_page_info(const ra8_comic_t* c,
                              uint32_t           page,
                              char*              name_buf,
                              uint16_t           name_cap,
                              uint16_t*          out_name_len,
                              uint64_t*          out_raw_size,
                              uint8_t*           out_extractable)
{
  RA8_CHECK_NULL_PTR(c, s_tag_comic, "info: null c");
  if (c->kind == k_ra8_comic_kind_none) {
    return k_ra8_err_invalid_state;
  }
  if (page >= c->page_count) {
    return k_ra8_err_out_of_range;
  }
  const ra8_comic_page_t* p = &c->pages[page];
  if (name_buf != nullptr) {
    const uint16_t want = (p->name_len > name_cap) ? name_cap : p->name_len;
    if (want > 0U) {
      (void)memcpy(name_buf, &c->names[p->name_off], (size_t)want);
    }
    if (out_name_len != nullptr) {
      *out_name_len = want;
    }
  } else if (out_name_len != nullptr) {
    *out_name_len = p->name_len;
  }
  if (out_raw_size != nullptr) {
    *out_raw_size = p->raw_size;
  }
  if (out_extractable != nullptr) {
    *out_extractable = p->extractable;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_comic_page_read(ra8_comic_t* c, uint32_t page, uint8_t* buf, size_t cap, size_t* got)
{
  RA8_CHECK_NULL_PTR(c, s_tag_comic, "read: null c");
  RA8_CHECK_NULL_PTR(buf, s_tag_comic, "read: null buf");
  RA8_CHECK_NULL_PTR(got, s_tag_comic, "read: null got");
  *got = 0U;
  if (c->kind == k_ra8_comic_kind_none) {
    return k_ra8_err_invalid_state;
  }
  if (page >= c->page_count) {
    return k_ra8_err_out_of_range;
  }
  const ra8_comic_page_t* p = &c->pages[page];
  if (c->kind == k_ra8_comic_kind_cbz) {
    return ra8_comic_cbz_extract(c, p, buf, cap, got);
  }
  if (c->kind == k_ra8_comic_kind_cbt) {
    return ra8_comic_cbt_extract(c, p, buf, cap, got);
  }
  return ra8_comic_cbr_extract(c, p, buf, cap, got);
}

ra8_err_t ra8_comic_close(ra8_comic_t* c)
{
  RA8_CHECK_NULL_PTR(c, s_tag_comic, "close: null c");
  if (c->kind == k_ra8_comic_kind_cbz) {
    (void)ra8_comic_cbz_close(c);
  }
  c->kind       = k_ra8_comic_kind_none;
  c->page_count = 0U;
  c->zip_active = 0U;
  c->tar.live   = false; /* the tar walker holds no resources to release */
  return k_ra8_ok;
}
