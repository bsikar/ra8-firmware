/**
 * @file ra_tls_internal.h
 * @brief Private constants and helpers shared by ra_tls_*.c files.
 *
 * @details
 * Not part of the public ABI. Lives next to ``ra_tls_*.c`` so the three
 * source files can share record-header offsets, AAD layout, and the
 * pinned ``{0x03, 0x03}`` TLS 1.2 ProtocolVersion bytes (RFC 5246 sec
 * A.1) without exposing them to consumers of ``ra_tls.h``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include "ra_err.h"
#include "ra_tls.h"

/**
 * @enum ra_tls_priv_offsets_t
 * @brief Byte offsets inside the 5-byte TLS record header.
 *
 * @details
 * RFC 5246 sec 6.2.1 lays the record header out as
 * ``ContentType (1) || ProtocolVersion (2) || uint16 length``.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_tls_off_record_type   = 0U, /**< ContentType byte. */
  k_ra_tls_off_record_ver_hi = 1U, /**< ProtocolVersion.major. */
  k_ra_tls_off_record_ver_lo = 2U, /**< ProtocolVersion.minor. */
  k_ra_tls_off_record_len_hi = 3U, /**< uint16 length, high byte. */
  k_ra_tls_off_record_len_lo = 4U, /**< uint16 length, low byte. */
} ra_tls_priv_offsets_t;

/**
 * @enum ra_tls_priv_version_t
 * @brief Pinned ProtocolVersion byte values for TLS 1.2.
 *
 * @details
 * RFC 5246 sec A.1: ``{ 3, 3 }``.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_tls_version_major = 3U, /**< Always 3 for TLS 1.0..1.2. */
  k_ra_tls_version_minor = 3U, /**< 3 for TLS 1.2 specifically. */
} ra_tls_priv_version_t;

/**
 * @enum ra_tls_priv_bits_t
 * @brief Bit-shift / mask helpers for byte serialisation.
 *
 * @details
 * Named so the no-magic-numbers rule is satisfied when writing
 * "shift right by 8 to get the high byte of a uint16".
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_tls_shift_byte = 8U,    /**< One-byte shift. */
  k_ra_tls_mask_byte  = 0xFFU, /**< Low-byte mask. */
} ra_tls_priv_bits_t;

/**
 * @enum ra_tls_priv_aad_t
 * @brief AAD layout (RFC 5246 sec 6.2.3.3).
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_tls_aad_seq_bytes   = 8U,  /**< 64-bit sequence number. */
  k_ra_tls_aad_total_bytes = 13U, /**< seq(8) + type(1) + ver(2) + len(2). */
  k_ra_tls_iv_pad_bytes    = 16U, /**< ra_sce IV staging area; pad GCM 12->16. */
} ra_tls_priv_aad_t;

/**
 * @enum ra_tls_priv_hs_offsets_t
 * @brief Byte offsets inside a 4-byte handshake header.
 *
 * @details
 * RFC 5246 sec 7.4: ``HandshakeType (1) || uint24 length``.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra_tls_hs_off_type   = 0U, /**< HandshakeType byte. */
  k_ra_tls_hs_off_len_hi = 1U, /**< uint24 length, high byte. */
  k_ra_tls_hs_off_len_md = 2U, /**< uint24 length, mid byte. */
  k_ra_tls_hs_off_len_lo = 3U, /**< uint24 length, low byte. */
} ra_tls_priv_hs_offsets_t;

/**
 * @brief Pack a 5-byte TLS record header into ``hdr_out``.
 *
 * @param[out] hdr_out 5-byte buffer; must be non-NULL.
 * @param[in]  ct      ContentType.
 * @param[in]  len     Body length (post-encryption).
 *
 * @return ``ra_err_t``.
 * @retval k_ra_ok Header packed.
 * @retval k_ra_err_null_ptr ``hdr_out`` was NULL.
 *
 * @pre ``hdr_out`` is non-NULL.
 * @post ``hdr_out[0..4]`` holds the packed header.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_tls_record_pack_header(uint8_t* hdr_out, ra_tls_content_type_t ct, uint16_t len);

/**
 * @brief Build the 13-byte AEAD additional_data block.
 *
 * @param[out] aad_out       13-byte buffer; non-NULL.
 * @param[in]  seq           Sequence number.
 * @param[in]  ct            ContentType.
 * @param[in]  plaintext_len Plaintext length.
 *
 * @pre ``aad_out`` is non-NULL.
 * @post ``aad_out[0..12]`` holds the AAD.
 *
 * @since 0.1.0
 */
void ra_tls_aad_build(uint8_t*              aad_out,
                      uint64_t              seq,
                      ra_tls_content_type_t ct,
                      uint16_t              plaintext_len);

/**
 * @brief Build the 12-byte AES-GCM nonce (salt || nonce_explicit).
 *
 * @param[out] nonce_out  12-byte buffer; non-NULL.
 * @param[in]  implicit_iv 4-byte salt; non-NULL.
 * @param[in]  seq        Sequence number used as the explicit half.
 *
 * @pre Both pointers non-NULL.
 * @post ``nonce_out[0..11]`` holds the GCM nonce.
 *
 * @since 0.1.0
 */
void ra_tls_gcm_nonce_build(uint8_t* nonce_out, const uint8_t* implicit_iv, uint64_t seq);

/**
 * @brief Encrypt a TLS 1.2 record (AES-128-GCM).
 *
 * @param[in,out] ctx            Established client context.
 * @param[in]     ct             ContentType.
 * @param[in]     plaintext      Source bytes.
 * @param[in]     plaintext_len  Source length.
 * @param[out]    record_out     Destination buffer.
 * @param[in]     record_cap     Destination capacity.
 * @param[out]    record_len_out Bytes written.
 *
 * @return ``ra_err_t``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tls_record_encrypt(ra_tls_client_t*      ctx,
                                             ra_tls_content_type_t ct,
                                             const uint8_t*        plaintext,
                                             uint32_t              plaintext_len,
                                             uint8_t*              record_out,
                                             uint32_t              record_cap,
                                             uint32_t*             record_len_out);

/**
 * @brief Decrypt a TLS 1.2 record (AES-128-GCM).
 *
 * @param[in,out] ctx               Established client context.
 * @param[in]     record            Whole record bytes (header included).
 * @param[in]     record_len        Total record length.
 * @param[out]    ct_out            ContentType extracted from the header.
 * @param[out]    plaintext_out     Destination buffer.
 * @param[in]     plaintext_cap     Destination capacity.
 * @param[out]    plaintext_len_out Bytes written.
 *
 * @return ``ra_err_t``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_tls_record_decrypt(ra_tls_client_t*       ctx,
                                             const uint8_t*         record,
                                             uint32_t               record_len,
                                             ra_tls_content_type_t* ct_out,
                                             uint8_t*               plaintext_out,
                                             uint32_t               plaintext_cap,
                                             uint32_t*              plaintext_len_out);

/**
 * @brief Absorb handshake bytes into the running transcript hash.
 *
 * @details
 * Updates ``ctx->transcript_len`` by ``len``. The actual hash is
 * computed lazily inside ``ra_tls_transcript_snapshot`` to avoid
 * keeping a streaming SHA-256 state alive concurrently with other
 * uses of the SCE engine.
 *
 * @param[in,out] ctx Initialised context.
 * @param[in]     buf Bytes to absorb.
 * @param[in]     len Length.
 *
 * @return ``ra_err_t``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_tls_transcript_update(ra_tls_client_t* ctx, const uint8_t* buf, uint32_t len);
