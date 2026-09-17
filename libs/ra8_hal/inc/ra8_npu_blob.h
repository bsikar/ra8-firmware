/**
 * @file ra8_npu_blob.h
 * @brief `.npub` linkable-blob container for a Vela-compiled Ethos-U55 model
 * @ingroup grp_hal_system
 *
 * @details
 * Defines the small, first-party, little-endian container the OFFLINE Vela build
 * step (`tools/vela/src/vela_gen.py`) emits and the ON-TARGET loader
 * (`ra8_npu_loader.c`) consumes. A bare-metal target does NOT parse a TFLite
 * flatbuffer at run time; instead the host build step distills a Vela
 * `_vela.tflite` (the `ethos-u` custom operator's command stream plus the tensor
 * region layout) into this lean `.npub` blob, and the loader maps that blob
 * straight into an ::ra8_npu_job_t for `ra8_npu_submit()`.
 *
 * ## Layout (all fields little-endian `uint32_t`)
 *
 * @code
 *   +---------------------------------------------------------------+
 *   | header  (::k_ra8_npu_blob_header_words words)                 |
 *   |   [0] magic       == ::k_ra8_npu_blob_magic  ("NPU1")         |
 *   |   [1] version     == ::k_ra8_npu_blob_version                 |
 *   |   [2] total_bytes  (whole blob length)                        |
 *   |   [3] region_count (number of region descriptors)            |
 *   |   [4] cmd_offset   (byte offset to the command stream)       |
 *   |   [5] cmd_bytes    (command-stream length, > 0)              |
 *   |   [6] accel        (::ra8_npu_blob_accel_t, informational)    |
 *   |   [7] checksum     (FNV-1a over every payload byte)          |
 *   +---------------------------------------------------------------+
 *   | region table  (region_count * ::k_ra8_npu_blob_region_words) |
 *   |   each: [role][flags][size][data_offset]                     |
 *   +---------------------------------------------------------------+
 *   | command stream  (cmd_bytes bytes at cmd_offset)             |
 *   +---------------------------------------------------------------+
 *   | baked region data blocks (weights / const inputs)           |
 *   +---------------------------------------------------------------+
 * @endcode
 *
 * A region descriptor is either BAKED (its bytes live in the blob at
 * `data_offset`, e.g. the read-only weight arena) or RUNTIME (allocated by the
 * caller in NPU-visible SRAM, e.g. scratch / output activations). The loader
 * turns BAKED regions into a pointer into the blob and RUNTIME regions into a
 * pointer inside a caller-provided arena; see `ra8_npu_loader.h`.
 *
 * ## Not guarded by `RA8_HAS_NPU`
 *
 * This header defines a pure DATA FORMAT (constants + one endianness-safe
 * accessor) and touches no NPU register, so -- exactly like `ra8_npu_fake_cmd.h`
 * -- it is deliberately NOT gated behind `RA8_HAS_NPU`: the host-side generator's
 * companion tests and the ra8_emulator model must be able to include it on a device
 * that is not the RA8P1. The RA8P1-only piece is the loader, which turns a blob
 * into an ::ra8_npu_job_t.
 *
 * @see @ref md_docs_2formats_2NPU1 -- the full NPU1 wire-format specification
 *      (rationale, algorithms, worked example, failure modes).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_npu_blob_ident_t
 * @brief Container magic marker and format version.
 *
 * @details ::k_ra8_npu_blob_magic is the first word of every `.npub` blob; its
 *          four little-endian bytes spell the ASCII tag "NPU1" (`0x4E 0x50 0x55
 *          0x31`). A loader rejects any buffer whose first word differs, so a
 *          truncated or foreign buffer is refused rather than mis-parsed.
 *          ::k_ra8_npu_blob_version is bumped on any incompatible layout change.
 *
 * @invariant ::k_ra8_npu_blob_magic little-endian byte order is 'N','P','U','1'.
 * @see ra8_npu_blob_word_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_blob_magic   = 0x3155504EU, /**< First word: "NPU1" little-endian. */
  k_ra8_npu_blob_version = 1U,          /**< Container format version.         */
} ra8_npu_blob_ident_t;

