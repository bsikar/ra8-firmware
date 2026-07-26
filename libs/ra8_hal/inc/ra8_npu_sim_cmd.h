/**
 * @file ra8_npu_sim_cmd.h
 * @brief board_sim / host-test command-stream convention for the Ethos-U55 model
 * @ingroup grp_hal_system
 *
 * @details
 * This header defines a TINY, DETERMINISTIC, SIM-ONLY command-stream layout that
 * stands in for a real Arm Vela-compiled Ethos-U55 command stream. It exists so
 * the `ra8_npu` driver's submit -> run -> poll -> read-output protocol can be
 * exercised end-to-end WITHOUT a Vela toolchain and WITHOUT an RA8P1 board:
 *
 *  - `examples/ra8p1_foundation/npu_smoke` BUILDS a stream in this layout, points
 *    the NPU command queue at it (`ra8_npu_submit`), and kicks it (`ra8_npu_run`).
 *  - `tools/board_sim/src/periph/board_periph_npu.c` DECODES a stream in this layout and
 *    performs the stand-in "execution" against the tensor arenas, so the OUTPUT
 *    arena ends up holding a checkable, deterministic result.
 *  - `tests/test_ra8_npu.c` MOCKS the same decode against the host MMIO backing
 *    store, so `make test` covers the identical convention.
 *
 * Keeping the magic value, the opcodes, the header word indices, and the
 * transfer bound in ONE place means the three consumers can never disagree: if
 * the layout changes here, all three move together.
 *
 * ## This is NOT hardware and NOT Vela
 *
 * A real Ethos-U55 command stream is a sequence of Vela-emitted NPU opcodes
 * (documented in the Arm "Ethos-U55 NPU Technical Reference Manual"); THIS
 * layout is a two-operation (COPY / add-constant) stand-in whose only purpose is
 * to make the driver protocol and the BASEPn region programming observable in a
 * host emulator. Real Vela-compiled inference is the follow-up (see the RA8P1
 * NPU epic). Because it is a pure convention -- integer constants, no register
 * access -- it carries no hardware citation and is deliberately NOT guarded by
 * `RA8_HAS_NPU`: the host-side board_sim model (which does not target the RA8P1)
 * must include it too.
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
 * @enum ra8_npu_sim_word0_t
 * @brief Magic marker and field masks of the command stream's first word.
 *
 * @details
 * The first 32-bit word of a sim command stream is `magic | opcode`: the high 16
 * bits carry ::k_ra8_npu_sim_magic (an "SE55" = Sim-Ethos-U55 marker) and the low
 * 16 bits carry a ::ra8_npu_sim_op_t opcode. The marker lets a decoder tell a sim
 * program apart from a real Vela stream (whose first word is an Ethos-U55 opcode
 * that never matches this marker), so a decoder can HONESTLY reject a stream it
 * cannot interpret instead of faking completion.
 *
 * @invariant ::k_ra8_npu_sim_magic has no bits in common with ::k_ra8_npu_sim_op_mask.
 * @see ra8_npu_sim_op_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_sim_magic      = 0x5E550000U, /**< Sim-Ethos-U55 marker in bits [31:16]. */
  k_ra8_npu_sim_magic_mask = 0xFFFF0000U, /**< Bits [31:16] select the magic marker. */
  k_ra8_npu_sim_op_mask    = 0x0000FFFFU, /**< Bits [15:0] select the opcode.        */
} ra8_npu_sim_word0_t;

/**
 * @enum ra8_npu_sim_op_t
 * @brief Opcodes of the sim command-stream convention.
 *
 * @details
 * Two elementwise byte operations, each reading the source region and writing
 * the destination region referenced by the command header. ::k_ra8_npu_sim_op_copy
 * copies bytes verbatim; ::k_ra8_npu_sim_op_addk adds a constant to every byte
 * (8-bit wrap). Both are deliberately trivial: the point is to prove the driver
 * protocol and region programming move real bytes deterministically, not to
 * model Ethos-U55 arithmetic.
 *
 * @invariant Every opcode fits in ::k_ra8_npu_sim_op_mask (16 bits).
 * @see ra8_npu_sim_word0_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_sim_op_copy = 0x0001U, /**< dst[i] = src[i].                     */
  k_ra8_npu_sim_op_addk = 0x0002U, /**< dst[i] = (src[i] + constant) & 0xFF. */
} ra8_npu_sim_op_t;

/**
 * @enum ra8_npu_sim_word_idx_t
 * @brief 32-bit word indices of the fixed sim command-stream header.
 *
 * @details
 * The header is ::k_ra8_npu_sim_word_num little-endian 32-bit words:
 *  - ::k_ra8_npu_sim_word_op    -- `magic | opcode` (see ::ra8_npu_sim_word0_t).
 *  - ::k_ra8_npu_sim_word_src   -- source region index (BASEPn the op reads).
 *  - ::k_ra8_npu_sim_word_dst   -- destination region index (BASEPn the op writes).
 *  - ::k_ra8_npu_sim_word_count -- element count in bytes (1 .. ::k_ra8_npu_sim_max_bytes).
 *  - ::k_ra8_npu_sim_word_const -- constant addend (used by add-constant only).
 *
 * @invariant ::k_ra8_npu_sim_word_num equals the number of populated header words.
 * @see ra8_npu_sim_bound_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_sim_word_op    = 0U, /**< magic | opcode.                      */
  k_ra8_npu_sim_word_src   = 1U, /**< Source region index (0 .. 7).        */
  k_ra8_npu_sim_word_dst   = 2U, /**< Destination region index (0 .. 7).   */
  k_ra8_npu_sim_word_count = 3U, /**< Byte count to process.               */
  k_ra8_npu_sim_word_const = 4U, /**< Constant addend (add-constant op).   */
  k_ra8_npu_sim_word_num   = 5U, /**< Header word count (populated words). */
} ra8_npu_sim_word_idx_t;

/**
 * @enum ra8_npu_sim_bound_t
 * @brief Size bounds and the 8-bit element mask of the sim convention.
 *
 * @details
 * ::k_ra8_npu_sim_header_bytes is the fixed header byte length (word count times
 * four) a submitted stream must be at least as long as. ::k_ra8_npu_sim_max_bytes
 * bounds a transfer so every decoder loop has a static upper bound (NASA Power of
 * 10 Rule 2). ::k_ra8_npu_sim_byte_mask is the 8-bit wrap applied by the
 * add-constant opcode.
 *
 * @invariant ::k_ra8_npu_sim_header_bytes == 4 * ::k_ra8_npu_sim_word_num.
 * @see ra8_npu_sim_word_idx_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_npu_sim_header_bytes = 20U,   /**< Fixed header length in bytes (5 words). */
  k_ra8_npu_sim_max_bytes    = 4096U, /**< Max processed bytes (bounds the loop).  */
  k_ra8_npu_sim_byte_mask    = 0xFFU, /**< 8-bit element wrap (add-constant op).   */
} ra8_npu_sim_bound_t;

#ifdef __cplusplus
}
#endif
