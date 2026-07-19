/**
 * @file ra8_fmt_internal.h
 * @brief Cross-TU declarations shared inside the `ra8_fmt` tool.
 *
 * @details
 * The tool is one module split across several translation units (CLI, shared
 * utilities, and one TU per format family). These are the helpers a format's
 * verbs share across those TUs; nothing here is public API -- consumers use the
 * ::ra8_fmt_desc_t seam in `ra8_fmt.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fmt.h"
#include "ra8_tileatlas.h"

/**
 * @brief Compute a safe memory-sink capacity for an atlas built from a source.
 *
 * @details A lossless DEFLATE atlas of a decoded image can exceed the encoded
 *          source by a wide margin (a small JPEG decodes to a large raster), so
 *          the sink is sized from a generous multiple of the source plus a
 *          fixed floor -- this is a host tool, so over-allocating is free.
 *
 * @param[in] src_len Encoded source length in bytes.
 *
 * @return Sink capacity in bytes.
 * @retval >=src_len Always at least the source length.
 *
 * @pre @p src_len is a real file length (below ::k_ra8_fmt_max_input).
 * @pre The caller frees the resulting allocation.
 * @post No state is mutated.
 * @post The result is non-zero.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] size_t ra8_fmt_atlas_sink_cap(size_t src_len);

/**
 * @brief Transcode one source image into an in-RAM JOF atlas.
 *
 * @details The shared encode step behind the atlas `convert` and `verify`
 *          verbs: drives the firmware `ra8_tileatlas_produce()` from a slurped
 *          blob into a memory sink, so the produced bytes can be written out or
 *          immediately read back without touching the filesystem.
 *
 * @param[in]  src       Encoded source image (JPEG / PNG), non-NULL.
 * @param[in]  max_w     Width cap for the work-arena sizing, pixels.
 * @param[in]  max_h     Height cap for the work-arena sizing, pixels.
 * @param[in]  tile_w    Tile width to request, pixels.
 * @param[in]  tile_h    Tile height to request, pixels.
 * @param[in]  codec     ::ra8_tileatlas_codec_t member.
 * @param[out] out_atlas Receives the owned atlas bytes (caller frees).
 * @param[out] out_info  Receives the produced atlas geometry.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Atlas produced into `*out_atlas`.
 * @retval k_ra8_err_null_ptr     A required pointer is NULL.
 * @retval k_ra8_err_invalid_size The work arena or sink could not be sized.
 * @retval k_ra8_err_no_mem       An allocation failed.
 * @retval other                  Propagated from the producer / decoders.
 *
 * @pre @p src holds a decodable image.
 * @pre @p out_atlas and @p out_info are writable.
 * @post On success `out_atlas->bytes` must be released by the caller.
 * @post On any error `*out_atlas` owns nothing.
 * @note Not thread-safe (the producer keeps module-static state).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_atlas_produce(const ra8_fmt_blob_t* src,
                                              uint16_t              max_w,
                                              uint16_t              max_h,
                                              uint16_t              tile_w,
                                              uint16_t              tile_h,
                                              uint8_t               codec,
                                              ra8_fmt_blob_t*       out_atlas,
                                              ra8_tileatlas_info_t* out_info);

/**
 * @brief Read a source image's pixel dimensions from its container header.
 *
 * @details Sizing the producer work arena needs the geometry up front, so the
 *          probe reads the PNG IHDR directly and defers to the firmware JPEG
 *          header scanner otherwise. This is header parsing only -- no pixel is
 *          decoded, so it is cheap enough to run before every encode.
 *
 * @param[in]  src   Encoded source image bytes (non-NULL).
 * @param[out] out_w Receives the image width, pixels.
 * @param[out] out_h Receives the image height, pixels.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Dimensions read into the outputs.
 * @retval k_ra8_err_null_ptr     A required pointer is NULL.
 * @retval k_ra8_err_invalid_size A dimension is zero or above the format cap.
 * @retval other                  Propagated from the JPEG header scanner.
 *
 * @pre @p src holds a PNG or JPEG source.
 * @pre @p out_w and @p out_h are writable.
 * @post On success both outputs are in `[1, k_ra8_tileatlas_max_dim]`.
 * @post On any error neither output is relied upon.
 * @note Thread-safe (pure over its input).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_fmt_atlas_probe(const ra8_fmt_blob_t* src, uint16_t* out_w, uint16_t* out_h);

/**
 * @brief Decode every tile of an atlas and reassemble the full raster.
 *
 * @details Places each tile at `(tile_x * tile_w, tile_y * tile_h)` using the
 *          tile's own edge-clamped dimensions, which is precisely the placement
 *          rule a correct consumer must follow. Reassembling here and comparing
 *          against the untiled reference is what separates a producer defect
 *          from a consumer defect.
 *
 * @param[in]  atlas    Atlas container bytes (non-NULL).
 * @param[in]  info     Parsed atlas geometry (non-NULL).
 * @param[out] out_px   Receives the owned `width * height * bpp` raster.
 * @param[out] out_len  Receives the raster byte count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Raster reassembled into `*out_px`.
 * @retval k_ra8_err_null_ptr     A required pointer is NULL.
 * @retval k_ra8_err_no_mem       An allocation failed.
 * @retval other                  Propagated from `ra8_tileatlas_read_tile()`.
 *
 * @pre @p info came from a successful parse over @p atlas.
 * @pre @p out_px and @p out_len are writable.
 * @post On success `*out_px` holds `*out_len` bytes and must be freed.
 * @post On any error `*out_px` is NULL.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_atlas_reassemble(const ra8_fmt_blob_t*       atlas,
                                                 const ra8_tileatlas_info_t* info,
                                                 uint8_t**                   out_px,
                                                 size_t*                     out_len);

/**
 * @brief JOF `convert` verb: one source image to one `.jof` atlas.
 * @param[in] src  Slurped source image (non-NULL).
 * @param[in] opts Options carrying `out_path` and the report sink.
 * @return k_ra8_ok when the atlas was written to `opts->out_path`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_atlas_convert(const ra8_fmt_blob_t* src,
                                              const ra8_fmt_opts_t* opts);

/**
 * @brief JOF `inspect` verb: dump header, tile table, footer and verdict.
 * @param[in] src  Atlas container bytes (non-NULL).
 * @param[in] opts Options carrying the report sink and verbosity.
 * @return k_ra8_ok when the container validated clean.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_atlas_inspect(const ra8_fmt_blob_t* src,
                                              const ra8_fmt_opts_t* opts);

/**
 * @brief JOF `verify` verb: banded encode versus untiled reference raster.
 * @param[in] src  Source image to round-trip (non-NULL).
 * @param[in] opts Options carrying the report sink and optional PPM dump path.
 * @return k_ra8_ok when the banded raster matched the reference exactly.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_atlas_verify(const ra8_fmt_blob_t* src, const ra8_fmt_opts_t* opts);

/**
 * @brief Report whether a blob carries the JOF header magic.
 * @param[in] src Candidate container bytes.
 * @return `true` when the first four bytes are "JOF1".
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_fmt_atlas_sniff(const ra8_fmt_blob_t* src);

/**
 * @brief Report whether a blob carries the RBKC rabook container magic.
 * @param[in] src Candidate container bytes.
 * @return `true` when the first four bytes are "RBKC".
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_fmt_rabook_sniff(const ra8_fmt_blob_t* src);

/**
 * @brief RABOOK `inspect` verb: dump the chunk inventory and section table.
 * @param[in] src  Container bytes (non-NULL).
 * @param[in] opts Options carrying the report sink and verbosity.
 * @return k_ra8_ok when the container validated clean.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_rabook_inspect(const ra8_fmt_blob_t* src,
                                               const ra8_fmt_opts_t* opts);

/**
 * @brief Report whether a blob carries the RCBZ container magic.
 * @param[in] src Candidate container bytes.
 * @return `true` when the first four bytes are "RCBZ".
 * @since 0.1.0
 */
[[nodiscard]] bool ra8_fmt_cbz_sniff(const ra8_fmt_blob_t* src);

/**
 * @brief RCBZ `inspect` verb: dump the page directory with offsets and lengths.
 * @param[in] src  Container bytes (non-NULL).
 * @param[in] opts Options carrying the report sink and verbosity.
 * @return k_ra8_ok when the container validated clean.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_fmt_cbz_inspect(const ra8_fmt_blob_t* src, const ra8_fmt_opts_t* opts);
