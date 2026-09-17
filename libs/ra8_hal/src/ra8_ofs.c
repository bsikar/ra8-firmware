/**
 * @file ra8_ofs.c
 * @brief RA boot-ROM Option Function Select register defaults
 *
 * @details
 * Emits 32-bit constants into the `.option_setting_*` linker
 * sections defined in `src/linker_script.ld`. The RA8D2 boot ROM
 * reads these regions directly out of MRAM before the Cortex-M85
 * reset vector runs, so they configure:
 *
 *  - `OFS0`: IWDT auto-start, brown-out reset, HOCO post-reset,
 *    security MPU, TrustZone boundaries.
 *  - `OFS1`: LVD0 reset / VCC voltage monitoring.
 *  - `OFS2`: extended oscillator settings, DPFPU.
 *  - `OFS3`: RA8-family extended settings + M33-side WDT1.
 *    **RA8D2 only** -- see the device-gating note below.
 *  - `SAS`: secure/non-secure attribution for the entire flash
 *    address space.
 *  - `OFS1_SEC/SEL`, `OFS3_SEC/SEL`: TrustZone secure-attribute
 *    selectors.
 *  - `BPS`, `BPS_SEC`: block-protection configuration.
 *  - `OTP_*`: one-time-programmable anti-rollback and boot-secure
 *    fields.
 *
 * Every entry here is defined as `0xFFFFFFFF` which is the
 * "erased / default / permissive" value -- exactly what a freshly
 * erased MRAM part would contain. This means:
 *
 *  - IWDT disabled (OFS0 default)
 *  - LVD0 disabled (OFS1 default)
 *  - Full flash / TrustZone access (SAS default)
 *  - No OTP anti-rollback (OTP_* defaults)
 *
 * Safe for bring-up, NOT appropriate for production. A real
 * deployment should override each of these with a vetted value in
 * a tamper-resistant build step.
 *
 * ## OFS3 is NOT device-gated: both parts have it
 *
 * The OFS3 word -- plus its OFS3_SEC / OFS3_SEL TrustZone selectors --
 * carries the M33-side (CPU1) WDT1 auto-start fields, and is present
 * on BOTH supported parts at the same addresses with the same bit
 * fields: RA8D2 HUM R01UH1065EJ0130 Ch 7.2.6 p 287 / Ch 7.2.7 p 289,
 * and RA8P1 HUM R01UH1064EJ0130 Ch 7.2.6 p 288 / Ch 7.2.7 p 290.
 * The whole OFS3 family is therefore emitted unconditionally.
 *
 * Issue #223 previously gated these three emissions out of RA8P1
 * builds behind `RA8_HAS_OFS3`, on the strength of FSP's
 * `BSP_FEATURE_BSP_HAS_OFS3 == 0` for ra8p1 -- a value that
 * contradicts Renesas' own RA8P1 manual. The effect was an RA8P1
 * image with no OFS3 word at all, leaving WDT1's boot-latched
 * configuration unprogrammed. Removed in #516; see `ra8_ofs.h`.
 *
 * ## Overriding
 *
 * The easy way: `#define BSP_CFG_OPTION_SETTING_OFS0 (0x12345678U)`
 * before including this file from another translation unit. For
 * compile-time programming, define the same macros via `-D` in
 * CMakeLists.txt.
 *
 * ## Placement
 *
 * Each entry uses `__attribute__((section(".option_setting_xxx")))`
 * matching the section names in `src/linker_script.ld`. The
 * `used` attribute prevents the linker from discarding the constants
 * even though nothing references them from C.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_ofs.h" /* OFS word inventory. */

#include <stdint.h>

/* =============================================================================
 * Defaults (override by defining the macro before including this file)
 * =============================================================================
 */

#ifndef BSP_CFG_OPTION_SETTING_OFS0
/** @brief BSP CFG OPTION SETTING OFS0. */
#define BSP_CFG_OPTION_SETTING_OFS0 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS1
/** @brief BSP CFG OPTION SETTING OFS1. */
#define BSP_CFG_OPTION_SETTING_OFS1 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS2
/** @brief BSP CFG OPTION SETTING OFS2. */
#define BSP_CFG_OPTION_SETTING_OFS2 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS3
/** @brief BSP CFG OPTION SETTING OFS3. */
#define BSP_CFG_OPTION_SETTING_OFS3 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_SAS
/** @brief BSP CFG OPTION SETTING SAS. */
#define BSP_CFG_OPTION_SETTING_SAS (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS1_SEC
/** @brief BSP CFG OPTION SETTING OFS1 SEC. */
#define BSP_CFG_OPTION_SETTING_OFS1_SEC (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS1_SEL
/** @brief BSP CFG OPTION SETTING OFS1 SEL. */
#define BSP_CFG_OPTION_SETTING_OFS1_SEL (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS3_SEC
/** @brief BSP CFG OPTION SETTING OFS3 SEC. */
#define BSP_CFG_OPTION_SETTING_OFS3_SEC (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS3_SEL
/** @brief BSP CFG OPTION SETTING OFS3 SEL. */
#define BSP_CFG_OPTION_SETTING_OFS3_SEL (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_BPS
/** @brief BSP CFG OPTION SETTING BPS. */
#define BSP_CFG_OPTION_SETTING_BPS (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_BPS_SEC
/** @brief BSP CFG OPTION SETTING BPS SEC. */
#define BSP_CFG_OPTION_SETTING_BPS_SEC (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL0
/** @brief BSP CFG OPTION SETTING OTP FSBLCTRL0. */
#define BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL0 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL1
/** @brief BSP CFG OPTION SETTING OTP FSBLCTRL1. */
#define BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL1 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL2
/** @brief BSP CFG OPTION SETTING OTP FSBLCTRL2. */
#define BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL2 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_SAMR
/** @brief BSP CFG OPTION SETTING OTP SAMR. */
#define BSP_CFG_OPTION_SETTING_OTP_SAMR (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_PBPS
/** @brief BSP CFG OPTION SETTING OTP PBPS. */
#define BSP_CFG_OPTION_SETTING_OTP_PBPS (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_PBPS_SEC
/** @brief BSP CFG OPTION SETTING OTP PBPS SEC. */
#define BSP_CFG_OPTION_SETTING_OTP_PBPS_SEC (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_ZHUK
/** @brief BSP CFG OPTION SETTING OTP ZHUK. */
#define BSP_CFG_OPTION_SETTING_OTP_ZHUK (0xFFFFFFFFU)
#endif

