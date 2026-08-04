/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_sdhi_regs.h
 * @brief SDHI (SD/MMC host interface) register layout for the Renesas RA8D2
 * @ingroup grp_hal_memory
 *
 * @details
 * Two SDHI instances at `0x40252000` (SDHI0) and `0x40252400`
 * (SDHI1). The full register window covers command issue, response,
 * status, interrupt mask, clock control, transfer-data length,
 * access option, error status, FIFO buffer, SDIO mode, DMA enable,
 * software reset, SD interface mode, and endianness swap.
 *
 * Layout cross-verified against FSP `R_SDHI0_Type` (sibling RA SoC
 * BSP header `R7FA6M3AH.h` lines 14964-15442, structure size
 * `0x1E4` bytes) -- the same hardware block ships unchanged across
 * RA6M3, RA6M5, and RA8 series. Register access widths and bit
 * positions are identical; only the base address differs on RA8D2.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

typedef enum : uintptr_t {
  k_ra8_sdhi0_base_addr = 0x40252000UL, /**< RA8 sdhi0 base address. */
  k_ra8_sdhi1_base_addr = 0x40252400UL, /**< RA8 sdhi1 base address. */
  k_ra8_sdhi_stride     = 0x400UL,      /**< RA8 SDHI stride.        */
} ra8_sdhi_addr_t;

typedef enum : uint8_t {
  k_ra8_sdhi_instance_count = 2U, /**< RA8 SDHI instance count. */
} ra8_sdhi_limits_t;

/**
 * @brief SD_INFO2 / SD_STOP bit-masks used by the block-transfer path.
 *
 * @details
 * These bits are documented in HUM Ch 47.2.15 / 47.2.13. FSP's
 * ``r_sdhi.c`` exposes them as private macros
 * ``SDHI_PRV_SDHI_INFO2_CARD_BRE`` / ``CARD_BWE`` and
 * ``SDHI_PRV_SD_STOP_SD_SECCNT_ENABLE``. We mirror them here so the
 * polled FIFO drain in :c:func:`ra8_sdhi_read_block` /
 * :c:func:`ra8_sdhi_write_block` does not carry magic numbers.
 *
 * - ``BRE`` (Buffer Read Enable, SD_INFO2 bit 8) - asserted when
 *   SD_BUF0 has at least one block worth of data ready to drain.
 * - ``BWE`` (Buffer Write Enable, SD_INFO2 bit 9) - asserted when
 *   the SD_BUF0 FIFO has space for one block worth of writes.
 * - ``BREM`` / ``BWEM`` mask bits live at the same positions in
 *   SD_INFO2_MASK and gate the two flags above when DMA is in use.
 * - ``SD_STOP_SECCNT_ENABLE`` (SD_STOP bit 8) tells the IP to stop
 *   the data phase after SD_SECCNT blocks have transferred.
 */
typedef enum : uint32_t {
  k_ra8_sdhi_info2_bre_mask  = 0x00000100UL, /**< SD_INFO2.BRE bit 8        */
  k_ra8_sdhi_info2_bwe_mask  = 0x00000200UL, /**< SD_INFO2.BWE bit 9        */
  k_ra8_sdhi_info2_brem_bwem = 0x00000300UL, /**< SD_INFO2_MASK.{BREM,BWEM} */
  k_ra8_sdhi_stop_seccnt_en  = 0x00000100UL, /**< SD_STOP.SEC bit 8         */
  k_ra8_sdhi_dmaen_set       = 0x00000002UL, /**< SD_DMAEN.DMAEN bit 1      */
} ra8_sdhi_xfer_bits_t;

/**
 * @brief SD command numbers used by the block-transfer API.
 *
 * @details
 * Mirrors FSP r_sdhi_private.h ``SDHI_PRV_CMD_*``. The hardware
 * SD_CMD register accepts the bare command index (0..63) for data
 * transfer commands; the SDHI block infers the response and data
 * direction from the command index alone -- no additional
 * data-phase or response-type bits are required for the four
 * commands used by ::ra8_sdhi_read_block and ::ra8_sdhi_write_block.
 */
