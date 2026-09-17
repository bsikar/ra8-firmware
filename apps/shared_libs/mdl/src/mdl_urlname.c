/**
 * @file mdl_urlname.c
 * @brief Bounded URL naming and portable image-type readers.
 *
 * @details Converts bounded URL path components into safe display and file
 * names, recognises explicit chapter markers, and identifies supported image
 * types from signatures or content type. All outputs use caller-owned fixed
 * buffers and no helper allocates memory. File signatures are read only through
 * the injected downloader storage binding.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#include "mdl_urlname.h"

#include <stdint.h>
#include <string.h>

#include "mdl_sanitize.h"
#include "ra8_attributes.h"

/** @brief On-stack buffers and parse radix for the URL-name helpers. */
typedef enum : uint16_t {
  k_urlname_raw_bytes   = 256,  /**< Pre-sanitise last-segment scratch bytes. */
  k_urlname_case_gap    = 32,   /**< 'a' - 'A': ASCII upper-to-lower offset.  */
  k_urlname_scan_max    = 2048, /**< Maximum URL bytes inspected.             */
  k_urlname_magic_bytes = 16,   /**< Largest file-signature prefix consumed.  */
  k_urlname_read_calls  = 17,   /**< One-byte short reads plus clean EOF.     */
} mdl_urlname_const_t;

/** @brief Numeric bounds keep conversion loops and sortable values finite. */
typedef enum : uint32_t {
  k_chapter_whole_max = 999999999U, /**< Largest accepted integral chapter. */
  k_chapter_frac_max  = 6U,         /**< Fractional digits retained.        */
  k_chapter_radix     = 10U,        /**< Decimal chapter-number radix.      */
} mdl_chapter_num_limit_t;

/** @brief Initial decimal place multiplier for a fractional chapter. */
static const double s_chapter_fraction_step = 0.1;

/** @brief Signature byte offsets not already exempted by the numeric policy. */
typedef enum : uint8_t {
  k_webp_sig_bytes = 12U, /**< RIFF header plus WEBP form type. */
  k_webp_e_offset  = 9U,  /**< 'E' in the WEBP form type.       */
  k_webp_b_offset  = 10U, /**< 'B' in the WEBP form type.       */
  k_webp_p_offset  = 11U, /**< 'P' in the WEBP form type.       */
  k_gif_a_offset   = 5U,  /**< 'a' trailer in GIF87a/GIF89a.    */
} mdl_image_signature_offset_t;

/** @brief Non-ASCII fixed bytes used by the JPEG and PNG signatures. */
typedef enum : uint8_t {
  k_jpeg_marker_byte = 0xFFU, /**< JPEG marker prefix.         */
  k_jpeg_soi_byte    = 0xD8U, /**< JPEG Start Of Image marker. */
  k_png_lead_byte    = 0x89U, /**< PNG signature lead byte.    */
} mdl_image_signature_byte_t;

/**
 * @brief End offset of a URL's path, before any `?query`/`#fragment`.
 * @details Finds the first query or fragment delimiter, otherwise uses string length.
 * @param[in] url NUL-terminated URL text.
 * @return Exclusive path-end offset.
 * @retval other A value from zero through `strlen(url)`.
 * @pre @p url is non-NULL and NUL-terminated.
 * @pre Query and fragment delimiters terminate the path for naming.
 * @post @p url is unchanged.
 * @post The return never exceeds the URL length.
 * @note Thread-safe: reads only its argument.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_urlname_path_end(const char* url)
{
  const char* q = strpbrk(url, "?#");
  return (q == nullptr) ? strlen(url) : (size_t)(q - url);
}

void mdl_urlname_last_segment(const char* url, char* out, size_t cap)
{
  if ((url == nullptr) || (out == nullptr) || (cap < 2U)) {
    if ((out != nullptr) && (cap > 0U)) {
      out[0] = '\0';
    }
    return;
  }
  size_t end = internal_urlname_path_end(url);
  while ((end > 0U) && (url[end - 1U] == '/')) {
    --end; /* trim trailing slashes */
  }
  size_t start = end;
  while ((start > 0U) && (url[start - 1U] != '/')) {
    --start;
  }
  char   raw[k_urlname_raw_bytes];
  size_t n = 0U;
  for (size_t i = start; (i < end) && (n + 1U < sizeof(raw)); ++i) {
    raw[n] = url[i];
    ++n;
  }
  raw[n] = '\0';
  (void)mdl_sanitize_segment(raw, out, cap);
}

