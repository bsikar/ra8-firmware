/**
 * @file examples/ek_ra8d2/hw_validated/hil/fault_crashlog_hil/main.c
 * @brief Prove the cross-reset crash-log + reset-loop guard (T2-03, #191).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `ra8_crashlog` (libs/ra8_core) persists the decoded fault snapshot into a
 * `.noinit` record the reset handler does not zero, counts crash-reboot
 * cycles, and latches a safe-mode request once the count crosses a
 * threshold. This app drives that state machine end to end so it is
 * provable BOTH headlessly (ra8_emulator) and on the bench:
 *
 * 1. Bring up clocks + the SCI8 J-Link VCOM console and route `ra8_log`
 *    there so a real fault dump would be visible.
 * 2. `ra8_crashlog_install()` -- arm the exception-persist hook.
 * 3. Peek the record. On a warm/watchdog reset a prior record survives and
 *    its `exc pc boot_loops` print; a cold boot (or the first ra8_emulator
 *    run) reads empty. If the loop counter crossed the threshold,
 *    `safe_mode_requested()` latches: print SAFE-MODE, `claim()` to reset
 *    the guard, and idle instead of running the risky path.
 * 4. In-process write+decode proof: synthesise a decoded snapshot and push
 *    it through the installed write path (`ra8_crashlog_record_fault`), then
 *    peek it back. This is the ONLY leg ra8_emulator can show -- it cold-loads
 *    on reset (no warm-reset RAM survival) and its Unicorn core cannot take
 *    a real CPU fault -- so it stands in for the cross-reset write here. The
 *    record is left in place so a bench warm reset shows it survive and the
 *    `boot_loops` count climb toward safe mode.
 *
 * @note **Headless vs bench.** ra8_emulator proves boot bring-up, the peek/
 * claim/safe-mode logic, and the synthesised write+read-back. The genuine
 * fault->hook->record path is proven in-process by the host unit test
 * (tests/test_ra8_crashlog.c, which drives `ra8_exception_report`); the real
 * cross-reset survival + loop climb is a bench-only leg (a warm/watchdog
 * reset that keeps SRAM powered) -- see README.md. The sibling
 * `fault_div0_hil` provokes the real CPU fault this app deliberately does
 * not.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_crashlog.h"
#include "ra8_err.h"
#include "ra8_exception.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_mstp.h"

/** @enum fc_consts_t @brief Console + hex-printer knobs (no magic numbers). */
typedef enum : uint32_t {
  k_fc_uart_baud    = 115200U, /**< SCI8 J-Link VCOM console baud.    */
  k_fc_hex_nib_mask = 0xFU,    /**< Low-nibble mask for hex printing. */
} fc_consts_t;

/** @enum fc_hex_t @brief Hex-printer iteration constants. */
typedef enum : uint8_t {
  k_fc_hex_nib_bits = 4U, /**< Bits per hex nibble.       */
  k_fc_hex_digits   = 8U, /**< Nibbles in a uint32 value. */
} fc_hex_t;

/** @enum fc_synth_t @brief Synthetic decoded-fault values for the write proof. */
typedef enum : uint32_t {
  k_fc_synth_exc  = 6U,          /**< Pretend UsageFault class.                  */
  k_fc_synth_pc   = 0x02001234U, /**< Representative faulting PC (MRAM).         */
  k_fc_synth_lr   = 0x02001200U, /**< Representative link register.              */
  k_fc_synth_cfsr = 0x02000000U, /**< CFSR.DIVBYZERO, mirrors fault_div0's dump. */
} fc_synth_t;

static const uint8_t k_msg_boot[]     = "crashlog: boot\r\n";
static const uint8_t k_msg_fail[]     = "crashlog: FAIL init\r\n";
static const uint8_t k_msg_none[]     = "crashlog: no prior record (cold/first boot)\r\n";
static const uint8_t k_msg_prior[]    = "crashlog: prior record exc=";
static const uint8_t k_msg_pc[]       = " pc=";
static const uint8_t k_msg_loops[]    = " boot_loops=";
static const uint8_t k_msg_safemode[] = "crashlog: SAFE-MODE requested -- reset loop, idling\r\n";
static const uint8_t k_msg_recorded[] = "crashlog: recorded+readback exc=";
static const uint8_t k_msg_crlf[]     = "\r\n";

