/**
 * @file ra8_imgdec_name.h
 * @brief Canonical extension and MIME names for a sniffed container.
 * @ingroup grp_io
 *
 * @par Tag
 * [Ring 3 / Imaging] {World: NS}
 *
 * @details
 * ::ra8_imgdec_sniff answers *which* container a buffer holds. It does not
 * answer what to call the thing, and until this header the only code in the
 * tree that could was a private table inside the media_dl URL namer: a
 * downloader could name a PNG, and the three host tools that also receive
 * bytes they did not produce (`rabook_imagepack`, `mkbookimg`,
 * `rabook_viewer`) could not, because the only identification API on their
 * side of the fence was the decoder probe.
 *
 * So the naming table lives here, next to the signature table it has to agree
 * with. One row per ::ra8_imgdec_format_t, and ::ra8_imgdec_identify is the
 * one-call form the consumers actually want: bytes in, `{format, ext, mime}`
 * out.
 *
 * @code
 * ra8_imgdec_name_t id = {};
 * if (ra8_imgdec_identify(head, head_len, &id) == k_ra8_ok) {
 *   printf("%s (%s)\n", id.ext, id.mime);
 * }
 * @endcode
 *
 * ## Naming is not a decode claim
 *
 * Naming a container is not a promise that any bound backend can open it.
 * ::k_ra8_imgdec_format_gif and ::k_ra8_imgdec_format_bmp are nameable here
 * and are not decodable by every consumer that can name them; ask
 * ::ra8_imgdec_get_caps for that, never this header.
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

/**
 * @struct ra8_imgdec_name_t
 * @brief One container and the canonical names this tree publishes for it.
 *
 * @details Both strings are process-lifetime constants owned by the library.
 * A caller borrows them and must not free or mutate either.
 *
 * @invariant On a successful call `format` is exactly one
 *            ::ra8_imgdec_format_t bit and both strings are non-NULL.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_imgdec_format_t format; /**< Container named, one format bit.    */
  const char*         ext;    /**< Canonical extension, without a dot. */
  const char*         mime;   /**< Canonical MIME type.                */
} ra8_imgdec_name_t;

/**
 * @brief Name one container format.
 *
 * @details A pure table lookup. It reads no bytes and makes no claim about
 * whether the format can be decoded here.
 *
 * ::k_ra8_imgdec_format_tga has a row even though ::ra8_imgdec_sniff can
 * never report it: TGA has no signature, so it can only arrive as a format a
 * caller declares, and a caller holding a declared TGA still needs to name
 * the file it writes.
 *
 * @param[in]  format One ::ra8_imgdec_format_t bit.
 * @param[out] out    Receives the format and its borrowed names.
 * @return Result code.
 * @retval k_ra8_ok            @p out holds a complete naming record.
 * @retval k_ra8_err_null_ptr  @p out is NULL.
 * @retval k_ra8_err_not_found @p format is `_none`, a combination of bits, or
 *                             a bit this tree does not define.
 * @pre @p out references writable storage.
 * @post On any non-ok return `*out` is zeroed.
 * @post The returned strings outlive the call and are never written through.
 * @note Thread-safe: reads only immutable static data.
 * @see ra8_imgdec_identify(), ra8_imgdec_sniff()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_imgdec_name(ra8_imgdec_format_t format, ra8_imgdec_name_t* out);

/**
 * @brief Sniff a buffer and name what it holds, in one call.
 *
 * @details ::ra8_imgdec_sniff followed by ::ra8_imgdec_name, which is the
 * whole of what a consumer receiving foreign bytes needs. Reads at most
 * ::k_ra8_imgdec_sniff_bytes leading bytes; a shorter prefix that cannot
 * complete a signature is refused rather than guessed.
 *
 * @param[in]  bytes      Readable buffer prefix.
 * @param[in]  byte_count Readable bytes at @p bytes.
 * @param[out] out        Receives the format and its borrowed names.
 * @return Result code.
 * @retval k_ra8_ok               @p out holds a complete naming record.
 * @retval k_ra8_err_null_ptr     @p bytes or @p out is NULL.
 * @retval k_ra8_err_invalid_size @p byte_count is zero.
 * @retval k_ra8_err_not_found    No complete supported signature is present.
 * @pre @p bytes holds at least @p byte_count readable bytes.
 * @post On any non-ok return `*out` is zeroed.
 * @post The inspected bytes and their ownership are unchanged.
 * @note Never reports ::k_ra8_imgdec_format_tga; TGA has no signature.
 * @see ra8_imgdec_sniff(), ra8_imgdec_name()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_imgdec_identify(const uint8_t* bytes, uint32_t byte_count, ra8_imgdec_name_t* out);

#ifdef __cplusplus
}
#endif
