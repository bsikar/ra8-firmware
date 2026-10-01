/**
 * @file ra8_imgdec_mux.h
 * @brief One format matrix over a set of decoder backends (RA8FW-308).
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @details
 * ::ra8_imgdec_t binds exactly one backend, which is the right shape for a
 * backend and the wrong shape for a consumer. RA8FW-308 is not really about any one
 * decoder: it is about four decode paths each reaching a different subset of
 * the formats, so that whether an app can show a WebP depends on which path it
 * happened to pick. A consumer needs to name *decode* and have the set of
 * backends answer as one.
 *
 * That is this file. A mux holds up to ::k_ra8_imgdec_mux_max bound handles in
 * the order they were added, reports the union of what they open, and routes a
 * request to the first member that advertises both the container and the
 * destination layout it asks for. Adding WebP to a consumer that had only stb
 * then becomes one more ::ra8_imgdec_mux_add call rather than a fifth special
 * case in the consumer, which is the shape #637 needs.
 *
 * @code
 * ra8_imgdec_mux_t mux = {};
 * (void)ra8_imgdec_mux_init(&mux);
 * (void)ra8_imgdec_mux_add(&mux, &jpeg_sw);   // preferred for JPEG
 * (void)ra8_imgdec_mux_add(&mux, &webp);
 * (void)ra8_imgdec_mux_add(&mux, &stb);       // residual GIF / BMP / TGA
 *
 * uint32_t openable = 0U;                     // "what can I show at all?"
 * (void)ra8_imgdec_mux_formats(&mux, k_ra8_imgdec_pixel_rgba8888, &openable);
 *
 * ra8_imgdec_image_t img = {};
 * (void)ra8_imgdec_mux_decode(&mux, &req, &img);
 * @endcode
 *
 * @par Why a mux is not itself an ra8_imgdec_t
 * It would be tidy to bind a mux as a backend and let ::ra8_imgdec_get_caps
 * report the union, and it would be wrong. ::ra8_imgdec_caps_t is a flat pair
 * of sets, one of formats and one of pixel layouts, which can only describe a
 * matrix where every format is available in every layout. That holds for a
 * single backend and does not hold for a set of them: a mux of one backend
 * doing WebP into RGBA and another doing GIF into grey8 would union to
 * "{WebP, GIF} x {RGBA, grey8}" and answer yes to WebP-into-grey8, which no
 * member can do. So the mux publishes its own queries, each of which is
 * evaluated per member as a *pair*, and never claims a capability by
 * construction. ::ra8_imgdec_mux_formats takes the layout as an argument for
 * exactly that reason.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_imgdec.h"
#include "ra8_imgdec_scratch.h"

/**
 * @enum ra8_imgdec_mux_limits_t
 * @brief Bounds of the member set.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_imgdec_mux_max = 4U, /**< Members a mux holds. One per backend named
                                  in RA8FW-308: the first-party JPEG, the
                                  first-party PNG, WebP, and the stb residue
                                  of GIF/BMP/TGA. */
} ra8_imgdec_mux_limits_t;

/**
 * @struct ra8_imgdec_mux_t
 * @brief Caller-allocated set of bound decoders, in priority order.
 *
 * @details
 * Zero-initialise and pass to ::ra8_imgdec_mux_init. Members are copies of the
 * handles added, so each backend's own storage still has to outlive the mux;
 * a handle is two pointers, and copying it is what keeps the mux free of any
 * allocation of its own.
 *
 * @invariant `count` is at most ::k_ra8_imgdec_mux_max.
 * @invariant Every member below `count` has a non-NULL `iface`.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_imgdec_t members[k_ra8_imgdec_mux_max]; /**< Bound handles (private).  */
  uint32_t     count;                         /**< Members in use (private). */
} ra8_imgdec_mux_t;

