/**
 * @file ra8_fmt.h
 * @brief One-file converter / inspector for the first-party content formats.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * `ra8_fmt` is the single-unit counterpart to the batch producers
 * (`tools/media_dl`, `tools/epub_compile`): it converts **one** input file into
 * **one** first-party container, and -- the half that makes it a debugging
 * instrument rather than a converter -- parses a container back and prints its
 * structure (magic, header fields, section/tile tables with byte offsets and
 * lengths, checksums) plus a validity verdict.
 *
 * The three verbs answer three different questions:
 *   - `convert` -- can we build this container from this input at all?
 *   - `inspect` -- is the container we built structurally what we think it is?
 *   - `verify`  -- does it decode back to the bytes we put in?
 *
 * `inspect` and `verify` together settle the question every rendering bug
 * raises: is the defect in the **file** (producer) or in the **reader**
 * (consumer)? A container that inspects clean and verifies byte-identical
 * exonerates the producer and moves the search to the consumer.
 *
 * ## Adding a format
 *
 * Formats plug in behind ::ra8_fmt_desc_t, a function-pointer descriptor
 * (Dependency Inversion): implement the verbs the format can support, leave the
 * rest NULL, and add one row to the registry in `ra8_fmt_registry.c`. Nothing
 * else changes -- CLI dispatch and the usage text are driven off the registry.
 *
 *
 * @see ra8_jof.h  The band-tile atlas this tool was first built to debug.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 *

 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "ra8_host_arena.h"
#include "ra8_err.h"

/**
 * @enum ra8_fmt_limits_t
 * @brief Fixed sizing limits of the tool's buffers and reports.
 *
 * @details These bound every loop and allocation in the tool (NASA Rule 2).
 *          `k_ra8_fmt_max_dump_rows` caps how many table rows `inspect` prints
 *          before eliding, so dumping a 65536-tile atlas cannot flood a
 *          terminal.
 *
 * @invariant `k_ra8_fmt_max_dump_rows <= k_ra8_jof_max_tiles`.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_fmt_max_dump_rows = 4096U,      /**< Table rows printed before eliding. */
  k_ra8_fmt_max_input     = 268435456U, /**< Largest input accepted (256 MiB).  */
  k_ra8_fmt_hex_row       = 16U,        /**< Bytes per hex-dump row.            */
} ra8_fmt_limits_t;

/**
 * @struct ra8_fmt_blob_t
 * @brief An owned byte buffer read from disk (the tool's input currency).
 *
 * @details Every verb receives its input as a fully resident blob: this is a
 *          host debugging instrument, not firmware, so bounded-RAM streaming is
 *          not a requirement here and a flat buffer makes byte-level inspection
 *          trivial.
 *
 * @invariant `bytes != nullptr` implies `len > 0`.
 * @see ra8_fmt_slurp()
 * @since 0.1.0
 */
typedef struct {
  uint8_t* bytes; /**< Heap-owned contents (released by ra8_fmt_blob_free). */
  size_t   len;   /**< Byte count held at `bytes`.                          */
} ra8_fmt_blob_t;

/**
 * @struct ra8_fmt_opts_t
 * @brief Parsed command-line options handed to every verb.
 *
 * @details One options struct serves all verbs (Open/Closed: a new format
 *          consumes the fields it needs and ignores the rest), passed to the
 *          descriptor entry points the way this repo passes a config struct to
 *          an `*_init()`.
 *
 * @invariant `in_path` and `report` are non-NULL for every verb.
 * @see ra8_fmt_desc_t
 * @since 0.1.0
 */
typedef struct {
  const char* in_path;  /**< Source file path (required).                    */
  const char* out_path; /**< Destination path, or NULL for a read-only verb. */
  bool        verbose;  /**< Print per-entry tables, not just the summary.   */
  FILE*       report;   /**< Human-readable output sink (never NULL).        */
} ra8_fmt_opts_t;

