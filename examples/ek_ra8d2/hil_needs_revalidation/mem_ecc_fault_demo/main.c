/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/mem_ecc_fault_demo/main.c
 * @brief SRAM ECC fault-injection + detection demo (EK-RA8D2, issue #130)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Where ``ecc_monitor_demo`` only brings ECC up and reads a (clean) status,
 * this demo closes the loop the SIL-3 / DAL-B bar actually cares about: it
 * *deliberately provokes* a memory error and proves the hardware-error path
 * latches it.
 *
 * It enables full SECDED ECC on a spare SRAM bank (bank 2, which the linker
 * leaves unused), then runs the HUM Ch 58.3.4 ECC decoder self-test twice via
 * ``ra8_sram_self_test``:
 *
 *  1. A **1-bit** (correctable) fault -- ``ra8_sram_self_test(..., false, ...)``
 *     reads the raw syndrome in bypass mode, flips one bit, writes it back, and
 *     the verify read latches ``SRAMESR`` -- ``out_caught`` confirms the latch.
 *  2. A **2-bit** (uncorrectable) fault -- the same path with two bits flipped,
 *     which on silicon also raises the NMI (see the silicon note below).
 *
 * The ESR status is cleared between injections so each is independently proven.
 * The detected per-bank 1-bit / 2-bit masks are published to globals for HIL
 * probing. Once a second the headless banner is emitted:
 *
 *   ``"ecc: sram2 1bit-inj=caught 2bit-inj=caught ok=Y\r\n"``  (both latched), or
 *   ``"ecc: sram2 fault-inject ok=N\r\n"``                     (any miss).
 *
 * LED1 toggles while both injections are caught; LED2 toggles on a miss.
 *
 * @note **Headless-emulator status.** ``tools/ra8_emulator`` (board_periph_sram.c)
 * models the decoder self-test: it latches ``SRAMESR`` on the bypass->verify CR
 * sequence so ``out_caught`` is true headlessly, proving the
 * detection/reporting plumbing end to end. It cannot observe the syndrome data
 * write that distinguishes a 1-bit from a 2-bit fault, so it latches *both*
 * ESR slots on any self-test; the precise per-slot fidelity (and 1-bit
 * correction vs the 2-bit NMI) is silicon-only -- hence ``hw_pending/``.
 *
 * @note **Silicon note.** The 2-bit (uncorrectable) injection raises a
 * non-maskable ECC interrupt on real hardware (``on_error = interrupt``); the
 * bench bring-up must install an NMI handler (or mask faults around the 2-bit
 * self-test). ra8_emulator does not raise the NMI, so the headless run reads
 * ``SRAMESR`` synchronously and reports cleanly.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
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
static const char* s_tag = "mem_ecc";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_mecc_baud      = 115200U, /**< SCI8 baud rate.                       */
  k_mecc_period_ms = 1000U,   /**< Delay between fault-injection passes. */
} mecc_config_t;

/** @brief ECC target geometry. */
typedef enum : uint32_t {
  k_mecc_bank         = 2U, /**< Spare SRAM bank 2 (program uses 0-1).      */
  k_mecc_probe_offset = 0U, /**< 8-byte-aligned probe line within the bank. */
} mecc_geom_t;

/** @brief Output banners. */
static const uint8_t k_mecc_ok_msg[]  = "ecc: sram2 1bit-inj=caught 2bit-inj=caught ok=Y\r\n";
static const uint8_t k_mecc_bad_msg[] = "ecc: sram2 fault-inject ok=N\r\n";

/**
 * @var g_mecc_1bit_caught
 * @brief 1 when the injected 1-bit fault was latched by SRAMESR.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_mecc_1bit_caught = 0U;

/**
 * @var g_mecc_2bit_caught
 * @brief 1 when the injected 2-bit fault was latched by SRAMESR.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_mecc_2bit_caught = 0U;

/**
 * @var g_mecc_1bit_mask
 * @brief Per-bank 1-bit-error bitmap decoded after the 1-bit injection.
 * @note Read externally only. Full per-slot fidelity on silicon.
 * @since 0.1.0
 */
volatile uint32_t g_mecc_1bit_mask = 0U;

/**
 * @var g_mecc_2bit_mask
 * @brief Per-bank 2-bit-error bitmap decoded after the 2-bit injection.
 * @note Read externally only. Full per-slot fidelity on silicon.
 * @since 0.1.0
 */
volatile uint32_t g_mecc_2bit_mask = 0U;

/**
 * @var g_mecc_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_mecc_heartbeat = 0U;

/** @brief Park forever after a fatal init failure. */
static void mecc_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void mecc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    mecc_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    mecc_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    mecc_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    mecc_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_mecc_baud) != k_ra8_ok) {
    mecc_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    mecc_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    mecc_panic_halt();
  }
}

