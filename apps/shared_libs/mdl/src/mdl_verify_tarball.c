/**
 * @file mdl_verify_tarball.c
 * @brief Streamed USTAR and gzip-framed CBT structural validation.
 * @details Assembles 512-byte USTAR records incrementally from borrowed reads
 *          and, for `.cbt.gz`, inflates the RFC 1952 frame into that same
 *          state machine while checking the stored CRC32 and ISIZE. No
 *          archive-sized buffer is ever retained.
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <string.h>

#include "mdl_verify_internal.h"
#include "miniz.h"
#include "ra8_attributes.h"

/** @brief POSIX tar field layout and record sizing. */
typedef enum : uint16_t {
  k_tar_name_bytes      = 100U, /**< USTAR name-field extent.                */
  k_tar_size_offset     = 124U, /**< USTAR size-field byte offset.           */
  k_tar_size_bytes      = 12U,  /**< USTAR size-field extent.                */
  k_tar_checksum_offset = 148U, /**< USTAR checksum-field byte offset.       */
  k_tar_checksum_end    = 156U, /**< First byte after the checksum field.    */
  k_tar_type_offset     = 156U, /**< USTAR type-flag byte offset.            */
  k_tar_block_bytes     = 512U, /**< TAR logical record extent.              */
  k_tar_padding_mask    = 511U, /**< Mask used to round payloads to records. */
} mdl_verify_tar_layout_t;

/** @brief Fixed RFC 1952 framing emitted and accepted by mdl. */
typedef enum : uint32_t {
  k_gzip_id_one         = 0x1FU, /**< First RFC 1952 magic byte.          */
  k_gzip_id_two         = 0x8BU, /**< Second RFC 1952 magic byte.         */
  k_gzip_method_deflate = 8U,    /**< RFC 1952 DEFLATE method identifier. */
  k_gzip_header_bytes   = 10U,   /**< Fixed gzip header extent.           */
  k_gzip_trailer_bytes  = 8U,    /**< CRC32 plus ISIZE trailer extent.    */
  k_gzip_isize_offset   = 4U,    /**< ISIZE offset inside that trailer.   */
  k_gzip_min_bytes      = 18U,   /**< Smallest fixed-frame gzip extent.   */
} mdl_verify_gzip_frame_t;

/** @brief Byte shifts for little-endian decoding. */
typedef enum : uint8_t {
  k_u32_byte_one_shift   = 8U,  /**< Shift for byte one.   */
  k_u32_byte_two_shift   = 16U, /**< Shift for byte two.   */
  k_u32_byte_three_shift = 24U, /**< Shift for byte three. */
} mdl_verify_u32_shift_t;

/** @brief Incremental TAR structural state. */
typedef struct {
  uint8_t  block[k_tar_block_bytes]; /**< Partial header record.         */
  uint64_t skip_bytes;               /**< Padded payload remaining.      */
  uint64_t total_bytes;              /**< Total decoded TAR bytes.       */
  size_t   block_used;               /**< Bytes resident in block.       */
  size_t   pages;                    /**< Image member count.            */
  size_t   members;                  /**< Regular member count.          */
  uint8_t  zero_blocks;              /**< Consecutive terminal records.  */
  bool     metadata;                 /**< ComicInfo.xml was found.       */
  bool     ended;                    /**< Two zero blocks were consumed. */
} mdl_tar_stream_t;

/** @brief Streaming gzip inflater and nested TAR consumer. */
typedef struct {
  mz_stream        stream;     /**< Raw-DEFLATE miniz state.       */
  mdl_tar_stream_t tar;        /**< Incremental decoded TAR state. */
  uint8_t*         output;     /**< One bounded decoded chunk.     */
  uint32_t         output_cap; /**< Extent of output.              */
  uint32_t         crc;        /**< Running decoded CRC32.         */
  uint64_t         raw_bytes;  /**< Exact decoded byte count.      */
  bool             ended;      /**< DEFLATE end marker observed.   */
} mdl_gzip_stream_t;

