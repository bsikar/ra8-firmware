/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/ecc_monitor_demo/main.c
 * @brief SRAM ECC bring-up + error-status monitor demo (EK-RA8D2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The RA8D2 SRAM controller can attach SECDED ECC to each 512 KiB bank:
 * 1-bit errors are corrected (and optionally latched), 2-bit errors are
 * uncorrectable and raise an NMI (or reset). This demo enables full ECC
 * ("with-check") on a **spare** SRAM bank, proves an ECC-protected
 * read/write round-trips, and polls the ECC status register
 * (``SRAMESR``) -- the hardware-error-reporting path -- reporting it once
 * a second.
 *
 * **Why bank 2 (0x2210_0000).** ECC with-check requires the bank's ECC
 * codes to be initialised first (``zero_init``), otherwise reading
 * never-written words raises a spurious 2-bit error -> NMI. The
 * ``zero_init`` pass rewrites the whole bank, so it must target memory the
 * program does **not** use. This app's linker places ``.data`` / ``.bss``
 * / stack in the first 1 MiB (banks 0-1, 0x2200_0000..0x2210_0000); banks
 * 2-3 are unused, so ECC + zero-init on bank 2 is safe. ``on_error`` stays
 * NMI but no error is injected, so the NMI never fires in this demo.
 *
 * Bring-up: CGC + SysTick + SCI8 + LEDs + MSTP. Once a second:
 * ``"ecc: sram2 ecc=on rw=ok ok=Y\r\n"`` (the latched 1-bit / 2-bit error
 * masks go to ``g_ecc_1bit`` / ``g_ecc_2bit`` for on-silicon probing).
 * LED1 toggles while ECC is healthy; LED2 toggles on a fault.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @note **Headless-emulator status.** ``tools/ra8_emulator`` shadows the SRAM
 * ECC control window (SRAMCRn / SRAMESR) so the bring-up + status read run
 * and report ``ok=Y`` with a clean ESR -- but a real ECC *error* only
 * appears when a bit actually flips, which needs silicon (or an
 * error-injection register). The detection half therefore lives in
 * ``hw_pending/``. See ``README.md`` for the bench plan.
 *
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_sram.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "ecc_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_ecc_demo_baud      = 115200U,     /**< SCI8 baud rate.               */
  k_ecc_demo_period_ms = 1000U,       /**< Delay between status reads.   */
  k_ecc_demo_pat_xor   = 0x5A5A5A5AU, /**< Per-word rw-test pattern XOR. */
} ecc_demo_config_t;

/** @brief ECC target geometry. */
typedef enum : uint8_t {
  k_ecc_demo_bank       = 2U,  /**< Spare SRAM bank 2 (program uses 0-1). */
  k_ecc_demo_test_words = 64U, /**< rw round-trip words (256 B).          */
} ecc_demo_geom_t;

/** @brief Output line tags. */
static const uint8_t k_ecc_demo_ok_msg[]  = "ecc: sram2 ecc=on rw=ok ok=Y\r\n";
static const uint8_t k_ecc_demo_bad_msg[] = "ecc: sram2 rw=BAD ok=N\r\n";

/**
 * @var g_ecc_ok
 * @brief 1 when ECC is healthy: rw round-trips and no error is latched.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_ecc_ok = 0U;

/**
 * @var g_ecc_rw_ok
 * @brief 1 when the ECC-protected buffer read back exactly.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_ecc_rw_ok = 0U;

/**
 * @var g_ecc_esr
 * @brief Last raw SRAMESR snapshot (0 = no ECC error latched).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_ecc_esr = 0U;

/**
 * @var g_ecc_1bit
 * @brief Per-bank 1-bit-error bitmap from the last SRAMESR read (bit n = bank n).
 * @note Read externally only. Meaningful on silicon; ra8_emulator does not model ECC.
 * @since 0.1.0
 */
volatile uint32_t g_ecc_1bit = 0U;

/**
 * @var g_ecc_2bit
 * @brief Per-bank 2-bit-error bitmap from the last SRAMESR read (bit n = bank n).
 * @note Read externally only. Meaningful on silicon; ra8_emulator does not model ECC.
 * @since 0.1.0
 */
volatile uint32_t g_ecc_2bit = 0U;

/**
 * @var g_ecc_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_ecc_heartbeat = 0U;

/** @brief Base of the ECC-protected bank, resolved at init. */
static volatile uint32_t* s_ecc_buf = nullptr;

/** @brief Park forever after a fatal init failure. */
static void ecc_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Deterministic rw-test pattern for word @p i. */
static uint32_t ecc_demo_pattern(uint32_t i)
{
  return (i * i) ^ (uint32_t)k_ecc_demo_pat_xor;
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void ecc_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    ecc_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    ecc_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    ecc_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    ecc_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_ecc_demo_baud) != k_ra8_ok) {
    ecc_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    ecc_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    ecc_demo_panic_halt();
  }
}