typedef enum : uint8_t {
  k_ra8_sdhi_cmd_set_bus_width      = 6U,  /**< ACMD6 SET_BUS_WIDTH (SD)   */
  k_ra8_sdhi_cmd_emmc_switch        = 6U,  /**< CMD6 SWITCH (eMMC EXT_CSD) */
  k_ra8_sdhi_cmd_stop_transmission  = 12U, /**< CMD12 STOP_TRANSMISSION    */
  k_ra8_sdhi_cmd_read_single_block  = 17U, /**< CMD17 READ_SINGLE_BLOCK    */
  k_ra8_sdhi_cmd_read_multi_block   = 18U, /**< CMD18 READ_MULTIPLE_BLOCK  */
  k_ra8_sdhi_cmd_write_single_block = 24U, /**< CMD24 WRITE_SINGLE_BLOCK   */
  k_ra8_sdhi_cmd_write_multi_block  = 25U, /**< CMD25 WRITE_MULTIPLE_BLOCK */
  k_ra8_sdhi_cmd_app_cmd            = 55U, /**< CMD55 APP_CMD prefix       */
} ra8_sdhi_cmd_id_t;

/**
 * @brief Block-transfer geometry constants.
 *
 * @details
 * SD cards use a fixed 512-byte block size for SDHC/SDXC. The SDHI
 * FIFO is 4 bytes wide (SD_BUF0 is a 32-bit register), so each block
 * requires 128 four-byte transfers.
 */
typedef enum : uint32_t {
  k_ra8_sdhi_block_bytes     = 512UL, /**< SD block size in bytes             */
  k_ra8_sdhi_fifo_word_bytes = 4UL,   /**< SD_BUF0 FIFO word width            */
  k_ra8_sdhi_words_per_block = 128UL, /**< 512 / 4 = 128 word fills per block */
} ra8_sdhi_geometry_t;

/**
 * @brief SD_OPTION bus-width selectors and timeout-field defaults.
 *
 * @details
 * SD_OPTION (HUM Ch 47.2.16 "SD_OPTION : SD Card Access Control Option
 * Register" p 3139-3140) packs two bus-width bits, a timeout mask, and
 * two timeout counters:
 *
 * - ``WIDTH``  (bit 15) and ``WIDTH8`` (bit 13) jointly select the bus
 *   width per the HUM truth table:
 *
 *     | WIDTH | WIDTH8 | Bus width |
 *     |-------|--------|-----------|
 *     |   1   |   0    |  1-bit    |
 *     |   0   |   0    |  4-bit    |
 *     |   0   |   1    |  8-bit    |
 *     |   1   |   1    |  1-bit    |
 *
 * - Bit 14 is reserved but documented "read as 1, the write value
 *   should be 1", so every value written here keeps it set.
 * - ``TOP[3:0]`` (bits 7..4) is the data-timeout counter; 0xE selects
 *   the largest non-prohibited timeout.
 * - ``CTOP[3:0]`` (bits 3..0) is the card-detect counter; left at 0.
 *
 * The three ``k_ra8_sdhi_option_bus_*`` values are the only ones the
 * driver writes whole; ::k_ra8_sdhi_option_width_mask is the
 * read-modify-write clear mask that isolates WIDTH+WIDTH8 so the
 * timeout fields survive a bus-width change.
 *
 * @note The legacy literal 0x40E0 (WIDTH=0, WIDTH8=0) is the *4-bit*
 *       encoding, not 1-bit as earlier scaffold comments claimed; the
 *       genuine 1-bit safe default is ::k_ra8_sdhi_option_bus_1bit
 *       (0xC0E0).
 */
