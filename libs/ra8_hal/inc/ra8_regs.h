/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_regs.h
 * @brief Top-level peripheral register header for the Renesas RA8D2
 * @ingroup grp_hal_system
 *
 * @details
 * This header is the single include point for every peripheral register
 * block on the RA8D2 (R7KA8D2KF). Drivers include this one file and get
 * access to every peripheral struct + accessor.
 *
 * Each individual `ra8d2_<peripheral>_regs.h` file is a hand-written
 * translation of the relevant section of the Hardware User's Manual
 * (R01UH1065EJ). We do NOT include the Renesas FSP CMSIS device header
 * (`R7KA8D2KF_core0.h`) for four reasons:
 *
 *  1. It is 92,000+ lines of auto-generated code that pulls in every
 *     peripheral the tool thinks the chip has, whether we use it or not.
 *  2. It uses untyped enums and unnamed struct bitfields that fail our
 *     clang-tidy naming rules.
 *  3. It cannot be re-used verbatim without CMSIS-Device being vendored,
 *     which would violate the "no vendor code in-tree" policy from
 *     CLAUDE.md.
 *  4. Hand-writing the register map forces us to read the Hardware
 *     User's Manual, which is the whole point of this project.
 *
 * ## Organisation
 *
 * Each `ra8d2_*_regs.h` file exports:
 *  - A `uintptr_t` typed enum with the base address(es) for the block.
 *  - A packed `r_<peripheral>_regs_t` struct matching the hardware layout.
 *  - One or more `static inline volatile r_<peripheral>_regs_t*
 *    <peripheral>(void)` accessor functions.
 *  - Bit-field and bit-position enums for non-trivial registers.
 *
 * ## Naming
 *
 * - File prefix: `ra8d2_` (chip specific).
 * - Struct prefix: `r_` (matches FSP convention so cross-referencing
 *   the Hardware User's Manual is trivial).
 * - Accessor: lowercase peripheral name (`sci0()`, `port6()`, `cgc()`).
 * - Typed-enum values: `k_ra8_<domain>_<name>`.
 */

#pragma once

/* Core system blocks. */
#include "ra8_cac_regs.h"
#include "ra8_cgc_regs.h"
#include "ra8_lpm_regs.h"
#include "ra8_lvd_regs.h"
#include "ra8_mpu_regs.h"
#include "ra8_mstp_regs.h"
#include "ra8_system_regs.h"

/* I/O and pin mux. */
#include "ra8_pfs_regs.h"
#include "ra8_port_regs.h"

/* Interrupts + events. */
#include "ra8_elc_regs.h"
#include "ra8_icu_regs.h"

/* Data movement. */
#include "ra8_dmac_regs.h"
#include "ra8_dtc_regs.h"

/* Timers. */
#include "ra8_agt_regs.h"
#include "ra8_gpt_regs.h"
#include "ra8_iwdt_regs.h"
#include "ra8_poeg_regs.h"
#include "ra8_wdt_regs.h"

/* Serial. */
#include "ra8_i3c_i2c_regs.h"
#include "ra8_sci_regs.h"
#include "ra8_spi_regs.h"

/* Analog. */
#include "ra8_adc_b_regs.h"
#include "ra8_tsn_regs.h"

/* Misc. */
#include "ra8_crc_regs.h"
#include "ra8_flash_regs.h"
#include "ra8_rtc_regs.h"

/* Connectivity / display. */
#include "ra8_canfd_regs.h"
#include "ra8_glcdc_regs.h"
#include "ra8_ospi_regs.h"
#include "ra8_usb_regs.h"
