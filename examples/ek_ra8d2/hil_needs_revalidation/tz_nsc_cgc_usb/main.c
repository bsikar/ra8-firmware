/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/tz_nsc_cgc_usb/main.c
 * @brief CPU0 (M85) Secure fallback entry for the single-core S->NS demo.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * On the happy path this function is never reached. ``SystemInit`` calls
 * ``ra8_trustzone_init``, which programmes the SAU via
 * ``ra8_tz_secure_boot_sau_init`` and then BLXNS-es into the NS image at
 * 0x02080000 (``ns_reset_handler`` in ``ns_main.c``). BLXNS does not
 * return on hardware, so the ``Reset_Handler`` step that calls
 * ``main()`` is unreachable.
 *
 * If the BLXNS path fails -- the NS vector table is blank, the SAU
 * could not be programmed, or the secure-boot library returned for
 * some other reason -- the fallback main() runs and stamps a
 * diagnostic counter. Bench memprobe scripts read this counter via SWD
 * to distinguish "BLXNS succeeded -> main never ran" (counter stays 0)
 * from "BLXNS failed -> main ran" (counter advances).
 *
 * The fallback parks the CPU in a NOP loop so it cannot accidentally
 * touch NS-attributed memory or peripherals from S state.
 *
 * @par Phase A+B note:
 * Until Phase C lands, the NS image is a minimal counter app rather
 * than the full ThreadX + USBX CDC demo. Phase C will re-introduce the
 * original NS app (preserved in ``ns_app_phase_c.c.disabled``) once
 * the linker can place vendored ThreadX + USBX object files inside
 * ``NS_MRAM`` and the USB-FS peripheral has been NS-attributed via an
 * extra SAU region. See issue #55.
 *
 * @since 0.1.0
 */

#include <stdint.h>

/**
 * @var g_tz_nsc_cgc_usb_s_fallback_count
 * @brief Bench diagnostic: bumps when the S-side fallback main runs.
 *
 * @details
 * Stays 0 on the happy path because BLXNS in ``ra8_trustzone_init``
 * never returns. Any non-zero value means the secure-boot library
 * bailed out before transferring control to the NS image -- bench
 * scripts should treat that as a regression in the TZ scaffolding,
 * not in the NS app logic.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 *
 * @since 0.1.0
 */
volatile uint32_t g_tz_nsc_cgc_usb_s_fallback_count = 0U;

/**
 * @brief CPU0 S-side fallback entry point.
 *
 * @details See file header.
 *
 * @return Never returns.
 * @retval 0 Never returned.
 *
 * @pre Boot init has completed.
 * @pre The secure-boot library's BLXNS into NS image either failed or
 *      was skipped (the call site in ``ra8_trustzone_init`` is a no-op
 *      on host builds).
 * @post Diagnostic counter latched, CPU parked in a halt loop.
 * @post Function never returns.
 *
 * @note Single-threaded entry.
 *
 * @since 0.1.0
 */
int main(void)
{
  g_tz_nsc_cgc_usb_s_fallback_count = 1U;

  for (;;) {
    __asm__ volatile("nop");
  }
}