/**
 * @brief Emit a byte run on the SCI8 console.
 *
 * @details Thin wrapper discarding the write status: console loss is
 *          acceptable for a print helper; the HIL gate re-checks output.
 * @param[in] msg Bytes to send (not NUL-inspected).
 * @param[in] len Number of bytes.
 * @pre `msg` points at `len` readable bytes.
 * @pre `ra8_board_uart_console_init` succeeded.
 * @post `len` bytes were pushed to the console (or dropped on error).
 * @post No other state modified.
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void fc_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/**
 * @brief Print a uint32 in decimal (0 prints as "0").
 *
 * @details Digits are pushed LSB-first into a local buffer then emitted
 *          MSB-first; at most 10 digits (uint32 max is 4294967295).
 * @param[in] value Value to print.
 * @pre The console is up.
 * @pre `value` is any uint32 (full range supported).
 * @post The decimal text was pushed to the console.
 * @post No other state modified.
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void fc_print_uint(uint32_t value)
{
  enum : uint32_t {
    k_fc_dec_base   = 10U, /**< Decimal base.                */
    k_fc_dec_digits = 10U, /**< Max digits in a uint32 (4G). */
  };
  uint8_t  buf[k_fc_dec_digits];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_fc_dec_digits)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_fc_dec_base));
    n++;
    value /= (uint32_t)k_fc_dec_base;
  }
  for (uint32_t i = 0U; i < n; i++) {
    fc_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Print a uint32 as "0x" + 8 fixed hex digits (MSB-first).
 *
 * @details Fixed-width so a faulting PC always reads as a full address.
 * @param[in] value Value to print in hexadecimal.
 * @pre The console is up.
 * @pre `value` is any uint32.
 * @post The "0x"-prefixed 8-digit text was pushed to the console.
 * @post No other state modified.
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void fc_print_hex32(uint32_t value)
{
  static const uint8_t k_hex[]  = "0123456789ABCDEF";
  const uint8_t        prefix[] = {'0', 'x'};
  fc_print(prefix, (uint32_t)sizeof(prefix));
  for (uint32_t i = 0U; i < (uint32_t)k_fc_hex_digits; i++) {
    const uint32_t shift = (uint32_t)((k_fc_hex_digits - 1U - i) * (uint32_t)k_fc_hex_nib_bits);
    const uint32_t nib   = (value >> shift) & (uint32_t)k_fc_hex_nib_mask;
    fc_print(&k_hex[nib], 1U);
  }
}

/**
 * @brief `ra8_log` byte sink: forward every log byte to the SCI8 console.
 *
 * @details The fault path logs through `ra8_log`, whose default ITM backend
 *          drops every byte in fault context; a registered polled sink does
 *          not, making a real dump visible on the bench UART.
 * @param[in] ctx  Unused opaque cookie (sink ABI).
 * @param[in] byte Log byte to emit.
 * @pre `ra8_board_uart_console_init` succeeded.
 * @pre Registered via `ra8_log_set_byte_sink`.
 * @post The byte was pushed to the console (or dropped on error).
 * @post No other state modified.
 * @note Callable from fault context (polled console write).
 * @since 0.1.0
 */
static void fc_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)ra8_board_uart_console_write(&byte, 1U);
}

/**
 * @brief Print the fail banner and trap (ra8_emulator halts on the BKPT).
 *
 * @param[in] msg Banner bytes.
 * @param[in] len Banner length.
 * @pre The console is up (best effort -- the write may be dropped).
 * @pre Called only on an unrecoverable bring-up failure.
 * @post Never returns; CPU parks in a BKPT + WFI loop.
 * @post The negative banner was pushed to the console.
 * @note Not thread-safe (terminal path).
 * @since 0.1.0
 */
