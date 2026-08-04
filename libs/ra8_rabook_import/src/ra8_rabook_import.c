/**
 * @file ra8_rabook_import.c
 * @brief On-import EPUB -> .rabook compile-and-cache manager (#151).
 * @since Version 0.1.0
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB Import] {World: NS}
 */

#include "ra8_rabook_import.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_log.h"

/* -------------------------------------------------------------------------- */
/* Private constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra8_import_crc_const_t
 * @brief CRC-32/ISO-HDLC parameters (reflected, matches `zlib.crc32`).
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_crc32_poly = 0xEDB88320U, /**< Reflected ISO-HDLC polynomial.        */
  k_crc32_seed = 0xFFFFFFFFU, /**< Pre/post conditioning (init = final). */
} ra8_import_crc_const_t;

/**
 * @enum ra8_import_misc_t
 * @brief Small fixed quantities used by the name + CRC helpers.
 * @since Version 0.1.0
 */
typedef enum : uint32_t {
  k_crc32_bits        = 8U,      /**< Bits processed per input byte.           */
  k_hex_shift         = 4U,      /**< Bit shift between adjacent hex nibbles.  */
  k_hex_mask          = 0x0FU,   /**< Low-nibble mask for one hex digit.       */
  k_import_max_chunks = 131072U, /**< Loop bound: chunks streamed for the CRC. */
} ra8_import_misc_t;

/**
 * @enum ra8_import_name_idx_t
 * @brief Byte offsets inside an `XXXXXXXX.EXT` 8.3 cache name.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_idx_dot  = 8U,  /**< '.' separator after the 8 hex digits. */
  k_idx_ext0 = 9U,  /**< First extension character.            */
  k_idx_ext1 = 10U, /**< Second extension character.           */
  k_idx_ext2 = 11U, /**< Third extension character.            */
  k_idx_nul  = 12U, /**< NUL terminator.                       */
} ra8_import_name_idx_t;

/** @brief Log tag for this module. @since Version 0.1.0 */
static const char* const s_tag = "ra8_rabook_import";