/**
 * @brief Read one exact structural span. @details Maps a successful early EOF to validation failure.
 * @param[in,out] io Open input. @param[out] destination Output buffer. @param[in] length Required bytes. @return Status. @retval k_ra8_ok When length bytes arrive.
 * @pre io is open. @pre destination spans length bytes.
 * @post Success initializes the complete span. @post Short EOF is explicit corruption. @note Backend faults are preserved. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_io_read_exact(mdl_verify_io_t* io, uint8_t* destination, size_t length)
{
  size_t          got = 0U;
  const ra8_err_t err = priv_mdl_verify_io_read_up_to(io, destination, length, &got);
  if (err != k_ra8_ok) {
    return err;
  }
  return (got == length) ? k_ra8_ok : k_ra8_err_validation_failed;
}

/**
 * @brief Parse a bounded TAR octal field. @details Accepts padding but rejects non-octal data and overflow.
 * @param[in] field Input field. @param[in] length Field extent. @param[out] out Parsed value. @return Status. @retval k_ra8_ok For valid octal.
 * @pre field and out are valid. @pre field spans length bytes.
 * @post Success initializes out. @post Failure exposes no partial value. @note Base-256 extensions are unsupported. @since v0.1.0
 */
RA8_INTERNAL static bool internal_parse_octal(const uint8_t* field, size_t length, uint64_t* out)
{
  size_t index = 0U;
  while ((index < length) && ((field[index] == (uint8_t)' ') || (field[index] == (uint8_t)'\0'))) {
    ++index;
  }
  uint64_t value = 0U;
  bool     any   = false;
  for (; (index < length) && (field[index] != (uint8_t)'\0') && (field[index] != (uint8_t)' ');
       ++index) {
    if ((field[index] < (uint8_t)'0') || (field[index] > (uint8_t)'7') ||
        (value > (UINT64_MAX >> 3U))) {
      return false;
    }
    const uint64_t digit = (uint64_t)field[index] - (uint64_t)(uint8_t)'0';
    value                = (value << 3U) + digit;
    any                  = true;
  }
  *out = value;
  return any;
}

