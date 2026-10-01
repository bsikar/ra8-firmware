/**
 * @file ra8_appimg.h
 * @brief The `.ra8app` container header: identity, capabilities, signed span (RA8FW-293).
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
 * @brief Fixed byte-lengths of the header's sized fields.
 *
 * @details
 * An Ed25519 signature is the raw 64-byte `R || S` string (RFC 8032 Sec 5.1.6).
 * The two text fields are fixed-width so the header has one size on every
 * toolchain; each carries a NUL-terminated string and is zero-padded to width.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_appimg_sig_bytes    = 64U, /**< Ed25519 raw R||S signature length.   */
  k_ra8_appimg_app_id_cap   = 32U, /**< `app_id` field width, incl. the NUL. */
  k_ra8_appimg_name_cap     = 32U, /**< `display_name` width, incl. the NUL. */
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
  /** Header format revision this implementation reads. */
  k_ra8_appimg_format_version = 0x00000001U,
  /** Syscall-table generation the running firmware publishes. */
  k_ra8_appimg_api_version_current = 0x00000001U,
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
  k_ra8_appimg_cap_known   = 0x00000007U, /**< Mask of all defined bits.       */
} ra8_appimg_capability_t;

/**
 * @struct ra8_appimg_header_t
 * @brief The `.ra8app` file header, as laid out on disk (little-endian).
 *
 * @details
 * Field order is the wire order: eight 32-bit words, the two fixed-width text
 * fields, then the signature. Every field is naturally aligned and the struct
 * carries no implicit padding, so a read of the first `sizeof(ra8_appimg_header_t)`
 * bytes of the file *is* the parsed header on any little-endian target.
 *
 * `entry_offset`, `code_size` and `data_size` describe the ThreadX module image
 * that follows the header; `stack_size` is the module thread's stack, sized by
 * the app and honoured by the Module Manager.
 *
 * @invariant `magic == k_ra8_appimg_magic` for a valid container.
 * @invariant `format_version == k_ra8_appimg_format_version`.
 * @invariant `(capabilities & ~k_ra8_appimg_cap_known) == 0`.
 * @invariant `app_id` and `display_name` each contain a NUL within their width.
 *
 * @note The signature field is the last member on purpose: the signed span is
 *       then one contiguous run of bytes ending where `signature` begins, plus
 *       the payload that follows the header. See ::ra8_appimg_signed_span.
 *
 * @see ra8_appimg_parse
 * @since 0.1.0
 */
typedef struct {
  uint32_t magic;           /**< ::k_ra8_appimg_magic.                      */
  uint32_t format_version;  /**< ::k_ra8_appimg_format_version.             */
  uint32_t entry_offset;    /**< Module entry, bytes from the payload base. */
  uint32_t code_size;       /**< Instruction-area length, bytes.            */
  uint32_t data_size;       /**< Data-area length, bytes.                   */
  uint32_t stack_size;      /**< Module thread stack, bytes.                */
  uint32_t min_api_version; /**< Lowest host API generation that will do.   */
  uint32_t capabilities;    /**< ::ra8_appimg_capability_t bitfield.        */
  /** Reverse-DNS application identity, e.g. `com.example.reader`. */
  char app_id[k_ra8_appimg_app_id_cap];
  /** Human-facing label the launcher shows. */
  char display_name[k_ra8_appimg_name_cap];
  /** Ed25519 raw `R || S` over the span ::ra8_appimg_signed_span reports. */
  uint8_t signature[k_ra8_appimg_sig_bytes];
} ra8_appimg_header_t;

/* Eight 32-bit words precede the two text fields and the signature block; the
 * assert pins the no-padding layout the on-disk format depends on. */
static_assert(sizeof(ra8_appimg_header_t) ==
                (8U * sizeof(uint32_t)) + (size_t)k_ra8_appimg_app_id_cap +
                  (size_t)k_ra8_appimg_name_cap + (size_t)k_ra8_appimg_sig_bytes,
              "ra8_appimg_header_t must have no implicit padding");

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
 * Copies the leading `sizeof(ra8_appimg_header_t)` bytes into @p out, then
 * refuses anything that is not a loadable container of this revision: wrong
 * magic or format version, an API generation this firmware does not implement,
 * an undefined capability bit, a text field with no NUL inside its width, a
 * zero or over-cap code size, an over-cap data size, a stack below
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
 * @retval k_ra8_err_out_of_range      A declared size or the entry offset is
 *                                     outside its permitted range.
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
 * header's first `offsetof(ra8_appimg_header_t, signature)` bytes, and then the
 * payload from the end of the header to the end of the file. Those two runs are
 * *not* contiguous, so the span returned here is the leading run and the caller
 * hashes the payload run after it; ::ra8_appimg_payload_span names the second.
 *
 * Defining both here is the point of the call: the signer in
 * `scripts/secrets/rot_sign.py` and the verifier of RA8FW-291 read one definition
 * rather than two implementations that agree until they do not.
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
 * The payload begins immediately after the header and runs to the end of the
 * declared image: `code_size + data_size` bytes. It is the second half of the
 * signed material and the region the Module Manager maps.
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
 * @brief Decide whether a host grant satisfies an app's declared manifest.
 *
 * @details
 * The one place the comparison is written: every bit the app declares must
 * appear in @p granted. An app that declares nothing is satisfied by any grant,
 * including ::k_ra8_appimg_cap_none.
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