[[noreturn]] static void fc_panic_halt(const uint8_t* msg, uint32_t len)
{
  fc_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up clocks / MSTP + the SCI8 console; halt on failure.
 *
 * @details Minimal bring-up: CGC, MSTP, console. No timers needed.
 * @pre Running after Reset_Handler with .data/.bss initialised.
 * @pre The J-Link OB VCOM bridge is on SCI8 (stock EK-RA8D2).
 * @post The console accepts writes at ::k_fc_uart_baud.
 * @post On any init failure the CPU is parked via ::fc_panic_halt.
 * @note Not thread-safe; boot context only.
 * @since 0.1.0
 */
static void fc_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    fc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    fc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_fc_uart_baud) != k_ra8_ok) {
    fc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/**
 * @brief Print a decoded record's exception class, PC, and loop count.
 *
 * @param[in] rec Validated record to print. Must not be `nullptr`.
 * @pre The console is up.
 * @pre @p rec came from a successful ra8_crashlog_peek().
 * @post One `exc=.. pc=.. boot_loops=..` line was pushed to the console.
 * @post No other state modified.
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void fc_print_record(const ra8_crashlog_record_t* rec)
{
  if (rec == nullptr) {
    return;
  }
  fc_print(k_msg_prior, (uint32_t)sizeof(k_msg_prior) - 1U);
  fc_print_uint(rec->fault.exc_number);
  fc_print(k_msg_pc, (uint32_t)sizeof(k_msg_pc) - 1U);
  fc_print_hex32(rec->fault.frame.pc);
  fc_print(k_msg_loops, (uint32_t)sizeof(k_msg_loops) - 1U);
  fc_print_uint(rec->boot_loops);
  fc_print(k_msg_crlf, (uint32_t)sizeof(k_msg_crlf) - 1U);
}

/**
 * @brief Fill a decoded snapshot with representative synthetic fault values.
 *
 * @param[out] out Snapshot to populate. Must not be `nullptr`.
 * @pre @p out points at writable storage.
 * @pre Used only for the in-process write+decode proof.
 * @post `*out` holds a fully-populated ::ra8_exception_last_t.
 * @post `out->magic == k_ra8_exc_magic_valid`.
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void fc_make_synth(ra8_exception_last_t* out)
{
  if (out == nullptr) {
    return;
  }
  *out            = (ra8_exception_last_t){};
  out->magic      = (uint32_t)k_ra8_exc_magic_valid;
  out->exc_number = (uint32_t)k_fc_synth_exc;
  out->frame.pc   = (uint32_t)k_fc_synth_pc;
  out->frame.lr   = (uint32_t)k_fc_synth_lr;
  out->diag.cfsr  = (uint32_t)k_fc_synth_cfsr;
}

void main(void)
{
  fc_setup_or_halt();
  ra8_isr_globals_enable();
  ra8_log_set_byte_sink(fc_log_sink, nullptr);
  fc_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  /* Arm the persist hook so any subsequent real fault is captured, then
   * read whatever a prior (warm-reset) boot left behind. */
  ra8_crashlog_install();

  ra8_crashlog_record_t rec = {};
  if (ra8_crashlog_peek(&rec)) {
    fc_print_record(&rec);
    if (ra8_crashlog_safe_mode_requested()) {
      fc_print(k_msg_safemode, (uint32_t)sizeof(k_msg_safemode) - 1U);
      ra8_crashlog_claim(); /* clean claim: reset the loop guard */
      while (1) {
        __asm__ volatile("wfi");
      }
    }
  } else {
    fc_print(k_msg_none, (uint32_t)sizeof(k_msg_none) - 1U);
  }

  /* In-process write+decode proof (the only leg ra8_emulator can show): push
   * a synthesised decoded snapshot through the installed write path, then
   * read it back. Left in place so a bench warm reset shows survival. */
  ra8_exception_last_t synth = {};
  fc_make_synth(&synth);
  ra8_crashlog_record_fault(&synth);

  if (ra8_crashlog_peek(&rec)) {
    fc_print(k_msg_recorded, (uint32_t)sizeof(k_msg_recorded) - 1U);
    fc_print_uint(rec.fault.exc_number);
    fc_print(k_msg_pc, (uint32_t)sizeof(k_msg_pc) - 1U);
    fc_print_hex32(rec.fault.frame.pc);
    fc_print(k_msg_loops, (uint32_t)sizeof(k_msg_loops) - 1U);
    fc_print_uint(rec.boot_loops);
    fc_print(k_msg_crlf, (uint32_t)sizeof(k_msg_crlf) - 1U);
  } else {
    fc_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }

  while (1) {
    __asm__ volatile("wfi");
  }
}
