/**
 * @file ra8_cnecc_regs.h
 * @brief CANFD ECC (CNECC) register layout for the RA8D2
 * @ingroup grp_hal_crypto
 *
 * @details
 * The RA8D2 ships two CANFD message-buffer SRAM (MBRAM) ECC blocks
 * (``ECCMB0`` and ``ECCMB1``). Each instance is a small 0x14-byte
 * window that detects single-bit ECC errors (correctable) and
 * dual-bit ECC errors (uncorrectable) on the 32-bit CANFD message
 * buffer SRAM that sits behind the corresponding CANFD channel
 * (``CAN0`` for ``ECCMB0``, ``CAN1`` for ``ECCMB1``).
 *
 * Per-instance registers (HUM Ch 42.2 p 2868-2873):
 *
 * | Offset | Name      | Width | Purpose                                  |
 * |-------:|-----------|------:|------------------------------------------|
 * |  0x00  | EC710CTL  | 32    | Enable / status / IRQ / clear            |
 * |  0x04  | EC710TMC  | 16    | Test-mode control (decoder fault inject) |
 * |  0x06  | (reserved)| 16    |                                          |
 * |  0x08  | (reserved)| 32    |                                          |
 * |  0x0C  | EC710TED  | 32    | Test substitute data (fault inject src)  |
 * |  0x10  | EC710EAD0 | 32    | Captured faulting RAM offset             |
 *
 * Layout cross-checked against FSP ``R_ECCMB0_Type`` in
 * ``R7KA8D2KF_core0.h`` (size = 20 bytes, base ``0x4036F200`` for
 * instance 0, ``0x4036F300`` for instance 1, NS aliases at
 * ``0x5036F200`` / ``0x5036F300``).
 *
 * The single shared interrupt vector for both instances is
 * ``CANn_MRAM_ERI`` (HUM 42.4 p 2875).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @enum ra8_cnecc_addr_t
 * @brief Memory-mapped base addresses for the two CNECC instances.
 *
 * @details
 * Secure aliases (``0x4036F200`` family). The non-secure aliases
 * (``0x5036F200`` family) are not mirrored here -- security-attribute
 * routing is the job of the SAR layer, not the per-driver register
 * header. HUM Ch 42.2.1 "Base address" line, p 2868.
 */
typedef enum : uintptr_t {
  k_ra8_cnecc0_base_addr = 0x4036F200UL, /**< ECCMB0 (CAN0 MRAM). */
  k_ra8_cnecc1_base_addr = 0x4036F300UL, /**< ECCMB1 (CAN1 MRAM). */
} ra8_cnecc_addr_t;

/**
 * @enum ra8_cnecc_limits_t
 * @brief Instance count and per-instance stride.
 */
typedef enum : uint16_t {
  k_ra8_cnecc_instance_count = 2U,     /**< ECCMB0 + ECCMB1.          */
  k_ra8_cnecc_stride         = 0x100U, /**< Bytes between instances.  */
  k_ra8_cnecc_ead_max        = 0x3FFU, /**< Largest valid ECEAD[9:0]. */
} ra8_cnecc_limits_t;

/**
 * @enum ra8_cnecc_ctl_bit_t
 * @brief EC710CTL bit positions.
 *
 * @details
 * Verified against HUM Ch 42.2.1 "EC710CTL" p 2868 and FSP
 * ``R_ECCMB0_EC710CTL_*_Pos`` defines in ``R7KA8D2KF_core0.h``.
 */