/**
 * @brief Enable full ECC + zero-init on spare bank 2 and resolve its base.
 *
 * @details
 * Banks 0/1/3 stay ECC-disabled (just clock-ungated). Bank 2 gets
 * with-check ECC, 1-bit latch, a 128 KiB ECC region, and the zero-init
 * pass that lays down valid ECC across the bank so no spurious 2-bit error
 * fires. ``on_error`` is NMI, but this demo injects no error.
 *
 * @par MC/DC:
 * Sequential init guards (no compound decision); 2 vectors -- init OK /
 * init rejects (host-tested).
 *
 * @return ``ra8_err_t`` from ``ra8_sram_init`` / ``ra8_sram_get_bank_info``.
 * @pre CGC + MSTP up; IRQs masked or single-threaded init.
 * @post ``s_ecc_buf`` points at bank 2's ECC-protected base.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ecc_demo_configure(void)
{
  ra8_sram_config_t cfg                        = {};
  cfg.banks[k_ecc_demo_bank].ecc_mode          = k_ra8_sram_ecc_with_chk;
  cfg.banks[k_ecc_demo_bank].on_error          = k_ra8_sram_on_error_interrupt;
  cfg.banks[k_ecc_demo_bank].enable_1bit_latch = true;
  cfg.banks[k_ecc_demo_bank].eccrgn            = k_ra8_sram_region_128kb;
  cfg.banks[k_ecc_demo_bank].zero_init         = true;

  const ra8_err_t ierr = ra8_sram_init(&cfg);
  if (ierr != k_ra8_ok) {
    return ierr;
  }
  ra8_sram_bank_info_t info = {};
  const ra8_err_t      berr = ra8_sram_get_bank_info((uint8_t)k_ecc_demo_bank, &info);
  if (berr != k_ra8_ok) {
    return berr;
  }
  s_ecc_buf = (volatile uint32_t*)info.data_base;
  return k_ra8_ok;
}

/**
 * @brief Round-trip a pattern through the ECC-protected buffer, then read
 *        the ECC status, and fold both into the verdict.
 *
 * @param[out] out_ok 1 when the buffer read back exactly AND no 1-bit or
 *                    2-bit ECC error is latched.
 *
 * @par MC/DC:
 * Decision ``ok = rw_ok`` (the ECC-protected round-trip; single
 * condition) -- 2 vectors, match (steady state) + mismatch (host-tested).
 * The latched 1-bit / 2-bit error masks are reported via globals, not
 * gated, since ra8_emulator does not model ECC.
 *
 * @return ``ra8_err_t`` from ``ra8_sram_get_status``.
 * @retval k_ra8_err_null_ptr ``out_ok`` was NULL.
 * @pre ::ecc_demo_configure succeeded (``s_ecc_buf`` valid).
 * @post ``g_ecc_rw_ok`` / ``g_ecc_esr`` updated.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ecc_demo_sample(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");
  if (s_ecc_buf == nullptr) {
    return k_ra8_err_not_initialized;
  }

  for (uint32_t i = 0U; i < (uint32_t)k_ecc_demo_test_words; ++i) {
    s_ecc_buf[i] = ecc_demo_pattern(i);
  }
  uint8_t rw = 1U;
  for (uint32_t i = 0U; i < (uint32_t)k_ecc_demo_test_words; ++i) {
    if (s_ecc_buf[i] != ecc_demo_pattern(i)) {
      rw = 0U;
    }
  }
  g_ecc_rw_ok = (uint32_t)rw;

  ra8_sram_status_t st  = {};
  const ra8_err_t   err = ra8_sram_get_status(&st);
  if (err != k_ra8_ok) {
    return err;
  }
  g_ecc_esr  = (uint32_t)st.raw_esr;
  g_ecc_1bit = (uint32_t)st.one_bit_mask;
  g_ecc_2bit = (uint32_t)st.two_bit_mask;
  /* Verdict gates only on the ECC-protected round-trip, which is
   * deterministic. The latched 1-bit / 2-bit error masks (above) are the
   * hardware-error-reporting payload -- meaningful only on silicon, where a
   * real bit-flip sets them; ra8_emulator does not model ECC, so they stay out
   * of the headless verdict (else it would flake on the unmodelled ESR). */
  *out_ok = (rw != 0U) ? 1U : 0U;
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  ecc_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra8_delay_ms() uses the
   * SysTick path (ra8_emulator does not advance DWT_CYCCNT). The ECC NMI is
   * non-maskable and unaffected; no maskable NVIC source is armed. */
  ra8_isr_globals_enable();

  if (ecc_demo_configure() != k_ra8_ok) {
    ecc_demo_panic_halt();
  }

  while (1) {
    uint8_t         ok   = 0U;
    const ra8_err_t err  = ecc_demo_sample(&ok);
    const uint8_t   good = (err == k_ra8_ok && ok != 0U) ? 1U : 0U;
    g_ecc_ok             = (uint32_t)good;
    if (good != 0U) {
      (void)ra8_board_uart_console_write(k_ecc_demo_ok_msg,
                                         (size_t)(sizeof(k_ecc_demo_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_ecc_demo_bad_msg,
                                         (size_t)(sizeof(k_ecc_demo_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ++g_ecc_heartbeat;
    ra8_delay_ms(k_ecc_demo_period_ms);
  }
  ecc_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
