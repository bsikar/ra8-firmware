/**
 * @file mdl_verify.c
 * @brief Bounded structural validators for native archive formats.
 * @details Validates container framing, member paths, required metadata, and
 *          page presence using only the caller-provided export workspace.
 */
#include "mdl_verify.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_jof.h"

typedef struct {
  size_t      bytes; /**< Payload size following this arena header. */
  max_align_t align; /**< Forces maximum alignment for the payload. */
} mdl_alloc_header_t;

/** @brief POSIX tar field layout and record sizing used by the verifier. */
typedef enum : uint16_t {
  k_tar_name_bytes      = 100U, /**< Width of the header name field.       */
  k_tar_type_offset     = 156U, /**< Header typeflag byte offset.          */
  k_tar_size_offset     = 124U, /**< Header file-size field offset.        */
  k_tar_size_bytes      = 12U,  /**< Width of the file-size field.         */
  k_tar_checksum_offset = 148U, /**< Header checksum field offset.         */
  k_tar_checksum_end    = 156U, /**< First byte after the checksum field.  */
  k_tar_padding_mask    = 511U, /**< Block-rounding mask.                  */
  k_tar_block_bytes     = 512U, /**< POSIX tar record size.                */
} mdl_verify_tar_layout_t;

/** @brief RFC 1952 fixed-header and trailer constants. */
typedef enum : uint8_t {
  k_gzip_id_one         = 0x1FU, /**< First gzip identification byte.      */
  k_gzip_id_two         = 0x8BU, /**< Second gzip identification byte.     */
  k_gzip_method_deflate = 8U,    /**< DEFLATE compression method.          */
  k_gzip_header_bytes   = 10U,   /**< Fixed gzip header size emitted here. */
  k_gzip_trailer_bytes  = 8U,    /**< CRC32 and ISIZE trailer size.        */
  k_gzip_min_bytes      = 18U,   /**< Header plus trailer minimum size.    */
  k_archive_arena_align = 8U,    /**< Workspace alignment for byte spans. */
} mdl_verify_gzip_layout_t;

/** @brief Byte shifts for decoding a little-endian 32-bit integer. */
typedef enum : uint8_t {
  k_u32_byte_one_shift   = 8U,  /**< Shift for little-endian byte one.   */
  k_u32_byte_two_shift   = 16U, /**< Shift for little-endian byte two.   */
  k_u32_byte_three_shift = 24U, /**< Shift for little-endian byte three. */
} mdl_verify_u32_shift_t;

/**
 * @brief Test a string suffix without ASCII case sensitivity
 * @details Compares only the tail of @p text and rejects a longer suffix.
 * @param[in] text NUL-terminated string to inspect.
 * @param[in] suffix NUL-terminated suffix to match.
 * @return Whether the complete suffix matches.
 * @retval true @p text ends with @p suffix ignoring ASCII case.
 * @retval false The suffix is longer or any byte differs.
 * @pre Both pointers are non-NULL and NUL-terminated.
 * @pre Inputs remain stable during the comparison.
 * @post Neither input is modified.
 * @post The result depends only on the two strings.
 * @note Thread-safe: this is a pure predicate.
 * @since 0.1.0
 */
