/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_dotf_regs.h
 * @brief Decryption On The Fly (DOTF) register layout for the Renesas RA8D2
 * @ingroup grp_hal_crypto
 *
 * @details
 * The RA8D2 has two DOTF channels which transparently decrypt read
 * data on the AXI side of the matching xSPI / OSPI controller using
 * an AES core in CTR mode. DOTF0 covers reads from XSPI0 (memory
 * mapped at ``0x8000_0000..0x9FFF_FFFF``) and DOTF1 covers XSPI1
 * (``0x7000_0000..0x7FFF_FFFF``). Each channel exposes a tiny
 * register window:
 *
 * | Offset | Name        | Width | Purpose                                     |
 * |-------:|-------------|------:|---------------------------------------------|
 * | 0x000  | CONVAREAST  | 32    | Conversion area start address (4 KB align)  |
 * | 0x004  | CONVAREAD   | 32    | Conversion area end address (4 KB align)    |
 * | 0x080  | REG00       | 32    | AES core / side-channel control bits        |
 * | 0x08C  | REG03       | 32    | AES IV / counter staging register           |
 *
 * ## Channel layout
 *
 * Two channels at base ``0x4026_8800`` and ``0x4026_8900`` (S alias)
 * with the matching ``0x5026_88xx`` non-secure aliases. Spacing is
 * ``0x100`` bytes per channel. Both channels share MSTPCRB bits with
 * their xSPI partner -- see ``ra8_mstp.h`` (``k_ra8_mstp_ospi0`` releases
 * ``MSTPB16`` which clocks both XSPI0 *and* DOTF0). HUM Ch 45.6.1
 * p 3050 explicitly says "DOTF operation can be disabled or enabled
 * using Module Stop Control Register B (MSTPCRB). The DOTF module is
 * initially stopped after reset."
 *
 * Layout source: cross-referenced against the Renesas FSP CMSIS
 * header ``R7KA8D2KF_core0.h`` lines 31125..31188 (R_DOTF_Type, total
 * size 0x90) and the HUM register descriptions on pages 3049..3050.
 *
 * ## CONVAREAST / CONVAREAD encoding
 *
 * The bottom 12 bits of the 32-bit address registers are reserved.
 * The hardware multiplies the stored value by ``0x1000`` (4 KB) to
 * derive the actual byte address, so any region must be 4 KB aligned
 * and is sized at 4 KB granularity. Hardware reset value of
 * CONVAREAD's reserved field is ``0xFFF`` (HUM 45.3.2 p 3049 "These
 * bits are read as 1.").
 *
 * ## REG00 control bits (raw FSP names; HUM does not symbolically
 *   name them in Ch 45)
 *
 * REG00 is the umbrella control register exposed via the DOTF APB
 * window. HUM Ch 45 only documents that REG00 is the "AES core /
 * side-channel control" word; the bit positions are taken from the
 * FSP CMSIS ``R_DOTF_REG00_b`` bit-field (R7KA8D2KF_core0.h lines
 * 31157..31176) and the FSP reference driver
 * (``r_ospi_b.c`` line 156 ``OSPI_B_PRV_DOTF_REG00_RESET_VALUE
 * = 0x22000000`` and line 1651 ``REG00 = ... | R_DOTF_REG00_B09_Msk``).
 *
 * The reset-plus-enable pattern that FSP writes is ``0x2200_0000 |
 * (1U << 9)``. The constant ``0x2200_0000`` decomposes as:
 *
 *  - bit 25:24 = ``10`` -- AES key size = 128-bit (default)
 *  - bit 29:28 = ``10`` -- AES mode = CTR (the only mode HUM 45.1
 *    p 3048 "Supports the following block cipher mode: Counter (CTR)
 *    mode following NIST SP800-38A" allows)
 *
 * Bit positions and helper masks for re-keying / rotating the key
 * size are exported here so the driver can write a different key
 * size (192 / 256 bit) without re-deriving the magic constant.
 *
 * ## REG03 (IV / counter stage)
 *
 * REG03 is a 32-bit FIFO-style register: writing four 32-bit words
 * in big-endian order loads the 128-bit IV. Per HUM 45.1 p 3048 the
 * counter is ``{IV[127:28], Address[31:4]}`` -- i.e. the bottom 28
 * bits of the IV are overwritten by the AXI address and only the
 * top 100 bits of the IV are programmable. The driver still accepts
 * a full 128-bit IV and forwards every word to REG03; the hardware
 * silently drops the low bits at AES counter generation time.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_dotf_addr_t
 * @brief DOTF channel base addresses on the peripheral bus.
 *
 * @details
 * Per HUM Ch 45.3.1 p 3049 the secure alias is at ``0x4026_8800 +
 * 0x0100 * n`` and the NS alias is at ``0x5026_8800 + 0x0100 * n``
 * for n = 0, 1. The driver only exposes the secure alias; NS reads
 * are governed by the OSPI TZF policy and are out of scope.
 */
typedef enum : uintptr_t {
  k_ra8_dotf0_base_addr    = 0x40268800UL, /**< DOTF channel 0 secure alias.     */
  k_ra8_dotf1_base_addr    = 0x40268900UL, /**< DOTF channel 1 secure alias.     */
  k_ra8_dotf0_ns_base_addr = 0x50268800UL, /**< DOTF channel 0 non-secure alias. */
  k_ra8_dotf1_ns_base_addr = 0x50268900UL, /**< DOTF channel 1 non-secure alias. */
  k_ra8_dotf_stride        = 0x100UL,      /**< Spacing between DOTF channels.   */
} ra8_dotf_addr_t;

/**
 * @enum ra8_dotf_xspi_window_t
 * @brief XSPI memory-mapped windows that DOTF channels decrypt over.
 *
 * @details
 * Per HUM Ch 45.3 p 3049 ("Image of decryption area setting") DOTF0
 * decrypts AXI reads inside the XSPI0 window
 * (``0x8000_0000..0x9FFF_FFFF``) and DOTF1 covers XSPI1
 * (``0x7000_0000..0x7FFF_FFFF``). Conversion-area start / end values
 * passed to ``ra8_dotf_set_region`` MUST land inside the matching
 * channel's window.
 */
typedef enum : uint32_t {
  k_ra8_dotf0_window_lo = 0x80000000UL, /**< First byte covered by DOTF0. */
  k_ra8_dotf0_window_hi = 0x9FFFFFFFUL, /**< Last byte covered by DOTF0.  */
  k_ra8_dotf1_window_lo = 0x70000000UL, /**< First byte covered by DOTF1. */
  k_ra8_dotf1_window_hi = 0x7FFFFFFFUL, /**< Last byte covered by DOTF1.  */
} ra8_dotf_xspi_window_t;

/**
 * @enum ra8_dotf_limits_t
 * @brief Channel count + alignment limits.
 */
typedef enum : uint8_t {
  k_ra8_dotf_channel_count       = 2U,  /**< DOTF0 + DOTF1.                             */
  k_ra8_dotf_align_shift         = 12U, /**< CONVAREAST/CONVAREAD value scales 1<<12.   */
  k_ra8_dotf_iv_word_count       = 4U,  /**< IV is 128-bit = 4 x 32-bit words.          */
  k_ra8_dotf_max_regions         = 4U,  /**< Max staged regions per channel.            */
  k_ra8_dotf_rsv_words_convaread = 30U, /**< +0x008..0x07C reserved gap (32-bit words). */
} ra8_dotf_limits_t;

/**
 * @enum ra8_dotf_align_t
 * @brief 4 KB granularity values for CONVAREAST / CONVAREAD.
 */
typedef enum : uint32_t {
  k_ra8_dotf_addr_granule  = 0x00001000UL, /**< 4 KB region granule.           */
  k_ra8_dotf_addr_mask     = 0xFFFFF000UL, /**< High 20 bits are the value.    */
  k_ra8_dotf_addr_low_mask = 0x00000FFFUL, /**< Low 12 bits read as 0/1 (HUM). */
} ra8_dotf_align_t;

/**
 * @enum ra8_dotf_reg00_bit_t
 * @brief REG00 control-bit positions.
 *
 * @details
 * Names mirror FSP ``R_DOTF_REG00_b`` (R7KA8D2KF_core0.h lines
 * 31157..31176). HUM Ch 45 does not give the bits symbolic names;
 * positions are kept verbatim against FSP so the driver can be
 * cross-checked against vendor reference code.
 */
typedef enum : uint8_t {
  k_ra8_dotf_reg00_bit09     = 9U,  /**< AES core enable trigger (FSP B09).  */
  k_ra8_dotf_reg00_bit_st    = 20U, /**< Self-test trigger (FSP B20).        */
  k_ra8_dotf_reg00_bit_sca0  = 16U, /**< Side-channel countermeasure enable. */
  k_ra8_dotf_reg00_bit_sca1  = 17U, /**< Side-channel countermeasure mode.   */
  k_ra8_dotf_reg00_bit_ksz0  = 24U, /**< Key-size select [25:24] LSB.        */
  k_ra8_dotf_reg00_bit_ksz1  = 25U, /**< Key-size select [25:24] MSB.        */
  k_ra8_dotf_reg00_bit_mode0 = 28U, /**< AES mode select [29:28] LSB.        */
  k_ra8_dotf_reg00_bit_mode1 = 29U, /**< AES mode select [29:28] MSB.        */
} ra8_dotf_reg00_bit_t;

/**
 * @enum ra8_dotf_reg00_mask_t
 * @brief REG00 bit-field masks (32-bit).
 *
 * @details
 * The reset-plus-enable pattern that FSP writes is
 * ``0x2200_0000 | (1U << 9)``. The constant ``0x2200_0000`` sets:
 *
 *  - bits[25:24] = 10 -> AES key size = 128 bit
 *  - bits[29:28] = 10 -> AES mode     = CTR
 *
 * The driver writes this exact pattern when a region is activated
 * and writes ``0`` to disable. ``ra8_dotf_set_key_size`` uses the
 * size masks below to swap to a 192-bit / 256-bit key.
 */
typedef enum : uint32_t {
  k_ra8_dotf_reg00_aes_enable    = 0x00000200UL, /**< Bit 9 -- enable AES core.      */
  k_ra8_dotf_reg00_self_test     = 0x00100000UL, /**< Bit 20 -- self-test trigger.   */
  k_ra8_dotf_reg00_sca_en        = 0x00010000UL, /**< Bit 16 -- side-channel enable. */
  k_ra8_dotf_reg00_sca_mode      = 0x00020000UL, /**< Bit 17 -- side-channel mode.   */
  k_ra8_dotf_reg00_key_size_mask = 0x03000000UL, /**< Bits 25:24 -- key size field.  */
  k_ra8_dotf_reg00_key_size_128  = 0x02000000UL, /**< Key size = 128-bit (FSP dflt). */
  k_ra8_dotf_reg00_key_size_192  = 0x01000000UL, /**< Key size = 192-bit.            */
  k_ra8_dotf_reg00_key_size_256  = 0x03000000UL, /**< Key size = 256-bit.            */
  k_ra8_dotf_reg00_mode_mask     = 0x30000000UL, /**< Bits 29:28 -- mode field.      */
  k_ra8_dotf_reg00_mode_ctr      = 0x20000000UL, /**< CTR mode (HUM 45.1 p 3048).    */
  k_ra8_dotf_reg00_default_field = 0x22000000UL, /**< Reset-pattern default value.   */
  k_ra8_dotf_reg00_disable_value = 0x00000000UL, /**< Disable -- AES bypassed.       */
  k_ra8_dotf_reg00_enable_value  = 0x22000200UL, /**< default | bit9 enable.         */
} ra8_dotf_reg00_mask_t;

/**
 * @struct ra8_dotf_regs_t
 * @brief Per-channel DOTF register window.
 *
 * @details
 * Mirrors FSP ``R_DOTF_Type`` (R7KA8D2KF_core0.h lines 31125..31188,
 * total size 0x90 / 144 bytes). FSP exposes 30 reserved 32-bit words
 * between ``CONVAREAD`` (0x04) and ``REG00`` (0x80), then 2 reserved
 * words between ``REG00`` (0x80) and ``REG03`` (0x8C). The same
 * padding is reflected here as ``_r0[]`` arrays sized in 32-bit words.
 *
 * Address-register fields drop the bottom 12 bits per HUM 45.3.1 /
 * 45.3.2 p 3049: software writes the full 32-bit address but only the
 * top 20 bits are honored.
 */
typedef struct {
  volatile uint32_t CONVAREAST; /**< +0x000 Conversion area start address. */
  volatile uint32_t CONVAREAD;  /**< +0x004 Conversion area end address.   */
  volatile uint32_t
    _r0[k_ra8_dotf_rsv_words_convaread]; /**< +0x008..0x07C Reserved (FSP RESERVED). */
  volatile uint32_t REG00;               /**< +0x080 AES enable / mode select.       */
  volatile uint32_t _r1[2];              /**< +0x084..0x088 Reserved.                */
  volatile uint32_t REG03;               /**< +0x08C AES IV staging window.          */
} ra8_dotf_regs_t;

/**
 * @brief Get pointer to DOTF channel ``channel``.
 *
 * @param[in] channel Channel index (0 or 1).
 * @return Volatile pointer to the channel register block, or
 *         ``nullptr`` if ``channel`` is out of range.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile ra8_dotf_regs_t* ra8_dotf_regs(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra8_dotf_channel_count) {
    return nullptr;
  }
  return (volatile ra8_dotf_regs_t*)(k_ra8_dotf0_base_addr +
                                     ((uintptr_t)channel * k_ra8_dotf_stride));
}

#ifdef __cplusplus
}
#endif
