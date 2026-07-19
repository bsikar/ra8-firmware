/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_export.c
 * @brief Package a chapter folder into a reader-openable container.
 *
 * @details
 * The self-contained formats are produced from vendored, in-tree code with no
 * system library or external process -- CBZ via the firmware's own miniz ZIP
 * writer, CBT via a hand-written ustar tar, CBT.GZ via miniz DEFLATE + RFC-1952
 * framing. Two formats are OPTIONAL external tools, spawned only when installed:
 * CBR (`rar`, since RAR has no open writer) and CBT.XZ (`xz`, since the in-tree
 * `xz_embedded` is decode-only -- vendoring a full encoder was judged overkill).
 * The gzip/xz variants wrap the whole tar -- the reader's `ra8_comic_open_wrapped`
 * decodes the layer, then re-probes the inner archive; xz is emitted with a
 * CRC32 check and a 1 MiB dictionary so the on-device xz scratch accepts it.
 */
#include "mdl_export.h"

#include <crt_externs.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_jpeg_sw.h"
#include "ra8_log.h"
#include "ra8_tileatlas.h"
#include "ra8_tileatlas_produce.h"

/** @brief Page-list limits. */
typedef enum : uint16_t {
  k_max_pages = 2048, /**< Max page entries per chapter. */
  k_name_max  = 256,  /**< Max entry-name bytes.         */
} mdl_export_limits_t;

/** @brief ustar (POSIX tar) header field offsets and lengths. */
typedef enum : uint16_t {
  k_tar_block   = 512, /**< Tar block size.           */
  k_off_name    = 0,   /**< name field offset.        */
  k_len_name    = 100, /**< name field width.         */
  k_off_mode    = 100, /**< mode field offset.        */
  k_off_uid     = 108, /**< uid field offset.         */
  k_off_gid     = 116, /**< gid field offset.         */
  k_len_id      = 8,   /**< mode/uid/gid field width. */
  k_off_size    = 124, /**< size field offset.        */
  k_len_size    = 12,  /**< size field width.         */
  k_off_mtime   = 136, /**< mtime field offset.       */
  k_len_mtime   = 12,  /**< mtime field width.        */
  k_off_chksum  = 148, /**< checksum field offset.    */
  k_len_chksum  = 8,   /**< checksum field width.     */
  k_off_type    = 156, /**< typeflag field offset.    */
  k_off_magic   = 257, /**< "ustar" magic offset.     */
  k_len_magic   = 6,   /**< "ustar" magic width.      */
  k_off_version = 263, /**< version field offset.     */
} mdl_tar_layout_t;

/** @brief File mode written into tar headers and the zip STORE flag source. */
typedef enum : uint16_t {
  k_file_mode = 0644, /**< Regular-file permission bits (octal). */
} mdl_tar_mode_t;

/** @brief gzip framing constants (RFC 1952). */
typedef enum : uint8_t {
  k_gz_id1 = 0x1F, /**< gzip magic byte 1.           */
  k_gz_id2 = 0x8B, /**< gzip magic byte 2.           */
  k_gz_cm  = 0x08, /**< compression method: DEFLATE. */
  k_gz_os  = 0xFF, /**< OS: unknown.                 */
} mdl_gzip_hdr_t;

/** @brief Byte-serialisation constants. */
typedef enum : uint16_t {
  k_byte_bits    = 8,    /**< Bits per byte.            */
  k_byte_mask    = 0xFF, /**< Low-byte mask.            */
  k_u32_bytes    = 4,    /**< Bytes in a u32.           */
  k_gzip_hdr_len = 10,   /**< gzip fixed header length. */
} mdl_serial_t;

mdl_format_t mdl_format_from_str(const char* s)
{
  if ((s == nullptr) || (strcmp(s, "loose") == 0)) {
    return k_mdl_fmt_loose;
  }
  if (strcmp(s, "cbz") == 0) {
    return k_mdl_fmt_cbz;
  }
  if (strcmp(s, "cbt") == 0) {
    return k_mdl_fmt_cbt;
  }
  if (strcmp(s, "cbr") == 0) {
    return k_mdl_fmt_cbr;
  }
  if (strcmp(s, "cbt.xz") == 0) {
    return k_mdl_fmt_cbt_xz;
  }
  if (strcmp(s, "cbt.gz") == 0) {
    return k_mdl_fmt_cbt_gz;
  }
  if (strcmp(s, "epub") == 0) {
    return k_mdl_fmt_epub;
  }
  if (strcmp(s, "jof") == 0) {
    return k_mdl_fmt_jof;
  }
  if (strcmp(s, "rabook") == 0) {
    return k_mdl_fmt_rabook;
  }
  return k_mdl_fmt_invalid;
}

