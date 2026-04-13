/**
 * @file ra8d2_ether_regs.h
 * @brief Ethernet controller base addresses for the Renesas RA8D2
 *
 * @details
 * RA8D2 has a gigabit Ethernet subsystem composed of several blocks
 * (GMAC A/B, MII forwarder, TSNSW, GPTP). Base addresses below come
 * from R7KA8D2KF. Drivers land with a real network stack.
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
  k_ra_etha0_base_addr = 0x403CA000UL,
  k_ra_etha1_base_addr = 0x403CC000UL,
  k_ra_rmac0_base_addr = 0x403CB000UL,
  k_ra_rmac1_base_addr = 0x403CD000UL,
  k_ra_mfwd_base_addr  = 0x403C0000UL,
  k_ra_gptp_base_addr  = 0x403E0000UL,
  k_ra_eswm_base_addr  = 0x403C8000UL,
  k_ra_gwca0_base_addr = 0x403CE000UL,
} ra_ether_addr_t;

/**
 * @struct r_eswm_regs_t
 * @brief Minimal Ethernet Switch Module (ESWM) register window.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is accessed via ``ra_eswm()`` in
 * ``libs/ra_hal/src/ra_eth.c``.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t ESWM_CTRL; /**< +0x00 Control.              */
  volatile uint32_t ESWM_STS;  /**< +0x04 Status.               */
  volatile uint32_t ESWM_IE;   /**< +0x08 Interrupt Enable.     */
  volatile uint32_t ESWM_ICLR; /**< +0x0C Interrupt Clear.      */
} r_eswm_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the ESWM register block. */
static inline volatile r_eswm_regs_t* ra_eswm(void)
{
  return (volatile r_eswm_regs_t*)k_ra_eswm_base_addr;
}

/**
 * @struct r_mfwd_regs_t
 * @brief Minimal Message Forwarding Engine (MFWD) register window.
 *
 * @details
 * The MFWD block sits between the GMAC ports and the CPU agent
 * (GWCA). The minimal CTRL/STS/IE/ICLR layout below is enough to
 * bring the block in and out of run mode + service IRQs; the
 * full forwarding table programming surface lands with the first
 * routing-aware consumer.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t MFWD_CTRL; /**< +0x00 Forwarding Control.   */
  volatile uint32_t MFWD_STS;  /**< +0x04 Status.               */
  volatile uint32_t MFWD_IE;   /**< +0x08 Interrupt Enable.     */
  volatile uint32_t MFWD_ICLR; /**< +0x0C Interrupt Clear.      */
} r_mfwd_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the MFWD register block. */
static inline volatile r_mfwd_regs_t* ra_mfwd(void)
{
  return (volatile r_mfwd_regs_t*)k_ra_mfwd_base_addr;
}

/**
 * @struct r_coma_regs_t
 * @brief Minimal Common Agent (COMA) register window.
 *
 * @details
 * COMA is the management agent that owns the bus arbitration
 * counters and the shared per-port descriptor fences. The
 * minimal control surface mirrors the other ethernet blocks.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t COMA_CTRL; /**< +0x00 Common Agent Control. */
  volatile uint32_t COMA_STS;  /**< +0x04 Status.               */
  volatile uint32_t COMA_IE;   /**< +0x08 Interrupt Enable.     */
  volatile uint32_t COMA_ICLR; /**< +0x0C Interrupt Clear.      */
} r_coma_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the COMA register block. */
static inline volatile r_coma_regs_t* ra_coma(void)
{
  /* COMA shares the MFWD window in the documented layout; if a
   * future revision moves it out, the linker constant above
   * needs a separate k_ra_coma_base_addr entry. */
  return (volatile r_coma_regs_t*)k_ra_mfwd_base_addr;
}

/**
 * @struct r_gwca_regs_t
 * @brief Minimal CPU Agent (GWCA) register window.
 *
 * @details
 * GWCA is the bridge between MFWD/COMA and CPU memory; it owns
 * the per-channel descriptor rings the host uses for TX/RX
 * staging. The Wave 7+ NIC consumer programmes the descriptor
 * rings; this scaffold just covers the lifecycle + IRQ surface.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t GWCA_CTRL; /**< +0x00 CPU Agent Control.    */
  volatile uint32_t GWCA_STS;  /**< +0x04 Status.               */
  volatile uint32_t GWCA_IE;   /**< +0x08 Interrupt Enable.     */
  volatile uint32_t GWCA_ICLR; /**< +0x0C Interrupt Clear.      */
} r_gwca_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the GWCA register block. */
static inline volatile r_gwca_regs_t* ra_gwca(void)
{
  return (volatile r_gwca_regs_t*)k_ra_gwca0_base_addr;
}

/**
 * @struct r_gptp_regs_t
 * @brief Minimal Generic PTP Timer (GPTP) register window.
 *
 * @details
 * GPTP is the IEEE-1588 hardware timestamp counter shared by the
 * GMAC ports for boundary-clock and ordinary-clock applications.
 * The scaffold covers the lifecycle + IRQ surface; the actual
 * timestamp counter / sync-frame encode-decode logic lands with
 * a real PTP stack.
 */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  volatile uint32_t GPTP_CTRL; /**< +0x00 PTP Timer Control.    */
  volatile uint32_t GPTP_STS;  /**< +0x04 Status.               */
  volatile uint32_t GPTP_IE;   /**< +0x08 Interrupt Enable.     */
  volatile uint32_t GPTP_ICLR; /**< +0x0C Interrupt Clear.      */
} r_gptp_regs_t;
/* cppcheck-suppress-end [unusedStructMember] */

/** @brief Get pointer to the GPTP register block. */
static inline volatile r_gptp_regs_t* ra_gptp(void)
{
  return (volatile r_gptp_regs_t*)k_ra_gptp_base_addr;
}

#ifdef __cplusplus
}
#endif
