/**
 * @file ra8_device.h
 * @brief Compile-time device selection for the RA8 multi-chip build (RA8D2 /
 * RA8P1)
 * @ingroup grp_core
 *
 * @details
 * This project's HAL was written for the Renesas RA8D2 (R7KA8D2KFLCAC). The
 * RA8P1 (R7KA8P1KFLCAC) is the same RA8 family part in the same pin-compatible
 * 289-pin BGA: primary sources (the two chips' FSP CMSIS device headers, their
 * Zephyr device trees, and the RA8P1 datasheet R01DS0439EJ0130 / Hardware
 * User's Manual R01UH1064EJ0130) show that the peripheral register map, the
 * memory map, the interrupt/event numbering, and the module-stop bit
 * assignments are IDENTICAL between the two parts. The RA8P1 reads as
 * "RA8D2 + an Arm Ethos-U55 NPU", with a handful of small deltas captured by
 * the feature flags below.
 *
 * Because so little differs, the whole port hangs off ONE preprocessor switch
 * added to the compile command by the toolchain file:
 *
 *     -DRA8_DEVICE_RA8P1        (cmake/toolchain-ra8p1.cmake)
 *
 * When neither `RA8_DEVICE_RA8D2` nor `RA8_DEVICE_RA8P1` is defined this header
 * defaults to `RA8_DEVICE_RA8D2`, so every existing RA8D2 build (and the host
 * unit tests, which pass no device define) is byte-for-behaviour unchanged.
 *
 * ## What lives here
 *
 * - The device identity (`ra8_device_id_t`, `k_ra8_device_current`).
 * - Feature-presence flags (`RA8_HAS_*` build-config macros + a typed-enum
 *   mirror `ra8_device_feature_t` for runtime code and clang-tidy hygiene).
 * - The device memory map (`ra8_device_mem_base_t` / `ra8_device_mem_size_t`)
 * as the single source of truth shared by C code and, by mirror, the linker
 *   scripts.
 *
 * ## What does NOT live here
 *
 * Peripheral register bases stay in `libs/ra8_hal/inc/ra8d2_*_regs.h`. Every
 * one of the 155 bases the RA8D2 defines is byte-identical on the RA8P1, so
 * those headers need NO device-conditional edits. Only genuinely NEW
 * peripherals get a new header (see `ra8_npu_regs.h` for the Ethos-U55,
 * RA8P1-only). If a future device ever DID shift a base, the fix is local: wrap
 * that one enum value in
 * `#if defined(RA8_DEVICE_RA8P1)` in its own register header -- the
 * base-address enum is deliberately the seam.
 *
 * @note This header is host-friendly: it defines only compile-time constants
 *       and touches no hardware, so it compiles unchanged under
 *       `RA8_OFF_TARGET`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Device selection */
/* -------------------------------------------------------------------------- */

/*
 * Exactly one device must be selected. Default to RA8D2 when the compile
 * command names neither, so pre-existing RA8D2 firmware builds and the
 * host unit-test build (which pass no -DRA8_DEVICE_* flag) keep the RA8D2
 * behaviour they have today, with zero source churn.
 */
#if !defined(RA8_DEVICE_RA8D2) && !defined(RA8_DEVICE_RA8P1)
/** @brief RA8 DEVICE RA8 D2. */
#define RA8_DEVICE_RA8D2 (1)
#endif

#if defined(RA8_DEVICE_RA8D2) && defined(RA8_DEVICE_RA8P1)
#error "ra8_device.h: define at most one of RA8_DEVICE_RA8D2 / RA8_DEVICE_RA8P1"
#endif

/**
 * @enum ra8_device_id_t
 * @brief Stable numeric identity of each supported RA8 device.
 *
 * @details
 * The value is the two hex nibbles of the marketing part name so it reads
 * clearly in a debugger (`0x8D2` for the RA8D2, `0x8P1` is not valid hex so
 * the RA8P1 uses `0x8F1`). Includes `k_ra8_device_current` for runtime code
 * that must branch on the build target.
 *
 * @invariant Exactly one of `RA8_DEVICE_RA8D2` / `RA8_DEVICE_RA8P1` is defined
 *            when this enum is evaluated.
 *
 * @see k_ra8_device_current
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_device_ra8d2 = 0x8D2U, /**< Renesas RA8D2, R7KA8D2KFLCAC (HUM R01UH1065EJ). */
  k_ra8_device_ra8p1 = 0x8F1U, /**< Renesas RA8P1, R7KA8P1KFLCAC (HUM R01UH1064EJ). */
