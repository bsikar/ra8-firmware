/**
 * @file ra8_appimg_verify.c
 * @brief `.ra8app` admission decision: parse, grant, signature (#663).
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / UI] {World: NS}
 *
 * @details
 * Default-deny and ordered. The gate refuses before it trusts: an image is
 * parsed before its manifest is read, its manifest is compared before its
 * signature is spent, and the signature is verified before the caller is handed
 * a header it could act on. No cryptographic primitive lives here -- the
 * Ed25519 verify arrives as ::ra8_appimg_verify_fn, which is what lets the
 * policy be proven off target while the primitive is proven where it lives.
 *
 * The backend is trusted for exactly one thing: ::k_ra8_ok means the signature
 * verified. Every other code, and every code this file does not recognise, is a
 * refusal. There is no path on which an unrecognised backend answer becomes an
 * admission.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_appimg_verify.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_appimg.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/**
 * @brief Report whether a signature field is the all-zero "unsigned" pattern.
 *
 * @details
 * An unsigned `.ra8app` is spelled by leaving the field zero, which is what a
 * builder that never ran the signer produces. Refusing it here rather than in
 * the backend means the acceptance criterion "unsigned modules are rejected"
 * holds even against a backend that would happily verify against a zero
 * signature.
 *
 * @param[in] signature Signature field of a parsed header.
 * @return true when every byte is zero.
 */
RA8_INTERNAL static bool
internal_signature_absent(const uint8_t* signature)
{
  for (size_t i = 0U; i < (size_t)k_ra8_appimg_sig_bytes; ++i) {
    if (signature[i] != 0U) {
      return false;
    }
  }
  return true;
}

ra8_err_t
ra8_appimg_signed_message(const ra8_appimg_header_t* header,
                          const uint8_t*             bytes,
                          size_t                     len,
                          ra8_appimg_msg_t*          out)
{
  if (out != nullptr) {
    *out = (ra8_appimg_msg_t){};
  }
  if ((header == nullptr) || (bytes == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  ra8_appimg_span_t head = {};
  const ra8_err_t   head_err = ra8_appimg_signed_span(header, len, &head);
  if (head_err != k_ra8_ok) {
    return head_err;
  }

  ra8_appimg_span_t tail = {};
  const ra8_err_t   tail_err = ra8_appimg_payload_span(header, len, &tail);
  if (tail_err != k_ra8_ok) {
    return tail_err;
  }

  out->head     = &bytes[head.offset];
  out->head_len = head.length;
  out->tail     = &bytes[tail.offset];
  out->tail_len = tail.length;
  return k_ra8_ok;
}

ra8_err_t
ra8_appimg_verify(const ra8_appimg_verifier_t* verifier,
                  const uint8_t*               bytes,
                  size_t                       len,
                  ra8_appimg_header_t*         out_header)
{
  if (out_header != nullptr) {
    *out_header = (ra8_appimg_header_t){};
  }
  if ((verifier == nullptr) || (bytes == nullptr) || (out_header == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if ((verifier->verify == nullptr) || (verifier->public_key == nullptr)) {
    return k_ra8_err_null_ptr;
  }

  /* 1. The container itself. Nothing below may read a field this refused. */
  ra8_appimg_header_t header = {};
  const ra8_err_t     parsed = ra8_appimg_parse(bytes, len, &header);
  if (parsed != k_ra8_ok) {
    return parsed;
  }

  /* 2. The manifest against the host grant, before a signature is spent. */
  const ra8_err_t granted = ra8_appimg_capabilities_permitted(&header, verifier->granted);
  if (granted != k_ra8_ok) {
    return granted;
  }

  /* 3. An unsigned image is refused without consulting the backend. */
  if (internal_signature_absent(header.signature)) {
    return k_ra8_err_validation_failed;
  }

  /* 4. The signature over the two signed runs, under the pinned key. */
  ra8_appimg_msg_t msg     = {};
  const ra8_err_t  msg_err = ra8_appimg_signed_message(&header, bytes, len, &msg);
  if (msg_err != k_ra8_ok) {
    return msg_err;
  }

  const ra8_err_t verdict =
    verifier->verify(verifier->verify_ctx, &msg, header.signature, verifier->public_key);
  if (verdict == k_ra8_err_not_supported) {
    return k_ra8_err_not_supported;
  }
  if (verdict != k_ra8_ok) {
    return k_ra8_err_crc_mismatch;
  }

  *out_header = header;
  return k_ra8_ok;
}
