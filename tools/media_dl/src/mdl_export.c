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
 *
 * The JOF arm lives in the sibling mdl_export_jof.c: it is the only format that
 * reaches into the firmware's `ra8_jof` decode/encode stack, so it owns
 * its own translation unit rather than widening this one's dependencies. This
 * file still dispatches to it, via mdl_export_jof() in mdl_export_internal.h.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/* glibc gates its posix_spawn chdir file-action and `environ` behind
 * _GNU_SOURCE. It is defined by the build (tools/media_dl/CMakeLists.txt and
 * the tools pass of scripts/checks/clang_tidy.sh) rather than here, because a
 * feature-test macro only works if it precedes EVERY system header -- including
 * ones pulled in ahead of this file -- which only the compile line can guarantee. */
#include "mdl_export.h"

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

#ifdef __APPLE__
/* macOS links every executable against a shared libc, so it exposes no
 * `environ` symbol to link against; `_NSGetEnviron()` in <crt_externs.h> is the
 * documented replacement. Every other POSIX host declares `environ` directly. */
#include <crt_externs.h>
#endif

#include "mdl_atomic.h"
#include "mdl_export_internal.h"
#include "mdl_sanitize.h"
#include "miniz.h"
#include "ra8_attributes.h"

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

/**
 * @brief ustar magic field: the 5 ASCII chars plus the NUL POSIX requires.
 * @details Raw bytes, not a string literal -- the NUL here is payload (the
 *          field is exactly 6 bytes wide), not a C string terminator.
 */
static const uint8_t k_ustar_magic[] = {'u', 's', 't', 'a', 'r', '\0'};
/** @brief ustar version field: the two ASCII digits "00", unterminated. */
static const uint8_t k_ustar_version[] = {'0', '0'};

static_assert(sizeof(k_ustar_magic) == (size_t)k_len_magic,
              "ustar magic constant must fill the 6-byte magic field exactly");

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

bool mdl_format_is_dir_output(mdl_format_t fmt)
{
  /* JOF is inherently per-page: one `.jof` band atlas is written beside each
   * source image, so a chapter is a directory of atlases rather than a single
   * container file at out_path. Every other archive format produces one file. */
  return fmt == k_mdl_fmt_jof;
}

/** @brief The process environment block, for the `posix_spawnp` calls below. */
RA8_INTERNAL static char* const* spawn_environ(void)
{
#ifdef __APPLE__
  return *_NSGetEnviron();
#else
  /* Declared by <unistd.h> under _GNU_SOURCE, which the build defines. */
  return environ;
#endif
}

/**
 * @brief Set a spawn file-actions' working directory, whichever spelling exists.
 *
 * @details `posix_spawn_file_actions_addchdir()` is POSIX.1-2024. macOS 26
 *          provides it and marks the older `_np` name deprecated; glibc 2.36
 *          provides only `_np` and does not declare the standard name. The
 *          build probes for the standard spelling (see this tool's
 *          CMakeLists.txt) and defines MDL_HAVE_POSIX_SPAWN_ADDCHDIR when it
 *          is present, so one source serves both without a platform guess.
 *
 * @param[in,out] actions Initialised file-actions object to extend.
 * @param[in]     dir     Directory the spawned child starts in (non-NULL).
 *
 * @return 0 on success, else an errno value from the underlying call.
 * @retval 0 The chdir action was appended.
 *
 * @pre @p actions has been through posix_spawn_file_actions_init().
 * @pre @p dir is a NUL-terminated path.
 * @post On success @p actions carries a chdir to @p dir.
 * @post @p dir is not retained beyond the call.
 *
 * @note Thread-safe: operates only on the caller's @p actions.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static int spawn_addchdir(posix_spawn_file_actions_t* actions, const char* dir)
{
#ifdef MDL_HAVE_POSIX_SPAWN_ADDCHDIR
  return posix_spawn_file_actions_addchdir(actions, dir);
#else
  return posix_spawn_file_actions_addchdir_np(actions, dir);
#endif
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

/**
 * @brief List a chapter's page-image files (only) into `names`, sorted.
 * @details Sets `*out_truncated` when a qualifying page image exists beyond
 *          `cap`, so the caller fails loudly instead of packaging a chapter
 *          short. Non-image junk beyond the cap does not trip the flag.
 */
