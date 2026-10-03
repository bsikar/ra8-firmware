/**
 * @file ra8_imgdec.h
 * @brief One image-decoder seam with format backends (RA8FW-308).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @details
 * Four decode paths ship today and each carries its own format matrix: the
 * reflow inline-image loader and the EPUB-to-RABOOK pipeline reach stb only,
 * the comic rasteriser reaches stb **and** WebP, and the JOF producer reaches
 * a first-party JPEG plus a first-party PNG plus WebP. Which formats an app
 * can open therefore depends on which path it happened to pick, so an EPUB's
 * inline `<img>` silently cannot show a WebP that transcodes fine through the
 * tiling path in the same library.
 *
 * This header is the seam that removes the choice. It is modelled on
 * `ra8_io_blockdev`: one caller-allocated handle (::ra8_imgdec_t), an opaque
 * backend vtable, a capability query, and per-backend binders published from
 * each backend's own header. A consumer names *decode*, never a decoder.
 *
 * ## Backend model
 *
 * Zero-initialise a handle and bind a backend into it with that backend's
 * `_init()` helper. No dynamic allocation occurs: a backend keeps its state in
 * caller-provided storage, and the scratch a decode needs is carved from the
 * caller's ::ra8_arena_t rather than from a decoder-private shim.
 *
 * @code
 * ra8_imgdec_t dec = {};
 * (void)ra8_imgdec_bind_webp(&dec, &webp_state);      // a backend's own header
 *
 * ra8_imgdec_caps_t caps = {};
 * (void)ra8_imgdec_get_caps(&dec, &caps);             // formats, scratch budget
 *
 * const ra8_imgdec_req_t req = {
 *   .bytes = file, .byte_count = file_len,
 *   .arena = &scratch, .dst = pixels, .dst_bytes = sizeof pixels,
 *   .want = k_ra8_imgdec_pixel_rgba8888,
 * };
 * ra8_imgdec_image_t out = {};
 * (void)ra8_imgdec_decode(&dec, &req, &out);
 * @endcode
 *
 * ## What this slice does and does not do
 *
 * It defines the seam, the format and pixel vocabulary, the capability record
 * and the request/result pair, and it implements the fabric: argument
 * validation, the shared container sniff (::ra8_imgdec_sniff), the capability
 * gate (a backend is never handed a format or a destination pixel layout it
 * did not advertise), and the dispatch. **No
 * backend is bound and no consumer is converted here.** The binders named in
 * RA8FW-308 (`ra8_imgdec_bind_jpeg_sw`, `_png`, `_webp`, `_stb`) and the
 * `ra8_reflow_set_image_loader()` signature change are later slices, each of
 * which can now be written against a seam that already exists and is proven.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#include "ra8_arena.h"
#include "ra8_err.h"

/* =============================================================================
 * Vocabulary
 * =============================================================================
 */

/**
 * @enum ra8_imgdec_format_t
 * @brief Container format of the encoded bytes, as a single-bit mask value.
 *
 * @details
 * A mask rather than an ordinal because ::ra8_imgdec_caps_t reports a *set*:
 * one backend answers for WebP alone, another for the stb residue of GIF, BMP
 * and TGA. ::k_ra8_imgdec_format_none is the empty set, never a format.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_imgdec_format_none = 0U,      /**< Empty set. Not a format.           */
  k_ra8_imgdec_format_jpeg = 1U << 0, /**< JFIF/EXIF baseline or progressive. */
  k_ra8_imgdec_format_png  = 1U << 1, /**< PNG.                               */
  k_ra8_imgdec_format_webp = 1U << 2, /**< WebP, lossy or lossless.           */
  k_ra8_imgdec_format_gif  = 1U << 3, /**< GIF (first frame).                 */
  k_ra8_imgdec_format_bmp  = 1U << 4, /**< Windows BMP.                       */
  k_ra8_imgdec_format_tga  = 1U << 5, /**< Truevision TGA.                    */
} ra8_imgdec_format_t;