typedef enum : uint32_t {
  k_ra8_sdhi_option_top_default = 0x000000E0UL, /**< TOP[3:0]=0xE, CTOP=0 (bits 7..0) */
  k_ra8_sdhi_option_rsvd_bit14  = 0x00004000UL, /**< bit 14 reserved, must write 1    */
  k_ra8_sdhi_option_width8_bit  = 0x00002000UL, /**< WIDTH8 (bit 13): 1 -> 8-bit      */
  k_ra8_sdhi_option_width_bit   = 0x00008000UL, /**< WIDTH  (bit 15): 1 -> narrow bus */
  /** Combined WIDTH+WIDTH8 clear mask for read-modify-write. */
  k_ra8_sdhi_option_width_mask = k_ra8_sdhi_option_width_bit | k_ra8_sdhi_option_width8_bit,
  /** 1-bit safe default: WIDTH=1, WIDTH8=0 (0xC0E0). */
  k_ra8_sdhi_option_bus_1bit =
    k_ra8_sdhi_option_width_bit | k_ra8_sdhi_option_rsvd_bit14 | k_ra8_sdhi_option_top_default,
  /** 4-bit: WIDTH=0, WIDTH8=0 (0x40E0). */
  k_ra8_sdhi_option_bus_4bit = k_ra8_sdhi_option_rsvd_bit14 | k_ra8_sdhi_option_top_default,
  /** 8-bit: WIDTH=0, WIDTH8=1 (0x60E0). */
  k_ra8_sdhi_option_bus_8bit =
    k_ra8_sdhi_option_width8_bit | k_ra8_sdhi_option_rsvd_bit14 | k_ra8_sdhi_option_top_default,
} ra8_sdhi_option_bits_t;

/**
 * @brief ACMD6 (SET_BUS_WIDTH) argument + R1 status-bit masks.
 *
 * @details
 * The ACMD6 application command negotiates the card-side data-bus
 * width. Its 32-bit argument carries the width in bits [1:0]
 * (``0b00`` = 1-bit, ``0b10`` = 4-bit); the SDHI replays the R1
 * card-status response in SD_RSP10 (HUM Ch 47.2.5 "SD_RSP10..76 :
 * SD Response Registers" p 3132). These constants name the bits the
 * ACMD6 negotiation inspects so the driver carries no magic numbers:
 *
 * - ``APP_CMD`` (R1 bit 5) is set in the CMD55 response to confirm the
 *   card accepted the application-command prefix.
 * - ``ILLEGAL_COMMAND`` (R1 bit 22) is a representative R1 error bit.
 * - ``R1_ERROR_MASK`` covers every R1 error/violation bit
 *   (31..19, 16, 3); a non-zero AND means the card rejected the
 *   command and the host must stay at 1-bit.
 */
typedef enum : uint32_t {
  k_ra8_sdhi_acmd6_arg_1bit     = 0x00000000UL, /**< ACMD6 arg: 1-bit data bus      */
  k_ra8_sdhi_acmd6_arg_4bit     = 0x00000002UL, /**< ACMD6 arg: 4-bit data bus      */
  k_ra8_sdhi_r1_app_cmd_mask    = 0x00000020UL, /**< R1 bit 5  APP_CMD acknowledged */
  k_ra8_sdhi_r1_illegal_command = 0x00400000UL, /**< R1 bit 22 ILLEGAL_COMMAND      */
  k_ra8_sdhi_r1_error_mask      = 0xFDF90008UL, /**< R1 error/violation bit-mask    */
} ra8_sdhi_acmd6_bits_t;

