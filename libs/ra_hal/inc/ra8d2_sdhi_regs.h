/**
 * @file ra8d2_sdhi_regs.h
 * @brief SDHI (SD/MMC host interface) register layout for the Renesas RA8D2
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
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_sdhi0_base_addr = 0x40252000UL,
  k_ra_sdhi1_base_addr = 0x40252400UL,
  k_ra_sdhi_stride     = 0x400UL,
} ra_sdhi_addr_t;

typedef enum : uint8_t {
  k_ra_sdhi_instance_count = 2U,
} ra_sdhi_limits_t;

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
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t SD_CMD; /**< +0x00 Command Type Register. */
  volatile uint32_t _r0;
  volatile uint32_t SD_ARG;          /**< +0x08 Argument (32-bit). */
  volatile uint32_t SD_ARG1;         /**< +0x0C Argument 1 (16-bit). */
  volatile uint32_t SD_STOP;         /**< +0x10 Stop. */
  volatile uint32_t SD_SECCNT;       /**< +0x14 Block Count. */
  volatile uint32_t SD_RSP10;        /**< +0x18 Response 10. */
  volatile uint32_t SD_RSP1;         /**< +0x1C Response 1. */
  volatile uint32_t SD_RSP32;        /**< +0x20 Response 32. */
  volatile uint32_t SD_RSP3;         /**< +0x24 Response 3. */
  volatile uint32_t SD_RSP54;        /**< +0x28 Response 54. */
  volatile uint32_t SD_RSP5;         /**< +0x2C Response 5. */
  volatile uint32_t SD_RSP76;        /**< +0x30 Response 76. */
  volatile uint32_t SD_RSP7;         /**< +0x34 Response 7. */
  volatile uint32_t SD_INFO1;        /**< +0x38 Interrupt Flag 1. */
  volatile uint32_t SD_INFO2;        /**< +0x3C Interrupt Flag 2. */
  volatile uint32_t SD_INFO1_MASK;   /**< +0x40 Interrupt Mask 1. */
  volatile uint32_t SD_INFO2_MASK;   /**< +0x44 Interrupt Mask 2. */
  volatile uint32_t SD_CLK_CTRL;     /**< +0x48 Clock Control. */
  volatile uint32_t SD_SIZE;         /**< +0x4C Transfer Data Length. */
  volatile uint32_t SD_OPTION;       /**< +0x50 Access Control Option. */
  volatile uint32_t _r1;             /**< +0x54 Reserved. */
  volatile uint32_t SD_ERR_STS1;     /**< +0x58 Error Status 1. */
  volatile uint32_t SD_ERR_STS2;     /**< +0x5C Error Status 2. */
  volatile uint32_t SD_BUF0;         /**< +0x60 Buffer Register. */
  volatile uint32_t _r2;             /**< +0x64 Reserved. */
  volatile uint32_t SDIO_MODE;       /**< +0x68 SDIO Mode Control. */
  volatile uint32_t SDIO_INFO1;      /**< +0x6C SDIO Interrupt Flag. */
  volatile uint32_t SDIO_INFO1_MASK; /**< +0x70 SDIO Interrupt Mask. */
  /* NOLINTNEXTLINE(readability-magic-numbers) -- 79 = (0x1B0 - 0x74) / 4, structural padding. */
  volatile uint32_t _r3[79];   /**< +0x74..+0x1AC Reserved. */
  volatile uint32_t SD_DMAEN;  /**< +0x1B0 DMA Mode Enable. */
  volatile uint32_t _r4[3];    /**< +0x1B4..+0x1BC Reserved. */
  volatile uint32_t SOFT_RST;  /**< +0x1C0 Software Reset. */
  volatile uint32_t _r5[2];    /**< +0x1C4..+0x1C8 Reserved. */
  volatile uint32_t SDIF_MODE; /**< +0x1CC SD Interface Mode. */
  volatile uint32_t _r6[4];    /**< +0x1D0..+0x1DC Reserved. */
  volatile uint32_t EXT_SWAP;  /**< +0x1E0 Endian Swap Control. */
} r_sdhi_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to SDHI instance N (0 or 1). */
static inline volatile r_sdhi_regs_t* ra_sdhi(uint8_t instance)
{
  if (instance >= k_ra_sdhi_instance_count) {
    return nullptr;
  }
  return (volatile r_sdhi_regs_t*)(k_ra_sdhi0_base_addr + ((uintptr_t)instance * k_ra_sdhi_stride));
}

#ifdef __cplusplus
}
#endif