const char* mdl_format_ext(mdl_format_t fmt)
{
  switch (fmt) {
    case k_mdl_fmt_cbz:
      return "cbz";
    case k_mdl_fmt_cbt:
      return "cbt";
    case k_mdl_fmt_cbr:
      return "cbr";
    case k_mdl_fmt_cbt_xz:
      return "cbt.xz";
    case k_mdl_fmt_cbt_gz:
      return "cbt.gz";
    case k_mdl_fmt_epub:
      return "epub";
    case k_mdl_fmt_jof:
      return "jof";
    case k_mdl_fmt_rabook:
      return "rabook";
    case k_mdl_fmt_loose:
    case k_mdl_fmt_invalid:
    default:
      return "";
  }
}

/** @brief qsort comparator over fixed-width name rows. */
RA8_INTERNAL static int name_cmp(const void* a, const void* b)
{
  return strcmp((const char*)a, (const char*)b);
}

/** @brief True if `name` ends (case-insensitively) with `suffix`. */
RA8_INTERNAL static bool ends_with_ci(const char* name, const char* suffix)
{
  const size_t nl = strlen(name);
  const size_t sl = strlen(suffix);
  if (sl > nl) {
    return false;
  }
  const char* tail = name + (nl - sl);
  for (size_t i = 0U; i < sl; ++i) {
    if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief True if `name` is a raster page image a reader engine can decode.
 *
 * Packaging must include ONLY page images. A chapter folder often also holds
 * this tool's own prior output (a sibling `.jof`/`.cbz`, a `.tar.tmp`) or OS
 * junk; folding those into an archive makes the reader choke when it decodes a
 * non-image "page" (the 0x107 that bit re-runs). Filtering by extension keeps
 * packaging idempotent -- re-running any format on a folder is safe.
 */
RA8_INTERNAL static bool is_page_image(const char* name)
{
  static const char* const k_img_exts[] = {".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp"};
  for (size_t i = 0U; i < (sizeof(k_img_exts) / sizeof(k_img_exts[0])); ++i) {
    if (ends_with_ci(name, k_img_exts[i])) {
      return true;
    }
  }
  return false;
}

/** @brief List a chapter's page-image files (only) into `names`, sorted. */
RA8_INTERNAL static size_t list_pages(const char* dir, char names[][k_name_max], size_t cap)
{
  DIR* d = opendir(dir);
  if (d == nullptr) {
    return 0U;
  }
  size_t               n = 0U;
  const struct dirent* e = readdir(d);
  while ((e != nullptr) && (n < cap)) {
    /* Skip hidden / AppleDouble, and anything that is not a page image so a
     * dir already holding this tool's output re-packages cleanly. */
    if ((e->d_name[0] != '.') && is_page_image(e->d_name)) {
      (void)snprintf(names[n], k_name_max, "%s", e->d_name);
      ++n;
    }
    e = readdir(d);
  }
  (void)closedir(d);
  qsort(names, n, k_name_max, name_cmp);
  return n;
}

/** @brief Byte size of a file, or 0 on error. */
RA8_INTERNAL static size_t file_size(const char* path)
{
  struct stat st = {};
  if (stat(path, &st) != 0) {
    return 0U;
  }
  return (size_t)st.st_size;
}

/** @brief Round `n` up to a whole tar block. */
RA8_INTERNAL static size_t round_block(size_t n)
{
  return ((n + (size_t)k_tar_block - 1U) / (size_t)k_tar_block) * (size_t)k_tar_block;
}

/** @brief Write a ustar header for `name`/`size` into a zeroed 512-byte block. */
RA8_INTERNAL static void tar_header(uint8_t* blk, const char* name, size_t size)
{
  memset(blk, 0, k_tar_block);
  (void)snprintf((char*)blk + k_off_name, k_len_name, "%s", name);
  (void)snprintf((char*)blk + k_off_mode, k_len_id, "%07o", (unsigned)k_file_mode);
  (void)snprintf((char*)blk + k_off_uid, k_len_id, "%07o", 0U);
  (void)snprintf((char*)blk + k_off_gid, k_len_id, "%07o", 0U);
  (void)snprintf((char*)blk + k_off_size, k_len_size, "%011zo", size);
  (void)snprintf((char*)blk + k_off_mtime, k_len_mtime, "%011o", 0U);
  blk[k_off_type] = '0';
  memcpy(blk + k_off_magic, "ustar", k_len_magic);
  memcpy(blk + k_off_version, "00", 2U);

  memset(blk + k_off_chksum, ' ', k_len_chksum);
  unsigned sum = 0U;
  for (size_t i = 0U; i < (size_t)k_tar_block; ++i) {
    sum += blk[i];
  }
  (void)snprintf((char*)blk + k_off_chksum, k_len_chksum - 1U, "%06o", sum);
  blk[k_off_chksum + k_len_chksum - 1U] = ' ';
}

/** @brief Build a tar of `names` from `dir` into a freshly-malloc'd buffer. */
RA8_INTERNAL static ra8_err_t build_tar(const char* dir,
                                        char        names[][k_name_max],
                                        size_t      count,
                                        uint8_t**   out_buf,
                                        size_t*     out_len)
{
  size_t total = 2U * (size_t)k_tar_block; /* two trailing zero blocks */
  for (size_t i = 0U; i < count; ++i) {
    char path[PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    total += (size_t)k_tar_block + round_block(file_size(path));
  }
  uint8_t* buf = (uint8_t*)calloc(1U, total);
  if (buf == nullptr) {
    return k_ra8_err_no_mem;
  }

  size_t pos = 0U;
  for (size_t i = 0U; i < count; ++i) {
    char path[PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    const size_t sz = file_size(path);
    tar_header(buf + pos, names[i], sz);
    pos += (size_t)k_tar_block;
    FILE* f = fopen(path, "rb");
    if ((f == nullptr) || (fread(buf + pos, 1U, sz, f) != sz)) {
      if (f != nullptr) {
        (void)fclose(f);
      }
      free(buf);
      return k_ra8_fail;
    }
    (void)fclose(f);
    pos += round_block(sz);
  }
  *out_buf = buf;
  *out_len = total;
  return k_ra8_ok;
}

/** @brief Write `len` bytes of `buf` to `path`. */
RA8_INTERNAL static ra8_err_t write_buf(const char* path, const uint8_t* buf, size_t len)
{
  FILE* f = fopen(path, "wb");
  if (f == nullptr) {
    return k_ra8_fail;
  }
  const size_t w  = fwrite(buf, 1U, len, f);
  const bool   ok = (fclose(f) == 0) && (w == len);
  return ok ? k_ra8_ok : k_ra8_fail;
}

/** @brief Append a little-endian u32 to `f`. */
RA8_INTERNAL static bool put_u32le(FILE* f, uint32_t v)
{
  uint8_t b[k_u32_bytes] = {};
  for (size_t i = 0U; i < (size_t)k_u32_bytes; ++i) {
    b[i] = (uint8_t)(v & (uint32_t)k_byte_mask);
    v >>= (uint32_t)k_byte_bits;
  }
  return fwrite(b, 1U, sizeof(b), f) == sizeof(b);
}

/** @brief gzip `buf` to `path` using vendored miniz DEFLATE + RFC-1952 framing. */
RA8_INTERNAL static ra8_err_t gzip_buf(const char* path, const uint8_t* buf, size_t len)
{
  size_t dfl_len = 0U;
  void*  dfl     = tdefl_compress_mem_to_heap(buf, len, &dfl_len, TDEFL_DEFAULT_MAX_PROBES);
  if (dfl == nullptr) {
    return k_ra8_fail;
  }
  FILE* f = fopen(path, "wb");
  if (f == nullptr) {
    mz_free(dfl);
    return k_ra8_fail;
  }
  const uint8_t hdr[k_gzip_hdr_len] =
    {k_gz_id1, k_gz_id2, k_gz_cm, 0U, 0U, 0U, 0U, 0U, 0U, k_gz_os};
  bool ok = (fwrite(hdr, 1U, sizeof(hdr), f) == sizeof(hdr)) &&
            (fwrite(dfl, 1U, dfl_len, f) == dfl_len) &&
            put_u32le(f, mz_crc32(MZ_CRC32_INIT, buf, len)) &&
            put_u32le(f, (uint32_t)len); /* ISIZE = len mod 2^32 */
  ok      = (fclose(f) == 0) && ok;
  mz_free(dfl);
  return ok ? k_ra8_ok : k_ra8_fail;
}

/** @brief Write a CBZ (STORE ZIP) via the vendored miniz writer. */
RA8_INTERNAL static ra8_err_t
export_cbz(const char* dir, char names[][k_name_max], size_t count, const char* out_path)
{
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  if (mz_zip_writer_init_file(&zip, out_path, 0) == MZ_FALSE) {
    return k_ra8_fail;
  }
  for (size_t i = 0U; i < count; ++i) {
    char src[PATH_MAX];
    (void)snprintf(src, sizeof(src), "%s/%s", dir, names[i]);
    if (mz_zip_writer_add_file(&zip, names[i], src, nullptr, 0, MZ_NO_COMPRESSION) == MZ_FALSE) {
      (void)mz_zip_writer_end(&zip);
      return k_ra8_fail;
    }
  }
  const bool ok = (mz_zip_writer_finalize_archive(&zip) != MZ_FALSE);
  (void)mz_zip_writer_end(&zip);
  return ok ? k_ra8_ok : k_ra8_fail;
}

/** @brief Build a tar then hand it to `compress` (gzip/xz) written to `out`. */
RA8_INTERNAL static ra8_err_t
export_tar_wrapped(const char* dir,
                   char        names[][k_name_max],
                   size_t      count,
                   const char* out_path,
                   ra8_err_t (*compress)(const char*, const uint8_t*, size_t))
{
  uint8_t*  tarbuf = nullptr;
  size_t    tarlen = 0U;
  ra8_err_t rc     = build_tar(dir, names, count, &tarbuf, &tarlen);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = compress(out_path, tarbuf, tarlen);
  free(tarbuf);
  return rc;
}

/** @brief Spawn the proprietary `rar` tool for CBR (the sole external path). */
RA8_INTERNAL static ra8_err_t export_cbr(const char* dir, const char* out_path)
{
  posix_spawn_file_actions_t actions;
  (void)posix_spawn_file_actions_init(&actions);
  (void)posix_spawn_file_actions_addchdir(&actions, dir);
  const char* const argv[] = {"rar", "a", "-ep1", "-idq", out_path, ".", nullptr};
  pid_t             pid    = 0;
  const int         rc =
    posix_spawnp(&pid, argv[0], &actions, nullptr, (char* const*)argv, *_NSGetEnviron());
  (void)posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    (void)fprintf(stderr,
                  "media_dl: cbr needs the 'rar' tool (brew install rar): %s\n",
                  strerror(rc));
    return k_ra8_err_not_supported;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return k_ra8_fail;
  }
  const bool ok = (WIFEXITED(status) != 0) && (WEXITSTATUS(status) == 0);
  return ok ? k_ra8_ok : k_ra8_fail;
}

/** @brief Spawn `argv` with stdout redirected to `out_path`; wait for it. */
RA8_INTERNAL static ra8_err_t run_to_file(const char* const argv[], const char* out_path)
{
  posix_spawn_file_actions_t actions;
  (void)posix_spawn_file_actions_init(&actions);
  (void)posix_spawn_file_actions_addopen(&actions,
                                         STDOUT_FILENO,
                                         out_path,
                                         O_WRONLY | O_CREAT | O_TRUNC,
                                         (mode_t)k_file_mode);
  pid_t     pid = 0;
  const int rc =
    posix_spawnp(&pid, argv[0], &actions, nullptr, (char* const*)argv, *_NSGetEnviron());
  (void)posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    (void)fprintf(stderr,
                  "media_dl: cbt.xz needs the 'xz' tool (brew install xz): %s\n",
                  strerror(rc));
    return k_ra8_err_not_supported;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return k_ra8_fail;
  }
  const bool ok = (WIFEXITED(status) != 0) && (WEXITSTATUS(status) == 0);
  return ok ? k_ra8_ok : k_ra8_fail;
}

/** @brief Optional external xz: tar the folder, then `xz` it with reader-safe flags. */
RA8_INTERNAL static ra8_err_t
export_cbt_xz(const char* dir, char names[][k_name_max], size_t count, const char* out_path)
{
  uint8_t*  tarbuf = nullptr;
  size_t    tarlen = 0U;
  ra8_err_t rc     = build_tar(dir, names, count, &tarbuf, &tarlen);
  if (rc != k_ra8_ok) {
    return rc;
  }
  char tmp[PATH_MAX];
  (void)snprintf(tmp, sizeof(tmp), "%s.tar.tmp", out_path);
  rc = write_buf(tmp, tarbuf, tarlen);
  free(tarbuf);
  if (rc != k_ra8_ok) {
    return rc;
  }
  /* CRC32 check + 1 MiB dict so the on-device xz scratch accepts the stream. */
  const char* const a[] = {"xz", "--check=crc32", "--lzma2=preset=6,dict=1MiB", "-c", tmp, nullptr};
  rc                    = run_to_file(a, out_path);
  (void)remove(tmp);
  return rc;
}

/* --- EPUB (self-contained: a valid EPUB3 of the page images via miniz) ---- */

/** @brief EPUB string-buffer sizing (grows with the page count). */
typedef enum : uint32_t {
  k_epub_frag_max       = 512U,  /**< One manifest/spine/nav fragment. */
  k_epub_xhtml_max      = 1024U, /**< One page's xhtml document.       */
  k_epub_base_bytes     = 4096U, /**< Fixed opf/nav overhead.          */
  k_epub_per_page_bytes = 512U,  /**< Per-page opf/nav growth.         */
} mdl_epub_size_t;

/** @brief OCF container pointing at the OPF package (fixed). */
static const char* const k_epub_container_xml =
  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
  "<container version=\"1.0\" "
  "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles>"
  "<rootfile full-path=\"OEBPS/content.opf\" "
  "media-type=\"application/oebps-package+xml\"/></rootfiles></container>";

/** @brief Image media-type for a page filename extension. */
RA8_INTERNAL static const char* epub_media_type(const char* name)
{
  const char* dot = strrchr(name, '.');
  if (dot != nullptr) {
    if (strcmp(dot, ".png") == 0) {
      return "image/png";
    }
    if (strcmp(dot, ".gif") == 0) {
      return "image/gif";
    }
    if (strcmp(dot, ".webp") == 0) {
      return "image/webp";
    }
    if (strcmp(dot, ".bmp") == 0) {
      return "image/bmp";
    }
  }
  return "image/jpeg";
}

/** @brief Append `text` to NUL-terminated `dst` if it fits (truncation-safe). */
RA8_INTERNAL static void str_cat(char* dst, size_t cap, const char* text)
{
  const size_t cur = strlen(dst);
  if (cur + 1U < cap) {
    (void)snprintf(dst + cur, cap - cur, "%s", text);
  }
}

/** @brief Add an in-memory string as a STORED zip entry. */
RA8_INTERNAL static bool epub_add_str(mz_zip_archive* zip, const char* name, const char* body)
{
  return mz_zip_writer_add_mem(zip, name, body, strlen(body), MZ_NO_COMPRESSION) != MZ_FALSE;
}

/** @brief Add one page's xhtml + image, and append its opf/spine/nav fragments. */
RA8_INTERNAL static ra8_err_t epub_add_page(mz_zip_archive* zip,
                                            const char*     dir,
                                            const char*     name,
                                            size_t          idx,
                                            char*           mani,
                                            char*           spine,
                                            char*           nav,
                                            size_t          cap)
{
  const unsigned n = (unsigned)(idx + 1U);
  char           xhtml[k_epub_xhtml_max];
  (void)snprintf(xhtml,
                 sizeof(xhtml),
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Page %u"
                 "</title></head><body><img src=\"images/%s\" alt=\"Page %u\"/>"
                 "</body></html>",
                 n,
                 name,
                 n);
  char entry[k_name_max];
  (void)snprintf(entry, sizeof(entry), "OEBPS/page_%03u.xhtml", n);
  if (!epub_add_str(zip, entry, xhtml)) {
    return k_ra8_fail;
  }
  char src[PATH_MAX];
  (void)snprintf(src, sizeof(src), "%s/%s", dir, name);
  (void)snprintf(entry, sizeof(entry), "OEBPS/images/%s", name);
  if (mz_zip_writer_add_file(zip, entry, src, nullptr, 0, MZ_NO_COMPRESSION) == MZ_FALSE) {
    return k_ra8_fail;
  }
  char frag[k_epub_frag_max];
  (void)snprintf(frag,
                 sizeof(frag),
                 "<item id=\"pg%zu\" href=\"page_%03u.xhtml\" "
                 "media-type=\"application/xhtml+xml\"/>"
                 "<item id=\"img%zu\" href=\"images/%s\" media-type=\"%s\"/>",
                 idx,
                 n,
                 idx,
                 name,
                 epub_media_type(name));
  str_cat(mani, cap, frag);
  (void)snprintf(frag, sizeof(frag), "<itemref idref=\"pg%zu\"/>", idx);
  str_cat(spine, cap, frag);
  (void)snprintf(frag, sizeof(frag), "<li><a href=\"page_%03u.xhtml\">Page %u</a></li>", n, n);
  str_cat(nav, cap, frag);
  return k_ra8_ok;
}

/** @brief Build + add content.opf and nav.xhtml, then finalize the archive. */
RA8_INTERNAL static ra8_err_t
epub_add_meta(mz_zip_archive* zip, const char* mani, const char* spine, const char* nav)
{
  const size_t opf_cap = strlen(mani) + strlen(spine) + (size_t)k_epub_base_bytes;
  const size_t nav_cap = strlen(nav) + (size_t)k_epub_base_bytes;
  char*        opf     = (char*)malloc(opf_cap);
  char*        navdoc  = (char*)malloc(nav_cap);
  if ((opf == nullptr) || (navdoc == nullptr)) {
    free(opf);
    free(navdoc);
    return k_ra8_err_no_mem;
  }
  (void)snprintf(opf,
                 opf_cap,
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
                 "unique-identifier=\"bookid\"><metadata "
                 "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                 "<dc:identifier id=\"bookid\">media_dl-chapter</dc:identifier>"
                 "<dc:title>chapter</dc:title><dc:language>en</dc:language>"
                 "<meta property=\"dcterms:modified\">2026-01-01T00:00:00Z</meta>"
                 "</metadata><manifest><item id=\"nav\" href=\"nav.xhtml\" "
                 "media-type=\"application/xhtml+xml\" properties=\"nav\"/>%s</manifest>"
                 "<spine>%s</spine></package>",
                 mani,
                 spine);
  (void)snprintf(navdoc,
                 nav_cap,
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                 "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>Contents"
                 "</title></head><body><nav epub:type=\"toc\"><ol>%s</ol></nav>"
                 "</body></html>",
                 nav);
  const bool ok = epub_add_str(zip, "OEBPS/content.opf", opf) &&
                  epub_add_str(zip, "OEBPS/nav.xhtml", navdoc) &&
                  (mz_zip_writer_finalize_archive(zip) != MZ_FALSE);
  free(opf);
  free(navdoc);
  return ok ? k_ra8_ok : k_ra8_fail;
}

/** @brief Package `dir`'s pages into a valid EPUB3 at `out_path`. */
RA8_INTERNAL static ra8_err_t
export_epub(const char* dir, char names[][k_name_max], size_t count, const char* out_path)
{
  const size_t cap   = (size_t)k_epub_base_bytes + (count * (size_t)k_epub_per_page_bytes);
  char*        mani  = (char*)calloc(1U, cap);
  char*        spine = (char*)calloc(1U, cap);
  char*        nav   = (char*)calloc(1U, cap);
  if ((mani == nullptr) || (spine == nullptr) || (nav == nullptr)) {
    free(mani);
    free(spine);
    free(nav);
    return k_ra8_err_no_mem;
  }
  mz_zip_archive zip;
  memset(&zip, 0, sizeof(zip));
  const bool zip_open = (mz_zip_writer_init_file(&zip, out_path, 0) != MZ_FALSE);
  ra8_err_t  rc       = zip_open ? k_ra8_ok : k_ra8_fail;
  if ((rc == k_ra8_ok) && (!epub_add_str(&zip, "mimetype", "application/epub+zip") ||
                           !epub_add_str(&zip, "META-INF/container.xml", k_epub_container_xml))) {
    rc = k_ra8_fail;
  }
  for (size_t i = 0U; (rc == k_ra8_ok) && (i < count); ++i) {
    rc = epub_add_page(&zip, dir, names[i], i, mani, spine, nav, cap);
  }
  if (rc == k_ra8_ok) {
    rc = epub_add_meta(&zip, mani, spine, nav);
  }
  if (zip_open) {
    (void)mz_zip_writer_end(&zip);
  }
  free(mani);
  free(spine);
  free(nav);
  if (rc != k_ra8_ok) {
    (void)remove(out_path);
  }
  return rc;
}

/* --- JOF (native tile atlas: reuse the firmware ra8_tileatlas producer) -- */

/**
 * @enum mdl_jof_geom_t
 * @brief JOF atlas tiling geometry for a long-strip page.
 * @details `tile_w` is set to the full image width, so each tile is one
 *          full-width horizontal band -- the shape the ra8_longstrip engine
 *          scrolls a viewport at a time.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_jof_band_h = 256, /**< Tile-band height in pixels. */
} mdl_jof_geom_t;

/** @brief Pull cursor over an in-RAM encoded image. */
typedef struct {
  const uint8_t* data; /**< Encoded bytes. */
  size_t         len;  /**< Total length.  */
  size_t         pos;  /**< Read cursor.   */
} jof_pull_ctx_t;

/** @brief ra8_log byte sink -> stderr (host-safe; avoids the ITM MMIO read). */
RA8_INTERNAL static void jof_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)fputc((int)byte, stderr);
}

