/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_flash_types.h
 * @brief Code MRAM driver -- shared types (enums, structs, callback typedef)
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Type definitions for the RA8D2 MRAM controller driver. This sub-header
 * is split out of ``ra8_flash.h`` (the thin umbrella) and holds every
 * typed enum, configuration / status struct, IRQ-event struct, and the
 * unified IRQ callback typedef the driver exposes. It is included by the
 * function-prototype sub-headers (``ra8_flash_core.h`` and
 * ``ra8_flash_fsp.h``) and, transitively, by the ``ra8_flash.h`` umbrella
 * that consumers continue to include unchanged.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_flash_regs.h"

/**
 * @enum ra8_flash_world_t
 * @brief Which secure-attribution gate to open for a write.
 *
 * @details
 * MRCPC0 controls writes to the non-secure MRAM half; MRCPC1 controls
 * the secure half. Cite HUM Ch 59 p 3601..3603 (MRCPC0/MRCPC1 register
 * descriptions in chapter range 3542..3625).
 */
typedef enum : uint8_t {
  k_ra8_flash_world_ns = 0U, /**< Open MRCPC0 (non-secure half). */
  k_ra8_flash_world_s  = 1U, /**< Open MRCPC1 (secure half).     */
} ra8_flash_world_t;

/**
 * @enum ra8_flash_irq_src_t
 * @brief Source identifier for unified flash IRQ dispatcher.
 */
typedef enum : uint8_t {
  k_ra8_flash_irq_code_ecc_ted  = 0U, /**< Code MRAM 1-bit ECC TED.       */
  k_ra8_flash_irq_code_ecc_dec  = 1U, /**< Code MRAM 2-bit ECC DEC.       */
  k_ra8_flash_irq_extra_ecc_ted = 2U, /**< Extra MRAM 1-bit ECC TED.      */
  k_ra8_flash_irq_extra_ecc_dec = 3U, /**< Extra MRAM 2-bit ECC DEC.      */
  k_ra8_flash_irq_program_err   = 4U, /**< Code MRAM program/erase error. */
  k_ra8_flash_irq_extra_err     = 5U, /**< Extra MRAM access error.       */
  k_ra8_flash_irq_extra_cmdlk   = 6U, /**< Extra MRAM command-lock.       */
  k_ra8_flash_irq_extra_ready   = 7U, /**< Extra MRAM ready.              */
  k_ra8_flash_irq_count         = 8U, /**< Sentinel: number of sources.   */
} ra8_flash_irq_src_t;

/**
 * @enum ra8_flash_startup_t
 * @brief Boot-area selection for ::ra8_flash_set_startup_area.
 */
typedef enum : uint8_t {
  k_ra8_flash_startup_default   = 0U, /**< Default startup area (BTFLG=1).   */
  k_ra8_flash_startup_alternate = 1U, /**< Alternate startup area (BTFLG=0). */
  k_ra8_flash_startup_btflg     = 2U, /**< Validation sentinel.              */
} ra8_flash_startup_t;

/**
 * @enum ra8_flash_arc_id_t
 * @brief Anti-rollback counter identifier.
 */
typedef enum : uint8_t {
  k_ra8_flash_arc_sec    = 0U, /**< Secure ARC.         */
  k_ra8_flash_arc_oembl  = 1U, /**< OEM bootloader ARC. */
  k_ra8_flash_arc_nsec_0 = 2U, /**< Non-secure ARC #0.  */
  k_ra8_flash_arc_nsec_1 = 3U, /**< Non-secure ARC #1.  */
  k_ra8_flash_arc_nsec_2 = 4U, /**< Non-secure ARC #2.  */
  k_ra8_flash_arc_nsec_3 = 5U, /**< Non-secure ARC #3.  */
  k_ra8_flash_arc_count  = 6U, /**< Sentinel.           */
} ra8_flash_arc_id_t;