#ifdef RA8_DEVICE_RA8P1
  k_ra8_device_current = k_ra8_device_ra8p1 /**< Device selected by this build. */
#else
  k_ra8_device_current = k_ra8_device_ra8d2 /**< Device selected by this build. */
#endif
} ra8_device_id_t;

/* -------------------------------------------------------------------------- */
/* Feature-presence flags */
/* -------------------------------------------------------------------------- */

/*
 * Build-configuration flags (an explicitly-allowed macro use: they gate
 * conditional compilation, not integer arithmetic). Prefer these semantic
 * names over `#if defined(RA8_DEVICE_RA8P1)` in feature-guarded code so the
 * intent ("this chip has an NPU") survives the arrival of a future part.
 *
 * The delta set below is the COMPLETE list of hardware differences found from
 * primary sources; every other peripheral, base address, and memory region is
 * identical across the two parts.
 *
 * NOTE (issue #224): an earlier draft of this delta set listed a "legacy
 * ETHERC/EDMAC MAC at 0x40354000" as an RA8P1-only addition. That was a misread
 * and is deliberately absent here. The RA8P1 Hardware User's Manual
 * (R01UH1064EJ0130) and datasheet (R01DS0439EJ0130) contain no ETHERC block,
 * none of the classic ETHERC/EDMAC registers (ECMR / EDMR / ...), and nothing
 * based at 0x40354000 (that window is USBHS 0x40351000 / SCI 0x40358000 /
 * SPI 0x4035C000). The token "EDMAC" appears only in the Buses chapter, where
 * BOTH manuals state verbatim that "EDMAC ... means the GWCA function of ESWM"
 * -- the descriptor-DMA bus initiator of the shared R-Switch, which the RA8D2
 * has too. The RA8P1's only Ethernet is the same R-Switch / ESWM subsystem as
 * the RA8D2 (identical HUM chapters 30-36 and register bases), so there is no
 * ETHERC feature flag. Do not re-add one without a primary-source register map.
 *
 * NOTE (issue #516): an earlier draft also listed "OFS3 / WDT1 option register"
 * as RA8D2-only, behind an `RA8_HAS_OFS3` flag. That was a misread of Renesas
 * FSP metadata and is deliberately absent here. BOTH parts have OFS3: RA8P1 HUM
 * R01UH1064EJ0130 Ch 7.2.6 "OFS3, OFS3_SEC : Option Function Select Register 3"
 * p 288 and Ch 7.2.7 "OFS3_SEL ... for Security" p 290 document the same word,
 * at the same addresses and with the same WDT1 bit fields, as RA8D2 HUM
 * R01UH1065EJ0130 Ch 7.2.6 p 287 / Ch 7.2.7 p 289. The RA8P1 likewise has the
 * WDT1 that OFS3 configures (datasheet R01DS0439EJ0130: "Watchdog Timer (WDT)
 * x 2"; WDT1 at 0x4020_2600). FSP's `BSP_FEATURE_BSP_HAS_OFS3` is 0 for ra8p1,
 * which contradicts Renesas' own manual and has no consumer in open FSP source;
 * it is not evidence. Do not re-add an OFS3 feature flag.
 */
#ifdef RA8_DEVICE_RA8P1
/** @brief RA8 HAS NPU. */
#define RA8_HAS_NPU                                                                                \
  (1) /**< Arm Ethos-U55 NPU present (see ra8_npu_regs.h).   \
                         */
/** @brief RA8 HAS NPUCLK. */
#define RA8_HAS_NPUCLK (1) /**< CGC drives a dedicated NPUCLK domain. */
#else
/* RA8_HAS_NPU / RA8_HAS_NPUCLK intentionally undefined (NPU is RA8P1-only). */
#endif

