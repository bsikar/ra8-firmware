/**
 * @file caps.h
 * @brief Cortex-M33 capability declaration for the arch tier.
 *
 * @details
 * The RA8D2 CPU1 core. Same ISA family as CPU0 and a deliberately different
 * capability answer, which is the in-tree divergence that decides where these
 * flags belong: cache is a CORE property, not an ISA property. CPU1 apps already
 * exist in this tree (`examples/ek_ra8d2/hw_validated/hil/blink_m33`,
 * `.../dualcore_mailbox`, `.../cpu1_pingpong`, `.../cache_coherency_hil`), so
 * this is a core the platform genuinely builds for, not a hypothetical.
 *
 * Every optional capability in `arch/arch.h` is answered with a value AND a
 * reason; a cleared flag with no decline note is a defect, not a default.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/** @addtogroup grp_arch
 *  @{
 */

/** @brief Core name, for diagnostics and for the build to echo. */
#define ARCH_CORE_NAME "cortex-m33"

/** @brief Instruction-set architecture this core implements. */
#define ARCH_ISA_NAME "armv8-m.main"

/** @brief Significant NVIC priority bits the RA8D2 implements on CPU1. */
#define ARCH_IRQ_PRIORITY_BITS (4U)
/** @brief Native load-exclusive/store-exclusive atomics are present. */
#define ARCH_HAS_NATIVE_ATOMICS (1)
/**
 * @brief ThreadX ports this core, so the RTOS context surface is required.
 *
 * @details
 * MIGRATION: the surface exists at
 * `libs/third_party/threadx/ports/cortex_m33/gnu/src`, vendored and not yet
 * selected by `cmake/threadx.cmake`, which wires the Cortex-M85 port only. The
 * flag records that this core has a port to implement the contract with, not
 * that a CPU1 RTOS image is built today; #694's migration slice is where the
 * two stop being different answers.
 */
#define ARCH_HAS_RTOS_CONTEXT (1)
/**
 * @brief PMSAv8 memory protection unit is present.
 *
 * @details
 * MIGRATION: implemented today by `libs/ra8_mpu/src/ra8_mpu_abi.zig`, shared with
 * CPU0. Same PMSAv8 programming model, half the regions, which is why the
 * count is answered per core below. Moves to `arch/armv8m/` in a later slice
 * of #694.
 */
#define ARCH_HAS_MEM_PROTECT (1)
/** @brief Protection flavour, as `arch.h` documents the term. */
#define ARCH_MEM_PROTECT_FLAVOUR "pmsav8"

/** @brief Number of MPU regions this core implements. */
#define ARCH_MEM_PROTECT_REGIONS (8U)
/**
 * @brief No cache to maintain.
 *
 * @details
 * DECLINED, and this is the honest no-op the epic calls out by name. CPU1 has no
 * L1 data cache, so `arch_cache_clean` / `arch_cache_invalidate` are not declared
 * for this core at all. Code that shares a buffer with CPU0 still has a coherency
 * problem, but it is CPU0's cache that must be maintained, from CPU0; pretending
 * CPU1 has a cache API that silently does nothing would hide exactly that.
 */
#define ARCH_HAS_CACHE (0)
/**
 * @brief No Helium.
 *
 * @details
 * DECLINED: Cortex-M33 has no MVE. The SIMD fast path is a compile-time
 * algorithm variant, so a build for this core takes the scalar one.
 */
#define ARCH_HAS_SIMD (0)
/**
 * @brief Armv8-M Security Extension is present; the SAU is programmable.
 *
 * @details
 * MIGRATION: implemented today by `libs/ra8_hal/src/ra8_sau.c`. Moves to
 * `arch/armv8m/trustzone/` in a later slice of #694.
 */
#define ARCH_HAS_TRUSTZONE_M (1)
/** @brief Number of SAU regions this core implements. */
#define ARCH_TRUSTZONE_REGIONS (8U)
/**
 * @brief No MMU.
 *
 * @details
 * DECLINED: protected-memory profile core, MPU only, no address translation.
 */
#define ARCH_HAS_MMU (0)
/** @} */