/**
 * @typedef ra8_fmt_convert_fn
 * @brief Convert one source file into one container of this format.
 * @param[in] src  Slurped source bytes (non-NULL).
 * @param[in] opts Options carrying `out_path` (non-NULL) and the report sink.
 * @return k_ra8_ok when `opts->out_path` holds a complete container.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_fmt_convert_fn)(ra8_arena_t*          arena,
                                        const ra8_fmt_blob_t* src,
                                        const ra8_fmt_opts_t* opts);

/**
 * @typedef ra8_fmt_inspect_fn
 * @brief Parse a container and print its structure plus a validity verdict.
 * @param[in] src  Container bytes (non-NULL).
 * @param[in] opts Options carrying the report sink and verbosity.
 * @return k_ra8_ok when the container validated clean; an error code when it is
 *         structurally invalid (the report states why).
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_fmt_inspect_fn)(ra8_arena_t*          arena,
                                        const ra8_fmt_blob_t* src,
                                        const ra8_fmt_opts_t* opts);

/**
 * @typedef ra8_fmt_verify_fn
 * @brief Round-trip a source: encode, decode back, compare against the source.
 * @param[in] src  Source bytes to round-trip (non-NULL).
 * @param[in] opts Options carrying the report sink and optional dump path.
 * @return k_ra8_ok when the decoded result matched the source exactly.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_fmt_verify_fn)(ra8_arena_t*          arena,
                                       const ra8_fmt_blob_t* src,
                                       const ra8_fmt_opts_t* opts);

/**
 * @typedef ra8_fmt_sniff_fn
 * @brief Report whether a blob's leading bytes match this format's magic.
 * @param[in] src Candidate container bytes (non-NULL).
 * @return `true` when the magic matches, else `false`.
 * @since 0.1.0
 */
typedef bool (*ra8_fmt_sniff_fn)(const ra8_fmt_blob_t* src);

/**
 * @struct ra8_fmt_desc_t
 * @brief One format's capabilities -- the Dependency-Inversion seam of the tool.
 *
 * @details A descriptor declares only the verbs its format can honestly
 *          support; a NULL entry point makes the CLI report "unsupported for
 *          this format" rather than failing obscurely. `sniff` lets `inspect`
 *          auto-detect the format so `--format` becomes optional.
 *
 * @invariant `name`, `ext` and `summary` are non-NULL; at least one verb is
 *            non-NULL.
 * @see ra8_fmt_registry()
 * @since 0.1.0
 */
typedef struct {
  const char*        name;    /**< CLI selector, e.g. "jof".                */
  const char*        ext;     /**< Canonical file extension, e.g. ".jof".   */
  const char*        summary; /**< One-line description for the usage text. */
  ra8_fmt_sniff_fn   sniff;   /**< Magic test for auto-detection, or NULL.  */
  ra8_fmt_convert_fn convert; /**< Single-unit converter, or NULL.          */
  ra8_fmt_inspect_fn inspect; /**< Structure dumper, or NULL.               */
  ra8_fmt_verify_fn  verify;  /**< Round-trip checker, or NULL.             */
} ra8_fmt_desc_t;

/**
 * @brief Return the registry of every format this tool knows.
 *
 * @details Single source of truth for CLI dispatch and the usage text; adding a
 *          format means adding one row here.
 *
 * @param[out] out_count Receives the descriptor count (non-NULL).
 *
 * @return Pointer to the static descriptor array.
 * @retval non-NULL Always; the registry is statically initialised.
 *
 * @pre @p out_count is writable.
 * @pre The registry needs no initialisation call.
 * @post `*out_count >= 1`.
 * @post The returned array is immutable for the process lifetime.
 * @note Thread-safe (returns immutable static data).
 * @since 0.1.0
 */
[[nodiscard]] const ra8_fmt_desc_t* ra8_fmt_registry(size_t* out_count);

/**
 * @brief Look up a format descriptor by its CLI name.
 *
 * @param[in] name Format selector to match, e.g. "rabook".
 *
 * @return Matching descriptor, or NULL when @p name is unknown.
 * @retval NULL @p name is NULL or matches no registered format.
 *
 * @pre @p name is a NUL-terminated string or NULL.
 * @pre The registry is statically initialised.
 * @post No state is mutated.
 * @post A non-NULL result points into the static registry.
 * @note Thread-safe (pure over immutable data).
 * @since 0.1.0
 */
[[nodiscard]] const ra8_fmt_desc_t* ra8_fmt_find(const char* name);

/**
 * @brief Auto-detect a blob's format by asking each descriptor's `sniff`.
 *
 * @param[in] src Candidate container bytes.
 *
 * @return The first descriptor whose magic matches, or NULL when none does.
 * @retval NULL @p src is NULL/empty, or no registered magic matched.
 *
 * @pre @p src is NULL or points at a slurped blob.
 * @pre Every registered `sniff` tolerates a short blob.
 * @post No state is mutated.
 * @post A non-NULL result points into the static registry.
 * @note Thread-safe (pure over immutable data).
 * @since 0.1.0
 */
[[nodiscard]] const ra8_fmt_desc_t* ra8_fmt_sniff(const ra8_fmt_blob_t* src);