RA8_INTERNAL static bool ends_ci(const char* text, const char* suffix)
{
  const size_t tl = strlen(text);
  const size_t sl = strlen(suffix);
  if (sl > tl) {
    return false;
  }
  for (size_t i = 0U; i < sl; ++i) {
    if (tolower((unsigned char)text[tl - sl + i]) != tolower((unsigned char)suffix[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Classify a supported raster member by filename suffix
 * @details Accepts the bounded image extensions emitted by the downloader.
 * @param[in] name NUL-terminated archive member name.
 * @return Whether @p name has a supported image suffix.
 * @retval true A JPEG, PNG, WebP, GIF, or BMP suffix matched.
 * @retval false No supported suffix matched.
 * @pre @p name is non-NULL and NUL-terminated.
 * @pre Filename classification, not byte decoding, is intended here.
 * @post @p name is unchanged.
 * @post The result is deterministic for the same name.
 * @note Thread-safe: this is a pure predicate.
 * @since 0.1.0
 */
RA8_INTERNAL static bool is_image(const char* name)
{
  return ends_ci(name, ".jpg") || ends_ci(name, ".jpeg") || ends_ci(name, ".png") ||
         ends_ci(name, ".webp") || ends_ci(name, ".gif") || ends_ci(name, ".bmp");
}

/**
 * @brief Reject absolute and parent-traversing container member names
 * @details Forbids empty segments, dot segments, backslashes, and leading
 *          separators so later readers cannot interpret an unsafe extraction path.
 * @param[in] name NUL-terminated member name, or NULL.
 * @return Whether the member is a safe relative slash-separated path.
 * @retval true Every segment is nonempty and relative.
 * @retval false The name is NULL, absolute, malformed, or traversing.
 * @pre A non-NULL @p name is NUL-terminated.
 * @pre The caller treats this as lexical validation, not filesystem resolution.
 * @post @p name is not modified.
 * @post Accepted names contain no parent or current-directory segments.
 * @note Thread-safe: this is a pure predicate.
 * @since 0.1.0
 */
RA8_INTERNAL static bool safe_member_name(const char* name)
{
  if ((name == nullptr) || (name[0] == '\0') || (name[0] == '/') || (name[0] == '\\')) {
    return false;
  }
  const char* seg = name;
  for (const char* p = name;; ++p) {
    if ((*p == '\\') || (*p == '/') || (*p == '\0')) {
      const size_t len = (size_t)(p - seg);
      if ((*p == '\\') || (len == 0U) || ((len == 1U) && (seg[0] == '.')) ||
          ((len == 2U) && (seg[0] == '.') && (seg[1] == '.'))) {
        return false;
      }
      if (*p == '\0') {
        return true;
      }
      seg = p + 1;
    }
  }
}

ra8_err_t mdl_format_from_path(const char* path, mdl_format_t* out_format)
{
  if ((path == nullptr) || (out_format == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  static const struct {
    const char*  suffix; /**< Complete artifact suffix. */
    mdl_format_t fmt;    /**< Format mapped from suffix. */
  } suffixes[] = {{".cbt.gz", k_mdl_fmt_cbt_gz},
                  {".epub", k_mdl_fmt_epub},
                  {".cbz", k_mdl_fmt_cbz},
                  {".cbt", k_mdl_fmt_cbt},
                  {".jof", k_mdl_fmt_jof}};
  for (size_t i = 0U; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    if (ends_ci(path, suffixes[i].suffix)) {
      *out_format = suffixes[i].fmt;
      return k_ra8_ok;
    }
  }
  *out_format = k_mdl_fmt_invalid;
  return k_ra8_err_not_supported;
}

bool mdl_format_is_verifiable(mdl_format_t format)
{
  return (format == k_mdl_fmt_cbz) || (format == k_mdl_fmt_cbt) || (format == k_mdl_fmt_cbt_gz) ||
         (format == k_mdl_fmt_epub) || (format == k_mdl_fmt_jof);
}

RA8_INTERNAL static void* arena_alloc(void* opaque, size_t items, size_t size)
{
  mdl_export_workspace_t* ws = (mdl_export_workspace_t*)opaque;
  if ((items != 0U) && (size > (SIZE_MAX / items))) {
    return nullptr;
  }
  const size_t bytes = items * size;
  if (bytes > SIZE_MAX - sizeof(mdl_alloc_header_t)) {
    return nullptr;
  }
  mdl_alloc_header_t* h = mdl_export_workspace_take(ws, sizeof(*h) + bytes, _Alignof(max_align_t));
  if (h == nullptr) {
    return nullptr;
  }
  h->bytes = bytes;
  return (void*)(h + 1);
}

/**
 * @brief Accept a miniz free request for monotonic arena storage
 * @details Individual allocations cannot be reclaimed from the bump arena, so
 *          miniz frees are intentional no-ops until the whole workspace resets.
 * @param[in] opaque Unused workspace context.
 * @param[in] address Unused allocation address.
 * @pre Both arguments may be NULL as permitted by miniz.
 * @pre The arena lifetime outlasts the active miniz reader.
 * @post Workspace offsets remain unchanged.
 * @post The address is not accessed after entry.
 * @note Thread-safe only for independent miniz/workspace instances.
 * @since 0.1.0
 */
RA8_INTERNAL static void arena_free(void* opaque, void* address)
{
  (void)opaque;
  (void)address;
}

RA8_INTERNAL static void* arena_realloc(void* opaque, void* address, size_t items, size_t size)
{
  if (address == nullptr) {
    return arena_alloc(opaque, items, size);
  }
  mdl_alloc_header_t* old_h = ((mdl_alloc_header_t*)address) - 1;
  void*               next  = arena_alloc(opaque, items, size);
  if (next == nullptr) {
    return nullptr;
  }
  const size_t next_bytes = items * size;
  memcpy(next, address, old_h->bytes < next_bytes ? old_h->bytes : next_bytes);
  return next;
}

/**
 * @brief Consume extracted ZIP bytes without retaining them
 * @details Lets miniz inflate and CRC-check every entry while bounding memory.
 * @param[in] opaque Unused callback context.
 * @param[in] offset Unused logical output offset.
 * @param[in] data Extracted bytes owned by miniz.
 * @param[in] bytes Number of bytes presented.
 * @return Number of bytes accepted by the sink.
 * @retval bytes Every presented byte is always accepted.
 * @pre @p data is readable for @p bytes when non-NULL.
 * @pre The callback is invoked by one active miniz extraction.
 * @post No extracted bytes are retained.
 * @post No caller-owned state is modified.
 * @note Thread-safe because the callback keeps no state.
 * @since 0.1.0
 */
RA8_INTERNAL static size_t
discard_zip(void* opaque, mz_uint64 offset, const void* data, size_t bytes)
{
  (void)opaque;
  (void)offset;
  (void)data;
  return bytes;
}

/**
 * @brief Validate CBZ or EPUB structure through bounded miniz callbacks
 * @details Opens every member, rejects unsafe paths, counts images, and requires
 *          ComicInfo for CBZ or the EPUB mimetype/container/OPF/navigation set.
 * @param[in] fmt Expected CBZ or EPUB format.
 * @param[in] path NUL-terminated ZIP container path.
 * @param[in,out] ws Caller-owned arena used by miniz.
 * @param[out] report Structural counts populated on success.
 * @return Validation status.
 * @retval k_ra8_ok Every member and required semantic marker is valid.
 * @retval k_ra8_err_validation_failed ZIP parsing or semantics failed.
 * @pre @p fmt is CBZ or EPUB and all pointers are non-NULL.
 * @pre @p ws is exclusive and has writable capacity.
 * @post Miniz reader resources are ended on every initialized path.
 * @post Success populates page/member counts and metadata presence.
 * @note Thread-safe across distinct paths, reports, and workspaces.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t verify_zip(mdl_format_t            fmt,
                                         const char*             path,
                                         mdl_export_workspace_t* ws,
                                         mdl_verify_report_t*    report)
{
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  zip.m_pAlloc        = arena_alloc;
  zip.m_pFree         = arena_free;
  zip.m_pRealloc      = arena_realloc;
  zip.m_pAlloc_opaque = ws;
  if (mz_zip_reader_init_file(&zip, path, 0) == MZ_FALSE) {
    return k_ra8_err_validation_failed;
  }
  bool          has_mimetype  = false;
  bool          has_container = false;
  bool          has_opf       = false;
  bool          has_nav       = false;
  bool          has_comicinfo = false;
  size_t        pages         = 0U;
  const mz_uint total         = mz_zip_reader_get_num_files(&zip);
  ra8_err_t     rc            = (total == 0U) ? k_ra8_err_validation_failed : k_ra8_ok;
  for (mz_uint i = 0U; (rc == k_ra8_ok) && (i < total); ++i) {
    mz_zip_archive_file_stat st;
    if (mz_zip_reader_file_stat(&zip, i, &st) == MZ_FALSE) {
      rc = k_ra8_err_validation_failed;
      break;
    }
    if (!st.m_is_directory &&
        (mz_zip_reader_extract_to_callback(&zip, i, discard_zip, nullptr, 0) == MZ_FALSE)) {
      rc = k_ra8_err_validation_failed;
      break;
    }
    const char* name = st.m_filename;
    if (!safe_member_name(name)) {
      rc = k_ra8_err_validation_failed;
      break;
    }
    has_comicinfo = has_comicinfo || ends_ci(name, "ComicInfo.xml");
    if (is_image(name)) {
      ++pages;
    }
    has_mimetype  = has_mimetype || (strcmp(name, "mimetype") == 0);
    has_container = has_container || (strcmp(name, "META-INF/container.xml") == 0);
    has_opf       = has_opf || ends_ci(name, ".opf");
    has_nav       = has_nav || ends_ci(name, "nav.xhtml");
  }
  (void)mz_zip_reader_end(&zip);
  if ((rc == k_ra8_ok) && (pages == 0U)) {
    rc = k_ra8_err_validation_failed;
  }
  if ((rc == k_ra8_ok) && (fmt == k_mdl_fmt_cbz) && !has_comicinfo) {
    rc = k_ra8_err_validation_failed;
  }
  if ((rc == k_ra8_ok) && (fmt == k_mdl_fmt_epub) &&
      !(has_mimetype && has_container && has_opf && has_nav)) {
    rc = k_ra8_err_validation_failed;
  }
  if (rc == k_ra8_ok) {
    report->page_count       = pages;
    report->member_count     = total;
    report->metadata_present = (fmt == k_mdl_fmt_cbz) ? has_comicinfo : has_opf;
  }
  return rc;
}

/**
 * @brief Parse a bounded POSIX tar octal field
 * @details Skips leading spaces/NULs and rejects non-octal bytes or size_t overflow.
 * @param[in] field Fixed-width tar header field.
 * @param[in] len Readable width of @p field.
 * @param[out] out Parsed value when at least one digit is present.
 * @return Whether a valid bounded octal value was parsed.
 * @retval true At least one digit was parsed without overflow.
 * @retval false The field is empty, malformed, or overflows size_t.
 * @pre @p field addresses @p len readable bytes.
 * @pre @p out points to writable size_t storage.
 * @post Success stores the parsed value in @p out.
 * @post No bytes in @p field are modified.
 * @note Thread-safe across distinct output objects.
 * @since 0.1.0
 */
RA8_INTERNAL static bool parse_octal(const uint8_t* field, size_t len, size_t* out)
{
  size_t value = 0U;
  size_t i     = 0U;
  while ((i < len) && ((field[i] == ' ') || (field[i] == '\0'))) {
    ++i;
  }
  bool any = false;
  for (; (i < len) && (field[i] != '\0') && (field[i] != ' '); ++i) {
    if ((field[i] < '0') || (field[i] > '7') || (value > (SIZE_MAX >> 3U))) {
      return false;
    }
    value = (value << 3U) + (size_t)(field[i] - '0');
    any   = true;
  }
  *out = value;
  return any;
}

/**
 * @brief Validate a CBT tar directly from a seekable file
 * @details Verifies record checksums, regular-file types, safe names, padded
 *          sizes, terminal zero blocks, images, and ComicInfo presence.
 * @param[in] path NUL-terminated CBT file path.
 * @param[out] report Structural counts populated on success.
 * @return Validation status.
 * @retval k_ra8_ok The complete tar is structurally valid.
 * @retval k_ra8_err_validation_failed A header, bound, or semantic rule failed.
 * @retval k_ra8_fail The file could not be opened.
 * @pre @p path and @p report are non-NULL.
 * @pre The file remains stable and seekable during validation.
 * @post The input file is closed on every opened path.
 * @post Success populates image/member counts and metadata presence.
 * @note Thread-safe for distinct files and reports.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t verify_tar(const char* path, mdl_verify_report_t* report)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return k_ra8_fail;
  }
  uint8_t   block[k_tar_block_bytes];
  size_t    pages       = 0U;
  size_t    members     = 0U;
  bool      metadata    = false;
  unsigned  zero_blocks = 0U;
  ra8_err_t rc          = k_ra8_ok;
  while (fread(block, 1U, sizeof(block), f) == sizeof(block)) {
    bool zero = true;
    for (size_t i = 0U; i < sizeof(block); ++i) {
      zero = zero && (block[i] == 0U);
    }
    if (zero) {
      if (++zero_blocks == 2U) {
        break;
      }
      continue;
    }
    zero_blocks  = 0U;
    unsigned sum = 0U;
    for (size_t i = 0U; i < sizeof(block); ++i) {
      sum += ((i >= (size_t)k_tar_checksum_offset) && (i < (size_t)k_tar_checksum_end))
               ? (unsigned)' '
               : block[i];
    }
    size_t expected  = 0U;
    size_t file_size = 0U;
    if (!parse_octal(block + k_tar_checksum_offset,
                     (size_t)(k_tar_checksum_end - k_tar_checksum_offset),
                     &expected) ||
        (sum != expected) ||
        !parse_octal(block + k_tar_size_offset, k_tar_size_bytes, &file_size) ||
        (block[k_tar_type_offset] != '0')) {
      rc = k_ra8_err_validation_failed;
      break;
    }
    char name[k_tar_name_bytes + 1U];
    memcpy(name, block, k_tar_name_bytes);
    name[k_tar_name_bytes] = '\0';
    if (!safe_member_name(name)) {
      rc = k_ra8_err_validation_failed;
      break;
    }
    ++members;
    pages += is_image(name) ? 1U : 0U;
    metadata          = metadata || (strcmp(name, "ComicInfo.xml") == 0);
    const size_t skip = ((file_size + k_tar_padding_mask) / k_tar_block_bytes) * k_tar_block_bytes;
    if ((skip > (size_t)LONG_MAX) || (fseek(f, (long)skip, SEEK_CUR) != 0)) {
      rc = k_ra8_err_validation_failed;
      break;
    }
  }
  if ((rc == k_ra8_ok) && ((zero_blocks < 2U) || (pages == 0U) || !metadata)) {
    rc = k_ra8_err_validation_failed;
  }
  (void)fclose(f);
  if (rc == k_ra8_ok) {
    report->page_count       = pages;
    report->member_count     = members;
    report->metadata_present = metadata;
  }
  return rc;
}

/**
 * @brief Decode one little-endian 32-bit word
 * @details Combines four bytes without alignment-dependent loads.
 * @param[in] p Pointer to four readable bytes.
 * @return Decoded unsigned value.
 * @retval uint32_t Little-endian value represented at @p p.
 * @pre @p p is non-NULL and addresses four readable bytes.
 * @pre Input storage remains stable during the read.
 * @post Input bytes are unchanged.
 * @post The result is independent of host endianness and alignment.
 * @note Thread-safe: this is a pure decoder.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t get_u32le(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << k_u32_byte_one_shift) |
         ((uint32_t)p[2] << k_u32_byte_two_shift) | ((uint32_t)p[3] << k_u32_byte_three_shift);
}

/**
 * @brief Validate an uncompressed CBT tar held in caller-owned memory
 * @details Mirrors file-based tar checks while advancing only within @p len,
 *          making it suitable for bounded gzip output.
 * @param[in] data Complete uncompressed tar bytes.
 * @param[in] len Readable byte count at @p data.
 * @param[out] report Structural counts populated on success.
 * @return Validation status.
 * @retval k_ra8_ok The tar framing and required semantics are valid.
 * @retval k_ra8_err_validation_failed A record, bound, or semantic rule failed.
 * @pre @p data addresses @p len stable readable bytes.
 * @pre @p report points to writable exclusive storage.
 * @post Input bytes are not modified.
 * @post Success populates image/member counts and metadata presence.
 * @note Thread-safe across distinct byte spans and reports.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
verify_tar_bytes(const uint8_t* data, size_t len, mdl_verify_report_t* report)
{
  size_t   offset      = 0U;
  size_t   pages       = 0U;
  size_t   members     = 0U;
  bool     metadata    = false;
  unsigned zero_blocks = 0U;
  while ((len - offset) >= k_tar_block_bytes) {
    const uint8_t* block = data + offset;
    offset += k_tar_block_bytes;
    bool zero = true;
    for (size_t i = 0U; i < k_tar_block_bytes; ++i) {
      zero = zero && (block[i] == 0U);
    }
    if (zero) {
      if (++zero_blocks == 2U) {
        break;
      }
      continue;
    }
    zero_blocks  = 0U;
    unsigned sum = 0U;
    for (size_t i = 0U; i < k_tar_block_bytes; ++i) {
      sum += ((i >= (size_t)k_tar_checksum_offset) && (i < (size_t)k_tar_checksum_end))
               ? (unsigned)' '
               : block[i];
    }
    size_t expected  = 0U;
    size_t file_size = 0U;
    if (!parse_octal(block + k_tar_checksum_offset,
                     (size_t)(k_tar_checksum_end - k_tar_checksum_offset),
                     &expected) ||
        (sum != expected) ||
        !parse_octal(block + k_tar_size_offset, k_tar_size_bytes, &file_size) ||
        (block[k_tar_type_offset] != '0') || (file_size > (SIZE_MAX - k_tar_padding_mask))) {
      return k_ra8_err_validation_failed;
    }
    const size_t stored =
      ((file_size + k_tar_padding_mask) / k_tar_block_bytes) * k_tar_block_bytes;
    if (stored > (len - offset)) {
      return k_ra8_err_validation_failed;
    }
    char name[k_tar_name_bytes + 1U];
    memcpy(name, block, k_tar_name_bytes);
    name[k_tar_name_bytes] = '\0';
    if (!safe_member_name(name)) {
      return k_ra8_err_validation_failed;
    }
    ++members;
    pages += is_image(name) ? 1U : 0U;
    metadata = metadata || (strcmp(name, "ComicInfo.xml") == 0);
    offset += stored;
  }
  if ((zero_blocks < 2U) || (pages == 0U) || !metadata) {
    return k_ra8_err_validation_failed;
  }
  report->page_count       = pages;
  report->member_count     = members;
  report->metadata_present = metadata;
  return k_ra8_ok;
}

/**
 * @brief Validate an RFC 1952 wrapped CBT using bounded workspace
 * @details Checks the fixed gzip header, exact ISIZE, DEFLATE result, CRC32,
 *          and then delegates semantic validation to the in-memory tar reader.
 * @param[in] path NUL-terminated CBT.GZ path.
 * @param[in,out] ws Caller-owned storage for compressed and raw bytes.
 * @param[out] report Structural report populated after tar validation.
 * @return Validation status.
 * @retval k_ra8_ok Both gzip and nested tar are valid.
 * @retval k_ra8_err_invalid_size Workspace capacity is insufficient.
 * @retval k_ra8_err_validation_failed Framing, decompression, CRC, or tar failed.
 * @retval k_ra8_fail The file could not be opened.
 * @pre All pointers are non-NULL and @p ws is exclusive.
 * @pre The input remains stable while it is read.
 * @post The input file is closed on every opened path.
 * @post Success populates @p report through the nested tar validator.
 * @note Thread-safe across distinct files, workspaces, and reports.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
verify_gzip_tar(const char* path, mdl_export_workspace_t* ws, mdl_verify_report_t* report)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return k_ra8_fail;
  }
  struct stat st;
  if ((fstat(fileno(f), &st) != 0) || (st.st_size < k_gzip_min_bytes) ||
      ((uintmax_t)st.st_size > (uintmax_t)SIZE_MAX)) {
    (void)fclose(f);
    return k_ra8_err_validation_failed;
  }
  const size_t compressed_len = (size_t)st.st_size;
  uint8_t*     compressed = mdl_export_workspace_take(ws, compressed_len, k_archive_arena_align);
  if (compressed == nullptr) {
    (void)fclose(f);
    return k_ra8_err_invalid_size;
  }
  const bool read_ok = fread(compressed, 1U, compressed_len, f) == compressed_len;
  (void)fclose(f);
  if (!read_ok || (compressed[0] != k_gzip_id_one) || (compressed[1] != k_gzip_id_two) ||
      (compressed[2] != k_gzip_method_deflate) || (compressed[3] != 0U)) {
    return k_ra8_err_validation_failed;
  }
  const uint8_t* trailer      = compressed + compressed_len - k_gzip_trailer_bytes;
  const uint32_t expected_crc = get_u32le(trailer);
  const uint32_t raw_len_u32  = get_u32le(trailer + 4U);
  if (raw_len_u32 == 0U) {
    return k_ra8_err_validation_failed;
  }
  const size_t raw_len = (size_t)raw_len_u32;
  uint8_t*     raw     = mdl_export_workspace_take(ws, raw_len, k_archive_arena_align);
  if (raw == nullptr) {
    return k_ra8_err_invalid_size;
  }
  const size_t produced = tinfl_decompress_mem_to_mem(raw,
                                                      raw_len,
                                                      compressed + k_gzip_header_bytes,
                                                      compressed_len - k_gzip_min_bytes,
                                                      0);
  if ((produced == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) || (produced != raw_len) ||
      ((uint32_t)mz_crc32(MZ_CRC32_INIT, raw, raw_len) != expected_crc)) {
    return k_ra8_err_validation_failed;
  }
  return verify_tar_bytes(raw, raw_len, report);
}

typedef struct {
  FILE* file; /**< Seekable file backing the JOF reader callback. */
} file_ctx_t;

/**
 * @brief Read a JOF byte range from a seekable host file
 * @details Adapts FILE seek/read operations to the firmware JOF pread contract.
 * @param[in,out] ctx File adapter context.
 * @param[in] offset Absolute byte offset to read.
 * @param[out] dst Destination buffer.
 * @param[in] len Requested byte count.
 * @param[out] got Actual byte count read.
 * @return Read status.
 * @retval k_ra8_ok Seek/read completed without a stream error.
 * @retval k_ra8_fail Offset, seek, or read failed.
 * @pre Context, destination, and count pointers are non-NULL.
 * @pre @p dst addresses @p len writable bytes.
 * @post `*got` is zero on seek failure and otherwise the fread count.
 * @post The file cursor ends after the returned byte span.
 * @note Not thread-safe for a shared FILE context.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
file_pread(void* ctx, uint64_t offset, uint8_t* dst, size_t len, size_t* got)
{
  *got           = 0U;
  file_ctx_t* fc = (file_ctx_t*)ctx;
  if ((offset > (uint64_t)LONG_MAX) || (fseek(fc->file, (long)offset, SEEK_SET) != 0)) {
    return k_ra8_fail;
  }
  *got = fread(dst, 1U, len, fc->file);
  return (ferror(fc->file) == 0) ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Validate a JOF through the production firmware parser
 * @details Supplies a bounded file adapter to ::ra8_jof_parse and reports the
 *          single logical page plus parsed tile count on success.
 * @param[in] path NUL-terminated JOF file path.
 * @param[out] report Structural counts populated on success.
 * @return JOF parse or file status.
 * @retval k_ra8_ok The production parser accepted the complete file.
 * @retval k_ra8_err_validation_failed File sizing is invalid.
 * @retval k_ra8_fail The file could not be opened or read.
 * @pre @p path and @p report are non-NULL.
 * @pre The file remains stable and seekable during parsing.
 * @post The input file is closed on every opened path.
 * @post Success reports one logical page and the parsed tile count.
 * @note Thread-safe for distinct files and reports.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t verify_jof(const char* path, mdl_verify_report_t* report)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return k_ra8_fail;
  }
  struct stat st;
  if ((fstat(fileno(f), &st) != 0) || (st.st_size <= 0)) {
    (void)fclose(f);
    return k_ra8_err_validation_failed;
  }
  file_ctx_t      ctx = {.file = f};
  ra8_jof_info_t  info;
  const ra8_err_t rc = ra8_jof_parse(file_pread, &ctx, (uint64_t)st.st_size, &info);
  (void)fclose(f);
  if (rc == k_ra8_ok) {
    report->page_count   = 1U;
    report->member_count = (size_t)info.tile_count;
  }
  return rc;
}

ra8_err_t mdl_verify_file(mdl_format_t            fmt,
                          const char*             path,
                          mdl_export_workspace_t* ws,
                          mdl_verify_report_t*    report)
{
  if ((path == nullptr) || (ws == nullptr) || (ws->data == nullptr) || (report == nullptr)) {
    return k_ra8_err_invalid_arg;
  }
  ws->used       = 0U;
  ws->high_water = 0U;
  *report        = (mdl_verify_report_t){.format = fmt};
  switch (fmt) {
    case k_mdl_fmt_cbz:
    case k_mdl_fmt_epub:
      return verify_zip(fmt, path, ws, report);
    case k_mdl_fmt_cbt:
      return verify_tar(path, report);
    case k_mdl_fmt_jof:
      return verify_jof(path, report);
    case k_mdl_fmt_cbt_gz:
      return verify_gzip_tar(path, ws, report);
    case k_mdl_fmt_cbr:
    case k_mdl_fmt_cbt_xz:
    case k_mdl_fmt_rabook:
      return k_ra8_err_not_supported;
    case k_mdl_fmt_loose:
    case k_mdl_fmt_invalid:
    default:
      return k_ra8_err_invalid_arg;
  }
}