/**
 * @enum ra8_device_feature_t
 * @brief Runtime-readable (0/1) mirror of the `RA8_HAS_*` presence flags.
 *
 * @details
 * The `RA8_HAS_*` macros drive `#if` guards; this typed-enum mirror gives the
 * same facts a named value that ordinary C code and clang-tidy can consume
 * without a bare `0`/`1` literal. Values are resolved from the active device
 * selection at compile time.
 *
 * @invariant Each member is 0 (absent) or 1 (present) on the current device.
 *
 * @see RA8_HAS_NPU
 * @since 0.1.0
 */
typedef enum : uint8_t {
#ifdef RA8_DEVICE_RA8P1
  k_ra8_feat_npu    = 1U, /**< Ethos-U55 NPU: present on RA8P1.       */
  k_ra8_feat_npuclk = 1U, /**< NPUCLK clock domain: present on RA8P1. */
#else
  k_ra8_feat_npu    = 0U, /**< Ethos-U55 NPU: absent on RA8D2.       */
  k_ra8_feat_npuclk = 0U, /**< NPUCLK clock domain: absent on RA8D2. */
#endif /**< (anon). */
} ra8_device_feature_t;

/* -------------------------------------------------------------------------- */
/* Memory map */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra8_device_mem_base_t
 * @brief Base addresses of the on-chip / external memory regions.
 *
 * @details
 * Verified byte-identical between RA8D2 and RA8P1 from the two chips' Zephyr
 * device trees and FSP linker descriptions, so these are defined once for both
 * parts. They are the runtime mirror of the `MEMORY { }` block in every app's
 * `linker_script.ld`; keep the two in lock-step. External-bus regions (SDRAM,
 * OSPI XIP) are board-dependent -- the addresses here are the CPU-side windows,
 * populated only when the corresponding controller and device are fitted.
 *
 * @invariant Values are CPU physical addresses; use `uintptr_t` so the 64-bit
 *            unit-test host does not truncate them.
 *
 * @see ra8_device_mem_size_t
 * @since 0.1.0
 */
typedef enum : uintptr_t {
  k_ra8_mem_mram_base     = 0x02000000U, /**< Code MRAM (non-volatile), 1 MB. */
  k_ra8_mem_itcm_base     = 0x00000000U, /**< M85 instruction TCM window. */
  k_ra8_mem_dtcm_base     = 0x20000000U, /**< M85 data TCM window. */
  k_ra8_mem_sram_base     = 0x22000000U, /**< On-chip system SRAM (ECC), 1664 KB. */
  k_ra8_mem_sdram_base    = 0x68000000U, /**< External SDRAM data window (EK: 64 MB). */
  k_ra8_mem_ospi_cs0_base = 0x80000000U, /**< OSPI/xSPI CS0 XIP window. */
  k_ra8_mem_ospi_cs1_base = 0x90000000U, /**< OSPI/xSPI CS1 XIP window. */
} ra8_device_mem_base_t;

/**
 * @enum ra8_device_mem_size_t
 * @brief Sizes (bytes) of the on-chip memory regions for the current device.
 *
 * @details
 * MRAM and system SRAM are identical on both parts (1 MB and 1664 KB). The TCM
 * sizes are the conservative per-core split the linker scripts use today
 * (64 KB ITCM + 64 KB DTCM); the RA8P1 exposes a larger M85 TCM budget (256 KB
 * total) but the exact ITCM/DTCM split is not yet confirmed against the RA8P1
 * HUM, so the safe shared floor is used until it is (tracked as a follow-up).
 *
 * @invariant Each size is a whole number of KiB.
 *
 * @see ra8_device_mem_base_t
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_mem_mram_size = 0x00100000U, /**< 1 MB code MRAM (both parts). */
  k_ra8_mem_sram_size = 0x001A0000U, /**< 1664 KB system SRAM (both parts). */
  k_ra8_mem_itcm_size = 0x00010000U, /**< 64 KB ITCM (linker floor; see @details). */
  k_ra8_mem_dtcm_size = 0x00010000U, /**< 64 KB DTCM (linker floor; see @details). */
} ra8_device_mem_size_t;

#ifdef __cplusplus
}
#endif