/**
 * @brief Enable full ECC + zero-init on spare bank 2.
 *
 * @details Bank 2 gets with-check ECC, the 1-bit latch, a 128 KiB ECC region,
 * and the zero-init pass that lays down valid ECC across the bank (so a read of
 * a never-written line does not raise a spurious 2-bit error before the
 * deliberate injection). ``on_error`` is NMI -- see the file's silicon note.
 *
 * @par MC/DC:
 * Sequential init guards (no compound decision); 2 vectors -- init OK / init
 * rejects (covered by the host test).
 *
 * @return ``ra8_err_t`` from ``ra8_sram_init``.
 * @retval k_ra8_ok Bank 2 configured for ECC with-check.
 * @pre CGC + MSTP up; IRQs masked or single-threaded init.
 * @pre Bank 2 holds no program data (linker keeps it spare).
 * @post Bank 2's SRAMCRn is in ECC + check mode.
 * @post No ECC error is latched yet (zero-init laid down valid codes).
 * @note Not thread-safe; boot-time use.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t mecc_configure(void)
{
  ra8_sram_config_t cfg                    = {};
  cfg.banks[k_mecc_bank].ecc_mode          = k_ra8_sram_ecc_with_chk;
  cfg.banks[k_mecc_bank].on_error          = k_ra8_sram_on_error_interrupt;
  cfg.banks[k_mecc_bank].enable_1bit_latch = true;
  cfg.banks[k_mecc_bank].eccrgn            = k_ra8_sram_region_128kb;
  cfg.banks[k_mecc_bank].zero_init         = true;
  return ra8_sram_init(&cfg);
}

/**
 * @brief Inject one ECC fault via the decoder self-test and record the verdict.
 *
 * @details Runs ``ra8_sram_self_test`` on bank 2 (which corrupts the syndrome of
 * the probe line and confirms the SRAMESR latch), snapshots the decoded
 * error masks for HIL probing, then clears the latched status so the next
 * injection starts clean.
 *
 * @param[in]  two_bit    ``true`` to inject a 2-bit (uncorrectable) fault.
 * @param[out] out_caught Receives 1 when SRAMESR latched the injected fault.
 *
 * @return ``ra8_err_t`` from the self-test / status calls.
 * @retval k_ra8_ok               Injection ran; ``*out_caught`` set.
 * @retval k_ra8_err_null_ptr     ``out_caught`` was NULL.
 * @retval k_ra8_err_invalid_arg  Bank / probe rejected by ``ra8_sram_self_test``.
 * @pre ::mecc_configure succeeded.
 * @pre ``out_caught`` is non-NULL.
 * @post SRAMESR latched status is cleared on success.
 * @post The decoded mask is published to the matching global.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t mecc_inject(bool two_bit, uint8_t* out_caught)
{
  RA8_CHECK_NULL_PTR(out_caught, s_tag, "out_caught must not be nullptr");

  bool            caught = false;
  const ra8_err_t serr =
    ra8_sram_self_test((uint8_t)k_mecc_bank, (uint32_t)k_mecc_probe_offset, two_bit, &caught);
  if (serr != k_ra8_ok) {
    return serr;
  }

  ra8_sram_status_t st   = {};
  const ra8_err_t   gerr = ra8_sram_get_status(&st);
  if (gerr != k_ra8_ok) {
    return gerr;
  }
  if (two_bit) {
    g_mecc_2bit_mask = (uint32_t)st.two_bit_mask;
  } else {
    g_mecc_1bit_mask = (uint32_t)st.one_bit_mask;
  }

  const ra8_err_t cerr = ra8_sram_clear_status(st.raw_esr);
  if (cerr != k_ra8_ok) {
    return cerr;
  }

  *out_caught = caught ? 1U : 0U;
  return k_ra8_ok;
}

/**
 * @brief Run both fault injections and fold them into a single verdict.
 *
 * @param[out] out_ok 1 only when both the 1-bit and 2-bit faults were caught.
 *
 * @return ``ra8_err_t`` from the injection helpers.
 * @retval k_ra8_ok           Both injections ran; ``*out_ok`` set.
 * @retval k_ra8_err_null_ptr ``out_ok`` was NULL.
 * @pre ::mecc_configure succeeded.
 * @pre ``out_ok`` is non-NULL.
 * @post ``g_mecc_1bit_caught`` / ``g_mecc_2bit_caught`` updated.
 * @post The latched ECC status is cleared after both injections.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t mecc_run_pass(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");
  *out_ok = 0U;

  uint8_t         caught1 = 0U;
  const ra8_err_t e1      = mecc_inject(false, &caught1);
  if (e1 != k_ra8_ok) {
    return e1;
  }
  g_mecc_1bit_caught = (uint32_t)caught1;

  uint8_t         caught2 = 0U;
  const ra8_err_t e2      = mecc_inject(true, &caught2);
  if (e2 != k_ra8_ok) {
    return e2;
  }
  g_mecc_2bit_caught = (uint32_t)caught2;

  if (caught1 == 0U) {
    return k_ra8_ok;
  }
  if (caught2 == 0U) {
    return k_ra8_ok;
  }
  *out_ok = 1U;
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  mecc_setup_or_halt();
  /* Clear PRIMASK so SysTick drives ra8_delay_ms(); the ECC NMI is
   * non-maskable and unaffected (no maskable NVIC source is armed). */
  ra8_isr_globals_enable();

  if (mecc_configure() != k_ra8_ok) {
    mecc_panic_halt();
  }

  while (1) {
    uint8_t         ok   = 0U;
    const ra8_err_t err  = mecc_run_pass(&ok);
    uint8_t         good = 0U;
    if (err == k_ra8_ok) {
      good = ok;
    }
    if (good != 0U) {
      (void)ra8_board_uart_console_write(k_mecc_ok_msg, (size_t)(sizeof(k_mecc_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_mecc_bad_msg, (size_t)(sizeof(k_mecc_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ++g_mecc_heartbeat;
    ra8_delay_ms(k_mecc_period_ms);
  }
  mecc_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