/**
 * @enum ra8_imgdec_pixel_t
 * @brief Destination pixel layout, as a single-bit mask value.
 *
 * @details
 * The request names what the *caller* wants written, and the capability record
 * names what the backend can produce; the fabric refuses the pair rather than
 * letting a backend silently substitute a layout. Grey8 is listed because the
 * e-paper paths want luminance and a decoder that produces it directly saves a
 * whole-image conversion pass.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_imgdec_pixel_none     = 0U,      /**< Empty set. Not a layout. */
  k_ra8_imgdec_pixel_grey8    = 1U << 0, /**< 1 byte/px luminance.     */
  k_ra8_imgdec_pixel_rgb888   = 1U << 1, /**< 3 bytes/px, R,G,B.       */
  k_ra8_imgdec_pixel_rgba8888 = 1U << 2, /**< 4 bytes/px, R,G,B,A.     */
} ra8_imgdec_pixel_t;

/**
 * @enum ra8_imgdec_limits_t
 * @brief Bounds the fabric enforces before a backend is reached.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_imgdec_dim_max     = 16384U, /**< Widest/tallest accepted.  */
  k_ra8_imgdec_format_mask = 0x3FU,  /**< Every defined format bit. */
  k_ra8_imgdec_pixel_mask  = 0x07U,  /**< Every defined pixel bit.  */
  k_ra8_imgdec_sniff_bytes = 12U,    /**< Leading bytes ra8_imgdec_sniff()
                                          needs to answer for every format. */
  k_ra8_imgdec_dims_bytes  = 30U,    /**< Leading bytes ra8_imgdec_dims()
                                          needs for every fixed-offset
                                          container. JPEG is the exception:
                                          its geometry sits behind a marker
                                          walk of unbounded length. */
} ra8_imgdec_limits_t;

/**
 * @brief Bytes one pixel of @p pixel occupies, or 0 when @p pixel is not a
 *        single defined layout.
 *
 * @param[in] pixel Destination pixel layout.
 *
 * @return uint32_t Bytes per pixel, 0 for ::k_ra8_imgdec_pixel_none or any
 *                  value that is not exactly one defined bit.
 *
 * @post No state is mutated.
 *
 * @note Thread-safe (pure function).
 *
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_imgdec_pixel_bytes(ra8_imgdec_pixel_t pixel);

/**
 * @brief Name the container format @p bytes opens with, from its leading bytes.
 *
 * @details
 * The container sniff is the other half of the duplication RA8FW-308 is about. The
 * format *matrix* was four-way, and so was the signature test that feeds it:
 * `reflow_image.c` carries a RIFF/WEBP predicate, `jof_produce.c` carries the
 * JPEG SOI plus PNG signature plus the same RIFF/WEBP pair, and the removed
 * downloader carried two more copies, one with GIF and BMP added. One buffer
 * could therefore be
 * "a WebP" to one path and "not an image" to the next.
 *
 * This is that test, written once, as a pure function of the bytes. It reads
 * at most ::k_ra8_imgdec_sniff_bytes and answers only from fixed signatures,
 * so it is cheap enough to call before choosing a decoder and says nothing
 * about whether the rest of the file is well-formed. TGA has no signature at
 * all (its header is bare geometry), so it is never sniffed and a consumer
 * holding one must declare ::k_ra8_imgdec_format_tga in the request.
 *
 * @param[in]  bytes      Encoded image bytes. Never NULL.
 * @param[in]  byte_count Readable bytes at @p bytes.
 * @param[out] out        Format recognised, one ::ra8_imgdec_format_t bit.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              `*out` names the recognised container.
 * @retval k_ra8_err_null_ptr    `bytes` or `out` was NULL.
 * @retval k_ra8_err_invalid_size `byte_count` was 0.
 * @retval k_ra8_err_not_found   No signature matched, including the case of a
 *                               buffer too short to carry one.
 *
 * @post On any non-ok return `*out` is ::k_ra8_imgdec_format_none.
 * @post @p bytes is never modified.
 *
 * @note Thread-safe (pure read of @p bytes).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_sniff(const uint8_t*       bytes,
                                         uint32_t             byte_count,
                                         ra8_imgdec_format_t* out);

/* =============================================================================
 * Capabilities
 * =============================================================================
 */

