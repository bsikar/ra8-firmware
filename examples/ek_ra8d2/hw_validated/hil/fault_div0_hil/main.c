/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/fault_div0_hil/main.c
 * @brief Prove CCR.DIV_0_TRP: a zero divisor must raise a decoded UsageFault.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The shared boot (`libs/ra8_board_ek_ra8d2/boot/system_init.c`) sets
 * `SHCSR.USGFAULTENA` and `CCR.DIV_0_TRP`, so an integer SDIV/UDIV by
 * zero must no longer silently return 0 -- it must raise a UsageFault
 * that the per-app vector table's trampoline forwards into
 * `ra8_exception_report()`. This app closes the loop on real silicon:
 *
 * 1. Bring up clocks + the SCI8 J-Link VCOM console, and register the
 *    console as the `ra8_log` byte sink so the fault handler's dump is
 *    visible on the bench (the default ITM path drops every byte in
 *    fault context; a registered sink does not).
 * 2. Read back `CCR` and confirm `DIV_0_TRP` is set (proves the boot
 *    write landed) -- `fault-div0: trap armed`.
 * 3. Perform a guarded volatile `u32 / 0`.
 *
 * On silicon the divide never completes: the CPU takes UsageFault,
 * the decoded dump prints (`exception=6`, `cfsr =33554432` -- that is
 * 0x02000000 = CFSR.DIVBYZERO -- plus pc/lr/xpsr), and the handler
 * parks at `ra8_exception_halt_loop`. The `hil.conf` gate scrapes that
 * dump.
 *
 * @note **Headless-emulator status.** `tools/ra8_emulator` models this trap:
 * a DIV_0_TRP-gated UDIV/SDIV seam takes the decoded UsageFault when (and
 * only when) the firmware has armed `CCR.DIV_0_TRP` and the divisor is
 * zero, so the emulator run reproduces the silicon dump headlessly
 * (`exception=6`, `cfsr =33554432`). The `survived divide` branch below is
 * therefore the on-silicon negative fallback -- reached only if the trap
 * ever fails to fire -- not the fake's normal path. The bench has captured
 * the real fault dump on silicon (tracker issue #191), so the app lives in
 * `hw_validated/hil/`.
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

/** @enum fd_consts_t @brief Console + divide-test knobs (no magic numbers). */
typedef enum : uint32_t {
  k_fd_uart_baud = 115200U, /**< SCI8 J-Link VCOM console baud.              */
  k_fd_dividend  = 100U,    /**< Numerator for the guarded zero-divide.      */
  k_fd_divisor   = 0U,      /**< The zero divisor that must trap on silicon. */
} fd_consts_t;

/** @enum fd_scb_addr_t @brief ARMv8-M SCB register addresses this app reads. */
typedef enum : uintptr_t {
  k_fd_scb_ccr_addr = 0xE000ED14UL, /**< Configuration and Control Register. */
} fd_scb_addr_t;

/** @enum fd_ccr_bits_t @brief CCR bits checked by the arming probe. */
typedef enum : uint32_t {
  k_fd_ccr_div_0_trp = 1UL << 4, /**< ARMv8-M CCR.DIV_0_TRP -- divide-by-zero traps. */
} fd_ccr_bits_t;

static const uint8_t k_msg_boot[]     = "fault-div0: boot\r\n";
static const uint8_t k_msg_fail[]     = "fault-div0: FAIL init\r\n";
static const uint8_t k_msg_armed[]    = "fault-div0: trap armed\r\n";
static const uint8_t k_msg_unarmed[]  = "fault-div0: FAIL trap not armed\r\n";
static const uint8_t k_msg_survived[] = "fault-div0: survived divide quotient=";
static const uint8_t k_msg_emu_tail[] = " (trap not modelled -- ra8_emulator)\r\n";

/**
 * @var s_fd_dividend
 * @brief Volatile numerator so the divide executes at run time.
 * @details Volatile (and file-scope) so the compiler can neither
 *          constant-fold the division nor prove the divisor zero and
 *          elide / poison the code path.
 * @note Written once at load; read once by `main`.
 * @warning Do not mark const -- the volatile load IS the test.
 * @since 0.1.0
 */
static volatile uint32_t s_fd_dividend = (uint32_t)k_fd_dividend;

/**
 * @var s_fd_divisor
 * @brief Volatile zero divisor -- the UDIV of this value must trap.
 * @details See ::s_fd_dividend; same reasoning, opposite operand.
 * @note Written once at load; read once by `main`.
 * @warning Stays zero for the app's whole life.
 * @since 0.1.0
 */
static volatile uint32_t s_fd_divisor = (uint32_t)k_fd_divisor;

/**
 * @var g_fd_quotient
 * @brief Quotient observed when the trap did NOT fire (emulator survival path).
 * @details 0 by ARM default divide-by-zero semantics; exported volatile
 *          for headless probing.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_fd_quotient = 0U;

/**
 * @var g_fd_heartbeat
 * @brief Bumps forever in the survival idle loop -- liveness for probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_fd_heartbeat = 0U;

/**
 * @brief Emit a byte run on the SCI8 console.
 *
 * @details Thin wrapper discarding the write status: console loss is
 *          acceptable for a print helper, the HIL gate re-checks output.
 * @param[in] msg Bytes to send (not NUL-inspected).
 * @param[in] len Number of bytes.
 * @pre `msg` points at `len` readable bytes.
 * @pre `ra8_board_uart_console_init` succeeded.
 * @post `len` bytes were pushed to the console (or dropped on error).
 * @post No other state modified.
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void fd_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/**
 * @brief `ra8_log` byte sink: forward every log byte to the SCI8 console.
 *
 * @details
 * The fault path (`ra8_exception_report`) logs through `ra8_log`, whose
 * default ITM backend deliberately drops every byte in fault context
 * (IPSR != 0). A registered byte sink bypasses that gate, so routing
 * it at the console makes the decoded dump visible on the bench UART.
 * The console write is polled (no IRQs needed), so it is safe from
 * the fault handler.
 *
 * @param[in] ctx  Unused opaque cookie (sink ABI).
 * @param[in] byte Log byte to emit.
 *
 * @pre `ra8_board_uart_console_init` succeeded.
 * @pre Registered via `ra8_log_set_byte_sink`.
 * @post The byte was pushed to the console (or dropped on error).
 * @post No other state modified.
 * @note Callable from fault context (polled console write).
 * @since 0.1.0
 */
static void fd_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)ra8_board_uart_console_write(&byte, 1U);
}

