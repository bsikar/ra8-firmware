/**
 * @file ra_ofs.c
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
 *  - `OFS3`: RA8-family extended settings.
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
 *
 * @since 0.1.0
 */

#include <stdint.h>

/* =============================================================================
 * Defaults (override by defining the macro before including this file)
 * =============================================================================
 */

#ifndef BSP_CFG_OPTION_SETTING_OFS0
#define BSP_CFG_OPTION_SETTING_OFS0 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS1
#define BSP_CFG_OPTION_SETTING_OFS1 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS2
#define BSP_CFG_OPTION_SETTING_OFS2 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS3
#define BSP_CFG_OPTION_SETTING_OFS3 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_SAS
#define BSP_CFG_OPTION_SETTING_SAS (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS1_SEC
#define BSP_CFG_OPTION_SETTING_OFS1_SEC (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS1_SEL
#define BSP_CFG_OPTION_SETTING_OFS1_SEL (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS3_SEC
#define BSP_CFG_OPTION_SETTING_OFS3_SEC (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OFS3_SEL
#define BSP_CFG_OPTION_SETTING_OFS3_SEL (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_BPS
#define BSP_CFG_OPTION_SETTING_BPS (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_BPS_SEC
#define BSP_CFG_OPTION_SETTING_BPS_SEC (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL0
#define BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL0 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL1
#define BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL1 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL2
#define BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL2 (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_SAMR
#define BSP_CFG_OPTION_SETTING_OTP_SAMR (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_PBPS
#define BSP_CFG_OPTION_SETTING_OTP_PBPS (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_PBPS_SEC
#define BSP_CFG_OPTION_SETTING_OTP_PBPS_SEC (0xFFFFFFFFU)
#endif
#ifndef BSP_CFG_OPTION_SETTING_OTP_ZHUK
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

#if defined(UNIT_TEST) || defined(RA_SIMULATOR_MODE) || defined(__APPLE__)
#define RA_SECTION(name) [[gnu::used]]
#else
#define RA_SECTION(name) [[gnu::used, gnu::section(name)]]
#endif

RA_SECTION(".option_setting_ofs0") static const uint32_t g_ra_ofs0 = BSP_CFG_OPTION_SETTING_OFS0;

RA_SECTION(".option_setting_ofs1") static const uint32_t g_ra_ofs1 = BSP_CFG_OPTION_SETTING_OFS1;

RA_SECTION(".option_setting_ofs2") static const uint32_t g_ra_ofs2 = BSP_CFG_OPTION_SETTING_OFS2;

RA_SECTION(".option_setting_ofs3") static const uint32_t g_ra_ofs3 = BSP_CFG_OPTION_SETTING_OFS3;

RA_SECTION(".option_setting_sas") static const uint32_t g_ra_sas = BSP_CFG_OPTION_SETTING_SAS;

RA_SECTION(".option_setting_ofs1_sec")
static const uint32_t g_ra_ofs1_sec = BSP_CFG_OPTION_SETTING_OFS1_SEC;

RA_SECTION(".option_setting_ofs1_sel")
static const uint32_t g_ra_ofs1_sel = BSP_CFG_OPTION_SETTING_OFS1_SEL;

RA_SECTION(".option_setting_ofs3_sec")
static const uint32_t g_ra_ofs3_sec = BSP_CFG_OPTION_SETTING_OFS3_SEC;

RA_SECTION(".option_setting_ofs3_sel")
static const uint32_t g_ra_ofs3_sel = BSP_CFG_OPTION_SETTING_OFS3_SEL;

RA_SECTION(".option_setting_bps") static const uint32_t g_ra_bps = BSP_CFG_OPTION_SETTING_BPS;

RA_SECTION(".option_setting_bps_sec")
static const uint32_t g_ra_bps_sec = BSP_CFG_OPTION_SETTING_BPS_SEC;

RA_SECTION(".option_setting_otp_fsblctrl0")
static const uint32_t g_ra_otp_fsblctrl0 = BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL0;

RA_SECTION(".option_setting_otp_fsblctrl1")
static const uint32_t g_ra_otp_fsblctrl1 = BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL1;

RA_SECTION(".option_setting_otp_fsblctrl2")
static const uint32_t g_ra_otp_fsblctrl2 = BSP_CFG_OPTION_SETTING_OTP_FSBLCTRL2;

RA_SECTION(".option_setting_otp_samr")
static const uint32_t g_ra_otp_samr = BSP_CFG_OPTION_SETTING_OTP_SAMR;

RA_SECTION(".option_setting_otp_pbps")
static const uint32_t g_ra_otp_pbps = BSP_CFG_OPTION_SETTING_OTP_PBPS;

RA_SECTION(".option_setting_otp_pbps_sec")
static const uint32_t g_ra_otp_pbps_sec = BSP_CFG_OPTION_SETTING_OTP_PBPS_SEC;

RA_SECTION(".option_setting_otp_zhuk")
static const uint32_t g_ra_otp_zhuk = BSP_CFG_OPTION_SETTING_OTP_ZHUK;