/* =============================================================================
 * Section-placed constants
 * =============================================================================
 *
 * The `used` attribute prevents the linker from discarding the
 * constants even though nothing references them from C. The linker
 * script pins each section to a fixed MRAM address.
 */

#if defined(UNIT_TEST) || defined(RA8_OFF_TARGET) || defined(__APPLE__)
/** @brief RA8 SECTION. */
#define RA8_SECTION(name) [[gnu::used]]
#else
/** @brief RA8 SECTION. */
#define RA8_SECTION(name) [[gnu::used, gnu::section(name)]]
#endif

RA8_SECTION(".option_setting_ofs0") static const uint32_t s_ra8_ofs0 = BSP_CFG_OPTION_SETTING_OFS0;

RA8_SECTION(".option_setting_ofs1") static const uint32_t s_ra8_ofs1 = BSP_CFG_OPTION_SETTING_OFS1;

RA8_SECTION(".option_setting_ofs2") static const uint32_t s_ra8_ofs2 = BSP_CFG_OPTION_SETTING_OFS2;

RA8_SECTION(".option_setting_ofs3") static const uint32_t s_ra8_ofs3 = BSP_CFG_OPTION_SETTING_OFS3;

RA8_SECTION(".option_setting_sas") static const uint32_t s_ra8_sas = BSP_CFG_OPTION_SETTING_SAS;

RA8_SECTION(".option_setting_ofs1_sec")
static const uint32_t s_ra8_ofs1_sec = BSP_CFG_OPTION_SETTING_OFS1_SEC;

RA8_SECTION(".option_setting_ofs1_sel")
static const uint32_t s_ra8_ofs1_sel = BSP_CFG_OPTION_SETTING_OFS1_SEL;

/* OFS3_SEC / OFS3_SEL are the TrustZone secure-attribution selectors for OFS3,
 * present on both parts alongside it. */
RA8_SECTION(".option_setting_ofs3_sec")
static const uint32_t s_ra8_ofs3_sec = BSP_CFG_OPTION_SETTING_OFS3_SEC;

RA8_SECTION(".option_setting_ofs3_sel")
static const uint32_t s_ra8_ofs3_sel = BSP_CFG_OPTION_SETTING_OFS3_SEL;

RA8_SECTION(".option_setting_bps") static const uint32_t s_ra8_bps = BSP_CFG_OPTION_SETTING_BPS;

RA8_SECTION(".option_setting_bps_sec")
static const uint32_t s_ra8_bps_sec = BSP_CFG_OPTION_SETTING_BPS_SEC;

RA8_SECTION(".option_setting_otp_fsblctrl0")
static const uint32_t s_ra8_otp_fsblctrl0 = BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL0;

RA8_SECTION(".option_setting_otp_fsblctrl1")
static const uint32_t s_ra8_otp_fsblctrl1 = BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL1;

RA8_SECTION(".option_setting_otp_fsblctrl2")
static const uint32_t s_ra8_otp_fsblctrl2 = BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL2;

RA8_SECTION(".option_setting_otp_samr")
static const uint32_t s_ra8_otp_samr = BSP_CFG_OPTION_SETTING_OTP_SAMR;

RA8_SECTION(".option_setting_otp_pbps")
static const uint32_t s_ra8_otp_pbps = BSP_CFG_OPTION_SETTING_OTP_PBPS;

RA8_SECTION(".option_setting_otp_pbps_sec")
static const uint32_t s_ra8_otp_pbps_sec = BSP_CFG_OPTION_SETTING_OTP_PBPS_SEC;

RA8_SECTION(".option_setting_otp_zhuk")
static const uint32_t s_ra8_otp_zhuk = BSP_CFG_OPTION_SETTING_OTP_ZHUK;

/* =============================================================================
 * OFS boot-map inventory -- contract in ra8_ofs.h
 * =============================================================================
 *
 * Gives the rest of the firmware a runtime-callable, host-testable view of the
 * compile-time OFS layout. A pure function of a compile-time constant.
 */

uint8_t ra8_ofs_word_count(void)
{
  return (uint8_t)k_ra8_ofs_word_count;
}