/**
 * @struct ra8_imgdec_caps_t
 * @brief What a bound backend can decode, and what scratch it needs to do it.
 *
 * @details
 * `scratch_bytes` is the whole point of publishing capabilities at all: the
 * five private arena shims this seam replaces made a decoder's scratch budget
 * a guess the consumer had to make from the outside. Here it is a query. A
 * backend that needs no scratch reports 0 and the fabric then accepts a
 * request with no arena.
 *
 * @invariant `formats` is a non-empty subset of ::k_ra8_imgdec_format_mask.
 * @invariant `pixels` is a non-empty subset of ::k_ra8_imgdec_pixel_mask.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t formats;       /**< OR of ::ra8_imgdec_format_t the backend opens.   */
  uint32_t pixels;        /**< OR of ::ra8_imgdec_pixel_t it can write.         */
  uint32_t scratch_bytes; /**< Peak arena bytes a decode may carve. 0 = none.   */
  uint32_t scratch_align; /**< Alignment the scratch carve needs. 0 treated 1.  */
  uint32_t dim_max;       /**< Widest/tallest image it accepts (<= dim_max).    */
  bool     streams;       /**< true => decodes without the whole file resident. */
} ra8_imgdec_caps_t;

/* =============================================================================
 * Request and result
 * =============================================================================
 */

/**
 * @struct ra8_imgdec_req_t
 * @brief One decode request: the encoded bytes in, the surface out.
 *
 * @details
 * `format` may be ::k_ra8_imgdec_format_none, which asks the *fabric* to sniff
 * the container with ::ra8_imgdec_sniff() and then gate the answer against the
 * backend's capability set, exactly as it gates a declared format. Naming a
 * format instead is not a hint: it is checked and refused up front, so a
 * consumer that knows what it holds gets a clean refusal rather than a decoder
 * failure deep inside a parse. Either way the backend is handed a request
 * whose `format` names exactly one bit it advertised; a buffer carrying no
 * recognised signature is refused ::k_ra8_err_not_supported before any decoder
 * runs. ::k_ra8_imgdec_format_tga has no signature, so a TGA must be declared.
 *
 * @since 0.1.0
 */
typedef struct {
  const uint8_t*     bytes;      /**< Encoded image bytes. Never NULL.         */
  uint32_t           byte_count; /**< Length of @ref bytes. Never 0.           */
  ra8_arena_t*       arena;      /**< Scratch source; NULL only if caps say 0. */
  uint8_t*           dst;        /**< Destination surface. Never NULL.         */
  uint32_t           dst_bytes;  /**< Writable bytes at @ref dst.              */
  uint32_t           dst_stride; /**< Row stride in bytes; 0 = tightly packed. */
  ra8_imgdec_format_t format;    /**< Declared container, or `_none` to sniff. */
  ra8_imgdec_pixel_t  want;      /**< Destination pixel layout. One bit.       */
} ra8_imgdec_req_t;

/**
 * @struct ra8_imgdec_image_t
 * @brief What a completed decode produced.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t            width_px;   /**< Decoded width in pixels.              */
  uint32_t            height_px;  /**< Decoded height in pixels.             */
  uint32_t            stride;     /**< Bytes per row actually written.       */
  uint32_t            used_bytes; /**< Bytes of `dst` written.               */
  ra8_imgdec_format_t format;     /**< Container the backend actually found. */
  ra8_imgdec_pixel_t  pixel;      /**< Layout written (equals `req->want`).  */
  bool                had_alpha;  /**< Source carried real transparency.     */
} ra8_imgdec_image_t;

/* =============================================================================
 * Interface + handle
 * =============================================================================
 */