/**
 * @brief Start offset of the path, excluding any URL scheme and authority.
 *
 * @details Relative URLs begin at zero. For an absolute URL, the first slash
 * after `://` begins the path; when no slash exists, the supplied end offset is
 * returned so authority digits cannot be mistaken for a chapter.
 *
 * @param[in] url NUL-terminated URL containing at least @p end readable bytes.
 * @param[in] end Exclusive end offset of the path scan.
 * @return Offset of the first path byte within @p url.
 * @retval 0 @p url is relative or has no scheme separator before @p end.
 * @retval other The first path slash, or @p end when an authority has no path.
 * @pre @p url is non-NULL and NUL-terminated.
 * @pre @p end does not exceed the URL length.
 * @post The return value is at most @p end.
 * @post @p url is never modified.
 * @note Query and fragment removal is handled by the caller when choosing @p end.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t internal_urlname_path_start(const char* url, size_t end)
{
  const char* scheme = strstr(url, "://");
  if ((scheme == nullptr) || ((size_t)(scheme - url) >= end)) {
    return 0U; /* Relative URL: its entire prefix is a path. */
  }
  const size_t authority = (size_t)(scheme - url) + strlen("://");
  const char*  slash     = memchr(url + authority, '/', end - authority);
  return (slash == nullptr) ? end : (size_t)(slash - url);
}

/** @brief Locate the last chapter marker wholly inside `[begin,end)`. */
RA8_INTERNAL static const char*
internal_urlname_last_path_marker(const char* url, size_t begin, size_t end, const char* marker)
{
  const size_t marker_len = strlen(marker);
  const char*  found      = nullptr;
  if (marker_len > (end - begin)) {
    return nullptr;
  }
  for (size_t i = begin; i <= (end - marker_len); ++i) {
    if (memcmp(url + i, marker, marker_len) == 0) {
      found = url + i;
    }
  }
  return found;
}