/* -------------------------------------------------------------------------- */
/* Private helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Fold a byte block into a running CRC-32/ISO-HDLC accumulator.
 * @details Bitwise reflected update; the caller seeds with @ref k_crc32_seed
 *          and XORs the final value with the same seed. Used to key the cache
 *          by source content (not a `.rabook` body CRC -- that is the compiler's
 *          job inside the emitted blob).
 * @param[in] crc  Running accumulator (seeded value before the first call).
 * @param[in] data Input bytes; may be NULL (treated as a no-op block).
 * @param[in] len  Number of bytes at @p data.
 * @return Updated accumulator.
 * @retval uint32_t Reflected CRC-32 over @p crc plus @p len bytes; returns @p crc
 *                  unchanged when @p data is NULL or @p len is 0.
 * @pre @p crc holds the running value from any prior block.
 * @pre @p data points at @p len readable bytes, or is NULL.
 * @post The result reflects all bytes folded so far.
 * @post @p data and the input are unmodified.
 * @note Thread-safe: pure function over its arguments.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint32_t s_crc32_block(uint32_t crc, const uint8_t* data, uint32_t len)
{
  if (data == nullptr) {
    return crc;
  }
  if (len == 0U) {
    return crc;
  }
  for (uint32_t i = 0U; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0U; b < (uint8_t)k_crc32_bits; b++) {
      bool lsb = (crc & 1U) != 0U;
      crc >>= 1U;
      if (lsb) {
        crc ^= (uint32_t)k_crc32_poly;
      }
    }
  }
  return crc;
}

/**
 * @brief Fold an already-open file through CRC-32 in bounded chunks.
 * @details Reads @p cap-sized chunks until EOF so the working set is one chunk,
 *          never the whole `.epub`.
 * @param[in]  file     Open, readable file handle.
 * @param[in]  buf      Streaming scratch buffer (>= @p cap bytes).
 * @param[in]  cap      Chunk size in bytes (> 0).
 * @param[out] out_size Receives the total byte length read.
 * @param[out] out_crc  Receives the finalised CRC-32 cache key.
 * @return Error code.
 * @retval k_ra8_ok           Stream consumed to EOF.
 * @retval k_ra8_err_null_ptr @p file or @p buf is NULL.
 * @retval k_ra8_err_*        Read error.
 * @pre @p file is open for reading; @p buf holds @p cap bytes.
 * @pre @p out_size and @p out_crc are non-NULL.
 * @post On success `*out_size`/`*out_crc` describe every byte read.
 * @post @p file's offset is at EOF (no close performed here).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_crc_stream(ra8_fs_file_t* file, uint8_t* buf, uint32_t cap, uint32_t* out_size, uint32_t* out_crc)
{
  RA8_CHECK_NULL_PTR(file, s_tag, "file");
  RA8_CHECK_NULL_PTR(buf, s_tag, "buf");

  uint32_t crc   = (uint32_t)k_crc32_seed;
  uint32_t total = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_import_max_chunks; i++) {
    uint32_t  got = 0U;
    ra8_err_t err = ra8_fs_read(file, buf, cap, &got);
    if (err != k_ra8_ok) {
      return err;
    }
    if (got == 0U) {
      break;
    }
    crc = s_crc32_block(crc, buf, got);
    total += got;
  }
  *out_size = total;
  *out_crc  = crc ^ (uint32_t)k_crc32_seed;
  return k_ra8_ok;
}

/**
 * @brief Stream a source file through CRC-32, reporting its size and key.
 * @details Opens @p epub_path read-only, delegates the fold to
 *          @ref s_crc_stream, and closes on every path. The NULL/open guards on
 *          @p mount, @p epub_path, and @p buf are delegated to `ra8_fs_open` and
 *          @ref s_crc_stream respectively.
 * @param[in]  mount     Mounted volume.
 * @param[in]  epub_path Source path (root-level 8.3).
 * @param[in]  buf       Streaming scratch buffer (> 0 bytes).
 * @param[in]  cap       Capacity of @p buf (> 0).
 * @param[out] out_size  Receives the source byte length.
 * @param[out] out_crc   Receives the finalised CRC-32 cache key.
 * @return Error code.
 * @retval k_ra8_ok            Key computed.
 * @retval k_ra8_err_null_ptr  @p out_size or @p out_crc is NULL.
 * @retval k_ra8_err_invalid_size `cap` is zero.
 * @retval k_ra8_err_*         Open/read error (e.g. source missing).
 * @pre @p out_size and @p out_crc are non-NULL.
 * @pre @p cap > 0.
 * @post On success `*out_size`/`*out_crc` describe the whole source.
 * @post The source file is closed on every return path.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_compute_source_key(ra8_fs_mount_t* mount,
                                      const char*     epub_path,
                                      uint8_t*        buf,
                                      uint32_t        cap,
                                      uint32_t*       out_size,
                                      uint32_t*       out_crc)
{
  RA8_CHECK_NULL_PTR(out_size, s_tag, "out_size");
  RA8_CHECK_NULL_PTR(out_crc, s_tag, "out_crc");
  if (cap == 0U) {
    return k_ra8_err_invalid_size;
  }

  ra8_fs_file_t* file = nullptr;
  ra8_err_t      err  = ra8_fs_open(mount, epub_path, k_ra8_fs_mode_read, &file);
  if (err != k_ra8_ok) {
    return err;
  }
  err            = s_crc_stream(file, buf, cap, out_size, out_crc);
  ra8_err_t cerr = ra8_fs_close(file);
  if (err != k_ra8_ok) {
    return err;
  }
  return cerr;
}

/**
 * @brief Build the `XXXXXXXX.EXT` 8.3 cache name for a CRC key + extension.
 * @details Formats @p crc as 8 uppercase hex digits (most-significant nibble
 *          first) into bytes 0..7, then writes '.', @p ext0, @p ext1, @p ext2,
 *          and a NUL, yielding a fixed 12-character `XXXXXXXX.EXT` string.
 * @param[in]  crc  Source CRC-32 (its 8 hex digits name the entry).
 * @param[in]  ext0 First extension char.
 * @param[in]  ext1 Second extension char.
 * @param[in]  ext2 Third extension char.
 * @param[out] out  Destination buffer.
 * @param[in]  cap  Capacity of @p out (>= @ref k_ra8_rabook_import_name_cap).
 * @return Error code.
 * @retval k_ra8_ok               Name written + NUL-terminated.
 * @retval k_ra8_err_null_ptr     @p out is NULL.
 * @retval k_ra8_err_invalid_size @p cap is too small.
 * @pre @p out is non-NULL.
 * @pre @p cap >= @ref k_ra8_rabook_import_name_cap.
 * @post On success @p out is a NUL-terminated 12-char 8.3 name.
 * @post On error @p out is unmodified.
 * @note Thread-safe: writes only @p out.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_make_name(uint32_t crc, char ext0, char ext1, char ext2, char* out, uint32_t cap)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "out");
  if (cap < (uint32_t)k_ra8_rabook_import_name_cap) {
    return k_ra8_err_invalid_size;
  }
  static const char digits[] = "0123456789ABCDEF";
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_rabook_import_key_hex_len; i++) {
    uint8_t shift =
      (uint8_t)(((uint8_t)k_ra8_rabook_import_key_hex_len - 1U - i) * (uint8_t)k_hex_shift);
    out[i] = digits[(crc >> shift) & (uint32_t)k_hex_mask];
  }
  out[(uint8_t)k_idx_dot]  = '.';
  out[(uint8_t)k_idx_ext0] = ext0;
  out[(uint8_t)k_idx_ext1] = ext1;
  out[(uint8_t)k_idx_ext2] = ext2;
  out[(uint8_t)k_idx_nul]  = '\0';
  return k_ra8_ok;
}

/**
 * @brief Derive the cache (`.RBK`), temp (`.RBT`), and marker (`.RBM`) names.
 * @details All three share the 8-hex CRC prefix and differ only in extension.
 * @param[in]  crc        Source CRC-32 cache key.
 * @param[out] cache_name Receives `XXXXXXXX.RBK` (cap @ref k_ra8_rabook_import_name_cap).
 * @param[out] tmp_name   Receives `XXXXXXXX.RBT`.
 * @param[out] mark_name  Receives `XXXXXXXX.RBM`.
 * @return Error code from the first failing name build.
 * @retval k_ra8_ok           All three names written.
 * @retval k_ra8_err_null_ptr A destination buffer is NULL.
 * @pre @p cache_name, @p tmp_name, @p mark_name each hold the name cap.
 * @pre @p crc is the finalised source key.
 * @post On success the three buffers hold consistent, distinct 8.3 names.
 * @post On error the buffers' contents are unspecified.
 * @note Thread-safe: writes only the destinations.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_derive_names(uint32_t crc, char* cache_name, char* tmp_name, char* mark_name)
{
  RA8_CHECK_NULL_PTR(cache_name, s_tag, "cache_name");
  RA8_CHECK_NULL_PTR(tmp_name, s_tag, "tmp_name");
  RA8_CHECK_NULL_PTR(mark_name, s_tag, "mark_name");
  const uint32_t cap = (uint32_t)k_ra8_rabook_import_name_cap;
  ra8_err_t      err = s_make_name(crc, 'R', 'B', 'K', cache_name, cap);
  if (err != k_ra8_ok) {
    return err;
  }
  err = s_make_name(crc, 'R', 'B', 'T', tmp_name, cap);
  if (err != k_ra8_ok) {
    return err;
  }
  return s_make_name(crc, 'R', 'B', 'M', mark_name, cap);
}

/**
 * @brief Report whether a root-level file can be opened for reading.
 * @details Probes existence by attempting a read-only `ra8_fs_open`; on success it
 *          immediately closes the handle. NULL arguments and any open failure are
 *          reported as "not present".
 * @param[in] mount Mounted volume.
 * @param[in] path  Root-level 8.3 path.
 * @return `true` if the file exists and opens; `false` otherwise.
 * @retval true  @p path opened read-only (and was closed again).
 * @retval false @p mount or @p path is NULL, or the open failed.
 * @pre @p mount and @p path describe a mounted volume + candidate name.
 * @pre A free file slot is available for the probe open.
 * @post Any handle opened by the probe is closed before return.
 * @post @p mount is otherwise unchanged.
 * @note Thread-safe only if the caller serialises filesystem access.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool s_file_exists(ra8_fs_mount_t* mount, const char* path)
{
  if (mount == nullptr) {
    return false;
  }
  if (path == nullptr) {
    return false;
  }
  ra8_fs_file_t* file = nullptr;
  ra8_err_t      err  = ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file);
  if (err != k_ra8_ok) {
    return false;
  }
  (void)ra8_fs_close(file);
  return true;
}

/**
 * @brief Read a freshness marker file into a stamp record.
 * @details Opens @p path read-only, reads exactly `sizeof(*out)` bytes into @p out,
 *          and closes on every path; a short read (marker smaller than one stamp)
 *          is reported as @ref k_ra8_err_invalid_size.
 * @param[in]  mount Mounted volume.
 * @param[in]  path  Marker path (root-level 8.3).
 * @param[out] out   Receives the stamp on success.
 * @return Error code.
 * @retval k_ra8_ok               Stamp read in full.
 * @retval k_ra8_err_null_ptr     A pointer argument is NULL.
 * @retval k_ra8_err_invalid_size Marker shorter than a stamp.
 * @retval k_ra8_err_*            Open/read error (e.g. marker absent).
 * @pre @p out is non-NULL.
 * @pre @p path names the candidate marker.
 * @post On success `*out` holds the on-disk stamp.
 * @post The marker file is closed on every path.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_read_stamp(ra8_fs_mount_t* mount, const char* path, ra8_rabook_import_stamp_t* out)
{
  RA8_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA8_CHECK_NULL_PTR(path, s_tag, "path");
  RA8_CHECK_NULL_PTR(out, s_tag, "out");

  ra8_fs_file_t* file = nullptr;
  ra8_err_t      err  = ra8_fs_open(mount, path, k_ra8_fs_mode_read, &file);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t got   = 0U;
  err            = ra8_fs_read(file, (uint8_t*)out, (uint32_t)sizeof(*out), &got);
  ra8_err_t cerr = ra8_fs_close(file);
  if (err != k_ra8_ok) {
    return err;
  }
  if (got != (uint32_t)sizeof(*out)) {
    return k_ra8_err_invalid_size;
  }
  return cerr;
}

/**
 * @brief Decide if the cached marker matches the expected stamp.
 * @details Single-`memcmp` comparison of the whole stamp -- deliberately not a
 *          field-by-field compound boolean, so there is no compound freshness
 *          decision requiring MC/DC vectors.
 * @param[in] mount     Mounted volume.
 * @param[in] mark_path Marker path (root-level 8.3).
 * @param[in] want      Stamp the running firmware expects for this source.
 * @return `true` if the marker exists and exactly equals @p want.
 * @retval true  The marker read back and its bytes equal @p want exactly.
 * @retval false @p mount or @p want is NULL, the marker read failed, or the
 *               bytes differ.
 * @pre @p mount names a mounted volume.
 * @pre @p want is non-NULL.
 * @post @p want and the marker file are unmodified.
 * @post Returns `false` on any read error (treated as stale).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static bool s_cache_is_fresh(ra8_fs_mount_t*                  mount,
                             const char*                      mark_path,
                             const ra8_rabook_import_stamp_t* want)
{
  if (mount == nullptr) {
    return false;
  }
  if (want == nullptr) {
    return false;
  }
  ra8_rabook_import_stamp_t got = {};
  ra8_err_t                 err = s_read_stamp(mount, mark_path, &got);
  if (err != k_ra8_ok) {
    return false;
  }
  return memcmp(&got, want, sizeof(got)) == 0;
}

/**
 * @brief Write a fresh marker file, replacing any stale one.
 * @details Unlinks first because `ra8_fs_write_file` will not overwrite.
 * @param[in] mount Mounted volume.
 * @param[in] path  Marker path (root-level 8.3).
 * @param[in] stamp Stamp to persist.
 * @return Error code from the write.
 * @retval k_ra8_ok           Marker written.
 * @retval k_ra8_err_null_ptr A pointer argument is NULL.
 * @retval k_ra8_err_*        Filesystem write error.
 * @pre @p stamp is non-NULL.
 * @pre @p path names the marker for the current source.
 * @post On success @p path holds exactly @p stamp.
 * @post On error no partial marker is relied upon (re-derived next open).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
s_write_stamp(ra8_fs_mount_t* mount, const char* path, const ra8_rabook_import_stamp_t* stamp)
{
  RA8_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA8_CHECK_NULL_PTR(path, s_tag, "path");
  RA8_CHECK_NULL_PTR(stamp, s_tag, "stamp");
  (void)ra8_fs_unlink(mount, path);
  return ra8_fs_write_file(mount, path, (const uint8_t*)stamp, (uint32_t)sizeof(*stamp));
}

/**
 * @brief Atomically replace @p dst_path with @p tmp_path (temp+rename).
 * @details Unlinks @p dst_path first because `ra8_fs_rename` will not overwrite.
 * @param[in] mount    Mounted volume.
 * @param[in] tmp_path Existing temp file to promote.
 * @param[in] dst_path Final cache path.
 * @return Error code from the rename.
 * @retval k_ra8_ok           Promoted.
 * @retval k_ra8_err_null_ptr A pointer argument is NULL.
 * @retval k_ra8_err_*        Filesystem rename error.
 * @pre @p tmp_path exists; @p dst_path is its intended final name.
 * @pre @p mount names a mounted volume.
 * @post On success @p dst_path holds the temp's bytes and the temp is gone.
 * @post On error the temp remains for a retry; no half cache is published.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_atomic_replace(ra8_fs_mount_t* mount, const char* tmp_path, const char* dst_path)
{
  RA8_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA8_CHECK_NULL_PTR(tmp_path, s_tag, "tmp_path");
  RA8_CHECK_NULL_PTR(dst_path, s_tag, "dst_path");
  (void)ra8_fs_unlink(mount, dst_path);
  return ra8_fs_rename(mount, tmp_path, dst_path);
}

/**
 * @brief Copy a NUL-terminated name into a caller buffer, bounded by capacity.
 * @details Copies @p src into @p dst byte-for-byte, stopping after the first NUL
 *          (which is copied too). If no terminator appears within the first @p cap
 *          bytes the copy is abandoned with @ref k_ra8_err_invalid_size.
 * @param[out] dst Destination buffer.
 * @param[in]  cap Capacity of @p dst.
 * @param[in]  src NUL-terminated 8.3 source name.
 * @return Error code.
 * @retval k_ra8_ok               Copied incl. terminator.
 * @retval k_ra8_err_null_ptr     A pointer argument is NULL.
 * @retval k_ra8_err_invalid_size @p src did not terminate within @p cap.
 * @pre @p dst and @p src are non-NULL.
 * @pre @p src is a NUL-terminated 8.3 name.
 * @post On success @p dst equals @p src including the terminator.
 * @post On error @p dst contents are unspecified.
 * @note Thread-safe: writes only @p dst.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_copy_name(char* dst, uint32_t cap, const char* src)
{
  RA8_CHECK_NULL_PTR(dst, s_tag, "dst");
  RA8_CHECK_NULL_PTR(src, s_tag, "src");
  for (uint32_t i = 0U; i < cap; i++) {
    dst[i] = src[i];
    if (src[i] == '\0') {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_invalid_size;
}

/**
 * @brief Compile-on-miss: temp compile, atomic promote, fresh marker.
 * @details Unlinks any stale temp, runs the injected `cfg->compile` seam to build
 *          @p tmp_name, atomically promotes it to @p cache_name via
 *          @ref s_atomic_replace, then stamps @p mark_name; the first failing step
 *          short-circuits and the live cache is left untouched.
 * @param[in] cfg        Injected dependencies + versioning.
 * @param[in] epub_path  Source path passed to the compiler.
 * @param[in] cache_name Final cache name to publish.
 * @param[in] tmp_name   Temp name the compiler writes to first.
 * @param[in] mark_name  Marker name to stamp on success.
 * @param[in] want       Stamp to persist.
 * @return Error code from compile / promote / marker write.
 * @retval k_ra8_ok    Cache + marker published.
 * @retval k_ra8_err_* Propagated compiler or filesystem error.
 * @pre @p cfg and its `compile` seam are non-NULL.
 * @pre The names are distinct 8.3 paths derived from the same source key.
 * @post On success the cache + marker are present and consistent.
 * @post On error the cache is left untouched (no partial publish).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_compile_and_cache(const ra8_rabook_import_cfg_t*   cfg,
                                     const char*                      epub_path,
                                     const char*                      cache_name,
                                     const char*                      tmp_name,
                                     const char*                      mark_name,
                                     const ra8_rabook_import_stamp_t* want)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  RA8_CHECK_NULL_PTR(cfg->compile, s_tag, "cfg->compile");
  RA8_CHECK_NULL_PTR(want, s_tag, "want");
  (void)ra8_fs_unlink(cfg->mount, tmp_name);
  ra8_err_t err = cfg->compile(cfg->compile_ctx, cfg->mount, epub_path, tmp_name);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "compile seam failed");
    return err;
  }
  err = s_atomic_replace(cfg->mount, tmp_name, cache_name);
  if (err != k_ra8_ok) {
    return err;
  }
  return s_write_stamp(cfg->mount, mark_name, want);
}

/**
 * @brief Validate the public entry's arguments and the mount dependency.
 * @details `cfg->scratch` and `cfg->compile` are not checked here -- they are
 *          guarded at their point of use (@ref s_crc_stream and
 *          @ref s_compile_and_cache respectively), so a NULL either still yields
 *          @ref k_ra8_err_null_ptr without inflating this validator.
 * @param[in] cfg            Config (and its `mount`).
 * @param[in] epub_path      Source path.
 * @param[in] out_cache_path Caller output buffer.
 * @param[in] cache_path_cap Capacity of @p out_cache_path.
 * @param[in] out_outcome    Caller outcome output.
 * @return Error code.
 * @retval k_ra8_ok               Arguments valid.
 * @retval k_ra8_err_null_ptr     A required pointer is NULL.
 * @retval k_ra8_err_invalid_size @p cache_path_cap is too small.
 * @pre Called first from @ref ra8_rabook_import_open.
 * @pre @p cfg may be NULL (checked here before any dereference).
 * @post On `k_ra8_ok` @p cfg and @p cfg->mount are safe to dereference.
 * @post On error the caller returns immediately without side effects.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t s_validate_open_args(const ra8_rabook_import_cfg_t*     cfg,
                                      const char*                        epub_path,
                                      const char*                        out_cache_path,
                                      uint32_t                           cache_path_cap,
                                      const ra8_rabook_import_outcome_t* out_outcome)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  RA8_CHECK_NULL_PTR(epub_path, s_tag, "epub_path");
  RA8_CHECK_NULL_PTR(out_cache_path, s_tag, "out_cache_path");
  RA8_CHECK_NULL_PTR(out_outcome, s_tag, "out_outcome");
  RA8_CHECK_NULL_PTR(cfg->mount, s_tag, "cfg->mount");
  if (cache_path_cap < (uint32_t)k_ra8_rabook_import_name_cap) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

ra8_err_t ra8_rabook_import_open(const ra8_rabook_import_cfg_t* cfg,
                                 const char*                    epub_path,
                                 char*                          out_cache_path,
                                 uint32_t                       cache_path_cap,
                                 ra8_rabook_import_outcome_t*   out_outcome)
{
  ra8_err_t err = s_validate_open_args(cfg, epub_path, out_cache_path, cache_path_cap, out_outcome);
  if (err != k_ra8_ok) {
    return err;
  }

  uint32_t src_size = 0U;
  uint32_t src_crc  = 0U;
  err               = s_compute_source_key(cfg->mount,
                                           epub_path,
                                           cfg->scratch,
                                           cfg->scratch_cap,
                                           &src_size,
                                           &src_crc);
  if (err != k_ra8_ok) {
    return err;
  }

  char cache_name[k_ra8_rabook_import_name_cap];
  char tmp_name[k_ra8_rabook_import_name_cap];
  char mark_name[k_ra8_rabook_import_name_cap];
  err = s_derive_names(src_crc, cache_name, tmp_name, mark_name);
  if (err != k_ra8_ok) {
    return err;
  }

  const ra8_rabook_import_stamp_t want = {
    .magic            = (uint32_t)k_ra8_rabook_import_stamp_magic,
    .format_version   = cfg->format_version,
    .importer_version = cfg->importer_version,
    .source_size      = src_size,
    .source_crc32     = src_crc,
  };

  if (s_cache_is_fresh(cfg->mount, mark_name, &want)) {
    if (s_file_exists(cfg->mount, cache_name)) {
      *out_outcome = k_ra8_rabook_import_hit;
      return s_copy_name(out_cache_path, cache_path_cap, cache_name);
    }
  }

  err = s_compile_and_cache(cfg, epub_path, cache_name, tmp_name, mark_name, &want);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_outcome = k_ra8_rabook_import_compiled;
  return s_copy_name(out_cache_path, cache_path_cap, cache_name);
}