/**
 * @enum ra8_npu_blob_word_t
 * @brief Word indices of the fixed `.npub` header (little-endian 32-bit words).
 *
 * @details The header is ::k_ra8_npu_blob_header_words words long. `total_bytes`
 *          is the whole blob length; `region_count` counts the region
 *          descriptors that follow the header; `cmd_offset` / `cmd_bytes` locate
 *          the Vela command stream; `accel` records the Vela accelerator config
 *          (informational); `checksum` is an FNV-1a digest over every payload
 *          byte (everything at or beyond ::k_ra8_npu_blob_header_bytes).
 *
 * @invariant ::k_ra8_npu_blob_header_words equals the number of header fields.
 * @see ra8_npu_blob_size_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_blob_word_magic        = 0U, /**< magic == ::k_ra8_npu_blob_magic.    */
  k_ra8_npu_blob_word_version      = 1U, /**< format version.                     */
  k_ra8_npu_blob_word_total_bytes  = 2U, /**< whole blob length in bytes.         */
  k_ra8_npu_blob_word_region_count = 3U, /**< number of region descriptors.       */
  k_ra8_npu_blob_word_cmd_offset   = 4U, /**< byte offset to the command stream.  */
  k_ra8_npu_blob_word_cmd_bytes    = 5U, /**< command-stream length in bytes.     */
  k_ra8_npu_blob_word_accel        = 6U, /**< accelerator config (informational). */
  k_ra8_npu_blob_word_checksum     = 7U, /**< FNV-1a digest over the payload.     */
  k_ra8_npu_blob_header_words      = 8U, /**< Header field / word count.          */
} ra8_npu_blob_word_t;

/**
 * @enum ra8_npu_blob_rdesc_t
 * @brief Word indices of one region descriptor in the region table.
 *
 * @details Each descriptor is ::k_ra8_npu_blob_region_words little-endian words:
 *          `role` (::ra8_npu_blob_role_t), `flags` (::ra8_npu_blob_rflag_t),
 *          `size` in bytes, and `data_offset` -- the byte offset of the region's
 *          baked bytes when ::k_ra8_npu_blob_rflag_baked is set, else unused.
 *
 * @invariant ::k_ra8_npu_blob_region_words equals the descriptor field count.
 * @see ra8_npu_blob_role_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_blob_rdesc_role        = 0U, /**< region role (::ra8_npu_blob_role_t).   */
  k_ra8_npu_blob_rdesc_flags       = 1U, /**< region flags (::ra8_npu_blob_rflag_t). */
  k_ra8_npu_blob_rdesc_size        = 2U, /**< region size in bytes.                  */
  k_ra8_npu_blob_rdesc_data_offset = 3U, /**< baked-data byte offset (if baked).     */
  k_ra8_npu_blob_region_words      = 4U, /**< Descriptor field / word count.         */
} ra8_npu_blob_rdesc_t;

/**
 * @enum ra8_npu_blob_size_t
 * @brief Fixed byte sizes derived from the word counts.
 *
 * @details ::k_ra8_npu_blob_header_bytes is the header length; the payload the
 *          checksum covers begins here. ::k_ra8_npu_blob_region_desc_bytes is one
 *          descriptor's length. ::k_ra8_npu_blob_arena_align is the alignment the
 *          loader applies to each runtime region base within the caller arena
 *          (Ethos-U55 tensor arenas are 16-byte aligned).
 *
 * @invariant ::k_ra8_npu_blob_header_bytes == 4 * ::k_ra8_npu_blob_header_words.
 * @see ra8_npu_blob_word_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_blob_header_bytes      = 32U, /**< Header length (8 words * 4).         */
  k_ra8_npu_blob_region_desc_bytes = 16U, /**< One region descriptor (4 words * 4). */
  k_ra8_npu_blob_word_bytes        = 4U,  /**< Bytes in one little-endian word.     */
  k_ra8_npu_blob_arena_align       = 16U, /**< Runtime-region alignment (bytes).    */
} ra8_npu_blob_size_t;

/**
 * @enum ra8_npu_blob_role_t
 * @brief Role a region plays in the model's tensor arena layout.
 *
 * @details Vela assigns each BASEPn region a role: the read-only weight/bias
 *          arena, the scratch arena, and the input / output activation buffers.
 *          The loader does not act on the role beyond reporting it; the command
 *          stream references regions by their BASEPn index (their order in the
 *          descriptor table), not by this label.
 *
 * @invariant Every value fits a `uint32_t`.
 * @see ra8_npu_blob_rflag_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_blob_role_weights = 0U, /**< Read-only weight/bias arena (baked). */
  k_ra8_npu_blob_role_scratch = 1U, /**< Scratch / working arena (runtime).   */
  k_ra8_npu_blob_role_input   = 2U, /**< Input activation tensor.             */
  k_ra8_npu_blob_role_output  = 3U, /**< Output activation tensor.            */
  k_ra8_npu_blob_role_other   = 4U, /**< Any other region.                    */
} ra8_npu_blob_role_t;