/**
 * @brief Parse digits at `start`, optionally followed by a decimal fraction.
 *
 * @details Accumulates a bounded integral value, then accepts `.` or `-` as a
 * fractional separator when followed by digits. More than the supported number
 * of fractional digits or an overflowing integral component is rejected.
 *
 * @param[in] start First candidate digit.
 * @param[in] end One-past-last byte available to the parser.
 * @param[out] out Parsed non-negative chapter value on success.
 * @param[out] out_next Optional first unconsumed byte on success.
 * @return Whether a bounded chapter value was parsed.
 * @retval true  At least one digit was parsed without exceeding a bound.
 * @retval false Input did not start with a digit or exceeded a numeric bound.
 * @pre @p start and @p end delimit one readable contiguous range.
 * @pre @p out points to writable storage for one `double`.
 * @post On true, @p out contains the parsed non-negative value and a non-NULL
 *       @p out_next receives the first unconsumed byte.
 * @post On false, @p out is left unchanged.
 * @note Bytes following the recognised numeric run are intentionally ignored.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_urlname_parse_chapter_digits(const char*  start,
                                                               const char*  end,
                                                               double*      out,
                                                               const char** out_next)
{
  if ((start >= end) || (*start < '0') || (*start > '9')) {
    return false;
  }
  uint32_t whole = 0U;
  while ((start < end) && (*start >= '0') && (*start <= '9')) {
    const uint32_t digit = (uint32_t)(*start - '0');
    if (whole > (((uint32_t)k_chapter_whole_max - digit) / (uint32_t)k_chapter_radix)) {
      return false;
    }
    whole = (whole * (uint32_t)k_chapter_radix) + digit;
    ++start;
  }
  double value = (double)whole;
  if ((start < end) && ((*start == '-') || (*start == '.')) && ((start + 1) < end) &&
      (start[1] >= '0') && (start[1] <= '9')) {
    ++start;
    double   scale  = s_chapter_fraction_step;
    uint32_t digits = 0U;
    while ((start < end) && (*start >= '0') && (*start <= '9')) {
      if (digits >= (uint32_t)k_chapter_frac_max) {
        return false;
      }
      value += (double)(*start - '0') * scale;
      scale *= s_chapter_fraction_step;
      ++digits;
      ++start;
    }
  }
  *out = value;
  if (out_next != nullptr) {
    *out_next = start;
  }
  return true;
}

bool mdl_urlname_chapter_parse(const char* url, double* out)
{
  if (out != nullptr) {
    *out = 0.0;
  }
  if ((url == nullptr) || (out == nullptr)) {
    return false;
  }
  const size_t bounded = strnlen(url, (size_t)k_urlname_scan_max);
  if (bounded == (size_t)k_urlname_scan_max) {
    return false;
  }
  const size_t             end       = internal_urlname_path_end(url);
  const size_t             begin     = internal_urlname_path_start(url, end);
  static const char* const markers[] = {"chapter-", "/ch-", "/ep"};
  const char*              found     = nullptr;
  size_t                   skip      = 0U;
  for (size_t i = 0U; i < (sizeof(markers) / sizeof(markers[0])); ++i) {
    const char* candidate = internal_urlname_last_path_marker(url, begin, end, markers[i]);
    if ((candidate != nullptr) && ((found == nullptr) || (candidate > found))) {
      found = candidate;
      skip  = strlen(markers[i]);
    }
  }
  return (found != nullptr) &&
         internal_urlname_parse_chapter_digits(found + skip, url + end, out, nullptr);
}

bool mdl_urlname_chapter_text_parse(const char* text, double* out)
{
  if (out != nullptr) {
    *out = 0.0;
  }
  if ((text == nullptr) || (out == nullptr)) {
    return false;
  }
  const size_t bounded = strnlen(text, (size_t)k_urlname_scan_max);
  if (bounded == (size_t)k_urlname_scan_max) {
    return false;
  }
  const char* begin = text;
  const char* end   = text + bounded;
  while ((begin < end) && ((*begin == ' ') || (*begin == '\t') || (*begin == '\r') ||
                           (*begin == '\n') || (*begin == '\f') || (*begin == '\v'))) {
    ++begin;
  }
  while ((end > begin) && ((end[-1] == ' ') || (end[-1] == '\t') || (end[-1] == '\r') ||
                           (end[-1] == '\n') || (end[-1] == '\f') || (end[-1] == '\v'))) {
    --end;
  }
  const char* next   = nullptr;
  double      parsed = 0.0;
  if (!internal_urlname_parse_chapter_digits(begin, end, &parsed, &next) || (next != end)) {
    return false;
  }
  *out = parsed;
  return true;
}

double mdl_urlname_chapter_value(const char* url)
{
  double value = 0.0;
  (void)mdl_urlname_chapter_parse(url, &value);
  return value;
}

long mdl_urlname_chapter_number(const char* url)
{
  return (long)mdl_urlname_chapter_value(url);
}

/**
 * @brief Lower-case an ASCII byte.
 * @details Maps uppercase ASCII letters and preserves every other byte.
 * @param[in] c Character to map.
 * @return Lower-case ASCII equivalent.
 * @retval other Mapped or unchanged character.
 * @pre @p c is representable as `char`.
 * @pre Locale-independent folding is required.
 * @post No state is modified.
 * @post Non-uppercase input is unchanged.
 * @note Thread-safe: pure arithmetic.
 * @since 0.1.0
 */