/**
 * @brief Read a whole file into a heap-owned blob.
 *
 * @param[in]  path Filesystem path to read (non-NULL).
 * @param[out] out  Receives the owned blob (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               File resident in `*out`.
 * @retval k_ra8_err_null_ptr     @p path or @p out is NULL.
 * @retval k_ra8_err_not_found    The file could not be opened or read.
 * @retval k_ra8_err_invalid_size The file is empty or above the input cap.
 * @retval k_ra8_err_no_mem       The buffer could not be allocated.
 *
 * @pre @p path names a regular readable file.
 * @pre @p out is writable.
 * @post On success `out->bytes` holds `out->len` bytes and must be freed.
 * @post On any error `*out` is zeroed and owns nothing.
 * @note Not thread-safe (uses stdio).
 * @see ra8_fmt_blob_free()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_slurp(ra8_arena_t* arena, const char* path, ra8_fmt_blob_t* out);

/**
 * @brief Release a blob's buffer and zero it.
 *
 * @details
 * Frees the buffer ::ra8_fmt_slurp allocated and resets the blob to the empty
 * state, so a released blob reads as owning nothing and a second call is a safe
 * no-op. A NULL blob is also a no-op.
 *
 * @param[in,out] blob Blob to release (NULL is a no-op).
 *
 * @pre @p blob is NULL or was filled by ::ra8_fmt_slurp.
 * @pre The buffer has not already been released.
 * @post `blob->bytes == nullptr` and `blob->len == 0`.
 * @post The previously owned buffer returned to the allocator.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void ra8_fmt_blob_free(ra8_fmt_blob_t* blob);

/**
 * @brief Write @p len bytes to @p path, replacing any existing file.
 *
 * @param[in] path Destination path (non-NULL).
 * @param[in] buf  Bytes to write (non-NULL when @p len > 0).
 * @param[in] len  Byte count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           All @p len bytes written and the file closed.
 * @retval k_ra8_err_null_ptr @p path is NULL, or @p buf is NULL with @p len > 0.
 * @retval k_ra8_fail         The file could not be opened, written or closed.
 *
 * @pre @p path names a writable location.
 * @pre @p buf holds @p len readable bytes.
 * @post On success the file holds exactly @p len bytes.
 * @post On failure the partial file is removed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_write_file(const char* path, const uint8_t* buf, size_t len);

/**
 * @brief Print a labelled hex + ASCII dump of a byte window.
 *
 * @details The byte-level evidence half of `inspect`: renders
 *          ::k_ra8_fmt_hex_row bytes per line against the absolute offset, so a
 *          reported header or index window can be read directly.
 *
 * @param[in] out   Report sink (non-NULL).
 * @param[in] label Section name printed above the dump (non-NULL).
 * @param[in] buf   Bytes to render (non-NULL when @p len > 0).
 * @param[in] len   Byte count to render.
 * @param[in] base  Absolute offset of `buf[0]`, printed in the margin.
 *
 * @pre @p out is an open stream and @p label is NUL-terminated.
 * @pre @p buf holds @p len readable bytes.
 * @post `ceil(len / 16)` dump lines were written after the label.
 * @post No state other than the stream is mutated.
 * @note Not thread-safe (writes one stream).
 * @since 0.1.0
 */
void ra8_fmt_hex_dump(FILE* out, const char* label, const uint8_t* buf, size_t len, uint64_t base);

/**
 * @brief Read a little-endian uint16 from a byte window.
 * @param[in] buf Source bytes (at least 2 readable).
 * @return The decoded value.
 * @retval 0 Both source bytes were zero.
 * @pre @p buf holds 2 readable bytes.
 * @pre The field is little-endian per the format spec.
 * @post No state is mutated.
 * @post The result equals `buf[0] | (buf[1] << 8)`.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] uint16_t ra8_fmt_rd_u16(const uint8_t* buf);

/**
 * @brief Read a little-endian uint32 from a byte window.
 * @param[in] buf Source bytes (at least 4 readable).
 * @return The decoded value.
 * @retval 0 All four source bytes were zero.
 * @pre @p buf holds 4 readable bytes.
 * @pre The field is little-endian per the format spec.
 * @post No state is mutated.
 * @post The result equals the four bytes assembled little-endian.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_fmt_rd_u32(const uint8_t* buf);

/**
 * @brief Compare a byte window against a 4-character format magic.
 * @param[in] src   Candidate container bytes.
 * @param[in] magic 4-character magic to match (non-NULL, NUL-terminated).
 * @return `true` when @p src is long enough and its first 4 bytes match.
 * @retval false @p src is NULL, shorter than 4 bytes, or does not match.
 * @pre @p magic points at exactly 4 significant characters.
 * @pre @p src is NULL or a slurped blob.
 * @post No state is mutated.
 * @post The result depends only on the first 4 bytes of @p src.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_fmt_magic_is(const ra8_fmt_blob_t* src, const char* magic);