RA8_INTERNAL static size_t
list_pages(const char* dir, char names[][k_name_max], size_t cap, bool* out_truncated)
{
  *out_truncated = false;
  DIR* d         = opendir(dir);
  if (d == nullptr) {
    return 0U;
  }
  size_t               n = 0U;
  const struct dirent* e = readdir(d);
  while (e != nullptr) {
    /* Skip hidden / AppleDouble, and anything that is not a page image so a
     * dir already holding this tool's output re-packages cleanly. A name that
     * would not fit is rejected rather than silently truncated (which could
     * collide two distinct pages onto one entry). */
    if ((e->d_name[0] != '.') && is_page_image(e->d_name) &&
        (strlen(e->d_name) < (size_t)k_name_max)) {
      if (n >= cap) {
        *out_truncated = true; /* more pages than the fixed table holds */
        break;
      }
      (void)snprintf(names[n], k_name_max, "%s", e->d_name);
      ++n;
    }
    e = readdir(d);
  }
  (void)closedir(d);
  qsort(names, n, k_name_max, name_cmp);
  return n;
}

/** @brief Round `n` up to a whole tar block. */
RA8_INTERNAL static size_t round_block(size_t n)
{
  return ((n + (size_t)k_tar_block - 1U) / (size_t)k_tar_block) * (size_t)k_tar_block;
}

/** @brief Write a ustar header for `name`/`size`; error if `name` will not fit. */
RA8_INTERNAL static ra8_err_t tar_header(uint8_t* blk, const char* name, size_t size)
{
  if (strlen(name) >= (size_t)k_len_name) {
    /* The ustar name field is 100 bytes; a longer name would silently truncate
     * and two distinct pages could collide onto one entry. Refuse instead. */
    return k_ra8_err_invalid_size;
  }
  memset(blk, 0, k_tar_block);
  (void)snprintf((char*)blk + k_off_name, k_len_name, "%s", name);
  (void)snprintf((char*)blk + k_off_mode, k_len_id, "%07o", (unsigned)k_file_mode);
  (void)snprintf((char*)blk + k_off_uid, k_len_id, "%07o", 0U);
  (void)snprintf((char*)blk + k_off_gid, k_len_id, "%07o", 0U);
  (void)snprintf((char*)blk + k_off_size, k_len_size, "%011zo", size);
  (void)snprintf((char*)blk + k_off_mtime, k_len_mtime, "%011o", 0U);
  blk[k_off_type] = '0';
  memcpy(blk + k_off_magic, k_ustar_magic, sizeof(k_ustar_magic));
  memcpy(blk + k_off_version, k_ustar_version, sizeof(k_ustar_version));

  memset(blk + k_off_chksum, ' ', k_len_chksum);
  unsigned sum = 0U;
  for (size_t i = 0U; i < (size_t)k_tar_block; ++i) {
    sum += blk[i];
  }
  (void)snprintf((char*)blk + k_off_chksum, k_len_chksum - 1U, "%06o", sum);
  blk[k_off_chksum + k_len_chksum - 1U] = ' ';
  return k_ra8_ok;
}

/** @brief Streaming copy-buffer size (bounds the per-file tar/gzip working set). */
typedef enum : uint32_t {
  k_stream_chunk = 65536U, /**< File-copy chunk in bytes. */
} mdl_stream_t;

/**
 * @brief Copy exactly `size` bytes from open `in` to `out`, padding to a block.
 * @details Reads through a fixed chunk buffer and writes straight to @p out, so
 *          no whole-file buffer is ever held. Copies precisely the byte count
 *          the header was written with, so a file that grew after `fstat`
 *          cannot spill past the entry and one that shrank fails loudly.
 */
RA8_INTERNAL static ra8_err_t tar_copy_file(FILE* in, FILE* out, size_t size)
{
  uint8_t chunk[k_stream_chunk];
  size_t  remaining = size;
  while (remaining > 0U) {
    const size_t want = (remaining < sizeof(chunk)) ? remaining : sizeof(chunk);
    const size_t got  = fread(chunk, 1U, want, in);
    if ((got == 0U) || (fwrite(chunk, 1U, got, out) != got)) {
      return k_ra8_fail;
    }
    remaining -= got;
  }
  const size_t pad = round_block(size) - size;
  if (pad > 0U) {
    const uint8_t zeros[k_tar_block] = {};
    if (fwrite(zeros, 1U, pad, out) != pad) {
      return k_ra8_fail;
    }
  }
  return k_ra8_ok;
}

