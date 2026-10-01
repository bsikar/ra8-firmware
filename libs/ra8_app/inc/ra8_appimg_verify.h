/**
 * @file ra8_appimg_verify.h
 * @brief Default-deny admission decision for a `.ra8app` module image (RA8FW-291).
 * @ingroup grp_board
 *
 * @details
 * `ra8_appimg.h` (RA8FW-293) answers *what an image claims*. This file answers the
 * only question the loader may act on: **may these bytes be loaded?** It is the
 * gate that sits in front of `txm_module_manager_memory_load`, and it refuses
 * by default -- an image reaches the Module Manager only when every one of the
 * following held:
 *
 * 1. the container parses (::ra8_appimg_parse);
 * 2. the declared manifest fits the host's grant
 *    (::ra8_appimg_capabilities_permitted);
 * 3. the signature field is not the all-zero "unsigned" pattern;
 * 4. an Ed25519 PureEdDSA verify over the signed material, against the pinned
 *    root public key, returned success.
 *
 * @par The verify seam
 * The Ed25519 primitive itself is **not** here. The caller supplies it as
 * ::ra8_appimg_verify_fn, so the same decision logic runs against the secure
 * world's NSC veneer on target and against a host stand-in off target. That is
 * what makes the admission policy testable without a board: the policy is
 * proven here, the primitive is proven where it lives.
 *
 * @par Why the message is two runs
 * The signed material is *not contiguous*: it is the header prefix up to the
 * signature field, then the payload after the header, with the 64 signature
 * bytes excised in between. ::ra8_appimg_msg_t carries both runs in signing
 * order, exactly as ::ra8_appimg_signed_span and ::ra8_appimg_payload_span
 * report them, so a verify backend concatenates rather than re-deriving the
 * layout. A backend that got the layout wrong would reject every honest image;
 * naming the runs removes the chance.
 *
 * @par Usage
 * @code
 * const ra8_appimg_verifier_t gate = {
 *   .verify     = board_ed25519_verify_nsc,
 *   .verify_ctx = nullptr,
 *   .public_key = k_root_public_key,
 *   .granted    = k_ra8_appimg_cap_display | k_ra8_appimg_cap_storage,
 * };
 * ra8_appimg_header_t app = {};
 * if (ra8_appimg_verify(&gate, file_bytes, file_len, &app) != k_ra8_ok) {
 *   return; // logged and refused; nothing is mapped
 * }
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#ifndef RA8_APPIMG_VERIFY_H
#define RA8_APPIMG_VERIFY_H

#include <stddef.h>
#include <stdint.h>

#include "ra8_appimg.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_appimg_verify_size_t
 * @brief Fixed widths the admission gate works in.
 *
 * @details
 * Ed25519 (RFC 8032) fixes both: a public key is 32 bytes, a signature is the
 * 64-byte raw `R || S` already carried in ::ra8_appimg_header_t::signature.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  /** Ed25519 public-key length, bytes. */
  k_ra8_appimg_pubkey_bytes = 32U,
} ra8_appimg_verify_size_t;

/**
 * @struct ra8_appimg_msg_t
 * @brief The signed material of an image, as the two runs it really is.
 *
 * @details
 * `head` is the header prefix ending where the signature field begins; `tail`
 * is the payload following the header. A verify backend hashes `head` then
 * `tail`, in that order, and hashes nothing else. `tail` may be NULL with
 * `tail_len == 0` only for an image whose declared payload is empty, which
 * ::ra8_appimg_parse already refuses, so in practice both runs are present.
 *
 * @invariant `head != NULL` and `head_len > 0`.
 * @invariant The two runs together are exactly the bytes the signer covered.
 *
 * @see ra8_appimg_signed_span
 * @see ra8_appimg_payload_span
 * @since 0.1.0
 */
typedef struct {
  const uint8_t* head;     /**< First signed run: header up to `signature`. */
  const uint8_t* tail;     /**< Second signed run: the module payload.      */
  uint32_t       head_len; /**< Length of `head`, bytes.                    */
  uint32_t       tail_len; /**< Length of `tail`, bytes.                    */
} ra8_appimg_msg_t;