/**
 * @struct ra8_imgdec_iface
 * @brief Opaque to consumers -- backends implement this and export a binder.
 *
 * @details
 * The concrete vtable layout lives in `ra8_imgdec_backend.h`, the
 * implementer-facing header, exactly as `ra8_io_blockdev_backend.h` does it.
 * Consumers never construct one.
 *
 * @since 0.1.0
 */
typedef struct ra8_imgdec_iface ra8_imgdec_iface_t;

/**
 * @struct ra8_imgdec_t
 * @brief Caller-allocated decoder handle binding a backend to its context.
 *
 * @invariant `iface` is non-NULL once a backend has been bound.
 *
 * @since 0.1.0
 */
typedef struct {
  const ra8_imgdec_iface_t* iface; /**< Bound backend vtable (private).    */
  void*                     ctx;   /**< Backend-private context (private). */
} ra8_imgdec_t;

/* =============================================================================
 * Public API
 * =============================================================================
 */

/**
 * @brief Report the bound backend's formats, pixel layouts and scratch budget.
 *
 * @param[in]  dec Bound decoder handle.
 * @param[out] out Capability snapshot.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*out` populated.
 * @retval k_ra8_err_null_ptr        `dec` or `out` was NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `dec`.
 * @retval k_ra8_err_invalid_state   The backend reported an unusable record.
 *
 * @pre A backend has been bound into `dec`.
 * @post On success `*out` describes the bound backend.
 * @post On any non-ok return `*out` is zeroed, never left half-written.
 *
 * @note Thread-safe (pure read of immutable backend state).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_get_caps(const ra8_imgdec_t* dec, ra8_imgdec_caps_t* out);

/**
 * @brief Ask whether the bound backend opens @p format into @p pixel.
 *
 * @details
 * The query a consumer makes *before* committing to a path, so "can I show
 * this?" stops being answered by trying and failing. Both arguments must name
 * exactly one defined bit.
 *
 * @param[in]  dec    Bound decoder handle.
 * @param[in]  format Container format to test.
 * @param[in]  pixel  Destination layout to test.
 * @param[out] out_ok Set true only when both are advertised.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*out_ok` answered.
 * @retval k_ra8_err_null_ptr        `dec` or `out_ok` was NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `dec`.
 * @retval k_ra8_err_invalid_arg     `format` or `pixel` was not one defined bit.
 *
 * @post On any non-ok return `*out_ok` is false.
 *
 * @note Thread-safe (pure read of immutable backend state).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_supports(const ra8_imgdec_t* dec,
                                            ra8_imgdec_format_t format,
                                            ra8_imgdec_pixel_t  pixel,
                                            bool*               out_ok);

/**
 * @brief Decode @p req through the bound backend.
 *
 * @details
 * The fabric validates the request, refuses anything the capability record
 * does not cover, and only then dispatches. A backend therefore never has to
 * re-check the arguments the seam already guarantees: non-NULL bytes with a
 * non-zero count, a destination large enough for one row of the requested
 * layout, a declared format it advertises, a pixel layout it advertises, and
 * an arena whenever it said it needs scratch.
 *
 * @param[in]  dec Bound decoder handle.
 * @param[in]  req Decode request.
 * @param[out] out What the decode produced.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Image decoded into `req->dst`.
 * @retval k_ra8_err_null_ptr        `dec`, `req`, `out`, `req->bytes` or
 *                                   `req->dst` was NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `dec`.
 * @retval k_ra8_err_invalid_arg     `req->want` was not one defined bit, or
 *                                   `req->format` was neither `_none` nor one
 *                                   defined bit.
 * @retval k_ra8_err_invalid_size    `byte_count` or `dst_bytes` was 0, or
 *                                   `dst_stride` cannot hold one pixel row.
 * @retval k_ra8_err_not_supported   The backend does not open that format, or
 *                                   cannot write that pixel layout, or
 *                                   `format` was `_none` and the bytes carry
 *                                   no recognised container signature.
 * @retval k_ra8_err_invalid_state   Scratch is required and `req->arena` is
 *                                   NULL, or the backend has no decode entry.
 * @retval k_ra8_err_no_mem          The arena cannot cover `scratch_bytes`.
 *
 * @pre A backend has been bound into `dec`.
 * @pre `req->dst` is writable for `req->dst_bytes` bytes.
 * @post On success `*out` describes the decoded image.
 * @post On any non-ok return `*out` is zeroed and `req->dst` is untouched by
 *       the fabric.
 *
 * @note Not thread-safe with respect to the same handle.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_imgdec_decode(const ra8_imgdec_t* dec, const ra8_imgdec_req_t* req, ra8_imgdec_image_t* out);

/**
 * @struct ra8_imgdec_geom_t
 * @brief The geometry a container declares about itself, before any decode.
 *
 * @details
 * This is the record behind ::ra8_imgdec_dims. It is deliberately not
 * ::ra8_imgdec_image_t: that one describes a surface a backend actually
 * produced, with a stride and a pixel layout the decode chose. This one is
 * only what the encoded bytes claim, which is what a caller sizing a buffer,
 * picking an atlas tile or naming a cache key needs before it commits to a
 * decoder.
 *
 * @invariant On success `format` is exactly one defined format bit.
 * @invariant On success both dimensions are non-zero and at most
 *            ::k_ra8_imgdec_dim_max.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_imgdec_format_t format;    /**< Container the geometry was read from. */
  uint32_t            width_px;  /**< Declared width in pixels.             */
  uint32_t            height_px; /**< Declared height in pixels.            */
} ra8_imgdec_geom_t;