/** @brief Stream a ustar of `names` from `dir` straight into the open file `out`. */
RA8_INTERNAL static ra8_err_t
build_tar_to_file(const char* dir, char names[][k_name_max], size_t count, FILE* out)
{
  for (size_t i = 0U; i < count; ++i) {
    char path[PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
      return k_ra8_fail;
    }
    struct stat st = {};
    if (fstat(fileno(f), &st) != 0) {
      (void)fclose(f);
      return k_ra8_fail;
    }
    /* One fstat on the open descriptor sizes the header and bounds the copy, so
     * the two never disagree (a file racing our output cannot overflow). */
    const size_t    sz = (size_t)st.st_size;
    uint8_t         hdr[k_tar_block];
    const ra8_err_t hrc = tar_header(hdr, names[i], sz);
    if (hrc != k_ra8_ok) {
      (void)fclose(f);
      return hrc;
    }
    ra8_err_t rc = (fwrite(hdr, 1U, sizeof(hdr), out) == sizeof(hdr)) ? k_ra8_ok : k_ra8_fail;
    if (rc == k_ra8_ok) {
      rc = tar_copy_file(f, out, sz);
    }
    (void)fclose(f);
    if (rc != k_ra8_ok) {
      return rc;
    }
  }
  uint8_t trailer[2U * (size_t)k_tar_block] = {}; /* two trailing zero blocks */
  return (fwrite(trailer, 1U, sizeof(trailer), out) == sizeof(trailer)) ? k_ra8_ok : k_ra8_fail;
}

/**
 * @brief Stream a ustar to `out_path`.
 * @details A partial file on failure is the CALLER's to discard: every path
 *          into here now writes a temp that ::export_atomic aborts, so a second
 *          remove() here would only race its own cleanup.
 */