RA8_INTERNAL static char internal_urlname_to_lower_ascii(char c)
{
  const int lowered = ((c >= 'A') && (c <= 'Z')) ? (c + (int)k_urlname_case_gap) : (int)c;
  return (char)lowered;
}

/**
 * @brief True when `ext` is one of the accepted raster image extensions.
 * @details Compares against the fixed lower-case extension allowlist.
 * @param[in] ext NUL-terminated lower-case extension without a dot.
 * @return Whether the extension is accepted.
 * @retval true The extension is a supported raster type.
 * @retval false It is absent from the allowlist.
 * @pre @p ext is non-NULL and NUL-terminated.
 * @pre The caller has already normalised ASCII case.
 * @post @p ext is unchanged.
 * @post No state is modified.
 * @note Thread-safe: reads constant storage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_urlname_is_known_ext(const char* ext)
{
  static const char* const known[] = {"jpg", "jpeg", "png", "gif", "webp", "bmp"};
  for (size_t i = 0U; i < (sizeof(known) / sizeof(known[0])); ++i) {
    if (strcmp(ext, known[i]) == 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Copy one NUL-terminated value into bounded caller storage.
 * @details Copies at most `capacity - 1` bytes and always terminates a writable,
 *          nonempty destination without consulting host stream state.
 * @param[out] out Optional output buffer.
 * @param[in] capacity Writable output bytes.
 * @param[in] value NUL-terminated source value.
 * @pre @p value is non-NULL and NUL-terminated.
 * @pre A non-NULL @p out addresses @p capacity writable bytes.
 * @post A nonempty output is NUL-terminated, with deterministic truncation.
 * @post Source bytes are unchanged and no pointer is retained.
 * @note Thread-safe across disjoint output buffers.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_urlname_copy(char* out, size_t capacity, const char* value)
{
  if ((out == nullptr) || (capacity == 0U)) {
    return;
  }
  size_t length = strnlen(value, capacity - 1U);
  memcpy(out, value, length);
  out[length] = '\0';
}

void mdl_urlname_ext(const char* url, char* out, size_t cap)
{
  if ((out == nullptr) || (cap == 0U)) {
    return;
  }
  internal_urlname_copy(out, cap, "jpg");
  if (url == nullptr) {
    return;
  }
  const size_t pend  = internal_urlname_path_end(url);
  size_t       start = pend;
  while ((start > 0U) && (url[start - 1U] != '/')) {
    --start; /* isolate the last path segment */
  }
  size_t dot = pend;
  for (size_t i = start; i < pend; ++i) {
    if (url[i] == '.') {
      dot = i;
    }
  }
  if (dot >= pend) {
    return; /* no '.' in the segment -> keep the default */
  }
  char   ext[8];
  size_t n = 0U;
  for (size_t i = dot + 1U; (i < pend) && (n + 1U < sizeof(ext)); ++i) {
    ext[n++] = internal_urlname_to_lower_ascii(url[i]);
  }
  ext[n] = '\0';
  if (internal_urlname_is_known_ext(ext)) {
    internal_urlname_copy(out, cap, ext);
  }
}