/**
 * @enum ra8_npu_blob_rflag_t
 * @brief Region descriptor flag bits.
 *
 * @details ::k_ra8_npu_blob_rflag_baked marks a region whose bytes are present
 *          IN the blob at the descriptor's `data_offset` (e.g. the weight arena);
 *          the loader points the region base at those bytes. A region without the
 *          flag is RUNTIME: the loader carves it out of the caller-provided arena.
 *
 * @invariant ::k_ra8_npu_blob_rflag_baked is a single set bit.
 * @see ra8_npu_blob_rdesc_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_blob_rflag_baked = 0x1U, /**< Region bytes are baked into the blob. */
} ra8_npu_blob_rflag_t;

/**
 * @enum ra8_npu_blob_accel_t
 * @brief Vela accelerator configuration recorded in the header (informational).
 *
 * @details The value is the Ethos-U55 MAC-per-cycle SKU Vela targeted. The RA8P1
 *          integrates the 256-MAC/cycle SKU (`ethos-u55-256`, see
 *          `ra8_npu_regs.h`). The loader does not validate it; it is carried so a
 *          consumer can confirm the blob was compiled for the right accelerator.
 *
 * @invariant Every value fits a `uint32_t`.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_blob_accel_ethos_u55_256 = 256U, /**< ethos-u55-256 (RA8P1 SKU). */
} ra8_npu_blob_accel_t;

/**
 * @enum ra8_npu_blob_fnv_t
 * @brief FNV-1a 32-bit constants for the payload checksum.
 *
 * @details The offline generator and the on-target loader fold every payload byte
 *          (offset >= ::k_ra8_npu_blob_header_bytes) with these constants and
 *          compare the result against the header `checksum` word, so a corrupted
 *          blob is rejected rather than executed. FNV-1a is used elsewhere in the
 *          NPU stand-in (ra8_emulator, npu_smoke) for the same deterministic-digest
 *          reason.
 *
 * @invariant Both values are the published FNV-1a 32-bit basis / prime.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_blob_fnv_offset = 0x811C9DC5U, /**< FNV-1a 32-bit offset basis. */
  k_ra8_npu_blob_fnv_prime  = 0x01000193U, /**< FNV-1a 32-bit prime.        */
} ra8_npu_blob_fnv_t;

/**
 * @brief Read one little-endian 32-bit word from a `.npub` byte buffer.
 *
 * @details Assembles `buf[byte_off .. byte_off+3]` as a little-endian `uint32_t`
 *          one byte at a time, so the read is both endianness-independent and
 *          free of any unaligned-access assumption (the byte buffer may sit at an
 *          arbitrary address). This performs a plain memory read of a caller
 *          buffer -- no hardware register is touched -- so it carries no HUM
 *          citation.
 *
 * @param[in] buf      Base of the `.npub` byte buffer (non-NULL, caller-checked).
 * @param[in] byte_off Byte offset of the word to read; `byte_off + 4` must not
 *                     exceed the buffer length (caller-checked).
 *
 * @return The little-endian 32-bit word at @p byte_off.
 * @retval value The assembled word; always defined for an in-bounds offset.
 *
 * @pre @p buf is non-NULL.
 * @pre `byte_off + 4` is within the buffer length.
 * @post No input is modified; the function is pure.
 * @post The returned value equals the four bytes read, low byte first.
 *
 * @note Re-entrant and thread-safe (reads only, no shared state).
 * @see ra8_npu_blob_word_t
 * @since 0.1.0
 */
static inline uint32_t ra8_npu_blob_read_word(const uint8_t* buf, uint32_t byte_off)
{
  uint32_t value = 0U;
  for (uint32_t i = 0U; i < k_ra8_npu_blob_word_bytes; i++) {
    value |= (uint32_t)buf[byte_off + i] << (i * 8U);
  }
  return value;
}

#ifdef __cplusplus
}
#endif
