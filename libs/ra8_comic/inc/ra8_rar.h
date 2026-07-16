/**
 * @file ra8_rar.h
 * @brief Clean-room, read-only RAR archive walker (RAR4 + RAR5 headers, STORE data).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * A first-party, hand-written RAR *reader* for the comic-book-archive (`.cbr`)
 * use case. It parses the two on-disk RAR container generations -- the classic
 * RAR 1.5-4.x block format ("RAR4") and the RAR 5.0 block format ("RAR5") -- and
 * exposes each archive member (name, sizes, data offset, compression method)
 * through a bounded, seek+read backing. It extracts only STORE-method
 * (uncompressed) members, which is the common shape for comic archives whose
 * pages are already-compressed JPEG/PNG images (RAR leaves those near-incompressible
 * bytes stored). Members packed with RAR's LZ/PPM compression are enumerated but
 * report ::k_ra8_rar_method_compressed and are not decoded here -- see the module
 * overview note on the follow-up for full RAR5 decompression.
 *
 * @par Licensing (clean-room, deliberate)
 * This reader is written from the public RAR 5.0 technical note (rarlab.com
 * technote) and the long-published RAR4 block layout. It vendors **no** code from
 * RARLAB's `unrar` package, whose license is non-free and forbids building a
 * RAR-compatible archiver. This file only *reads* an archive and only extracts
 * the STORE (memcpy) case, so it needs none of `unrar`'s decompressor. It is
 * therefore first-party MIT (root `LICENSE.txt`), not SOUP, and carries no SBOM
 * entry. Do not replace it with a restrictively-licensed library.
 *
 * @par Bounded RAM (NASA P10 Rule 3)
 * The walker never materialises the archive. ::ra8_rar_open reads only the
 * signature; ::ra8_rar_next reads one block header at a time into a small internal
 * scratch (a fixed ::k_ra8_rar_hdr_scratch bytes on the stack) plus the caller's
 * name buffer; ::ra8_rar_extract_stored streams one member's data area through the
 * caller's output buffer. Resident set is O(one header + one member), never the
 * file size -- so a multi-hundred-MB volume walks inside a few hundred bytes.
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access.
 *
 * @see ra8_comic.h  The comic facade that turns this walk into a page list.
 * @see https://www.rarlab.com/technote.htm  RAR 5.0 archive format (reference).
 *
 * @since Version 0.1.0
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @typedef ra8_rar5_state_t
 * @brief Forward declaration of the RAR5 decompressor scratch (see ra8_rar5.h).
 * @details ::ra8_rar_extract routes a compressed RAR5 member through the decoder,
 *          which needs this caller-owned pool; the full definition lives in
 *          `ra8_rar5.h` so this header stays free of the decoder internals.
 * @since Version 0.1.0
 */
typedef struct ra8_rar5_state ra8_rar5_state_t;

/**
 * @typedef ra8_rar_read_fn
 * @brief Seek+read backing over the RAR container bytes.
 *
 * @details
 * Mirrors the streaming EPUB/CBZ read seam (offset + length, bytes-actually-read
 * return), so the same `ra8_fs` file / `ra8_vmem` page-cache backing that streams a
 * `.epub` off storage drives a `.cbr` with no whole-file residency. A return
 * shorter than @p len is treated as end-of-file.
 *
 * @param[in]  ctx    Opaque backing context (::ra8_rar_t::ctx).
 * @param[in]  offset Absolute byte offset within the archive.
 * @param[out] buf    Destination buffer (@p len writable bytes).
 * @param[in]  len    Bytes requested.
 * @return Bytes actually read (0 at/after EOF or on error).
 *
 * @note Not thread-safe; the reader serialises access.
 * @since Version 0.1.0
 */
typedef size_t (*ra8_rar_read_fn)(void* ctx, uint64_t offset, void* buf, size_t len);