typedef enum : uint8_t {
  k_ra8_cnecc_bit_ecemf   = 0U,  /**< Present-cycle ECC error flag (R).        */
  k_ra8_cnecc_bit_ecer1f  = 1U,  /**< 1-bit error latched (R).                 */
  k_ra8_cnecc_bit_ecer2f  = 2U,  /**< 2-bit error latched (R).                 */
  k_ra8_cnecc_bit_ec1edic = 3U,  /**< 1-bit error IRQ enable (R/W).            */
  k_ra8_cnecc_bit_ec2edic = 4U,  /**< 2-bit error IRQ enable (R/W).            */
  k_ra8_cnecc_bit_ec1ecp  = 5U,  /**< 1-bit-error correction permission (R/W). */
  k_ra8_cnecc_bit_ecervf  = 6U,  /**< Error judgment enable (R/W, EMCA-gated). */
  k_ra8_cnecc_bit_ecer1c  = 9U,  /**< Write-1 to clear ECER1F (R/W).           */
  k_ra8_cnecc_bit_ecer2c  = 10U, /**< Write-1 to clear ECER2F (R/W).           */
  k_ra8_cnecc_bit_ecovff  = 11U, /**< Address-capture overflow flag (R).       */
  k_ra8_cnecc_bit_emca0   = 14U, /**< EMCA[1:0] write-trigger lo (R/W).        */
  k_ra8_cnecc_bit_emca1   = 15U, /**< EMCA[1:0] write-trigger hi (R/W).        */
  k_ra8_cnecc_bit_ecsedf0 = 16U, /**< 1-bit error address captured flag (R).   */
  k_ra8_cnecc_bit_ecdedf0 = 17U, /**< 2-bit error address captured flag (R).   */
} ra8_cnecc_ctl_bit_t;

/**
 * @enum ra8_cnecc_ctl_mask_t
 * @brief EC710CTL bit masks (32-bit register).
 *
 * @details
 * Cross-checked against FSP ``R_ECCMB0_EC710CTL_*_Msk`` defines.
 * HUM Ch 42.2.1 p 2870 EMCA[1:0] = 01b is the write-unlock pattern
 * for ECERVF.
 */
typedef enum : uint32_t {
  k_ra8_cnecc_mask_ecemf       = 0x00000001UL, /**< ECEMF.                                */
  k_ra8_cnecc_mask_ecer1f      = 0x00000002UL, /**< ECER1F.                               */
  k_ra8_cnecc_mask_ecer2f      = 0x00000004UL, /**< ECER2F.                               */
  k_ra8_cnecc_mask_ec1edic     = 0x00000008UL, /**< EC1EDIC.                              */
  k_ra8_cnecc_mask_ec2edic     = 0x00000010UL, /**< EC2EDIC.                              */
  k_ra8_cnecc_mask_ec1ecp      = 0x00000020UL, /**< EC1ECP.                               */
  k_ra8_cnecc_mask_ecervf      = 0x00000040UL, /**< ECERVF.                               */
  k_ra8_cnecc_mask_ecer1c      = 0x00000200UL, /**< ECER1C (write-1-clear).               */
  k_ra8_cnecc_mask_ecer2c      = 0x00000400UL, /**< ECER2C (write-1-clear).               */
  k_ra8_cnecc_mask_ecovff      = 0x00000800UL, /**< ECOVFF overflow.                      */
  k_ra8_cnecc_mask_emca        = 0x0000C000UL, /**< EMCA[1:0] @ [15:14].                  */
  k_ra8_cnecc_mask_emca_unlock = 0x00004000UL, /**< EMCA = 01b -> unlock ECERVF.          */
  k_ra8_cnecc_mask_ecsedf0     = 0x00010000UL, /**< 1-bit address captured.               */
  k_ra8_cnecc_mask_ecdedf0     = 0x00020000UL, /**< 2-bit address captured.               */
  k_ra8_cnecc_mask_clear_all   = 0x00000600UL, /**< ECER1C | ECER2C bundle.               */
  k_ra8_cnecc_mask_status_all  = 0x00030806UL, /**< ECER1F|ECER2F|ECOVFF|ECSEDF0|ECDEDF0. */
  k_ra8_cnecc_mask_irq_all     = 0x00000018UL, /**< EC1EDIC | EC2EDIC.                    */
  k_ra8_cnecc_mask_ctl_writable =
    0x0000C67FUL, /**< Bits we may legally write (excludes RO + reserved). */
} ra8_cnecc_ctl_mask_t;

/**
 * @enum ra8_cnecc_tmc_bit_t
 * @brief EC710TMC bit positions (16-bit register).
 *
 * @details
 * HUM Ch 42.2.2 p 2871, cross-checked against FSP
 * ``R_ECCMB0_EC710TMC_*_Pos``.
 */