RA8_INTERNAL static ra8_err_t
write_tar_file(const char* dir, char names[][k_name_max], size_t count, const char* out_path)
{
  FILE* f = fopen(out_path, "wb");
  if (f == NULL) {
    // cppcheck-suppress resourceLeak
    // f is NULL here
    return k_ra8_fail;
  }
  ra8_err_t  rc       = build_tar_to_file(dir, names, count, f);
  const bool close_ok = (fclose(f) == 0);
  if ((rc == k_ra8_ok) && !close_ok) {
    rc = k_ra8_fail;
  }
  return rc;
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

/** @brief put_buf sink for streaming deflate: append compressed bytes to a FILE*. */
RA8_INTERNAL static mz_bool gz_put(const void* buf, int len, void* user)
{
  FILE* f = (FILE*)user;
  return (fwrite(buf, 1U, (size_t)len, f) == (size_t)len) ? MZ_TRUE : MZ_FALSE;
}

/**
 * @brief gzip `in_path` to `out_path`, streaming miniz DEFLATE + RFC-1952 framing.
 * @details Feeds the source through a fixed chunk buffer into miniz's streaming
 *          deflator (its put-buf callback writes straight to `out`), so neither
 *          the source nor the compressed stream is ever held whole. CRC32 and
 *          ISIZE are accumulated incrementally across the chunks.
 */
RA8_INTERNAL static ra8_err_t gzip_file(ra8_arena_t* arena, const char* in_path, const char* out_path)
{
  FILE* in = fopen(in_path, "rb");
  if (in == NULL) {
    // cppcheck-suppress resourceLeak
    // in is NULL here
    return k_ra8_fail;
  }
  FILE* out = fopen(out_path, "wb");
  if (out == NULL) {
    (void)fclose(in);
    // cppcheck-suppress resourceLeak
    // out is NULL here
    return k_ra8_fail;
  }
  tdefl_compressor* d = (tdefl_compressor*)ra8_arena_alloc(arena, (uint32_t)sizeof(*d));
  if (d == NULL) {
    (void)fclose(in);
    (void)fclose(out);
    
    return k_ra8_err_no_mem;
  }
  const uint8_t hdr[k_gzip_hdr_len] =
    {k_gz_id1, k_gz_id2, k_gz_cm, 0U, 0U, 0U, 0U, 0U, 0U, k_gz_os};
  bool     ok    = (fwrite(hdr, 1U, sizeof(hdr), out) == sizeof(hdr)) &&
                   (tdefl_init(d, gz_put, out, TDEFL_DEFAULT_MAX_PROBES) == TDEFL_STATUS_OKAY);
  uint32_t crc   = (uint32_t)MZ_CRC32_INIT;
  uint32_t isize = 0U; /* ISIZE = total input length mod 2^32 */
  uint8_t  chunk[k_stream_chunk];
  while (ok) {
    const size_t got = fread(chunk, 1U, sizeof(chunk), in);
    if (got == 0U) {
      break;
    }
    crc = (uint32_t)mz_crc32(crc, chunk, got);
    isize += (uint32_t)got;
    ok = (tdefl_compress_buffer(d, chunk, got, TDEFL_NO_FLUSH) == TDEFL_STATUS_OKAY);
  }
  if (ok && (ferror(in) != 0)) {
    ok = false;
  }
  if (ok && (tdefl_compress_buffer(d, nullptr, 0U, TDEFL_FINISH) != TDEFL_STATUS_DONE)) {
    ok = false;
  }
  
  ok = ok && put_u32le(out, crc) && put_u32le(out, isize);
  ok = (fclose(out) == 0) && ok;
  (void)fclose(in);
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

/** @brief Spawn the proprietary `rar` tool for CBR (the sole external path). */
RA8_INTERNAL static ra8_err_t export_cbr(const char* dir, const char* out_path)
{
  posix_spawn_file_actions_t actions;
  (void)posix_spawn_file_actions_init(&actions);
  (void)spawn_addchdir(&actions, dir);
  const char* const argv[] = {"rar", "a", "-ep1", "-idq", out_path, ".", nullptr};
  pid_t             pid    = 0;
  const int         rc =
    posix_spawnp(&pid, argv[0], &actions, nullptr, (char* const*)argv, spawn_environ());
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
    posix_spawnp(&pid, argv[0], &actions, nullptr, (char* const*)argv, spawn_environ());
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

/** @brief Compress `in_path` to `out_path` via the external `xz` with reader-safe flags. */
RA8_INTERNAL static ra8_err_t xz_file(ra8_arena_t* arena, const char* in_path, const char* out_path)
{
  (void)arena;
  /* CRC32 check + 1 MiB dict so the on-device xz scratch accepts the stream. */
  const char* const a[] =
    {"xz", "--check=crc32", "--lzma2=preset=6,dict=1MiB", "-c", in_path, nullptr};
  return run_to_file(a, out_path);
}

/**
 * @brief Stream a tar to `<out_path>.tar.tmp`, then `compress` it to out_path.
 * @details The tar is streamed to a sibling temp file (never held whole in RAM),
 *          `compress` reads that file and streams its own output, and the temp
 *          file is removed on every exit path.
 */
RA8_INTERNAL static ra8_err_t export_tar_wrapped(ra8_arena_t* arena,
                                                 const char* dir,
                                                 char        names[][k_name_max],
                                                 size_t      count,
                                                 const char* out_path,
                                                 ra8_err_t (*compress)(ra8_arena_t* arena,
                                                                       const char* in_path,
                                                                       const char* out_path))
{
  /* A truncated suffix would name a DIFFERENT file than intended -- possibly
   * one that already exists -- so overflow aborts rather than proceeding. */
  char      tmp[PATH_MAX];
  const int n = snprintf(tmp, sizeof(tmp), "%s.tar.tmp", out_path);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return k_ra8_fail;
  }
  ra8_err_t rc = write_tar_file(dir, names, count, tmp);
  if (rc == k_ra8_ok) {
    rc = compress(arena, tmp, out_path);
  }
  (void)remove(tmp);
  return rc;
}

/* --- EPUB (self-contained: a valid EPUB3 of the page images via miniz) ---- */

/**
 * @brief EPUB string-buffer sizing (grows with the page count).
 * @details Sized for the WORST case, not the typical `page_NNN.jpg`: a page
 *          filename may be up to ::k_name_max bytes and, XML-escaped, expand
 *          6x (a name of all `&quot;`). That escaped name is embedded once in
 *          the page's manifest fragment and once in its xhtml document, so both
 *          the fragment buffer and the per-page accumulator budget must exceed
 *          the fixed template text plus ::k_epub_name_esc_max. Undersizing here
 *          does not truncate silently -- ::str_cat and ::snprintf_fit report it
 *          and the export fails -- but correct sizing is what lets a legitimate
 *          long-name chapter package rather than error.
 */
typedef enum : uint32_t {
  k_epub_name_esc_max   = 1536U, /**< XML-escaped page name (k_name_max * 6).       */
  k_epub_frag_max       = 2048U, /**< One manifest fragment (fixed + escaped name). */
  k_epub_xhtml_max      = 2048U, /**< One page's xhtml document (embeds the name).  */
  k_epub_entry_max      = 320U,  /**< A zip entry path ("OEBPS/images/" + name).    */
  k_epub_base_bytes     = 4096U, /**< Fixed opf/nav overhead.                       */
  k_epub_per_page_bytes = 2048U, /**< Per-page opf/nav accumulator growth.          */
} mdl_epub_size_t;

/** @brief True if an snprintf result fully fit its buffer (no truncation). */
RA8_INTERNAL static bool snprintf_fit(int written, size_t cap)
{
  return (written >= 0) && ((size_t)written < cap);
}

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

/**
 * @brief Append `text` to NUL-terminated `dst`; report whether it fully fit.
 * @details Never truncates: if the append (plus its NUL) would not fit in
 *          @p cap it leaves @p dst unchanged and returns false, so the caller
 *          can fail loudly rather than emit a manifest cut off mid-element.
 * @return true when the whole of @p text was appended, false if it would overrun.
 */
RA8_INTERNAL static bool str_cat(char* dst, size_t cap, const char* text)
{
  const size_t cur = strlen(dst);
  const size_t add = strlen(text);
  if (cur + add + 1U > cap) {
    return false;
  }
  memcpy(dst + cur, text, add + 1U);
  return true;
}

/** @brief Add an in-memory string as a STORED zip entry. */
RA8_INTERNAL static bool epub_add_str(mz_zip_archive* zip, const char* name, const char* body)
{
  return mz_zip_writer_add_mem(zip, name, body, strlen(body), MZ_NO_COMPRESSION) != MZ_FALSE;
}

/** @brief Append this page's manifest/spine/nav fragments (escaped href). */
RA8_INTERNAL static ra8_err_t epub_append_frags(char*       mani,
                                                char*       spine,
                                                char*       nav,
                                                size_t      cap,
                                                const char* esc_name,
                                                const char* media,
                                                size_t      idx,
                                                unsigned    n)
{
  char      frag[k_epub_frag_max];
  const int fn = snprintf(frag,
                          sizeof(frag),
                          "<item id=\"pg%zu\" href=\"page_%03u.xhtml\" "
                          "media-type=\"application/xhtml+xml\"/>"
                          "<item id=\"img%zu\" href=\"images/%s\" media-type=\"%s\"/>",
                          idx,
                          n,
                          idx,
                          esc_name,
                          media);
  if (!snprintf_fit(fn, sizeof(frag))) {
    return k_ra8_fail;
  }
  if (!str_cat(mani, cap, frag)) {
    return k_ra8_fail;
  }
  (void)snprintf(frag, sizeof(frag), "<itemref idref=\"pg%zu\"/>", idx);
  if (!str_cat(spine, cap, frag)) {
    return k_ra8_fail;
  }
  (void)snprintf(frag, sizeof(frag), "<li><a href=\"page_%03u.xhtml\">Page %u</a></li>", n, n);
  if (!str_cat(nav, cap, frag)) {
    return k_ra8_fail;
  }
  return k_ra8_ok;
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
  char           esc[k_epub_name_esc_max];
  if (!mdl_xml_escape(name, esc, sizeof(esc))) {
    return k_ra8_fail; /* untrusted filename must not break the container XML */
  }
  char      xhtml[k_epub_xhtml_max];
  const int xn = snprintf(xhtml,
                          sizeof(xhtml),
                          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                          "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Page %u"
                          "</title></head><body><img src=\"images/%s\" alt=\"Page %u\"/>"
                          "</body></html>",
                          n,
                          esc,
                          n);
  if (!snprintf_fit(xn, sizeof(xhtml))) {
    return k_ra8_fail;
  }
  char entry[k_epub_entry_max];
  (void)snprintf(entry, sizeof(entry), "OEBPS/page_%03u.xhtml", n);
  if (!epub_add_str(zip, entry, xhtml)) {
    return k_ra8_fail;
  }
  char src[PATH_MAX];
  (void)snprintf(src, sizeof(src), "%s/%s", dir, name);
  const int en = snprintf(entry, sizeof(entry), "OEBPS/images/%s", name);
  if (!snprintf_fit(en, sizeof(entry))) {
    return k_ra8_fail;
  }
  if (mz_zip_writer_add_file(zip, entry, src, nullptr, 0, MZ_NO_COMPRESSION) == MZ_FALSE) {
    return k_ra8_fail;
  }
  return epub_append_frags(mani, spine, nav, cap, esc, epub_media_type(name), idx, n);
}

/** @brief Build + add content.opf and nav.xhtml, then finalize the archive. */
RA8_INTERNAL static ra8_err_t
epub_add_meta(ra8_arena_t* arena, mz_zip_archive* zip, const char* mani, const char* spine, const char* nav)
{
  const size_t opf_cap = strlen(mani) + strlen(spine) + (size_t)k_epub_base_bytes;
  const size_t nav_cap = strlen(nav) + (size_t)k_epub_base_bytes;
  char*        opf     = (char*)ra8_arena_alloc(arena, (uint32_t)opf_cap);
  char*        navdoc  = (char*)ra8_arena_alloc(arena, (uint32_t)nav_cap);
  if ((opf == nullptr) || (navdoc == nullptr)) {
    
    
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
  
  
  return ok ? k_ra8_ok : k_ra8_fail;
}

/** @brief Package `dir`'s pages into a valid EPUB3 at `out_path`. */
RA8_INTERNAL static ra8_err_t
export_epub(ra8_arena_t* arena, const char* dir, char names[][k_name_max], size_t count, const char* out_path)
{
  const size_t cap   = (size_t)k_epub_base_bytes + (count * (size_t)k_epub_per_page_bytes);
  char*        mani  = (char*)ra8_arena_calloc(arena, 1U, (uint32_t)cap);
  char*        spine = (char*)ra8_arena_calloc(arena, 1U, (uint32_t)cap);
  char*        nav   = (char*)ra8_arena_calloc(arena, 1U, (uint32_t)cap);
  if ((mani == nullptr) || (spine == nullptr) || (nav == nullptr)) {
    
    
    
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
    rc = epub_add_meta(arena, &zip, mani, spine, nav);
  }
  if (zip_open) {
    (void)mz_zip_writer_end(&zip);
  }
  
  
  
  /* A partial EPUB is ::export_atomic's temp to discard, not ours. */
  return rc;
}

/* --- RABOOK (optional external: the tools/epub_compile python emitter) ---- */

/** @brief Run cbz_compile.py to turn `cbz` into an RBKC `.rabook` at `out_path`. */
RA8_INTERNAL static ra8_err_t run_rabook_python(const char* cbz, const char* out_path)
{
#ifdef MDL_EPUB_COMPILE_DIR
  char script[PATH_MAX];
  (void)snprintf(script, sizeof(script), "%s/cbz_compile.py", (const char*)(MDL_EPUB_COMPILE_DIR));
  (void)setenv("PYTHONPATH", (const char*)(MDL_EPUB_COMPILE_DIR), 1);
  const char* const argv[] = {"python3", script, cbz, out_path, "--rtl", nullptr};
  pid_t             pid    = 0;
  const int rc = posix_spawnp(&pid, argv[0], nullptr, nullptr, (char* const*)argv, spawn_environ());
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
  /* As in export_tar_wrapped: a truncated suffix would name a different file. */
  char      tmp_cbz[PATH_MAX];
  const int n = snprintf(tmp_cbz, sizeof(tmp_cbz), "%s.tmp.cbz", out_path);
  if ((n < 0) || ((size_t)n >= sizeof(tmp_cbz))) {
    return k_ra8_fail;
  }
  ra8_err_t rc = export_cbz(dir, names, count, tmp_cbz);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = run_rabook_python(tmp_cbz, out_path);
  (void)remove(tmp_cbz);
  return rc;
}

/** @brief Run one format's writer, producing the container at `out_path`. */
RA8_INTERNAL static ra8_err_t export_dispatch(ra8_arena_t* arena, mdl_format_t fmt,
                                              const char*  dir,
                                              char         names[][k_name_max],
                                              size_t       count,
                                              const char*  out_path)
{
  switch (fmt) {
    case k_mdl_fmt_cbz:
      return export_cbz(dir, names, count, out_path);
    case k_mdl_fmt_cbt:
      return write_tar_file(dir, names, count, out_path);
    case k_mdl_fmt_cbt_gz:
      return export_tar_wrapped(arena, dir, names, count, out_path, gzip_file);
    case k_mdl_fmt_cbt_xz:
      return export_tar_wrapped(arena, dir, names, count, out_path, xz_file);
    case k_mdl_fmt_epub:
      return export_epub(arena, dir, names, count, out_path);
    case k_mdl_fmt_rabook:
      return export_rabook(dir, names, count, out_path);
    case k_mdl_fmt_cbr:
      return export_cbr(dir, out_path);
    case k_mdl_fmt_jof:
    case k_mdl_fmt_loose:
    case k_mdl_fmt_invalid:
    default:
      return k_ra8_err_invalid_arg;
  }
}

/**
 * @brief Build `out_path` through a sibling temp, committing only on success.
 * @details The ONE atomicity seam for every container format, rather than seven
 *          writers each remembering. Each of them used to build straight on top
 *          of out_path, so a re-export that failed part-way -- a missing
 *          `rar`/`xz`, an undecodable page, a full disk -- left the
 *          previously-good archive truncated or deleted. A failed re-export now
 *          costs nothing: the destination is not touched until a complete good
 *          copy exists. See mdl_atomic.h.
 */
RA8_INTERNAL static ra8_err_t export_atomic(ra8_arena_t* arena, mdl_format_t fmt,
                                            const char*  dir,
                                            char         names[][k_name_max],
                                            size_t       count,
                                            const char*  out_path)
{
  char tmp_path[PATH_MAX];
  if (!mdl_atomic_tmp_path(out_path, tmp_path, sizeof(tmp_path))) {
    return k_ra8_fail;
  }
  const ra8_err_t rc = export_dispatch(arena, fmt, dir, names, count, tmp_path);
  if (rc != k_ra8_ok) {
    mdl_atomic_abort(tmp_path);
    return rc;
  }
  return mdl_atomic_commit(tmp_path, out_path) ? k_ra8_ok : k_ra8_fail;
}

ra8_err_t mdl_export_chapter(ra8_arena_t* arena, mdl_format_t fmt, const char* chapter_dir, const char* out_path)
{
  if ((chapter_dir == nullptr) || (out_path == nullptr) || (fmt == k_mdl_fmt_loose) ||
      (fmt == k_mdl_fmt_invalid)) {
    return k_ra8_err_invalid_arg;
  }
  if (fmt == k_mdl_fmt_cbr) {
    /* `rar` archives the directory itself, so it needs no page table. */
    return export_atomic(arena, fmt, chapter_dir, nullptr, 0U, out_path);
  }

  static char  s_names[k_max_pages][k_name_max];
  bool         truncated = false;
  const size_t count     = list_pages(chapter_dir, s_names, (size_t)k_max_pages, &truncated);
  if (truncated) {
    /* More page images than the fixed table holds: fail rather than silently
     * package a short chapter the reader would discover missing pages in. */
    return k_ra8_err_invalid_size;
  }
  if (count == 0U) {
    return k_ra8_err_empty;
  }
  if (fmt == k_mdl_fmt_jof) {
    /* JOF writes one `.jof` sibling per page into chapter_dir; out_path names
     * no single container (see mdl_format_is_dir_output), so there is no single
     * file to rename into place -- mdl_export_jof commits each page itself. */
    return mdl_export_jof(arena, chapter_dir, s_names, count);
  }
  return export_atomic(arena, fmt, chapter_dir, s_names, count, out_path);
}