/** @brief Producer pull callback: hand over the next source bytes. */
RA8_INTERNAL static ra8_err_t jof_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got)
{
  jof_pull_ctx_t* s = (jof_pull_ctx_t*)ctx;
  size_t          n = s->len - s->pos;
  if (n > cap) {
    n = cap;
  }
  memcpy(buf, s->data + s->pos, n);
  s->pos += n;
  *got = n;
  return k_ra8_ok;
}

/** @brief Producer sink callback: append atlas bytes to an open file. */
RA8_INTERNAL static ra8_err_t jof_sink(void* ctx, const uint8_t* buf, size_t len)
{
  return (fwrite(buf, 1U, len, (FILE*)ctx) == len) ? k_ra8_ok : k_ra8_fail;
}

/** @brief Read a whole file into a malloc'd buffer. */
RA8_INTERNAL static ra8_err_t slurp(const char* path, uint8_t** out_buf, size_t* out_len)
{
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    return k_ra8_fail;
  }
  (void)fseek(f, 0, SEEK_END);
  const long sz = ftell(f);
  (void)fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
    (void)fclose(f);
    return k_ra8_fail;
  }
  uint8_t* buf = (uint8_t*)malloc((size_t)sz);
  if ((buf == nullptr) || (fread(buf, 1U, (size_t)sz, f) != (size_t)sz)) {
    free(buf);
    (void)fclose(f);
    return k_ra8_fail;
  }
  (void)fclose(f);
  *out_buf = buf;
  *out_len = (size_t)sz;
  return k_ra8_ok;
}