/**
 * @brief Validate one TAR header. @details Checks checksum, type, path, count, and padded payload size.
 * @param[in,out] state Incremental TAR state. @return Status. @retval k_ra8_ok For a supported member.
 * @pre state holds one complete header. @pre state counters are bounded.
 * @post Success advances member accounting. @post Failure stops validation. @note Only regular files are accepted. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tar_member(mdl_tar_stream_t* state)
{
  unsigned checksum = 0U;
  for (size_t i = 0U; i < k_tar_block_bytes; ++i) {
    checksum +=
      ((i >= k_tar_checksum_offset) && (i < k_tar_checksum_end)) ? (unsigned)' ' : state->block[i];
  }
  uint64_t expected = 0U;
  uint64_t size     = 0U;
  if (!internal_parse_octal(&state->block[k_tar_checksum_offset],
                            k_tar_checksum_end - k_tar_checksum_offset,
                            &expected) ||
      ((uint64_t)checksum != expected) ||
      !internal_parse_octal(&state->block[k_tar_size_offset], k_tar_size_bytes, &size) ||
      (state->block[k_tar_type_offset] != (uint8_t)'0') ||
      (size > UINT64_MAX - k_tar_padding_mask)) {
    return k_ra8_err_validation_failed;
  }
  char name[k_tar_name_bytes + 1U];
  (void)memcpy(name, state->block, k_tar_name_bytes);
  name[k_tar_name_bytes] = '\0';
  if (!priv_mdl_verify_safe_member_name(name)) {
    return k_ra8_err_validation_failed;
  }
  if (state->members >= k_verify_member_max) {
    return k_ra8_err_invalid_size;
  }
  ++state->members;
  state->pages += priv_mdl_verify_is_image(name) ? 1U : 0U;
  state->metadata   = state->metadata || (strcmp(name, "ComicInfo.xml") == 0);
  state->skip_bytes = ((size + k_tar_padding_mask) / k_tar_block_bytes) * k_tar_block_bytes;
  return k_ra8_ok;
}

/**
 * @brief Process one complete 512-byte TAR block once fully buffered.
 * @details Classifies the block as all-zero (an end-of-archive marker) or a
 * real header, updating end-of-archive and skip-byte state.
 * @param[in,out] state Incremental TAR validation state with a full block.
 * @return Status. @retval k_ra8_ok The block was a zero marker or a valid header.
 * @pre state->block_used == k_tar_block_bytes on entry.
 * @pre @p state is non-NULL and its member counters are bounded.
 * @post state->block_used is reset to zero. @post A zero block advances the
 * end-of-archive counter; a header block resets it and derives the next
 * skip-byte count. @note Not thread-safe; shares the caller's incremental
 * state. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tar_process_block(mdl_tar_stream_t* state)
{
  bool zero = true;
  for (size_t i = 0U; i < k_tar_block_bytes; ++i) {
    zero = zero && (state->block[i] == 0U);
  }
  state->block_used = 0U;
  if (zero) {
    state->zero_blocks += 1U;
    state->ended = state->zero_blocks >= 2U;
    return k_ra8_ok;
  }
  state->zero_blocks = 0U;
  return internal_tar_member(state);
}

/**
 * @brief Feed bytes into TAR validation. @details Assembles headers and skips bounded padded payloads incrementally.
 * @param[in,out] state Incremental state. @param[in] bytes Input bytes. @param[in] length Input extent. @return Status. @retval k_ra8_ok When the prefix remains valid.
 * @pre state and bytes are valid. @pre bytes spans length bytes.
 * @post Every input byte is consumed or rejected. @post Counters remain bounded. @note Data after termination must be zero. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_tar_feed(mdl_tar_stream_t* state, const uint8_t* bytes, size_t length)
{
  if (state->total_bytes > (UINT64_MAX - length)) {
    return k_ra8_err_invalid_size;
  }
  state->total_bytes += length;
  size_t offset = 0U;
  while (offset < length) {
    if (state->ended) {
      if (bytes[offset++] != 0U) {
        return k_ra8_err_validation_failed;
      }
      continue;
    }
    if (state->skip_bytes != 0U) {
      const uint64_t available = (uint64_t)length - (uint64_t)offset;
      const size_t   consumed =
        (state->skip_bytes < available) ? (size_t)state->skip_bytes : (length - offset);
      state->skip_bytes -= consumed;
      offset += consumed;
      continue;
    }
    const size_t missing = k_tar_block_bytes - state->block_used;
    const size_t copied  = ((length - offset) < missing) ? (length - offset) : missing;
    (void)memcpy(&state->block[state->block_used], &bytes[offset], copied);
    state->block_used += copied;
    offset += copied;
    if (state->block_used == k_tar_block_bytes) {
      const ra8_err_t error = internal_tar_process_block(state);
      if (error != k_ra8_ok) {
        return error;
      }
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Finish TAR validation. @details Requires aligned complete termination before publishing counts.
 * @param[in,out] state Completed stream state. @param[in,out] report Candidate report. @return Status. @retval k_ra8_ok For a complete TAR.
 * @pre state and report are valid. @pre All source bytes were fed.
 * @post Success fills report counts. @post Failure leaves caller output private. @note Two zero records are required. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_tar_finish(const mdl_tar_stream_t* state,
                                                  mdl_verify_report_t*    report)
{
  if (!state->ended || (state->block_used != 0U) || (state->skip_bytes != 0U) ||
      ((state->total_bytes % k_tar_block_bytes) != 0U) || (state->pages == 0U) ||
      !state->metadata) {
    return k_ra8_err_validation_failed;
  }
  report->page_count       = state->pages;
  report->member_count     = state->members;
  report->metadata_present = state->metadata;
  return k_ra8_ok;
}

RA8_PRIV ra8_err_t priv_mdl_verify_tar(mdl_verify_io_t* io, mdl_verify_report_t* report)
{
  mdl_storage_t*   storage = io->storage;
  ra8_err_t        error   = k_ra8_ok;
  mdl_tar_stream_t tar     = {};
  bool             done    = false;
  while (!done) {
    size_t got = 0U;
    error = priv_mdl_verify_io_read_up_to(io, storage->io_buffer, storage->io_buffer_bytes, &got);
    if (error != k_ra8_ok) {
      done = true;
    } else if (got == 0U) {
      error = internal_tar_finish(&tar, report);
      done  = true;
    } else {
      error = internal_tar_feed(&tar, storage->io_buffer, got);
      done  = (error != k_ra8_ok);
    }
  }
  return error;
}

/**
 * @brief Decode one little-endian word. @details Combines exactly four bytes without alignment assumptions.
 * @param[in] bytes Four input bytes. @return Decoded word. @retval UINT32_MAX When encoded as all ones.
 * @pre bytes spans four bytes. @pre bytes remains readable for the call.
 * @post bytes is unchanged. @post The result is host-endian independent. @note Used by gzip trailers. @since v0.1.0
 */
