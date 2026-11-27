/**
 * @file ra8_appimg.h
 * @brief The `.ra8app` container header: identity, capabilities, signed span (RA8FW-293, RA8FW-531).
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / UI] {World: NS}
 *
 * @details
 * A loadable app ships as a `.ra8app` file: a fixed-size little-endian header
 * followed by the ThreadX module image the Module Manager loads. The header
 * answers the three questions the loader must settle *before* any of the image
 * is trusted:
 *
 * 1. **Is this one of ours, and can this firmware run it?** Magic, format
 *    version and ::ra8_appimg_header_t::min_api_version.
 * 2. **What is it allowed to reach?** The ::ra8_appimg_capability_t bitfield,
 *    declared by the app and granted (or refused) by the host.
 * 3. **Which bytes does the signature cover?** ::ra8_appimg_signed_span, the
 *    single definition of the signed region, so the signer and the verifier
 *    cannot disagree about it.
 *
 * Format revision 2 (RA8FW-531) adds icon metadata: a fixed 16x16 2-bit
 * packed grayscale icon (64 bytes) carried in the signed payload.
 *
 * This header is the *format*, not the loader. It parses and validates; it
 * performs no cryptography and reads no filesystem. Signature verification
 * over the span this file defines is issue RA8FW-291, and the Module Manager hook
 * that refuses a failing header is gated on the Cortex-M85 module port (RA8FW-295).
 *
 * @code
 * ra8_appimg_header_t app = {0};
 * if (ra8_appimg_parse(file_bytes, file_len, &app) != k_ra8_ok) {
 *   return k_ra8_err_validation_failed;  // default-deny, nothing is loaded
 * }
 *
 * ra8_appimg_span_t span = {0};
 * (void)ra8_appimg_signed_span(&app, file_len, &span);
 * // hash [file_bytes + span.offset, + span.length), then verify app.signature
 * @endcode
 *
 * @note Default-deny, like ::ra8_rot_verify_image: every refusal path returns a
 *       non-`k_ra8_ok` error and the caller must not load the image. An
 *       undefined capability bit is a refusal rather than an ignored bit, so a
 *       newer app cannot silently lose a permission on older firmware.
 *
 * @see ra8_rot.h -- the image-level root of trust this container defers to for
 *      the algorithm choice (Ed25519 here, per RA8FW-291).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_appimg_size_t
 * @brief Fixed byte-lengths of the header's sized fields and icon geometry.
 *
 * @details
 * An Ed25519 signature is the raw 64-byte `R || S` string (RFC 8032 Sec 5.1.6).
 * The two text fields are fixed-width so the header has one size on every
 * toolchain; each carries a NUL-terminated string and is zero-padded to width.
 * Format v2 adds fixed 16x16 2-bit packed grayscale icon (64 bytes).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_appimg_sig_bytes    = 64U, /**< Ed25519 raw R||S signature length.    */
  k_ra8_appimg_app_id_cap   = 32U, /**< `app_id` field width, incl. the NUL.  */
  k_ra8_appimg_name_cap     = 32U, /**< `display_name` width, incl. the NUL.  */
  k_ra8_appimg_icon_width   = 16U, /**< Format v2 fixed icon width (pixels).  */
  k_ra8_appimg_icon_height  = 16U, /**< Format v2 fixed icon height (pixels). */
  k_ra8_appimg_icon_bytes   = 64U, /**< Format v2 2-bit packed icon bytes.    */
} ra8_appimg_size_t;

/**
 * @enum ra8_appimg_const_t
 * @brief Container magic, format version and the loadable-size caps.
 *
 * @details
 * The code + data cap is the Non-Secure SRAM a module region can be given on
 * this board (1 MiB), which bounds every length this header can ask the loader
 * to map before a single byte is read (NASA Rule 2). `api_version_current` is
 * the API generation the running firmware publishes; an app asking for more
 * than this is refused as too new.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  /** ASCII "RA8A": the container marker at the head of every `.ra8app` file. */
  k_ra8_appimg_magic = 0x52413841U,
  /** Legacy format revision (v1). */
  k_ra8_appimg_format_version_v1 = 0x00000001U,
  /** Format revision with icon metadata (v2). */
  k_ra8_appimg_format_version_v2 = 0x00000002U,
  /** Default header format revision. */
  k_ra8_appimg_format_version = 0x00000002U,
  /** Syscall-table generation the running firmware publishes. */
  k_ra8_appimg_api_version_current = 0x00000002U,
  /** Per-segment byte cap, the Non-Secure region a module can be given. */
  k_ra8_appimg_segment_max = 0x00100000U,
  /** Smallest usable module stack, bytes. */
  k_ra8_appimg_stack_min = 0x00000400U,
} ra8_appimg_const_t;