/**
 * @enum ra8_rar_version_t
 * @brief Which RAR container generation an archive uses.
 * @details Set by ::ra8_rar_open from the file signature; selects the block-header
 *          grammar ::ra8_rar_next applies.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_rar_ver_none = 0U, /**< Not a recognised RAR archive.      */
  k_ra8_rar_ver_4    = 4U, /**< RAR 1.5-4.x block format ("RAR4"). */
  k_ra8_rar_ver_5    = 5U, /**< RAR 5.0 block format ("RAR5").     */
} ra8_rar_version_t;

/**
 * @enum ra8_rar_method_t
 * @brief Normalised compression method of a file member.
 * @details The RAR4 `METHOD` byte and the RAR5 compression-info method field both
 *          collapse to this: either the member is stored verbatim or it is packed
 *          with one of RAR's compressors (which this reader does not decode).
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_rar_method_store      = 0U, /**< Uncompressed: data area is the file bytes.  */
  k_ra8_rar_method_compressed = 1U, /**< Packed with a RAR compressor (not decoded). */
} ra8_rar_method_t;

/**
 * @enum ra8_rar_limits_t
 * @brief Fixed sizes and grammar constants for the RAR block walk.
 * @details Signature lengths, the header scratch window, and the maximum bytes in
 *          one variable-length integer -- the values the parser is coded against.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_rar_sig4_len    = 7U,   /**< "Rar!\x1A\x07\x00" marker length (RAR4).        */
  k_ra8_rar_sig5_len    = 8U,   /**< "Rar!\x1A\x07\x01\x00" signature length (RAR5). */
  k_ra8_rar_hdr_scratch = 128U, /**< Per-block header read window (fixed fields).    */
  k_ra8_rar_vint_max    = 10U,  /**< Max bytes in one RAR5 vint (64-bit value).      */
} ra8_rar_limits_t;

/**
 * @struct ra8_rar_t
 * @brief One open RAR archive: the backing plus the detected generation.
 *
 * @details Populated by ::ra8_rar_open; treat every field as read-only afterwards.
 *          @p read addresses the container file bytes; @p first_off is the offset
 *          of the first block just past the signature, where ::ra8_rar_next begins.
 *
 * @invariant `version != k_ra8_rar_ver_none` and `first_off <= size`.
 * @invariant `read != NULL` between a successful open and the last use.
 *
 * @see ra8_rar_open()
 * @since Version 0.1.0
 */
typedef struct {
  ra8_rar_read_fn   read;      /**< Byte reader over the container file.          */
  void*             ctx;       /**< Context for @ref read.                        */
  uint64_t          size;      /**< Container length in bytes.                    */
  uint64_t          first_off; /**< Offset of the first block past the signature. */
  ra8_rar_version_t version;   /**< Detected container generation.                */
} ra8_rar_t;

/**
 * @struct ra8_rar_entry_t
 * @brief One decoded RAR block: a file member, a directory, or a skipped block.
 *
 * @details Filled by ::ra8_rar_next. @p next_off always advances strictly past the
 *          current block, so a caller can walk the archive with a bounded loop.
 *          For a file member (@p is_file == 1) the data area is
 *          `[data_off, data_off + pack_size)`; a STORE member (@p method ==
 *          ::k_ra8_rar_method_store) has `pack_size == unp_size` literal bytes.
 *
 * @invariant `next_off > <block offset>` for every well-formed block.
 * @invariant `is_file == 0` implies the name buffer is left empty.
 *
 * @see ra8_rar_next()
 * @see ra8_rar_extract_stored()
 * @since Version 0.1.0
 */
typedef struct {
  uint64_t data_off;  /**< Absolute offset of the member's data area.          */
  uint64_t pack_size; /**< Packed (stored/compressed) data length, bytes.      */
  uint64_t unp_size;  /**< Unpacked length, bytes.                             */
  uint64_t next_off;  /**< Absolute offset of the next block header.           */
  uint16_t name_len;  /**< Name bytes copied into the caller buffer (clamped). */
  uint8_t  method;    /**< @ref ra8_rar_method_t (normalised; 0 = store).      */
  uint8_t  is_file;   /**< 1 if this block is a file member header.            */
  uint8_t  is_dir;    /**< 1 if the file member is a directory entry.          */
} ra8_rar_entry_t;