/**
 * @brief Print the fail banner and trap (ra8_emulator halts on the BKPT).
 *
 * @details Mirrors the sibling apps' panic idiom: one negative banner
 *          the HIL gate can match, then a debugger-visible stop.
 * @param[in] msg Banner bytes.
 * @param[in] len Banner length.
 * @pre The console is up (best effort -- the write may be dropped).
 * @pre Called only on an unrecoverable bring-up failure.
 * @post Never returns; CPU parks in a BKPT + WFI loop.
 * @post The negative banner was pushed to the console.
 * @note Not thread-safe (terminal path).
 * @since 0.1.0
 */
[[noreturn]] static void fd_panic_halt(const uint8_t* msg, uint32_t len)
{
  fd_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Print a small unsigned integer in decimal (quotient display).
 *
 * @details Emits at most 10 digits; 0 prints as "0". Digits are pushed
 *          LSB-first into a local buffer, then emitted MSB-first.
 * @param[in] value Value to print.
 * @pre The console is up.
 * @pre `value` is any uint32 (full range supported).
 * @post The decimal text was pushed to the console.
 * @post No other state modified.
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void fd_print_uint(uint32_t value)
{
  enum : uint32_t {
    k_fd_dec_base   = 10U, /**< Decimal base.                */
    k_fd_dec_digits = 10U, /**< Max digits in a uint32 (4G). */
  };
  uint8_t  buf[k_fd_dec_digits];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_fd_dec_digits)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_fd_dec_base));
    n++;
    value /= (uint32_t)k_fd_dec_base;
  }
  for (uint32_t i = 0U; i < n; i++) {
    fd_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Bring up clocks / MSTP + the SCI8 console; halt on failure.
 *
 * @details Minimal bring-up: CGC, MSTP, console. No timers -- the app
 *          needs no delays, only one divide.
 * @pre Running after Reset_Handler with .data/.bss initialised.
 * @pre The J-Link OB VCOM bridge is on SCI8 PD02/PD03 (stock EK-RA8D2).
 * @post The console accepts writes at ::k_fd_uart_baud.
 * @post On any init failure the CPU is parked via ::fd_panic_halt.
 * @note Not thread-safe; boot context only.
 * @since 0.1.0
 */
static void fd_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    fd_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    fd_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_fd_uart_baud) != k_ra8_ok) {
    fd_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  fd_setup_or_halt();
  ra8_isr_globals_enable();
  /* Route the fault handler's ra8_log dump onto the bench console. */
  ra8_log_set_byte_sink(fd_log_sink, nullptr);
  fd_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  /* Prove the boot-path write landed before relying on it: CCR must
   * read back with DIV_0_TRP set (works on silicon AND ra8_emulator --
   * the fake's SCS window stores the write even though its CPU model
   * ignores it). ARMv8-M SCB->CCR, read-only probe. */
  const uint32_t ccr = *(volatile uint32_t*)k_fd_scb_ccr_addr;
  if ((ccr & (uint32_t)k_fd_ccr_div_0_trp) == 0U) {
    fd_panic_halt(k_msg_unarmed, (uint32_t)sizeof(k_msg_unarmed) - 1U);
  }
  fd_print(k_msg_armed, (uint32_t)sizeof(k_msg_armed) - 1U);

  /* The guarded zero-divide. Volatile operands force a run-time UDIV.
   * Silicon: UsageFault here -> decoded dump prints -> halt (the app
   * never reaches the next line). ra8_emulator: quotient 0, fall through. */
  const uint32_t quotient = s_fd_dividend / s_fd_divisor;

  g_fd_quotient = quotient;
  fd_print(k_msg_survived, (uint32_t)sizeof(k_msg_survived) - 1U);
  fd_print_uint(quotient);
  fd_print(k_msg_emu_tail, (uint32_t)sizeof(k_msg_emu_tail) - 1U);

  while (1) {
    g_fd_heartbeat++;
    __asm__ volatile("wfi");
  }
  return 0;
}
#pragma GCC diagnostic pop