/**
 * @brief Classify a supported image from its leading magic bytes.
 * @details Checks bounded JPEG, PNG, GIF, WebP, and AVIF signatures in a fixed
 *          order and returns immutable canonical extension and MIME strings.
 * @param[in] buf Readable response prefix.
 * @param[in] buf_len Number of readable bytes at @p buf.
 * @param[out] ext Receives a borrowed canonical extension pointer.
 * @param[out] mime Receives a borrowed canonical MIME pointer.
 * @return Whether a complete supported signature was recognized.
 * @retval true @p ext and @p mime were initialized consistently.
 * @retval false No supported complete signature was present.
 * @pre @p ext and @p mime are non-NULL.
 * @pre @p buf is non-NULL when @p buf_len is nonzero.
 * @post Success initializes both outputs to process-lifetime constants.
 * @post Input bytes and caller ownership remain unchanged.
 * @note Partial signatures are rejected rather than guessed.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_sniff_magic_image(const void* buf, size_t buf_len, const char** ext, const char** mime)
{
  if ((buf == nullptr) || (buf_len < 2U)) {
    return false;
  }
  const uint8_t* b = (const uint8_t*)buf;
  if ((buf_len >= 3U) && (b[0] == (uint8_t)k_jpeg_marker_byte) &&
      (b[1] == (uint8_t)k_jpeg_soi_byte) && (b[2] == (uint8_t)k_jpeg_marker_byte)) {
    *ext  = "jpg";
    *mime = "image/jpeg";
    return true;
  }
  if ((b[0] == (uint8_t)'B') && (b[1] == (uint8_t)'M')) {
    *ext  = "bmp";
    *mime = "image/bmp";
    return true;
  }
  if ((buf_len >= 4U) && (b[0] == (uint8_t)k_png_lead_byte) && (b[1] == (uint8_t)'P') &&
      (b[2] == (uint8_t)'N') && (b[3] == (uint8_t)'G')) {
    *ext  = "png";
    *mime = "image/png";
    return true;
  }
  if ((buf_len >= (size_t)k_webp_sig_bytes) && (b[0] == 'R') && (b[1] == 'I') && (b[2] == 'F') &&
      (b[3] == 'F') && (b[8] == 'W') && (b[k_webp_e_offset] == 'E') &&
      (b[k_webp_b_offset] == 'B') && (b[k_webp_p_offset] == 'P')) {
    *ext  = "webp";
    *mime = "image/webp";
    return true;
  }
  if ((buf_len >= 6U) && (b[0] == 'G') && (b[1] == 'I') && (b[2] == 'F') && (b[3] == '8') &&
      ((b[4] == '7') || (b[4] == '9')) && (b[k_gif_a_offset] == 'a')) {
    *ext  = "gif";
    *mime = "image/gif";
    return true;
  }
  return false;
}

/**
 * @brief Classify a supported image from an HTTP Content-Type value.
 * @details Compares the media type case-insensitively while accepting bounded
 *          parameters after `;`, then returns the canonical extension/MIME pair.
 * @param[in] content_type NUL-terminated response Content-Type value.
 * @param[out] ext Receives a borrowed canonical extension pointer.
 * @param[out] mime Receives a borrowed canonical MIME pointer.
 * @return Whether a supported image media type was recognized.
 * @retval true @p ext and @p mime were initialized consistently.
 * @retval false The value was missing, malformed, or unsupported.
 * @pre @p ext and @p mime are non-NULL.
 * @pre @p content_type, when non-NULL, is NUL-terminated.
 * @post Success initializes both outputs to process-lifetime constants.
 * @post The supplied header text and its ownership remain unchanged.
 * @note Magic-byte classification takes precedence at the public seam.
 * @since 0.1.0
 */