/**
 * @typedef ra8_appimg_verify_fn
 * @brief Ed25519 PureEdDSA verify seam supplied by the platform.
 *
 * @details
 * Implementations verify @p signature over the concatenation `msg->head ||
 * msg->tail` under @p public_key, and return ::k_ra8_ok **only** on a
 * cryptographically valid signature. Any other return is treated as a refusal
 * by ::ra8_appimg_verify, which never inspects the code: a backend that cannot
 * decide must not return ::k_ra8_ok.
 *
 * @param[in] ctx        Backend context handed through unchanged; may be NULL.
 * @param[in] msg        Signed material, two runs in signing order; non-NULL.
 * @param[in] signature  Raw 64-byte `R || S`; non-NULL.
 * @param[in] public_key Pinned 32-byte Ed25519 public key; non-NULL.
 *
 * @return ra8_err_t ::k_ra8_ok when, and only when, the signature verifies.
 *
 * @warning Returning ::k_ra8_ok on an unverified message defeats the whole
 *          gate. A backend without a working primitive returns
 *          ::k_ra8_err_not_supported.
 *
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_appimg_verify_fn)(void*                   ctx,
                                          const ra8_appimg_msg_t* msg,
                                          const uint8_t*          signature,
                                          const uint8_t*          public_key);

/**
 * @struct ra8_appimg_verifier_t
 * @brief Everything the admission decision is made against.
 *
 * @details
 * Caller-owned and read-only for the duration of a ::ra8_appimg_verify call.
 * `public_key` must point at ::k_ra8_appimg_pubkey_bytes readable bytes that
 * came from secure storage, never from the image being judged.
 *
 * @invariant `verify != NULL`.
 * @invariant `public_key` addresses ::k_ra8_appimg_pubkey_bytes bytes.
 *
 * @since 0.1.0
 */
typedef struct {
  ra8_appimg_verify_fn verify;     /**< Ed25519 verify backend; non-NULL.    */
  void*                verify_ctx; /**< Opaque backend context; may be NULL. */
  const uint8_t*       public_key; /**< Pinned root public key; non-NULL.    */
  uint32_t             granted;    /**< Capability bits the host will give.  */
} ra8_appimg_verifier_t;

/**
 * @brief Describe the signed material of an already-parsed image.
 *
 * @details
 * Builds the two-run view from ::ra8_appimg_signed_span and
 * ::ra8_appimg_payload_span, so a caller that wants to hash or re-sign an image
 * uses the same definition the gate does. Exposed separately because the signer
 * side needs it without needing a verify backend.
 *
 * @param[in]  header Header already proven by ::ra8_appimg_parse; non-NULL.
 * @param[in]  bytes  Base of the whole file image; non-NULL.
 * @param[in]  len    Total file length, bytes.
 * @param[out] out    Receives the two signed runs; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Runs reported.
 * @retval k_ra8_err_null_ptr     @p header, @p bytes or @p out is NULL.
 * @retval k_ra8_err_invalid_size @p len is shorter than header plus payload.
 *
 * @post On any non-`k_ra8_ok` return @p out is zeroed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_appimg_signed_message(const ra8_appimg_header_t* header,
                                                  const uint8_t*             bytes,
                                                  size_t                     len,
                                                  ra8_appimg_msg_t*          out);

/**
 * @brief Decide whether a `.ra8app` image may be loaded.
 *
 * @details
 * The whole admission policy, in one call and in one order: parse, capability
 * grant, unsigned-image refusal, then the Ed25519 verify. The first refusal
 * wins and nothing after it runs, so a tampered image never reaches the
 * capability comparison with a forged manifest treated as authoritative.
 *
 * @p out_header receives the parsed header only on success; on every refusal it
 * is zeroed, so a caller that ignores the return code loads nothing rather than
 * loading whatever the file claimed.
 *
 * @param[in]  verifier   Backend, pinned key and host grant; non-NULL.
 * @param[in]  bytes      Whole file image; non-NULL.
 * @param[in]  len        Total file length, bytes.
 * @param[out] out_header Receives the admitted header; non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Image is authentic and may be loaded.
 * @retval k_ra8_err_null_ptr          A required pointer is NULL, including
 *                                     ::ra8_appimg_verifier_t::verify or
 *                                     ::ra8_appimg_verifier_t::public_key.
 * @retval k_ra8_err_invalid_size      @p len cannot hold what is declared.
 * @retval k_ra8_err_validation_failed The container is malformed, or the
 *                                     signature field is the all-zero
 *                                     "unsigned" pattern.
 * @retval k_ra8_err_access_denied     The manifest asks for a capability the
 *                                     host withheld.
 * @retval k_ra8_err_not_supported     @p min_api_version is newer than this
 *                                     firmware, or the backend has no Ed25519.
 * @retval k_ra8_err_crc_mismatch      The signature did not verify -- the
 *                                     image is unsigned by this key or was
 *                                     modified after signing.
 *
 * @post On any non-`k_ra8_ok` return @p out_header is zeroed.
 *
 * @warning Success means *authentic*, not *safe*: the capability grant, the
 *          MPU region and the stack bound still do their own work.
 *
 * @see ra8_appimg_parse
 * @see ra8_appimg_capabilities_permitted
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_appimg_verify(const ra8_appimg_verifier_t* verifier,
                                          const uint8_t*               bytes,
                                          size_t                       len,
                                          ra8_appimg_header_t*         out_header);

#ifdef __cplusplus
}
#endif

#endif /* RA8_APPIMG_VERIFY_H */