/**
 * @brief eMMC CMD6 (SWITCH) argument fields for the 8-bit bus-width switch.
 *
 * @details
 * SD cards top out at a 4-bit bus (negotiated with ACMD6); the 8-bit
 * data bus is an eMMC-only feature reached through the JEDEC CMD6
 * SWITCH command (JESD84-B51 Section 6.6.4) rather than an application
 * command -- there is no CMD55 prefix. The 32-bit argument packs four
 * fields:
 *
 *     | Bits    | Field  | Value here                              |
 *     |---------|--------|-----------------------------------------|
 *     | [25:24] | Access | 0b11 Write Byte                         |
 *     | [23:16] | Index  | 183 (0xB7) = EXT_CSD BUS_WIDTH byte      |
 *     | [15:8]  | Value  | 2 = 8-bit SDR (1 = 4-bit, 0 = 1-bit)    |
 *     | [2:0]   | CmdSet | 0                                       |
 *
 * The card replays an R1b card-status response in SD_RSP10; the same
 * ::k_ra8_sdhi_r1_error_mask that guards ACMD6 rejects a switch that
 * the card refused. ::k_ra8_sdhi_cmd6_arg_8bit is the whole argument
 * composed from the three named field selectors so the driver carries
 * no inline magic number.
 */
typedef enum : uint32_t {
  k_ra8_sdhi_switch_access_write_byte = 0x03000000UL, /**< Access[25:24] = 0b11 Write Byte */
  k_ra8_sdhi_switch_index_bus_width   = 0x00B70000UL, /**< Index[23:16] = 183 BUS_WIDTH    */
  k_ra8_sdhi_switch_value_8bit        = 0x00000200UL, /**< Value[15:8] = 2 -> 8-bit SDR    */
  /** Whole CMD6 SWITCH argument: BUS_WIDTH <- 8-bit (0x03B70200). */
  k_ra8_sdhi_cmd6_arg_8bit = k_ra8_sdhi_switch_access_write_byte |
                             k_ra8_sdhi_switch_index_bus_width | k_ra8_sdhi_switch_value_8bit,
} ra8_sdhi_cmd6_switch_bits_t;

/**
 * @brief Bit-shift amounts for SD command-argument packing.
 *
 * @details
 * CMD55 (APP_CMD) carries the card's relative address (RCA) in the
 * upper half of SD_ARG, i.e. ``SD_ARG[31:16] = RCA``; the lower 16
 * bits are stuff bits. Naming the shift keeps the packing free of
 * inline magic numbers.
 */
typedef enum : uint8_t {
  k_ra8_sdhi_rca_arg_shift = 16U, /**< RCA occupies SD_ARG[31:16]. */
} ra8_sdhi_arg_shift_t;

/**
 * @struct r_sdhi_regs_t
 * @brief SDHI register window.
 *
 * @details
 * Layout cross-verified against FSP `R_SDHI0_Type` (BSP header
 * `R7FA6M3AH.h` lines 14964-15442). All registers are 32-bit. The
 * total structure size is `0x1E4` bytes -- the trailing reserved
 * blocks RESERVED3..RESERVED6 are necessary so SD_DMAEN, SOFT_RST,
 * SDIF_MODE, and EXT_SWAP land at their architectural offsets
 * (`0x1B0`, `0x1C0`, `0x1CC`, `0x1E0`). Earlier scaffold revisions
 * stopped at SDIO_MODE which silently rerouted SOFT_RST writes onto
 * SDIO_INFO1 -- caught by FSP cross-check.
 *
 * The driver currently touches SD_CMD / SD_ARG / SD_INFO1 /
 * SD_INFO2 / SD_CLK_CTRL / SD_OPTION / SOFT_RST / SDIO_MODE /
 * SD_DMAEN / SDIF_MODE / EXT_SWAP. Response, transfer-length, and
 * FIFO registers are documented here for future block-transfer
 * work.
 */