/**
 * @brief Empty a mux so it holds no members.
 *
 * @param[out] mux Mux to reset.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           `*mux` is empty and usable.
 * @retval k_ra8_err_null_ptr `mux` was NULL.
 *
 * @post `*mux` holds no members; any previously added handle is forgotten.
 *
 * @note Not thread-safe with respect to the same mux.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_mux_init(ra8_imgdec_mux_t* mux);

/**
 * @brief Append a bound decoder to the mux.
 *
 * @details
 * Order is priority: routing walks the members in the order they were added
 * and stops at the first one advertising the pair asked for, so a consumer
 * that prefers the first-party JPEG over stb's simply adds it first. Overlap
 * between members is expected rather than refused.
 *
 * The member's capability record is fetched and checked here, not at the first
 * decode, so a backend that was never bound or that reports an unusable record
 * is rejected while the caller is still assembling the set.
 *
 * @param[in,out] mux Mux to append to.
 * @param[in]     dec Bound decoder handle. Copied, not retained by pointer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Member appended.
 * @retval k_ra8_err_null_ptr        `mux` or `dec` was NULL.
 * @retval k_ra8_err_no_mem          The mux already holds
 *                                   ::k_ra8_imgdec_mux_max members.
 * @retval k_ra8_err_not_initialized No backend is bound to `dec`.
 * @retval k_ra8_err_invalid_state   `dec` reported an unusable record.
 *
 * @post On any non-ok return the member set is unchanged.
 *
 * @note Not thread-safe with respect to the same mux.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_mux_add(ra8_imgdec_mux_t* mux, const ra8_imgdec_t* dec);

/**
 * @brief Report every container the set can open into @p pixel.
 *
 * @details
 * The question the four separate format matrices made unanswerable: given the
 * surface I want written, what can I show? Only members that advertise
 * @p pixel contribute their formats, so the answer never promises a pair no
 * single member can satisfy.
 *
 * @param[in]  mux         Mux to query.
 * @param[in]  pixel       Destination layout. Exactly one defined bit.
 * @param[out] out_formats OR of ::ra8_imgdec_format_t bits, possibly
 *                         ::k_ra8_imgdec_format_none.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*out_formats` answered.
 * @retval k_ra8_err_null_ptr        `mux` or `out_formats` was NULL.
 * @retval k_ra8_err_invalid_arg     `pixel` was not exactly one defined bit.
 * @retval k_ra8_err_not_initialized The mux holds no members.
 * @retval other                     Propagated from a member's capability
 *                                   query.
 *
 * @post On any non-ok return `*out_formats` is ::k_ra8_imgdec_format_none.
 *
 * @note Thread-safe (pure read of immutable backend state).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_mux_formats(const ra8_imgdec_mux_t* mux,
                                               ra8_imgdec_pixel_t      pixel,
                                               uint32_t*               out_formats);

/**
 * @brief Ask whether any member opens @p format into @p pixel.
 *
 * @param[in]  mux    Mux to query.
 * @param[in]  format Container format. Exactly one defined bit.
 * @param[in]  pixel  Destination layout. Exactly one defined bit.
 * @param[out] out_ok Set true only when one member advertises both.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*out_ok` answered.
 * @retval k_ra8_err_null_ptr        `mux` or `out_ok` was NULL.
 * @retval k_ra8_err_invalid_arg     `format` or `pixel` was not exactly one
 *                                   defined bit.
 * @retval k_ra8_err_not_initialized The mux holds no members.
 * @retval other                     Propagated from a member's capability
 *                                   query.
 *
 * @post On any non-ok return `*out_ok` is false.
 *
 * @note Thread-safe (pure read of immutable backend state).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_mux_supports(const ra8_imgdec_mux_t* mux,
                                                ra8_imgdec_format_t     format,
                                                ra8_imgdec_pixel_t      pixel,
                                                bool*                   out_ok);

/**
 * @brief Name the member that would serve @p format into @p pixel.
 *
 * @details
 * Exposed because a consumer sometimes needs the chosen backend rather than
 * the decode: to read its ::ra8_imgdec_caps_t and size the arena it will have
 * to supply. The returned handle points into @p mux and stays valid until the
 * mux is re-initialised.
 *
 * @param[in]  mux    Mux to route through.
 * @param[in]  format Container format. Exactly one defined bit.
 * @param[in]  pixel  Destination layout. Exactly one defined bit.
 * @param[out] out    Receives the first member advertising both.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*out` names the member that would serve.
 * @retval k_ra8_err_null_ptr        `mux` or `out` was NULL.
 * @retval k_ra8_err_invalid_arg     `format` or `pixel` was not exactly one
 *                                   defined bit.
 * @retval k_ra8_err_not_initialized The mux holds no members.
 * @retval k_ra8_err_not_supported   No member opens that pair.
 * @retval other                     Propagated from a member's capability
 *                                   query.
 *
 * @post On any non-ok return `*out` is NULL.
 *
 * @note Thread-safe (pure read of immutable backend state).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_mux_route(const ra8_imgdec_mux_t* mux,
                                             ra8_imgdec_format_t     format,
                                             ra8_imgdec_pixel_t      pixel,
                                             const ra8_imgdec_t**    out);

/**
 * @brief Decode @p req through whichever member can serve it.
 *
 * @details
 * The container is settled once, here: a request leaving `format` as
 * ::k_ra8_imgdec_format_none is sniffed with ::ra8_imgdec_sniff() before any
 * member is chosen, because the choice depends on the answer. The member then
 * receives the request with that format already named, so it is sniffed once
 * per decode however many backends are in the set.
 *
 * Everything past the routing decision is the single-backend fabric's job and
 * is not repeated here: ::ra8_imgdec_decode re-validates the request, gates it
 * against the chosen backend's own record, checks the arena against that
 * backend's scratch budget, and clears `*out` on failure.
 *
 * @param[in]  mux Mux to route through.
 * @param[in]  req Decode request.
 * @param[out] out What the decode produced.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Image decoded into `req->dst`.
 * @retval k_ra8_err_null_ptr        `mux`, `req`, `out`, `req->bytes` or
 *                                   `req->dst` was NULL.
 * @retval k_ra8_err_invalid_arg     `req->want` was not exactly one defined
 *                                   bit, or `req->format` was neither
 *                                   `_none` nor exactly one defined bit.
 * @retval k_ra8_err_invalid_size    `req->byte_count` was 0.
 * @retval k_ra8_err_not_initialized The mux holds no members.
 * @retval k_ra8_err_not_supported   The bytes carry no recognised signature,
 *                                   or no member opens that container into
 *                                   that layout.
 * @retval other                     Propagated from ::ra8_imgdec_decode.
 *
 * @post On any non-ok return `*out` is zeroed.
 *
 * @note Not thread-safe with respect to the routed member's handle.
 *
 * @see ra8_imgdec_decode()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_mux_decode(const ra8_imgdec_mux_t* mux,
                                              const ra8_imgdec_req_t* req,
                                              ra8_imgdec_image_t*     out);

/**
 * @brief Report the one scratch budget that covers every member of the set.
 *
 * @details
 * Each backend publishes its own `scratch_bytes` / `scratch_align`, which is
 * the right shape for a backend and the wrong shape for a consumer: a consumer
 * does not know which member a given file will route to, so it cannot know
 * which member's budget to honour. The answer is the peak. Carve the peak once
 * and any member the router picks is funded, which is what made the five
 * private shims a guess from the outside and makes this a query.
 *
 * A set whose every member decodes without scratch answers `*out_bytes` 0.
 * That is a real answer, not an error: the fabric already accepts a request
 * with no arena for such a backend.
 *
 * @param[in]  mux       Mux to query.
 * @param[out] out_bytes Peak `scratch_bytes` over the members. May be 0.
 * @param[out] out_align Strongest `scratch_align` over the members, or
 *                       ::k_ra8_imgdec_scratch_align when every member
 *                       reported 0 (the carve's own default).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Budget answered.
 * @retval k_ra8_err_null_ptr        `mux`, `out_bytes` or `out_align` was NULL.
 * @retval k_ra8_err_not_initialized The mux holds no members.
 * @retval other                     Propagated from a member's capability
 *                                   query.
 *
 * @post On any non-ok return `*out_bytes` is 0 and `*out_align` is 0.
 *
 * @note Thread-safe (pure read of immutable backend state).
 *
 * @see ra8_imgdec_caps_t
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_mux_scratch_budget(const ra8_imgdec_mux_t* mux,
                                                      uint32_t*               out_bytes,
                                                      uint32_t*               out_align);

/**
 * @brief Carve one scratch out of @p arena that funds any member of the set.
 *
 * @details
 * ::ra8_imgdec_mux_scratch_budget then ::ra8_imgdec_scratch_carve, as the one
 * call a consumer actually makes at binding time. Carving per member would
 * spend the arena once per backend for a scratch only one of them uses at a
 * time; carving per decode would spend it a decode at a time, because
 * ::ra8_arena_carve has no matching free. One carve, rewound by the scratch
 * between decodes, is the whole lifecycle.
 *
 * A set whose every member decodes without scratch carves nothing: @p out is
 * left empty, @p arena is untouched, and the return is ::k_ra8_ok. Handing
 * that empty scratch to a decode is correct, because no member of such a set
 * asks for one.
 *
 * @param[in]     mux   Mux whose members must all be funded.
 * @param[in,out] arena Arena to carve from; advanced only when a carve happens.
 * @param[out]    out   Scratch record to bind over the carved block.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Carved, or nothing to carve.
 * @retval k_ra8_err_null_ptr        `mux`, `arena` or `out` was NULL.
 * @retval k_ra8_err_not_initialized The mux holds no members.
 * @retval k_ra8_err_not_supported   A member asks for an alignment stronger
 *                                   than ::k_ra8_imgdec_scratch_align.
 * @retval k_ra8_err_no_mem          The arena has no room for the peak.
 * @retval other                     Propagated from a member's capability
 *                                   query or from the carve.
 *
 * @pre @p arena was populated by ::ra8_arena_init.
 * @post On any non-ok return @p out is empty and @p arena is unchanged.
 *
 * @note Not thread-safe: one scratch belongs to one decode.
 *
 * @see ra8_imgdec_scratch_carve()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_mux_carve(const ra8_imgdec_mux_t* mux,
                                             ra8_arena_t*            arena,
                                             ra8_imgdec_scratch_t*   out);

/**
 * @brief Ask the set whether it can open these bytes into @p pixel, and how
 *        big the image says it is.
 *
 * @details
 * The set-level form of ::ra8_imgdec_probe, and the question a consumer with a
 * mux actually has before it reserves a destination surface: *can any of my
 * backends show this file at the layout I want, and what size is it*. Asking
 * it by hand means ::ra8_imgdec_sniff, then ::ra8_imgdec_mux_route, then
 * ::ra8_imgdec_probe on whatever came back, with the error mapping written out
 * again at every call site. Four decode paths' worth of that is the
 * duplication RA8FW-308 exists to remove, so the mux does it once.
 *
 * Nothing is decoded: the answer costs a signature read plus a header parse,
 * not a decode, and no backend hook beyond its capability query is reached.
 *
 * @par Which member answers
 * The routed member is the one ::ra8_imgdec_mux_decode would hand the request
 * to: the first in priority order advertising both the sniffed container and
 * @p pixel. A member that covers the pair but publishes a `dim_max` smaller
 * than this image makes the probe ::k_ra8_err_not_supported; it does *not*
 * fall through to a later member. Routing on anything but the pair would mean
 * probe and decode disagreeing about who serves a request, and a probe whose
 * answer does not describe the decode that follows is worse than no probe.
 *
 * @param[in]  mux        Mux to query.
 * @param[in]  bytes      Leading bytes of the encoded image.
 * @param[in]  byte_count Readable length of @p bytes.
 * @param[in]  pixel      Destination layout the caller intends to decode into.
 * @param[out] out_geom   Receives the container and its declared geometry.
 * @param[out] out_member Receives the member that would serve the decode, or
 *                        NULL when the caller only wants the geometry.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  `*out_geom` holds a container this set
 *                                   opens into @p pixel, at a size the routed
 *                                   member accepts.
 * @retval k_ra8_err_null_ptr        `mux`, `bytes` or `out_geom` was NULL.
 * @retval k_ra8_err_not_initialized The mux holds no members.
 * @retval k_ra8_err_invalid_arg     `pixel` did not name exactly one defined
 *                                   layout.
 * @retval k_ra8_err_invalid_size    `byte_count` was 0, or a declared
 *                                   dimension is 0 or over
 *                                   ::k_ra8_imgdec_dim_max.
 * @retval k_ra8_err_not_supported   The set cannot open these bytes into
 *                                   @p pixel: no recognised signature, a
 *                                   container whose geometry is not readable
 *                                   here, no member advertising the pair, or a
 *                                   size past the routed member's `dim_max`.
 * @retval k_ra8_err_invalid_state   A member reported an unusable record.
 *
 * @pre @p bytes holds @p byte_count readable bytes.
 * @post `*out_geom` is zeroed and `*out_member` (when given) is NULL on every
 *       non-ok return.
 * @post The buffer is never written and no decoder is invoked.
 *
 * @note A buffer carrying no recognised signature is reported
 *       ::k_ra8_err_not_supported, the same code ::ra8_imgdec_mux_decode gives
 *       it, so the two doors answer the same bytes the same way.
 *
 * @note Thread-safe (pure read of @p bytes and of immutable backend state).
 *
 * @see ra8_imgdec_probe()
 * @see ra8_imgdec_mux_route()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_mux_probe(const ra8_imgdec_mux_t* mux,
                                             const uint8_t*          bytes,
                                             uint32_t                byte_count,
                                             ra8_imgdec_pixel_t      pixel,
                                             ra8_imgdec_geom_t*      out_geom,
                                             const ra8_imgdec_t**    out_member);

#ifdef __cplusplus
}
#endif