/**
 * @brief Read a container's declared geometry without decoding it.
 *
 * @details
 * The companion to ::ra8_imgdec_sniff: that one answers *which* container
 * this is, this one answers *how big it says it is*. Both are pure functions
 * of the leading bytes and neither links a decoder, which is what lets a
 * caller size a destination surface before it has chosen, or even bound, a
 * backend.
 *
 * The tree has carried exactly one geometry probe until now,
 * `jof_probe_dims()` in `apps/shared_libs/jof`, and three consumers reach up
 * into the JOF producer to call it: the RABOOK exporter,
 * the comic tiler (`comic_tiles.c`) and the host
 * worker (`jof_worker.c`). None of them is producing a JOF at that moment;
 * they want the geometry. That is the same ring inversion RA8FW-308 records for
 * the arenas, one layer up.
 *
 * Each container is read at its own fixed offsets:
 * - PNG: the IHDR chunk, whose type tag is checked rather than assumed;
 * - GIF: the logical screen descriptor;
 * - BMP: the DIB header, both the 12-byte core and the 40-byte info shapes,
 *   with a negative (top-down) height taken as its magnitude;
 * - WebP: the first chunk, `VP8 ` lossy, `VP8L` lossless or `VP8X` extended
 *   canvas, each of which stores its size differently;
 * - JPEG: a marker walk to the first SOF, the only format here whose
 *   geometry is not at a fixed offset.
 *
 * TGA is not answered. It has no signature for ::ra8_imgdec_sniff to find,
 * so there is nothing to key a geometry read off, and a caller holding a TGA
 * knows it by other means.
 *
 * @param[in]  bytes      Leading bytes of the encoded image.
 * @param[in]  byte_count Readable length of @p bytes.
 * @param[out] out        Receives the container and its declared geometry.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Geometry read and in range.
 * @retval k_ra8_err_null_ptr      `bytes` or `out` was NULL.
 * @retval k_ra8_err_invalid_size  `byte_count` was 0, or a declared
 *                                 dimension is 0 or over
 *                                 ::k_ra8_imgdec_dim_max.
 * @retval k_ra8_err_not_found     The bytes carry no recognised signature.
 * @retval k_ra8_err_not_supported The container was recognised but its
 *                                 geometry is not readable here: a truncated
 *                                 header, a WebP chunk that is none of the
 *                                 three VP8 flavours, a JPEG with no SOF in
 *                                 the bytes supplied, or TGA.
 *
 * @pre @p bytes holds @p byte_count readable bytes.
 * @post When `dec`, `bytes` and `out` are non-NULL, `*out` is zeroed on every
 *       non-ok return. A missing pointer leaves any available output untouched.
 * @post The buffer is never written and no decoder is invoked.
 *
 * @note Pure apart from `*out`; thread-safe.
 *
 * @see ra8_imgdec_sniff()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_imgdec_dims(const uint8_t* bytes, uint32_t byte_count, ra8_imgdec_geom_t* out);

/**
 * @brief Ask the bound backend whether it can open these bytes, and how big
 *        the image says it is.
 *
 * @details
 * The one question a consumer actually has before it reserves a destination
 * surface: *can this decoder take this file, and what size is it*. Answering
 * it today means calling ::ra8_imgdec_sniff, then ::ra8_imgdec_dims, then
 * ::ra8_imgdec_get_caps, then testing the format bit and the backend's own
 * `dim_max` by hand, and getting the four-way error mapping right at every
 * call site. That is four decode paths' worth of duplicated gating, which is
 * the duplication RA8FW-308 exists to remove, so the fabric does it once.
 *
 * Nothing is decoded and no backend hook is reached beyond its capability
 * query, so this stays a pure read of the leading bytes: the answer costs a
 * header parse, not a decode.
 *
 * Two deliberate limits on what this answers:
 * - It is about the *image*, not the destination layout. Whether the backend
 *   can write the layout the caller wants is ::ra8_imgdec_supports, which
 *   needs no bytes at all; keeping them apart means neither query has to
 *   carry the other's arguments.
 * - `dim_max` is enforced here and nowhere else in the fabric.
 *   ::ra8_imgdec_decode cannot enforce it, because it has no geometry until a
 *   backend has already parsed the header. A caller that wants the bound
 *   backend's size limit honoured before it commits asks here.
 *
 * @param[in]  dec        Bound decoder handle.
 * @param[in]  bytes      Leading bytes of the encoded image.
 * @param[in]  byte_count Readable length of @p bytes.
 * @param[out] out        Receives the container and its declared geometry.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*out` holds a container this backend
 *                                   opens, at a size it accepts.
 * @retval k_ra8_err_null_ptr        `dec`, `bytes` or `out` was NULL.
 * @retval k_ra8_err_not_initialized No backend is bound to `dec`.
 * @retval k_ra8_err_invalid_state   The backend reported an unusable record.
 * @retval k_ra8_err_invalid_size    `byte_count` was 0, or a declared
 *                                   dimension is 0 or over
 *                                   ::k_ra8_imgdec_dim_max.
 * @retval k_ra8_err_not_supported   The seam cannot open these bytes: no
 *                                   recognised signature, a container whose
 *                                   geometry is not readable here, a format
 *                                   this backend does not advertise, or a
 *                                   declared size past this backend's own
 *                                   `dim_max`.
 *
 * @pre A backend has been bound into `dec`.
 * @pre @p bytes holds @p byte_count readable bytes.
 * @post `*out` is zeroed on every non-ok return.
 * @post The buffer is never written and no decoder is invoked.
 *
 * @note A buffer carrying no recognised signature is reported
 *       ::k_ra8_err_not_supported, the same code ::ra8_imgdec_decode gives
 *       it, rather than the ::k_ra8_err_not_found that ::ra8_imgdec_dims
 *       returns on its own. Two doors into the seam answering the same bytes
 *       differently is what a consumer then has to paper over; a caller that
 *       wants the finer distinction still has ::ra8_imgdec_dims.
 *
 * @note Thread-safe (pure read of @p bytes and of immutable backend state).
 *
 * @see ra8_imgdec_dims()
 * @see ra8_imgdec_supports()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_probe(const ra8_imgdec_t* dec,
                                         const uint8_t*      bytes,
                                         uint32_t            byte_count,
                                         ra8_imgdec_geom_t*  out);

#ifdef __cplusplus
}
#endif