RA8_INTERNAL static uint32_t internal_get_u32le(const uint8_t* bytes)
{
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << k_u32_byte_one_shift) |
         ((uint32_t)bytes[2] << k_u32_byte_two_shift) |
         ((uint32_t)bytes[3] << k_u32_byte_three_shift);
}

/**
 * @brief Inflate a raw-DEFLATE chunk. @details Feeds bounded decoded chunks into TAR while tracking CRC and size.
 * @param[in,out] gzip Inflate and TAR state. @param[in] bytes Compressed input. @param[in] length Input extent. @return Status. @retval k_ra8_ok While the stream is valid.
 * @pre gzip is initialized. @pre bytes is non-null even when length is zero.
 * @post Input is consumed or rejected. @post Produced bytes update TAR and CRC. @note Output never exceeds its fixed chunk. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_gzip_feed(mdl_gzip_stream_t* gzip, const uint8_t* bytes, uint32_t length)
{
  gzip->stream.next_in  = bytes;
  gzip->stream.avail_in = length;
  do {
    const mz_uint before_in = gzip->stream.avail_in;
    gzip->stream.next_out   = gzip->output;
    gzip->stream.avail_out  = gzip->output_cap;
    const int      status   = mz_inflate(&gzip->stream, MZ_NO_FLUSH);
    const uint32_t produced = gzip->output_cap - gzip->stream.avail_out;
    if (gzip->raw_bytes > (uint64_t)UINT32_MAX - produced) {
      return k_ra8_err_invalid_size;
    }
    if (produced != 0U) {
      const ra8_err_t error = internal_tar_feed(&gzip->tar, gzip->output, produced);
      if (error != k_ra8_ok) {
        return error;
      }
      gzip->crc = (uint32_t)mz_crc32(gzip->crc, gzip->output, produced);
      gzip->raw_bytes += produced;
    }
    if (status == MZ_STREAM_END) {
      gzip->ended = true;
      return (gzip->stream.avail_in == 0U) ? k_ra8_ok : k_ra8_err_validation_failed;
    }
    if ((status != MZ_OK) || ((before_in == gzip->stream.avail_in) && (produced == 0U))) {
      return k_ra8_err_validation_failed;
    }
  } while ((gzip->stream.avail_in != 0U) || (gzip->stream.avail_out == 0U));
  return k_ra8_ok;
}

/**
 * @brief Finish raw-DEFLATE validation. @details Requires an explicit end marker without additional compressed bytes.
 * @param[in,out] gzip Initialized stream state. @return Status. @retval k_ra8_ok When the end marker is observed.
 * @pre gzip is initialized. @pre All compressed input was supplied.
 * @post Success sets ended. @post Stalls become validation failures. @note A non-null zero-byte sentinel avoids miniz UB. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_gzip_finish_deflate(mdl_gzip_stream_t* gzip)
{
  const uint8_t empty_input = 0U;
  while (!gzip->ended) {
    /* miniz performs pointer arithmetic even for zero-byte input. */
    const ra8_err_t error = internal_gzip_feed(gzip, &empty_input, 0U);
    if (error != k_ra8_ok) {
      return error;
    }
    if (!gzip->ended && (gzip->stream.avail_out != 0U)) {
      return k_ra8_err_validation_failed;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Validate the fixed gzip header. @details Accepts DEFLATE with no optional RFC 1952 fields.
 * @param[in] header Fixed header bytes. @return Whether supported. @retval true For the accepted frame.
 * @pre header spans k_gzip_header_bytes. @pre header is readable.
 * @post header is unchanged. @post The result is deterministic. @note Optional fields are rejected explicitly. @since v0.1.0
 */
RA8_INTERNAL static bool internal_gzip_header_valid(const uint8_t* header)
{
  return (header[0] == k_gzip_id_one) && (header[1] == k_gzip_id_two) &&
         (header[2] == k_gzip_method_deflate) && (header[3] == 0U);
}

/**
 * @brief Validate gzip trailer accounting. @details Compares stored CRC32 and ISIZE with streamed output.
 * @param[in] trailer Eight trailer bytes. @param[in,out] gzip Completed stream state. @return Whether both fields match. @retval true On an exact match.
 * @pre Both pointers are valid. @pre raw_bytes fits RFC 1952 ISIZE policy.
 * @post Inputs are unchanged. @post Both fields are checked. @note ISIZE is compared modulo uint32_t. @since v0.1.0
 */
RA8_INTERNAL static bool internal_gzip_trailer_valid(const uint8_t*           trailer,
                                                     const mdl_gzip_stream_t* gzip)
{
  return (internal_get_u32le(trailer) == gzip->crc) &&
         (internal_get_u32le(&trailer[k_gzip_isize_offset]) == (uint32_t)gzip->raw_bytes);
}

/**
 * @brief Initialize the gzip inflater bound to workspace-backed scratch.
 * @details Reserves the output buffer from workspace, wires the miniz
 * allocator to the arena, and opens the raw-deflate inflater.
 * @param[in,out] workspace Scratch arena backing both the output buffer and
 * miniz's internal allocations.
 * @param[in,out] arena Miniz allocator state bound to @p workspace.
 * @param[in,out] gzip Stream state to initialize.
 * @return Status. @retval k_ra8_ok The output buffer and inflater are ready.
 * @pre gzip->output_cap and gzip->crc are already set by the caller.
 * @pre @p workspace was reset and @p arena is bound to it.
 * @post On success gzip->output and gzip->stream are ready to feed.
 * @post Failure latches arena exhaustion and opens no inflater.
 * @note Not thread-safe for a shared workspace. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_gzip_init_inflate(mdl_export_workspace_t* workspace,
                                                         mdl_verify_arena_t*     arena,
                                                         mdl_gzip_stream_t*      gzip)
{
  gzip->output =
    (uint8_t*)priv_mdl_verify_workspace_take(workspace, gzip->output_cap, alignof(max_align_t));
  arena->exhausted    = gzip->output == nullptr;
  gzip->stream.zalloc = priv_mdl_verify_arena_alloc;
  gzip->stream.zfree  = priv_mdl_verify_arena_free;
  gzip->stream.opaque = arena;
  if ((gzip->output == nullptr) ||
      (mz_inflateInit2(&gzip->stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK)) {
    return arena->exhausted ? k_ra8_err_invalid_size : k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Feed the compressed remainder into the inflater and validate the trailer.
 * @details Streams chunked reads until end-of-stream or @p remaining is
 * exhausted, finishes any pending deflate output, then reads and checks the
 * eight-byte gzip trailer against the streamed CRC/size.
 * @param[in,out] io Borrowed input positioned after the gzip header.
 * @param[in,out] arena Miniz allocator state; inspected for exhaustion.
 * @param[in,out] gzip Stream state advanced by this call.
 * @param[in] remaining Compressed bytes left to read, excluding the trailer.
 * @return Status. @retval k_ra8_ok The stream ended cleanly and the trailer matches.
 * @pre @p io is positioned at the first compressed byte.
 * @pre @p gzip was successfully initialized by ::internal_gzip_init_inflate.
 * @post On k_ra8_ok every compressed byte and the trailer have been consumed.
 * @post Failure reports the first framing, CRC, or capacity fault.
 * @note Not thread-safe for a shared input or workspace. @since v0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_gzip_consume(mdl_verify_io_t*    io,
                                                    mdl_verify_arena_t* arena,
                                                    mdl_gzip_stream_t*  gzip,
                                                    uint64_t            remaining)
{
  mdl_storage_t* storage = io->storage;
  ra8_err_t      error   = k_ra8_ok;
  uint64_t       left    = remaining;
  while ((error == k_ra8_ok) && (left != 0U) && !gzip->ended) {
    const uint32_t chunk =
      (left < storage->io_buffer_bytes) ? (uint32_t)left : storage->io_buffer_bytes;
    error = internal_io_read_exact(io, storage->io_buffer, chunk);
    if (error == k_ra8_ok) {
      left -= chunk;
      error = internal_gzip_feed(gzip, storage->io_buffer, chunk);
    }
  }
  if ((error == k_ra8_ok) && gzip->ended && (left != 0U)) {
    error = k_ra8_err_validation_failed;
  }
  if ((error == k_ra8_ok) && !gzip->ended) {
    error = internal_gzip_finish_deflate(gzip);
  }
  uint8_t trailer[k_gzip_trailer_bytes];
  if (error == k_ra8_ok) {
    error = internal_io_read_exact(io, trailer, sizeof(trailer));
  }
  if ((error == k_ra8_ok) && !internal_gzip_trailer_valid(trailer, gzip)) {
    error = k_ra8_err_validation_failed;
  }
  if ((error == k_ra8_ok) && arena->exhausted) {
    error = k_ra8_err_invalid_size;
  }
  return error;
}

RA8_PRIV ra8_err_t priv_mdl_verify_gzip_tar(mdl_verify_io_t*        io,
                                            mdl_export_workspace_t* workspace,
                                            mdl_verify_report_t*    report)
{
  ra8_err_t error;
  uint8_t   header[k_gzip_header_bytes];
  error = (io->size_bytes < k_gzip_min_bytes) ? k_ra8_err_validation_failed
                                              : internal_io_read_exact(io, header, sizeof(header));
  if ((error == k_ra8_ok) && !internal_gzip_header_valid(header)) {
    error = k_ra8_err_validation_failed;
  }
  mdl_verify_arena_t arena = {.workspace = workspace};
  mdl_gzip_stream_t  gzip  = {.output_cap = k_mdl_storage_io_bytes, .crc = MZ_CRC32_INIT};
  if (error == k_ra8_ok) {
    error = internal_gzip_init_inflate(workspace, &arena, &gzip);
  }
  if (error == k_ra8_ok) {
    const uint64_t remaining =
      (io->size_bytes >= k_gzip_min_bytes) ? io->size_bytes - k_gzip_min_bytes : 0U;
    error = internal_gzip_consume(io, &arena, &gzip, remaining);
  }
  if (gzip.stream.state != nullptr) {
    (void)mz_inflateEnd(&gzip.stream);
  }
  if (error == k_ra8_ok) {
    error = internal_tar_finish(&gzip.tar, report);
  }
  return error;
}
