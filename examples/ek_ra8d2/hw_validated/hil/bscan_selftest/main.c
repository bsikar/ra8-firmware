/**
 * @file examples/ek_ra8d2/hw_validated/hil/bscan_selftest/main.c
 * @brief Headless on-silicon self-test gate for the JTAG boundary-scan
 *        TAP bookkeeping driver `ra8_bscan` (#138).
 *
 * @details
 * The RA8D2 boundary-scan TAP (HUM Ch 50, p 3257-3262) is driven by an
 * external IEEE-1149.1 manufacturing-test fixture over the four JTAG pins
 * while RES is held low; its four registers (JTIR/JTIDR/JTBPR/JTBSR) are
 * *not* CPU-reachable. So the actual scan chain cannot be exercised from
 * firmware -- that validation is inherently external-tool-driven.
 *
 * What firmware *can* validate is its own contribution to boundary scan:
 * the `ra8_bscan` bookkeeping object (HUM-sourced IDCODE constant, opcode
 * validation, lifecycle). This app runs that contract end-to-end as a
 * power-on self-test -- no panel / SD / touch / JTAG fixture needed -- and
 * prints a deterministic banner on the SCI8 J-Link OB console:
 *
 *   `bscan: idcode=085DA447 checks=17 PASS`
 *
 * The IDCODE is the chip's hardwired device ID (`0x085D_A447`, HUM
 * Ch 50.2.2 p 3258) that the external fixture should scan out over TDO;
 * asserting it here is the firmware's authoritative cross-check. Every
 * sub-check (positive and negative API paths) must pass or the app halts
 * on a FAIL banner before reaching the PASS line, so the gate is exact.
 * The banner is identical on host, ra8_emulator, and silicon.
 *
 *
 * [Ring 7 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_bscan.h"
#include "ra8_bscan_regs.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @enum bs_consts_t @brief Console / self-check knobs (no magic numbers). */
typedef enum : uint32_t {
  k_bs_uart_baud    = 115200U, /**< Console baud.                        */
  k_bs_reserved_op  = 0x02U,   /**< A reserved JTIR opcode (HUM 50.2.1). */
  k_bs_clear_none   = 0x00U,   /**< Legal clear mask (the only one).     */
  k_bs_bad_mask     = 0x01U,   /**< Illegal non-zero clear mask.         */
  k_bs_checks_total = 17U,     /**< Count of self-checks below.          */
  k_bs_hex_nibbles  = 8U,      /**< Hex digits in a 32-bit value.        */
  k_bs_nibble_bits  = 4U,      /**< Bits per hex nibble.                 */
  k_bs_nibble_mask  = 0x0FU,   /**< Low-nibble mask.                     */
  k_bs_dec_ten      = 10U,     /**< Hex digit / decimal split.           */
} bs_consts_t;

static const uint8_t k_msg_boot[] = "bscan-selftest: boot\r\n";
static const uint8_t k_msg_fail[] = "bscan-selftest: FAIL init\r\n";
static const uint8_t k_msg_chk[]  = "bscan-selftest: FAIL check\r\n";
static const uint8_t k_msg_pre[]  = "bscan: idcode=";
static const uint8_t k_msg_mid[]  = " checks=";
static const uint8_t k_msg_ok[]   = " PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void bs_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void bs_panic_halt(const uint8_t* msg, uint32_t len)
{
  bs_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Self-check assertion: halt on the FAIL-check banner if @p ok is false. */
static void bs_require(bool ok)
{
  if (!ok) {
    bs_panic_halt(k_msg_chk, (uint32_t)sizeof(k_msg_chk) - 1U);
  }
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void bs_print_hex(uint32_t value)
{
  uint8_t buf[k_bs_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_bs_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_bs_hex_nibbles - 1U - i) * (uint32_t)k_bs_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_bs_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_bs_dec_ten) ? ('0' + nib) : ('A' + (nib - k_bs_dec_ten)));
  }
  bs_print(buf, (uint32_t)k_bs_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void bs_print_uint(uint32_t value)
{
  uint8_t  buf[k_bs_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_bs_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_bs_dec_ten));
    n++;
    value /= (uint32_t)k_bs_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    bs_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Run the full `ra8_bscan` CPU-side contract; halt on any failure.
 *
 * @details
 * Exercises every public entry point on both its positive and negative
 * paths: the IDCODE constant cross-check (the firmware's boundary-scan
 * contribution), opcode validation, the clear-mask guard, and the NULL
 * guards. Returns the chip device ID for the banner.
 *
 * @param[out] out_idcode Receives the JTIDR device ID read back.
 * @return The number of checks performed (== k_bs_checks_total).
 */
static uint32_t bs_run_checks(uint32_t* out_idcode)
{
  uint32_t           id = 0U;
  ra8_bscan_status_t st = {};

  bs_require(out_idcode != nullptr);                                                       /* 1  */
  bs_require(ra8_bscan_init() == k_ra8_ok);                                                /* 2  */
  bs_require(ra8_bscan_get_idcode(nullptr) == k_ra8_err_null_ptr);                         /* 3  */
  bs_require(ra8_bscan_get_idcode(&id) == k_ra8_ok);                                       /* 4  */
  bs_require(id == (uint32_t)k_ra8_bscan_jtidr_reset);                                     /* 5  */
  bs_require(ra8_bscan_get_status(nullptr) == k_ra8_err_null_ptr);                         /* 6  */
  bs_require(ra8_bscan_get_status(&st) == k_ra8_ok);                                       /* 7  */
  bs_require(st.initialized && (st.expected_idcode == (uint32_t)k_ra8_bscan_jtidr_reset)); /* 8  */
  bs_require(st.last_instruction == k_ra8_bscan_instr_bypass);                             /* 9  */
  bs_require(ra8_bscan_set_instruction(k_ra8_bscan_instr_idcode) == k_ra8_ok);             /* 10 */
  bs_require(ra8_bscan_get_status(&st) == k_ra8_ok);                                       /* 11 */
  bs_require(st.last_instruction == k_ra8_bscan_instr_idcode);                             /* 12 */
  bs_require(ra8_bscan_set_instruction((ra8_bscan_instr_t)k_bs_reserved_op) ==
             k_ra8_err_invalid_arg);                                                    /* 13 */
  bs_require(ra8_bscan_clear_status((uint32_t)k_bs_bad_mask) == k_ra8_err_invalid_arg); /* 14 */
  bs_require(ra8_bscan_clear_status((uint32_t)k_bs_clear_none) == k_ra8_ok);            /* 15 */
  bs_require(ra8_bscan_get_status(&st) == k_ra8_ok);                                    /* 16 */
  bs_require(st.last_instruction == k_ra8_bscan_instr_bypass);                          /* 17 */

  *out_idcode = id;
  return (uint32_t)k_bs_checks_total;
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void bs_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    bs_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    bs_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    bs_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_bs_uart_baud) != k_ra8_ok) {
    bs_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/**
 * @brief App entry: run the bscan self-check, print the banner.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The IDCODE/checks PASS banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
void main(void)
{
  bs_setup_or_halt();
  ra8_isr_globals_enable();
  bs_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  uint32_t       idcode = 0U;
  const uint32_t checks = bs_run_checks(&idcode);

  bs_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  bs_print_hex(idcode);
  bs_print(k_msg_mid, (uint32_t)sizeof(k_msg_mid) - 1U);
  bs_print_uint(checks);
  bs_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