/**
 * @struct ra8_flash_cfg_t
 * @brief Initialisation descriptor for ``ra8_flash_init``.
 *
 * @details
 * MRAM has no MSTP gate (it is the code memory and is always powered)
 * so the only thing the driver needs at init time is the operating
 * frequency the silicon should advertise to the controller via
 * ``MRCFREQ``. cppcheck cannot see callers in tests/ so it flags
 * every field as unused; the suppress comment matches what
 * ra8_acmphs.h does.
 */
typedef struct {
  uint16_t mrcfreq_mhz;        /**< Code MRAM clock in MHz, 0..0x0FA (250 MHz).  */
  uint8_t  mrefreq_mhz;        /**< Extra MRAM clock in MHz, 0..0x07D (125 MHz). */
  bool     prefetch_en;        /**< true -> leave prefetch enabled at init.      */
  bool     ecc_encoder_enable; /**< true -> MRCEECC.ECCEN=1 (default).           */
  bool     ecc_decoder_enable; /**< true -> MRCDECC.DECECEN=1 (default).         */
} ra8_flash_cfg_t;

/**
 * @struct ra8_flash_status_ext_t
 * @brief Extended status snapshot (MRCPS + MSTATR + MASTAT + MREZS).
 *
 * @details
 * One-shot snapshot of every status register that callers may want to
 * inspect after an operation. Populated by
 * ``ra8_flash_get_extended_status``.
 */
typedef struct {
  uint8_t  mrcps;  /**< MRCPS bits (HUM Ch 59 p 3601).  */
  uint8_t  mastat; /**< MASTAT bits (HUM Ch 59 p 3577). */
  uint8_t  mrezs;  /**< MREZS bits (HUM Ch 59 p 3565).  */
  uint16_t mcmdr;  /**< MCMDR bits (HUM Ch 59 p 3589).  */
  uint32_t mstatr; /**< MSTATR bits (HUM Ch 59 p 3578). */
} ra8_flash_status_ext_t;

/**
 * @struct ra8_flash_status_t
 * @brief Decoded status snapshot, FSP-parity surface.
 *
 * @details
 * Mirrors the ``flash_status_t`` field set R_MRAM exposes in FSP, but
 * widened so callers can also see the controller-error bits without a
 * second ``ra8_flash_get_extended_status`` call. Populated by
 * ``ra8_flash_status``. HUM Ch 59 p 3577..3578.
 */
typedef struct {
  bool programming_busy; /**< MRCPS.PRGBSYC or MENTRYR.MENTRY set. */
  bool erase_busy;       /**< Same as programming_busy on MRAM.    */
  bool illegal_command;  /**< MASTAT.CMDLK or MSTATR.ILGCOMERR.    */
  bool voltage_error;    /**< MSTATR.OTERR (other / supply error). */
  bool sector_protected; /**< MRCBPROT0/1 lock observed.           */
  bool program_error;    /**< MRCPS.PRGERRC.                       */
  bool ecc_error;        /**< MRCPS.ECCERRC.                       */
} ra8_flash_status_t;

/**
 * @struct ra8_flash_isr_event_t
 * @brief One IRQ event delivered to ``ra8_flash_callback_t``.
 *
 * @details
 * The dispatcher records which source fired and (where applicable)
 * the captured fault address from MRCRTEA / MRCRDEA / MRCPEA.
 */
typedef struct {
  ra8_flash_irq_src_t src;         /**< Which source fired.         */
  uint32_t            fault_addr;  /**< MRCRTEA / MRCRDEA / MRCPEA. */
  uint32_t            status_word; /**< Snapshot of the source reg. */
  void*               user_ctx;    /**< Caller-provided context.    */
} ra8_flash_isr_event_t;

/**
 * @brief Callback signature for the unified flash IRQ dispatcher.
 *
 * @param[in] ev Event descriptor (source, fault address, status).
 *
 * @pre Caller registered this callback via ``ra8_flash_callback_set``.
 * @post Callback returns; dispatcher continues with the next pending
 * source (if any) or returns to the caller of
 * ``ra8_flash_dispatch_isr``.
 *
 * @note Runs in IRQ context: keep it short, do not block.
 */
typedef void (*ra8_flash_callback_t)(const ra8_flash_isr_event_t* ev);
