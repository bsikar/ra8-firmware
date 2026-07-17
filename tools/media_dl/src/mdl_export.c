/*
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/**
 * @file mdl_export.c
 * @brief Package a chapter folder into a reader-openable container.
 *
 * @details
 * Formats are produced from vendored, in-tree code -- CBZ via the firmware's own
 * miniz ZIP writer, CBT via a hand-written ustar tar, CBT.GZ via miniz DEFLATE +
 * RFC-1952 framing -- so no system library or external process is needed. Two
 * formats are exceptions: CBR spawns the proprietary `rar` tool (RAR has no open
 * writer) only when it is installed; CBT.XZ awaits a vendored xz encoder (the
 * in-tree `xz_embedded` is decode-only), so it is reported unsupported for now.
 * The gzip variant wraps the whole tar -- the reader's `ra8_comic_open_wrapped`
 * decodes the layer, then re-probes the inner archive.
 */
#include "mdl_export.h"

#include <crt_externs.h>
#include <dirent.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "miniz.h"

/** @brief Page-list limits. */
typedef enum : uint16_t {
  k_max_pages = 2048, /**< Max page entries per chapter. */
  k_name_max  = 256,  /**< Max entry-name bytes. */
} mdl_export_limits_t;

/** @brief ustar (POSIX tar) header field offsets and lengths. */
typedef enum : uint16_t {
  k_tar_block   = 512, /**< Tar block size. */
  k_off_name    = 0,   /**< name field offset. */
  k_len_name    = 100, /**< name field width. */
  k_off_mode    = 100, /**< mode field offset. */
  k_off_uid     = 108, /**< uid field offset. */
  k_off_gid     = 116, /**< gid field offset. */
  k_len_id      = 8,   /**< mode/uid/gid field width. */
  k_off_size    = 124, /**< size field offset. */
  k_len_size    = 12,  /**< size field width. */
  k_off_mtime   = 136, /**< mtime field offset. */
  k_len_mtime   = 12,  /**< mtime field width. */
  k_off_chksum  = 148, /**< checksum field offset. */
  k_len_chksum  = 8,   /**< checksum field width. */
  k_off_type    = 156, /**< typeflag field offset. */
  k_off_magic   = 257, /**< "ustar" magic offset. */
  k_len_magic   = 6,   /**< "ustar" magic width. */
  k_off_version = 263, /**< version field offset. */
} mdl_tar_layout_t;

/** @brief File mode written into tar headers and the zip STORE flag source. */
typedef enum : uint16_t {
  k_file_mode = 0644, /**< Regular-file permission bits (octal). */
} mdl_tar_mode_t;

/** @brief gzip framing constants (RFC 1952). */
typedef enum : uint8_t {
  k_gz_id1 = 0x1F, /**< gzip magic byte 1. */
  k_gz_id2 = 0x8B, /**< gzip magic byte 2. */
  k_gz_cm  = 0x08, /**< compression method: DEFLATE. */
  k_gz_os  = 0xFF, /**< OS: unknown. */
} mdl_gzip_hdr_t;

/** @brief Byte-serialisation constants. */
typedef enum : uint16_t {
  k_byte_bits    = 8,    /**< Bits per byte. */
  k_byte_mask    = 0xFF, /**< Low-byte mask. */
  k_u32_bytes    = 4,    /**< Bytes in a u32. */
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
    case k_mdl_fmt_loose:
    case k_mdl_fmt_invalid:
    default:
      return "";
  }
}

/** @brief qsort comparator over fixed-width name rows. */
static int name_cmp(const void* a, const void* b)
{
  return strcmp((const char*)a, (const char*)b);
}

/** @brief List a chapter's non-hidden page files into `names`, sorted. */
static size_t list_pages(const char* dir, char names[][k_name_max], size_t cap)
{
  DIR* d = opendir(dir);
  if (d == nullptr) {
    return 0U;
  }
  size_t               n = 0U;
  const struct dirent* e = readdir(d);
  while ((e != nullptr) && (n < cap)) {
    if (e->d_name[0] != '.') { /* skip hidden / AppleDouble, like the reader */
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
static size_t file_size(const char* path)
{
  struct stat st = {};
  if (stat(path, &st) != 0) {
    return 0U;
  }
  return (size_t)st.st_size;
}

/** @brief Round `n` up to a whole tar block. */
static size_t round_block(size_t n)
{
  return ((n + (size_t)k_tar_block - 1U) / (size_t)k_tar_block) * (size_t)k_tar_block;
}

/** @brief Write a ustar header for `name`/`size` into a zeroed 512-byte block. */
static void tar_header(uint8_t* blk, const char* name, size_t size)
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
static ra8_err_t build_tar(const char* dir,
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
static ra8_err_t write_buf(const char* path, const uint8_t* buf, size_t len)
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
static bool put_u32le(FILE* f, uint32_t v)
{
  uint8_t b[k_u32_bytes] = {};
  for (size_t i = 0U; i < (size_t)k_u32_bytes; ++i) {
    b[i] = (uint8_t)(v & (uint32_t)k_byte_mask);
    v >>= (uint32_t)k_byte_bits;
  }
  return fwrite(b, 1U, sizeof(b), f) == sizeof(b);
}

/** @brief gzip `buf` to `path` using vendored miniz DEFLATE + RFC-1952 framing. */
static ra8_err_t gzip_buf(const char* path, const uint8_t* buf, size_t len)
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
static ra8_err_t
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
static ra8_err_t export_tar_wrapped(const char* dir,
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
static ra8_err_t export_cbr(const char* dir, const char* out_path)
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
      (void)fprintf(stderr, "media_dl: cbt.xz needs a vendored xz encoder (not yet in-tree)\n");
      return k_ra8_err_not_supported;
    default:
      return k_ra8_err_invalid_arg;
  }
}