/** @brief Transcode one baseline-JPEG page to a `.jof` full-width-column atlas. */
RA8_INTERNAL static ra8_err_t jof_one(const char* in_path, const char* out_path)
{
  uint8_t*  src  = nullptr;
  size_t    slen = 0U;
  ra8_err_t rc   = slurp(in_path, &src, &slen);
  if (rc != k_ra8_ok) {
    return rc;
  }
  uint16_t w = 0U;
  uint16_t h = 0U;
  if (ra8_jpeg_sw_get_dimensions(src, (uint32_t)slen, &w, &h) != k_ra8_ok) {
    free(src);
    return k_ra8_err_not_supported;
  }
  const uint16_t tile_h   = (h < (uint16_t)k_jof_band_h) ? h : (uint16_t)k_jof_band_h;
  const uint32_t work_cap = ra8_tileatlas_work_bytes(w, h, w, tile_h);
  uint8_t*       work     = (work_cap > 0U) ? (uint8_t*)malloc(work_cap) : nullptr;
  FILE*          out      = (work != nullptr) ? fopen(out_path, "wb") : nullptr;
  if (out == nullptr) {
    free(work);
    free(src);
    return (work_cap == 0U) ? k_ra8_err_invalid_size : k_ra8_fail;
  }
  jof_pull_ctx_t              pull = {.data = src, .len = slen, .pos = 0U};
  ra8_tileatlas_produce_cfg_t cfg  = {.pull          = jof_pull,
                                      .pull_ctx      = &pull,
                                      .sink          = jof_sink,
                                      .sink_ctx      = out,
                                      .tile_w        = w,
                                      .tile_h        = tile_h,
                                      .codec         = (uint8_t)k_ra8_tileatlas_codec_deflate,
                                      .max_width     = w,
                                      .max_height    = h,
                                      .work          = work,
                                      .work_cap      = work_cap,
                                      .webp_work     = nullptr,
                                      .webp_work_cap = 0U};
  ra8_tileatlas_info_t        info = {};
  rc                               = ra8_tileatlas_produce(&cfg, &info);
  (void)fclose(out);
  free(work);
  free(src);
  if (rc != k_ra8_ok) {
    (void)remove(out_path);
  }
  return rc;
}