/**
 * @enum ra8_appimg_capability_t
 * @brief What a module declares it needs, one bit per host resource.
 *
 * @details
 * The manifest is a declaration, never a grant: the host compares these bits
 * against what it is willing to give and refuses the load when the app asks
 * for more. ::k_ra8_appimg_cap_known is the mask of every bit this format
 * revision defines; a bit outside it makes the header invalid.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_appimg_cap_none    = 0x00000000U, /**< Compute only, reaches nothing.  */
  k_ra8_appimg_cap_display = 0x00000001U, /**< Draws into its screen region.   */
  k_ra8_appimg_cap_storage = 0x00000002U, /**< Reads/writes under its own dir. */
  k_ra8_appimg_cap_network = 0x00000004U, /**< Opens network connections.      */
  k_ra8_appimg_cap_led     = 0x00000008U, /**< Drives board status LEDs.       */
  k_ra8_appimg_cap_known   = 0x0000000FU, /**< Mask of all defined bits.       */
} ra8_appimg_capability_t;

/**
 * @struct ra8_appimg_header_t
 * @brief The `.ra8app` file header, as laid out on disk (little-endian).
 *
 * @details
 * Field order is the wire order: eight 32-bit words, the two fixed-width text
 * fields, icon offset and length (v2), then the signature. Every field is
 * naturally aligned and the struct carries no implicit padding.
 *
 * On disk, format v1 headers occupy 160 bytes (ending after `display_name`
 * directly into the 64-byte `signature`). Format v2 headers occupy 168 bytes
 * (including `icon_offset` and `icon_length` before `signature`). Both parse
 * into this struct, with `icon_offset = 0` and `icon_length = 0` for v1.
 *
 * @since 0.1.0
 */
typedef struct {
  /** ASCII "RA8A" (`0x52413841`). */
  uint32_t magic;
  /** Format revision (`1` for v1, `2` for v2). */
  uint32_t format_version;
  /** Byte offset of the entry point into the code segment. */
  uint32_t entry_offset;
  /** Byte length of the module code (text + rodata) segment. */
  uint32_t code_size;
  /** Byte length of the module data segment (0 for pure code). */
  uint32_t data_size;
  /** Stack allocation the loader must give the module, bytes. */
  uint32_t stack_size;
  /** Lowest syscall-table generation the app can run against. */
  uint32_t min_api_version;
  /** Capability bitfield the app requests. */
  uint32_t capabilities;
  /** Reverse-DNS identifier: unique, immutable, NUL-terminated. */
  char app_id[k_ra8_appimg_app_id_cap];
  /** Human-facing label the launcher shows. */
  char display_name[k_ra8_appimg_name_cap];
  /** Byte offset of the icon from the start of the payload (v2, 0 if none). */
  uint32_t icon_offset;
  /** Byte length of the icon (v2, 0 if none; 64 for 16x16 2-bit grayscale). */
  uint32_t icon_length;
  /** Ed25519 raw `R || S` over the span ::ra8_appimg_signed_span reports. */
  uint8_t signature[k_ra8_appimg_sig_bytes];
} ra8_appimg_header_t;

static_assert(sizeof(ra8_appimg_header_t) ==
                (10U * sizeof(uint32_t)) + (size_t)k_ra8_appimg_app_id_cap +
                  (size_t)k_ra8_appimg_name_cap + (size_t)k_ra8_appimg_sig_bytes,
              "ra8_appimg_header_t must have no implicit padding");
static_assert(sizeof(ra8_appimg_header_t) == 168U,
              "ra8_appimg_header_t must be exactly 168 bytes");

/**
 * @struct ra8_appimg_span_t
 * @brief A byte range within the `.ra8app` file, measured from its first byte.
 *
 * @details
 * Produced by ::ra8_appimg_signed_span so the signer and the verifier name the
 * signed region the same way: an offset from the start of the file and a
 * length, never a pointer, so it survives being written to a tool's stdout.
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t offset; /**< First signed byte, from the file's base. */
  uint32_t length; /**< Signed byte count.                       */
} ra8_appimg_span_t;

