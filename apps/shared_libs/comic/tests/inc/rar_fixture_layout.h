/**
 * @file rar_fixture_layout.h
 * @brief Layout constants and memory budgets for the synthetic RAR fixtures.
 * @details Centralizes the public-format offsets, flags, and bounded fixture
 *          capacities shared by the RAR4 and RAR5 archive builders.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

/** @brief RAR main-archive-header layout used by the fixture builder. */
typedef enum : uint8_t {
  k_tr_off_high_pos_av = 7U, /**< HighPosAV + PosAV field offset.   */
  k_tr_len_high_pos_av = 6U, /**< HighPosAV + PosAV combined width. */
  k_tr_len_signature   = 5U, /**< RAR4 signature length, bytes.     */
} tr_layout_t;

/**
 * @enum t_rar4_off_t
 * @brief Byte offsets of the RAR4 block-header fields, which are fixed by the
 *        format, so `tr4_file()` reads as the on-disk record it writes.
 */
typedef enum : uint8_t {
  k_t_r4_off_head_size = 5U,  /**< HEAD_SIZE, 16-bit.                  */
  k_t_r4_off_pack_size = 7U,  /**< PACK_SIZE, 32-bit.                  */
  k_t_r4_off_unp_size  = 11U, /**< UNP_SIZE, 32-bit.                   */
  k_t_r4_off_host_os   = 15U, /**< HOST_OS, 8-bit.                     */
  k_t_r4_off_ftime     = 20U, /**< FTIME, 32-bit MS-DOS timestamp.     */
  k_t_r4_off_unp_ver   = 24U, /**< UNP_VER, 8-bit.                     */
  k_t_r4_off_method    = 25U, /**< METHOD '0': stored, not compressed. */
  k_t_r4_off_name_size = 26U, /**< NAME_SIZE, 16-bit.                  */
  k_t_r4_off_attr      = 28U, /**< ATTR, 32-bit.                       */
} t_rar4_off_t;

/** @brief RAR4 block types and field values the fixtures declare. */
typedef enum : uint16_t {
  k_t_r4_type_main    = 0x73U,   /**< Block type: main archive header.       */
  k_t_r4_type_file    = 0x74U,   /**< Block type: file header.               */
  k_t_r4_type_end     = 0x7BU,   /**< Block type: end-of-archive marker.     */
  k_t_r4_flag_long    = 0x8000U, /**< LONG_BLOCK: an ADD_SIZE field follows. */
  k_t_r4_unp_ver_20   = 20U,     /**< UNP_VER 20: RAR 2.0 stream format.     */
  k_t_r4_method_store = 0x30U,   /**< METHOD '0': stored, not compressed.    */
  k_t_r4_main_hdr_len = 13U,     /**< Total main-header length, bytes.       */
  k_t_r4_end_hdr_len  = 7U,      /**< Total end-header length, bytes.        */
} t_rar4_field_t;

/** @brief Base-128 vint: 7 payload bits per byte, bit 7 set on all but the last. */
typedef enum : uint8_t {
  k_t_vint_payload_mask = 0x7FU, /**< Payload bits carried by one vint byte.  */
  k_t_vint_shift        = 7U,    /**< Bits consumed per vint byte.            */
  k_t_vint_more_flag    = 0x80U, /**< Continuation bit: another byte follows. */
  k_t_le32_hi_shift     = 24U,   /**< Top-byte shift of a 32-bit LE field.    */
} t_vint_t;

/** @brief RAR5 header types and the offsets the hostile arms poke. */
typedef enum : uint8_t {
  k_t_r5_type_end      = 5U, /**< RAR5 header type 5: end of archive.             */
  k_t_r5_off_hdr_flags = 5U, /**< Header-flags offset inside the crafted wrapper. */
} t_rar5_t;

/** @brief Geometry and sizes of the synthetic archives and their members. */
typedef enum : uint16_t {
  k_t_img2_h        = 5U,   /**< Height of img02.png.                             */
  k_t_img4_w        = 5U,   /**< Width of sub/img04.png.                          */
  k_t_img3_h        = 7U,   /**< Height of img03.png.                             */
  k_t_filler_stride = 31U,  /**< Filler multiplier; co-prime with the block size. */
  k_t_short_arc_len = 5U,   /**< Backing bytes for the short-signature arm.       */
  k_t_backing_len   = 100U, /**< Backing behind a reader that declares more.      */
  k_t_member_len    = 20U,  /**< Declared size of the overrun member.             */
  k_t_off_overrun   = 990U, /**< Data offset past the declared archive size.      */
  k_t_off_shortread = 50U,  /**< Offset inside the archive but past the backing.  */
  k_t_extract_buf   = 256U, /**< Extraction destination buffer, bytes.            */
} t_rar_fixture_t;

/** @brief Fixture geometry and buffer budgets. */
typedef enum : uint32_t {
  k_tr_img_count = 4U,           /**< PNG page images in the RAR5 fixture. */
  k_tr_page_cap  = 16U,          /**< Page-index capacity.                 */
  k_tr_name_cap  = 1024U,        /**< Name-arena capacity, bytes.          */
  k_tr_filler    = 100U * 1024U, /**< Big non-image STORE member.          */
  k_tr_arc_cap   = 256U * 1024U, /**< Archive build buffer.                */
  k_tr_name_buf  = 64U,          /**< Per-query name buffer.               */
  k_tr_body_max  = 256U,         /**< RAR5 header-body scratch.            */
} tr_dim_t;