/** @brief Convert every page in `dir` to a sibling `.jof` atlas (in place). */
RA8_INTERNAL static ra8_err_t
export_jof(const char* dir, const char names[][k_name_max], size_t count)
{
  ra8_log_set_byte_sink(jof_log_sink, nullptr);
  ra8_err_t rc = k_ra8_ok;
  for (size_t i = 0U; (rc == k_ra8_ok) && (i < count); ++i) {
    char in_path[PATH_MAX];
    char stem[k_name_max];
    char out_path[PATH_MAX];
    (void)snprintf(in_path, sizeof(in_path), "%s/%s", dir, names[i]);
    (void)snprintf(stem, sizeof(stem), "%s", names[i]);
    char* dot = strrchr(stem, '.');
    if (dot != nullptr) {
      *dot = '\0';
    }
    (void)snprintf(out_path, sizeof(out_path), "%s/%s.jof", dir, stem);
    rc = jof_one(in_path, out_path);
  }
  return rc;
}

/* --- RABOOK (optional external: the tools/epub_compile python emitter) ---- */

/** @brief Run cbz_compile.py to turn `cbz` into an RBKC `.rabook` at `out_path`. */
RA8_INTERNAL static ra8_err_t run_rabook_python(const char* cbz, const char* out_path)
{
#ifdef MDL_EPUB_COMPILE_DIR
  char script[PATH_MAX];
  (void)snprintf(script, sizeof(script), "%s/cbz_compile.py", MDL_EPUB_COMPILE_DIR);
  (void)setenv("PYTHONPATH", MDL_EPUB_COMPILE_DIR, 1);
  const char* const argv[] = {"python3", script, cbz, out_path, "--rtl", nullptr};
  pid_t             pid    = 0;
  const int         rc =
    posix_spawnp(&pid, argv[0], nullptr, nullptr, (char* const*)argv, *_NSGetEnviron());
  if (rc != 0) {
    (void)fprintf(stderr, "media_dl: rabook needs python3 + Pillow: %s\n", strerror(rc));
    return k_ra8_err_not_supported;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    return k_ra8_fail;
  }
  const bool ok = (WIFEXITED(status) != 0) && (WEXITSTATUS(status) == 0);
  return ok ? k_ra8_ok : k_ra8_fail;
#else
  (void)cbz;
  (void)out_path;
  (void)fprintf(stderr, "media_dl: rabook support not built (MDL_EPUB_COMPILE_DIR unset)\n");
  return k_ra8_err_not_supported;
#endif
}