/**
 * @brief Read and validate a `.ra8app` header from the head of a file image.
 *
 * @details
 * Parses format v1 or v2 headers into @p out, then refuses anything that is
 * not a loadable container: wrong magic or format version, an API generation
 * this firmware does not implement, an undefined capability bit, a text field
 * with no NUL inside its width, an invalid icon specification, a zero or
 * over-cap code size, an over-cap data size, a stack below
 * ::k_ra8_appimg_stack_min, an entry offset outside the code area, or a file
 * too short to hold the payload the header declares.
 *
 * @p out is written only on success, so a refused header cannot leave a caller
 * holding half-parsed fields.
 *
 * @param[in]  bytes Start of the file image; non-NULL.
 * @param[in]  len   Readable byte length of @p bytes.
 * @param[out] out   Receives the validated header; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Header is well-formed and loadable.
 * @retval k_ra8_err_null_ptr          @p bytes or @p out is NULL.
 * @retval k_ra8_err_invalid_size      @p len is shorter than the header, or
 *                                     shorter than the payload it declares.
 * @retval k_ra8_err_validation_failed Magic, format version, capability bits or
 *                                     a text field is malformed.
 * @retval k_ra8_err_not_supported     `min_api_version` exceeds
 *                                     ::k_ra8_appimg_api_version_current.
 * @retval k_ra8_err_out_of_range      A declared size, entry offset or icon
 *                                     range is outside its permitted range.
 *
 * @pre @p bytes addresses at least @p len readable bytes.
 * @post On any non-`k_ra8_ok` return @p out is unmodified and nothing is loaded.
 *
 * @note Pure: no allocation, no I/O, no cryptography. A header that parses is
 *       well-formed, not yet authentic -- see ::ra8_appimg_signed_span.
 *
 * @see ra8_appimg_signed_span
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_appimg_parse(const uint8_t* bytes, size_t len, ra8_appimg_header_t* out);

/**
 * @brief Report the byte range an `.ra8app` signature covers.
 *
 * @details
 * The signed region is everything except the signature field itself: the
 * header's leading run up to `signature`, and then the payload from the end
 * of the header to the end of the file.
 *
 * @param[in]  header Validated header; non-NULL.
 * @param[in]  len    Total file length in bytes.
 * @param[out] out    Receives the leading signed run; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Span reported.
 * @retval k_ra8_err_null_ptr     @p header or @p out is NULL.
 * @retval k_ra8_err_invalid_size @p len cannot hold the header.
 *
 * @post On any non-`k_ra8_ok` return @p out is zeroed.
 *
 * @see ra8_appimg_payload_span
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_appimg_signed_span(const ra8_appimg_header_t* header, size_t len, ra8_appimg_span_t* out);

/**
 * @brief Report the byte range the module payload occupies.
 *
 * @details
 * The payload begins immediately after the header on disk and runs to the end
 * of the declared material: `code_size + data_size` bytes (and icon if present).
 *
 * @param[in]  header Validated header; non-NULL.
 * @param[in]  len    Total file length in bytes.
 * @param[out] out    Receives the payload run; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Span reported.
 * @retval k_ra8_err_null_ptr     @p header or @p out is NULL.
 * @retval k_ra8_err_invalid_size @p len is shorter than header + payload.
 *
 * @post On any non-`k_ra8_ok` return @p out is zeroed.
 *
 * @see ra8_appimg_signed_span
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_appimg_payload_span(const ra8_appimg_header_t* header, size_t len, ra8_appimg_span_t* out);

/**
 * @brief Report the byte range the app icon occupies within the `.ra8app` file.
 *
 * @details
 * If the header declares no icon (`icon_length == 0`), @p out is zeroed and
 * `k_ra8_ok` is returned. If an icon is present, @p out receives the
 * file-relative offset and length.
 *
 * @param[in]  header Validated header; non-NULL.
 * @param[in]  len    Total file length in bytes.
 * @param[out] out    Receives the icon run; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Span reported.
 * @retval k_ra8_err_null_ptr     @p header or @p out is NULL.
 * @retval k_ra8_err_invalid_size @p len is shorter than header + icon range.
 * @retval k_ra8_err_out_of_range Declared icon range is invalid.
 *
 * @post On any non-`k_ra8_ok` return @p out is zeroed.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_appimg_icon_span(const ra8_appimg_header_t* header, size_t len, ra8_appimg_span_t* out);

/**
 * @brief Retrieve a pointer to the app icon data from an `.ra8app` image buffer.
 *
 * @details
 * If the header declares an icon, reports a direct pointer into @p bytes and
 * sets `*out_len = 64`. If no icon is present, sets `*out_icon = NULL` and
 * `*out_len = 0` and returns `k_ra8_ok`.
 *
 * @param[in]  header   Validated header; non-NULL.
 * @param[in]  bytes    Start of the `.ra8app` file image; non-NULL.
 * @param[in]  len      Total readable byte length of @p bytes.
 * @param[out] out_icon Receives pointer to 64-byte icon (or NULL); non-NULL.
 * @param[out] out_len  Receives icon byte length (64 or 0); non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Icon reported.
 * @retval k_ra8_err_null_ptr     Any argument is NULL.
 * @retval k_ra8_err_invalid_size @p len cannot hold the icon.
 * @retval k_ra8_err_out_of_range Icon range is invalid.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_appimg_get_icon(const ra8_appimg_header_t* header,
                    const uint8_t*            bytes,
                    size_t                    len,
                    const uint8_t**           out_icon,
                    size_t*                   out_len);

/**
 * @brief Decide whether a host grant satisfies an app's declared manifest.
 *
 * @param[in] header  Validated header; non-NULL.
 * @param[in] granted Capability bits the host is willing to give.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                   Every declared capability is granted.
 * @retval k_ra8_err_null_ptr         @p header is NULL.
 * @retval k_ra8_err_access_denied    The app declares a capability the host
 *                                    withheld -- the load must be refused.
 * @retval k_ra8_err_validation_failed @p granted carries an undefined bit.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_appimg_capabilities_permitted(const ra8_appimg_header_t* header, uint32_t granted);

#ifdef __cplusplus
}
#endif
