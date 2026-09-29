/**
 * @file caps.h
 * @brief Cortex-M85 capability declaration for the arch tier.
 *
 * @details
 * Every optional capability in `arch/arch.h` is answered here, for this core,
 * with a value AND a reason. A flag set to 1 owes a backend translation unit; a
 * flag cleared to 0 owes the decline note beside it. The port-completeness gate
 * (epic invariant #4, #694) reads both halves, which is why a bare `0` with no
 * comment is a defect rather than a default.
 *
 * This is the RA8D2 CPU0 core. It is the reason the capability flags live per
 * core rather than per ISA: CPU1 on the same silicon is a Cortex-M33 with no
 * data cache and no Helium, and both cores are Armv8-M.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/** @addtogroup grp_arch
 *  @{
 */

/** @brief Core name, for diagnostics and for the build to echo. */
#define ARCH_CORE_NAME "cortex-m85"

/** @brief Instruction-set architecture this core implements. */
#define ARCH_ISA_NAME "armv8.1-m"

/** @brief Significant NVIC priority bits the RA8D2 implements on CPU0. */
#define ARCH_IRQ_PRIORITY_BITS (4U)
/** @brief Native load-exclusive/store-exclusive atomics are present. */
#define ARCH_HAS_NATIVE_ATOMICS (1)
/** @brief ThreadX ports this core, so the RTOS context surface is required. */
#define ARCH_HAS_RTOS_CONTEXT (1)
/** @brief PMSAv8 memory protection unit is present. */
#define ARCH_HAS_MEM_PROTECT (1)
/** @brief Protection flavour, as `arch.h` documents the term. */
#define ARCH_MEM_PROTECT_FLAVOUR "pmsav8"

/** @brief Number of MPU regions this core implements. */
#define ARCH_MEM_PROTECT_REGIONS (16U)
/** @brief L1 data and instruction caches are present and maintainable. */
#define ARCH_HAS_CACHE (1)
/** @brief Cache line size in bytes, the granularity of every maintenance call. */
#define ARCH_CACHE_LINE_BYTES (32U)
/** @brief Helium (MVE) is present, so the SIMD algorithm variant is buildable. */
#define ARCH_HAS_SIMD (1)
/** @brief Armv8-M Security Extension is present; the SAU is programmable. */
#define ARCH_HAS_TRUSTZONE_M (1)
/** @brief Number of SAU regions this core implements. */
#define ARCH_TRUSTZONE_REGIONS (8U)
/**
 * @brief No MMU.
 *
 * @details
 * DECLINED: Cortex-M85 is a protected-memory profile core. It has an MPU and no
 * address translation, so there is nothing to implement rather than something
 * left undone. A hosted backend is where ::ARCH_HAS_MMU turns on.
 */
#define ARCH_HAS_MMU (0)
/** @} */
