/**
 * @file mdl_verify.c
 * @brief Bounded structural validators for native archive formats.
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
  size_t      bytes;
  max_align_t align;
} mdl_alloc_header_t;

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

RA8_INTERNAL static bool is_image(const char* name)
{
  return ends_ci(name, ".jpg") || ends_ci(name, ".jpeg") || ends_ci(name, ".png") ||
         ends_ci(name, ".webp") || ends_ci(name, ".gif") || ends_ci(name, ".bmp");
}

/** @brief Reject absolute and parent-traversing container member names. */
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
    const char*  suffix;
    mdl_format_t fmt;
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

RA8_INTERNAL static size_t
discard_zip(void* opaque, mz_uint64 offset, const void* data, size_t bytes)
{
  (void)opaque;
  (void)offset;
  (void)data;
  return bytes;
}

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

RA8_INTERNAL static ra8_err_t verify_tar(const char* path, mdl_verify_report_t* report)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return k_ra8_fail;
  }
  uint8_t   block[512];
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
      sum += ((i >= 148U) && (i < 156U)) ? (unsigned)' ' : block[i];
    }
    size_t expected  = 0U;
    size_t file_size = 0U;
    if (!parse_octal(block + 148U, 8U, &expected) || (sum != expected) ||
        !parse_octal(block + 124U, 12U, &file_size) || (block[100U] != '0')) {
      rc = k_ra8_err_validation_failed;
      break;
    }
    char name[101];
    memcpy(name, block, 100U);
    name[100] = '\0';
    if (!safe_member_name(name)) {
      rc = k_ra8_err_validation_failed;
      break;
    }
    ++members;
    pages += is_image(name) ? 1U : 0U;
    metadata          = metadata || (strcmp(name, "ComicInfo.xml") == 0);
    const size_t skip = ((file_size + 511U) / 512U) * 512U;
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

RA8_INTERNAL static uint32_t get_u32le(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
         ((uint32_t)p[3] << 24U);
}

RA8_INTERNAL static ra8_err_t
verify_tar_bytes(const uint8_t* data, size_t len, mdl_verify_report_t* report)
{
  size_t   offset      = 0U;
  size_t   pages       = 0U;
  size_t   members     = 0U;
  bool     metadata    = false;
  unsigned zero_blocks = 0U;
  while ((len - offset) >= 512U) {
    const uint8_t* block = data + offset;
    offset += 512U;
    bool zero = true;
    for (size_t i = 0U; i < 512U; ++i) {
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
    for (size_t i = 0U; i < 512U; ++i) {
      sum += ((i >= 148U) && (i < 156U)) ? (unsigned)' ' : block[i];
    }
    size_t expected  = 0U;
    size_t file_size = 0U;
    if (!parse_octal(block + 148U, 8U, &expected) || (sum != expected) ||
        !parse_octal(block + 124U, 12U, &file_size) || (block[100U] != '0') ||
        (file_size > (SIZE_MAX - 511U))) {
      return k_ra8_err_validation_failed;
    }
    const size_t stored = ((file_size + 511U) / 512U) * 512U;
    if (stored > (len - offset)) {
      return k_ra8_err_validation_failed;
    }
    char name[101];
    memcpy(name, block, 100U);
    name[100] = '\0';
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

RA8_INTERNAL static ra8_err_t
verify_gzip_tar(const char* path, mdl_export_workspace_t* ws, mdl_verify_report_t* report)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return k_ra8_fail;
  }
  struct stat st;
  if ((fstat(fileno(f), &st) != 0) || (st.st_size < 18) ||
      ((uintmax_t)st.st_size > (uintmax_t)SIZE_MAX)) {
    (void)fclose(f);
    return k_ra8_err_validation_failed;
  }
  const size_t compressed_len = (size_t)st.st_size;
  uint8_t*     compressed     = mdl_export_workspace_take(ws, compressed_len, 8U);
  if (compressed == nullptr) {
    (void)fclose(f);
    return k_ra8_err_invalid_size;
  }
  const bool read_ok = fread(compressed, 1U, compressed_len, f) == compressed_len;
  (void)fclose(f);
  if (!read_ok || (compressed[0] != 0x1FU) || (compressed[1] != 0x8BU) || (compressed[2] != 8U) ||
      (compressed[3] != 0U)) {
    return k_ra8_err_validation_failed;
  }
  const uint8_t* trailer      = compressed + compressed_len - 8U;
  const uint32_t expected_crc = get_u32le(trailer);
  const uint32_t raw_len_u32  = get_u32le(trailer + 4U);
  if (raw_len_u32 == 0U) {
    return k_ra8_err_validation_failed;
  }
  const size_t raw_len = (size_t)raw_len_u32;
  uint8_t*     raw     = mdl_export_workspace_take(ws, raw_len, 8U);
  if (raw == nullptr) {
    return k_ra8_err_invalid_size;
  }
  const size_t produced =
    tinfl_decompress_mem_to_mem(raw, raw_len, compressed + 10U, compressed_len - 18U, 0);
  if ((produced == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) || (produced != raw_len) ||
      ((uint32_t)mz_crc32(MZ_CRC32_INIT, raw, raw_len) != expected_crc)) {
    return k_ra8_err_validation_failed;
  }
  return verify_tar_bytes(raw, raw_len, report);
}

typedef struct {
  FILE* file;
} file_ctx_t;

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