typedef struct {
  volatile uint32_t SD_CMD;          /**< +0x00 Command Type Register. */
  volatile uint32_t _r0;             /**< Reserved.                    */
  volatile uint32_t SD_ARG;          /**< +0x08 Argument (32-bit).     */
  volatile uint32_t SD_ARG1;         /**< +0x0C Argument 1 (16-bit).   */
  volatile uint32_t SD_STOP;         /**< +0x10 Stop.                  */
  volatile uint32_t SD_SECCNT;       /**< +0x14 Block Count.           */
  volatile uint32_t SD_RSP10;        /**< +0x18 Response 10.           */
  volatile uint32_t SD_RSP1;         /**< +0x1C Response 1.            */
  volatile uint32_t SD_RSP32;        /**< +0x20 Response 32.           */
  volatile uint32_t SD_RSP3;         /**< +0x24 Response 3.            */
  volatile uint32_t SD_RSP54;        /**< +0x28 Response 54.           */
  volatile uint32_t SD_RSP5;         /**< +0x2C Response 5.            */
  volatile uint32_t SD_RSP76;        /**< +0x30 Response 76.           */
  volatile uint32_t SD_RSP7;         /**< +0x34 Response 7.            */
  volatile uint32_t SD_INFO1;        /**< +0x38 Interrupt Flag 1.      */
  volatile uint32_t SD_INFO2;        /**< +0x3C Interrupt Flag 2.      */
  volatile uint32_t SD_INFO1_MASK;   /**< +0x40 Interrupt Mask 1.      */
  volatile uint32_t SD_INFO2_MASK;   /**< +0x44 Interrupt Mask 2.      */
  volatile uint32_t SD_CLK_CTRL;     /**< +0x48 Clock Control.         */
  volatile uint32_t SD_SIZE;         /**< +0x4C Transfer Data Length.  */
  volatile uint32_t SD_OPTION;       /**< +0x50 Access Control Option. */
  volatile uint32_t _r1;             /**< +0x54 Reserved.              */
  volatile uint32_t SD_ERR_STS1;     /**< +0x58 Error Status 1.        */
  volatile uint32_t SD_ERR_STS2;     /**< +0x5C Error Status 2.        */
  volatile uint32_t SD_BUF0;         /**< +0x60 Buffer Register.       */
  volatile uint32_t _r2;             /**< +0x64 Reserved.              */
  volatile uint32_t SDIO_MODE;       /**< +0x68 SDIO Mode Control.     */
  volatile uint32_t SDIO_INFO1;      /**< +0x6C SDIO Interrupt Flag.   */
  volatile uint32_t SDIO_INFO1_MASK; /**< +0x70 SDIO Interrupt Mask.   */
  /* NOLINTNEXTLINE(readability-magic-numbers) -- 79 = (0x1B0 - 0x74) / 4, structural padding. */
  volatile uint32_t _r3[79];   /**< +0x74..+0x1AC Reserved.     */
  volatile uint32_t SD_DMAEN;  /**< +0x1B0 DMA Mode Enable.     */
  volatile uint32_t _r4[3];    /**< +0x1B4..+0x1BC Reserved.    */
  volatile uint32_t SOFT_RST;  /**< +0x1C0 Software Reset.      */
  volatile uint32_t _r5[2];    /**< +0x1C4..+0x1C8 Reserved.    */
  volatile uint32_t SDIF_MODE; /**< +0x1CC SD Interface Mode.   */
  volatile uint32_t _r6[4];    /**< +0x1D0..+0x1DC Reserved.    */
  volatile uint32_t EXT_SWAP;  /**< +0x1E0 Endian Swap Control. */
} r_sdhi_regs_t;

/** @brief Get pointer to SDHI instance N (0 or 1). */
RA8_HW_REGISTER_ACCESS
static inline volatile r_sdhi_regs_t* ra8_sdhi(uint8_t instance)
{
  if (instance >= k_ra8_sdhi_instance_count) {
    return nullptr;
  }
  return (volatile r_sdhi_regs_t*)(k_ra8_sdhi0_base_addr +
                                   ((uintptr_t)instance * k_ra8_sdhi_stride));
}

#ifdef __cplusplus
}
#endif