/**
 * @brief Detect a RAR archive's generation and locate its first block.
 *
 * @details Reads the leading signature bytes through @p read, matches the RAR4
 *          marker or the RAR5 signature, and records the offset just past it in
 *          `rar->first_off`. Performs no other I/O; the walk proceeds through
 *          ::ra8_rar_next.
 *
 * @param[out] rar   Reader to populate (caller-owned).
 * @param[in]  read  Byte reader over the container file (non-NULL).
 * @param[in]  ctx   Context passed to @p read.
 * @param[in]  size  Container file length in bytes (> 0).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Recognised RAR archive; @p rar bound.
 * @retval k_ra8_err_null_ptr      @p rar or @p read was NULL.
 * @retval k_ra8_err_invalid_size  @p size is 0 or shorter than a signature.
 * @retval k_ra8_err_not_supported The bytes are not a RAR4 or RAR5 signature.
 *
 * @pre @p read serves offsets `[0, size)` of the container file.
 * @pre @p rar is a writable ::ra8_rar_t.
 * @post On k_ra8_ok, `rar->version` is 4 or 5 and `rar->first_off <= size`.
 * @post On any error @p rar is left with `version == k_ra8_rar_ver_none`.
 *
 * @note Not thread-safe.
 * @see ra8_rar_next()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_rar_open(ra8_rar_t* rar, ra8_rar_read_fn read, void* ctx, uint64_t size);

/**
 * @brief Decode the block header at @p off and advance to the next block.
 *
 * @details Reads one block header through the archive's reader, decodes it under
 *          the archive's generation grammar, and fills @p out -- including
 *          `out->next_off`, the absolute offset of the following block. For a file
 *          member the member name is copied into @p name_buf (up to @p name_cap
 *          bytes; longer names are clamped, `out->name_len` is the copied length).
 *          Non-file blocks (archive header, service, end) set `out->is_file == 0`
 *          and still yield a valid `next_off` so the walk continues.
 *
 * @param[in]  rar      Archive bound by ::ra8_rar_open (non-NULL).
 * @param[in]  off      Absolute offset of the block header (`< rar->size`).
 * @param[out] name_buf Buffer for the member name (non-NULL if @p name_cap > 0).
 * @param[in]  name_cap Capacity of @p name_buf in bytes.
 * @param[out] out      Decoded block descriptor (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Block decoded; @p out (and @p name_buf) filled.
 * @retval k_ra8_err_null_ptr      @p rar or @p out was NULL.
 * @retval k_ra8_err_invalid_state @p rar was never bound by ::ra8_rar_open.
 * @retval k_ra8_err_invalid_arg   @p off is at or past `rar->size`.
 * @retval k_ra8_err_validation_failed A truncated / malformed / non-advancing block.
 *
 * @pre @p rar was populated by ::ra8_rar_open.
 * @pre @p off is the start of a block header.
 * @post On k_ra8_ok, `out->next_off > off` (the walk strictly advances).
 * @post On any error @p out is left zeroed.
 *
 * @note Not thread-safe.
 * @see ra8_rar_extract_stored()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rar_next(const ra8_rar_t* rar,
                                     uint64_t         off,
                                     char*            name_buf,
                                     uint16_t         name_cap,
                                     ra8_rar_entry_t* out);

/**
 * @brief Extract a STORE-method member's data into the caller buffer.
 *
 * @details Streams `ent->unp_size` literal bytes from the member's data area
 *          (`[ent->data_off, ent->data_off + ent->pack_size)`) through the
 *          archive reader into @p buf. Only STORE members are supported: a member
 *          with `ent->method != k_ra8_rar_method_store` returns
 *          ::k_ra8_err_not_supported (the RAR compressor is not implemented).
 *
 * @param[in]  rar Archive bound by ::ra8_rar_open (non-NULL).
 * @param[in]  ent A file member from ::ra8_rar_next (non-NULL, `is_file == 1`).
 * @param[out] buf Destination for the member's bytes (non-NULL).
 * @param[in]  cap Capacity of @p buf in bytes; must be >= `ent->unp_size`.
 * @param[out] got Receives the number of bytes written (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Member copied; `*got == ent->unp_size`.
 * @retval k_ra8_err_null_ptr      A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_state @p rar was never bound by ::ra8_rar_open.
 * @retval k_ra8_err_not_supported @p ent is a directory or a compressed member.
 * @retval k_ra8_err_no_mem        @p cap is smaller than `ent->unp_size`.
 * @retval k_ra8_err_invalid_size  The member overruns the archive, or the reader
 *                                returned a short read.
 *
 * @pre @p rar was populated by ::ra8_rar_open.
 * @pre @p ent came from ::ra8_rar_next on the same @p rar.
 * @post On k_ra8_ok, `buf[0..*got)` holds the member's literal bytes.
 * @post On any error @p buf contents are unspecified and `*got == 0`.
 *
 * @note Not thread-safe.
 * @see ra8_rar_next()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rar_extract_stored(const ra8_rar_t*       rar,
                                               const ra8_rar_entry_t* ent,
                                               uint8_t*               buf,
                                               size_t                 cap,
                                               size_t*                got);

/**
 * @brief Extract any file member -- STORE by copy, RAR5-compressed by decode.
 *
 * @details Dispatches on @p ent's normalised method: a STORE member streams through
 *          ::ra8_rar_extract_stored, while a compressed member of a RAR5 archive is
 *          inflated through ::ra8_rar5_decompress using the caller-owned @p st pool.
 *          A compressed member of a RAR4 archive (the legacy codec) is reported
 *          unsupported. This is the single entry the CBR facade uses so both page
 *          shapes decode behind one call (SOLID Liskov: STORE and compressed pages
 *          are interchangeable to the caller).
 *
 * @param[in]  rar Archive bound by ::ra8_rar_open (non-NULL).
 * @param[in]  ent A file member from ::ra8_rar_next (non-NULL, `is_file == 1`).
 * @param[out] buf Destination for the member's decoded bytes (non-NULL).
 * @param[in]  cap Capacity of @p buf in bytes; must be >= `ent->unp_size`.
 * @param[in,out] st RAR5 decoder scratch, required only for a compressed member.
 * @param[out] got Receives the number of bytes written (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Member decoded; `*got == ent->unp_size`.
 * @retval k_ra8_err_null_ptr      A required pointer argument was NULL (incl. @p st
 *                                for a compressed member).
 * @retval k_ra8_err_invalid_state @p rar was never bound by ::ra8_rar_open.
 * @retval k_ra8_err_not_supported A directory, a non-file, or a RAR4-compressed member.
 * @retval k_ra8_err_no_mem        @p cap is smaller than `ent->unp_size`.
 * @retval k_ra8_err_invalid_size  A STORE member overruns the archive / short read.
 * @retval k_ra8_err_validation_failed A malformed / truncated compressed stream.
 *
 * @pre @p rar was populated by ::ra8_rar_open.
 * @pre @p ent came from ::ra8_rar_next on the same @p rar.
 * @post On k_ra8_ok, `buf[0..*got)` holds the member's original bytes.
 * @post On any error @p buf contents are unspecified and `*got == 0`.
 *
 * @note Not thread-safe.
 * @see ra8_rar5_decompress()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_rar_extract(const ra8_rar_t*       rar,
                                        const ra8_rar_entry_t* ent,
                                        uint8_t*               buf,
                                        size_t                 cap,
                                        ra8_rar5_state_t*      st,
                                        size_t*                got);

#ifdef __cplusplus
}
#endif