/** @brief Build a temp CBZ of the pages, then compile it to `.rabook`. */
RA8_INTERNAL static ra8_err_t
export_rabook(const char* dir, char names[][k_name_max], size_t count, const char* out_path)
{
  char tmp_cbz[PATH_MAX];
  (void)snprintf(tmp_cbz, sizeof(tmp_cbz), "%s.tmp.cbz", out_path);
  ra8_err_t rc = export_cbz(dir, names, count, tmp_cbz);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = run_rabook_python(tmp_cbz, out_path);
  (void)remove(tmp_cbz);
  return rc;
}

ra8_err_t mdl_export_chapter(mdl_format_t fmt, const char* chapter_dir, const char* out_path)
{
  if ((chapter_dir == nullptr) || (out_path == nullptr) || (fmt == k_mdl_fmt_loose) ||
      (fmt == k_mdl_fmt_invalid)) {
    return k_ra8_err_invalid_arg;
  }
  if (fmt == k_mdl_fmt_cbr) {
    return export_cbr(chapter_dir, out_path);
  }

  static char  s_names[k_max_pages][k_name_max];
  const size_t count = list_pages(chapter_dir, s_names, (size_t)k_max_pages);
  if (count == 0U) {
    return k_ra8_err_empty;
  }

  switch (fmt) {
    case k_mdl_fmt_cbz:
      return export_cbz(chapter_dir, s_names, count, out_path);
    case k_mdl_fmt_cbt: {
      uint8_t*  tarbuf = nullptr;
      size_t    tarlen = 0U;
      ra8_err_t rc     = build_tar(chapter_dir, s_names, count, &tarbuf, &tarlen);
      if (rc == k_ra8_ok) {
        rc = write_buf(out_path, tarbuf, tarlen);
      }
      free(tarbuf);
      return rc;
    }
    case k_mdl_fmt_cbt_gz:
      return export_tar_wrapped(chapter_dir, s_names, count, out_path, gzip_buf);
    case k_mdl_fmt_cbt_xz:
      return export_cbt_xz(chapter_dir, s_names, count, out_path);
    case k_mdl_fmt_epub:
      return export_epub(chapter_dir, s_names, count, out_path);
    case k_mdl_fmt_jof:
      return export_jof(chapter_dir, s_names, count);
    case k_mdl_fmt_rabook:
      return export_rabook(chapter_dir, s_names, count, out_path);
    default:
      return k_ra8_err_invalid_arg;
  }
}