RA8_INTERNAL static bool
internal_sniff_content_type(const char* content_type, const char** ext, const char** mime)
{
  if ((content_type == nullptr) || (content_type[0] == '\0')) {
    return false;
  }
  char   ct_lower[128];
  size_t i = 0U;
  for (; (content_type[i] != '\0') && (i + 1U < sizeof(ct_lower)); ++i) {
    ct_lower[i] = internal_urlname_to_lower_ascii(content_type[i]);
  }
  ct_lower[i] = '\0';
  if ((strstr(ct_lower, "image/jpeg") != nullptr) || (strstr(ct_lower, "image/jpg") != nullptr)) {
    *ext  = "jpg";
    *mime = "image/jpeg";
  } else if (strstr(ct_lower, "image/png") != nullptr) {
    *ext  = "png";
    *mime = "image/png";
  } else if (strstr(ct_lower, "image/webp") != nullptr) {
    *ext  = "webp";
    *mime = "image/webp";
  } else if (strstr(ct_lower, "image/gif") != nullptr) {
    *ext  = "gif";
    *mime = "image/gif";
  } else if (strstr(ct_lower, "image/bmp") != nullptr) {
    *ext  = "bmp";
    *mime = "image/bmp";
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Copy one canonical image classification to bounded outputs.
 * @details Copies extension and MIME independently only when the corresponding
 *          output buffer is present and nonempty, always preserving termination.
 * @param[in] ext NUL-terminated canonical extension.
 * @param[in] mime NUL-terminated canonical MIME type.
 * @param[out] out_ext Optional extension output buffer.
 * @param[in] ext_cap Capacity of @p out_ext in bytes.
 * @param[out] out_mime Optional MIME output buffer.
 * @param[in] mime_cap Capacity of @p out_mime in bytes.
 * @pre @p ext and @p mime are non-NULL and NUL-terminated.
 * @pre Each non-NULL output references its declared writable capacity.
 * @post Every nonempty output is NUL-terminated within its capacity.
 * @post No input or output pointer is retained or transferred.
 * @note Truncation is deterministic and confined to the affected output.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_copy_image_type(const char* ext,
                                                  const char* mime,
                                                  char*       out_ext,
                                                  size_t      ext_cap,
                                                  char*       out_mime,
                                                  size_t      mime_cap)
{
  if ((out_ext != nullptr) && (ext_cap > 0U)) {
    internal_urlname_copy(out_ext, ext_cap, ext);
  }
  if ((out_mime != nullptr) && (mime_cap > 0U)) {
    internal_urlname_copy(out_mime, mime_cap, mime);
  }
}

bool mdl_urlname_sniff_image_type(const void* buf,
                                  size_t      buf_len,
                                  const char* content_type,
                                  char*       out_ext,
                                  size_t      ext_cap,
                                  char*       out_mime,
                                  size_t      mime_cap)
{
  const char* ext  = nullptr;
  const char* mime = nullptr;
  if (!internal_sniff_magic_image(buf, buf_len, &ext, &mime) &&
      !internal_sniff_content_type(content_type, &ext, &mime)) {
    return false;
  }
  internal_copy_image_type(ext, mime, out_ext, ext_cap, out_mime, mime_cap);
  return true;
}
ra8_err_t mdl_urlname_sniff_file(mdl_storage_t* storage,
                                 const char*    file_path,
                                 const char*    content_type,
                                 char*          out_ext,
                                 size_t         ext_cap,
                                 char*          out_mime,
                                 size_t         mime_cap)
{
  if ((storage == nullptr) || (storage->fs == nullptr) || (storage->file_workspace == nullptr) ||
      (file_path == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  fw_fs_file_t file = {};
  ra8_err_t    err  = fw_fs_open(&storage->fs->streams,
                                 file_path,
                                 k_fw_fs_open_read,
                                 &file,
                                 storage->file_workspace,
                                 storage->file_workspace_bytes);
  if (err != k_ra8_ok) {
    return err;
  }

  uint8_t  header[k_urlname_magic_bytes] = {};
  uint32_t length                        = 0U;
  uint32_t calls                         = 0U;
  while ((length < sizeof(header)) && (err == k_ra8_ok)) {
    if (calls >= (uint32_t)k_urlname_read_calls) {
      err = k_ra8_err_invalid_size;
      break;
    }
    uint32_t count = 0U;
    err            = fw_fs_read(&file, header + length, (uint32_t)sizeof(header) - length, &count);
    ++calls;
    if ((err == k_ra8_ok) && (count == 0U)) {
      break;
    }
    length += count;
  }
  const ra8_err_t close_err = fw_fs_close(&file);
  if (close_err != k_ra8_ok) {
    return close_err;
  }
  if (err != k_ra8_ok) {
    return err;
  }
  return mdl_urlname_sniff_image_type(header,
                                      length,
                                      content_type,
                                      out_ext,
                                      ext_cap,
                                      out_mime,
                                      mime_cap)
           ? k_ra8_ok
           : k_ra8_err_validation_failed;
}