typedef enum : uint8_t {
  k_ra8_cnecc_bit_ecdcs  = 1U,  /**< ECDCS decoder input source (R/W). */
  k_ra8_cnecc_bit_ectmce = 7U,  /**< ECTMCE test-mode enable (R/W).    */
  k_ra8_cnecc_bit_etma0  = 14U, /**< ETMA[1:0] write-trigger lo.       */
  k_ra8_cnecc_bit_etma1  = 15U, /**< ETMA[1:0] write-trigger hi.       */
} ra8_cnecc_tmc_bit_t;

/**
 * @enum ra8_cnecc_tmc_mask_t
 * @brief EC710TMC bit masks (16-bit register).
 *
 * @details
 * HUM Ch 42.2.2 p 2871. ETMA[1:0] = 10b is the write-unlock pattern
 * for ECTMCE; the figure 42.2 procedure on p 2875 also writes the
 * literal values ``0x8000`` (disable) / ``0x8080`` (enable) /
 * ``0x8082`` (substitute decoder input) to this register.
 */
typedef enum : uint16_t {
  k_ra8_cnecc_mask_ecdcs        = 0x0002U, /**< ECDCS decoder input select.  */
  k_ra8_cnecc_mask_ectmce       = 0x0080U, /**< ECTMCE test-mode enable.     */
  k_ra8_cnecc_mask_etma         = 0xC000U, /**< ETMA[1:0] @ [15:14].         */
  k_ra8_cnecc_mask_etma_unlock  = 0x8000U, /**< ETMA = 10b -> unlock ECTMCE. */
  k_ra8_cnecc_mask_test_disable = 0x8000U, /**< Figure 42.2 "Disable test".  */
  k_ra8_cnecc_mask_test_enable  = 0x8080U, /**< Figure 42.2 "Enable test".   */
  k_ra8_cnecc_mask_test_subst   = 0x8082U, /**< Figure 42.2 "Use ECEDB".     */
} ra8_cnecc_tmc_mask_t;

/**
 * @enum ra8_cnecc_ead_mask_t
 * @brief EC710EAD0 field mask (10-bit RAM offset).
 *
 * @details
 * HUM Ch 42.2.4 p 2872: ECEAD[9:0] holds the captured RAM word
 * offset; bits [31:10] are reserved and read as 0.
 */
typedef enum : uint32_t {
  k_ra8_cnecc_mask_ecead        = 0x000003FFUL, /**< ECEAD[9:0].            */
  k_ra8_cnecc_mask_ead_reserved = 0xFFFFFC00UL, /**< Bits [31:10] always 0. */
} ra8_cnecc_ead_mask_t;

/**
 * @struct r_cnecc_regs_t
 * @brief Per-instance CNECC register window (size = 0x14 bytes).
 *
 * @details
 * Verified against FSP ``R_ECCMB0_Type`` in ``R7KA8D2KF_core0.h``
 * (sizeof = 20). HUM Ch 42.2 p 2868-2873.
 */
typedef struct {
  volatile uint32_t EC710CTL;  /**< +0x00 ECC control / status / IRQ enables. */
  volatile uint16_t EC710TMC;  /**< +0x04 Test-mode control.                  */
  volatile uint16_t _r0;       /**< +0x06 Reserved.                           */
  volatile uint32_t _r1;       /**< +0x08 Reserved.                           */
  volatile uint32_t EC710TED;  /**< +0x0C Test substitute data.               */
  volatile uint32_t EC710EAD0; /**< +0x10 Captured error address (read-only). */
} r_cnecc_regs_t;

/**
 * @brief Get pointer to CNECC instance ``instance``.
 *
 * @param[in] instance Instance index (0..1).
 * @return Volatile pointer to the instance register block, or
 *         ``nullptr`` if ``instance`` is out of range.
 *
 * @pre  ``instance < k_ra8_cnecc_instance_count``.
 * @post Returned pointer is either ``nullptr`` or a stable address
 *       within the CNECC peripheral window.
 */
RA8_HW_REGISTER_ACCESS
static inline volatile r_cnecc_regs_t* ra8_cnecc(uint8_t instance)
{
  if ((uint16_t)instance >= k_ra8_cnecc_instance_count) {
    return nullptr;
  }
  const uintptr_t base =
    k_ra8_cnecc0_base_addr + ((uintptr_t)instance * (uintptr_t)k_ra8_cnecc_stride);
  return (volatile r_cnecc_regs_t*)base;
}

#ifdef __cplusplus
}
#endif
