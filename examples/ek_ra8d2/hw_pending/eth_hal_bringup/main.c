/**
 * @file examples/ek_ra8d2/hw_pending/eth_hal_bringup/main.c
 * @brief HAL-based ESWM/COMA Ethernet media bring-up demo (issue #581)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Demonstrates the two chip-generic ETH HAL primitives extracted from the
 * EK-RA8D2 board Ethernet bring-up (issue #581), driven directly instead of
 * through ``ra8_board_ethernet_init``:
 *
 *   1. ``ra8_eth_coma_bringup`` -- pulses COMA.RRC, enables the switch clock
 *      (RCEC.RCE), kicks the CABPIRM buffer-pool init and polls CABPIRM.BPR,
 *      then fans every per-agent clock out (RCEC.ACE[6:0]). Until this runs
 *      the per-port RMAC / ETHA register windows read back 0.
 *   2. ``ra8_eth_rgmii_select`` -- selects RGMII on port 1's ESWM media mux
 *      (MIICR1 = TXCIDE | RGMII) and releases the per-port block reset
 *      (MIIRR.RGRST1 = 1). The EK-RA8D2 RJ45 is wired to RMAC1 / ETHA1.
 *
 * The prerequisites the primitives document -- ESWCLK up and the ESWM
 * module-stop gate released -- are established first via ``ra8_cgc_eswclk_init``
 * and ``ra8_mstp_enable(k_ra8_mstp_eswm)``. Each step's status code is narrated
 * on the SCI8 J-Link VCOM console; the fixed verdict ``eth-hal: bringup PASS``
 * prints only when every call returned ``k_ra8_ok``.
 *
 * Kept ALONGSIDE ``examples/.../hil/eth_open_probe`` (which drives the full
 * board bring-up + ``ra8_eth_open``): this app is the minimal witness that the
 * two extracted primitives compose on their own.
 *
 * @note **hw_pending.** eth is HW-blocked on silicon (the EK-RA8D2 Ethernet
 * wire is marginal, issue #21), so this app is compile-gated and bench-only;
 * it makes no claim of hardware validation. The COMA/RGMII register accesses
 * are host-tested in ``tests/test_ra8_eth_coma.c`` / ``tests/test_ra8_eth.c``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_eth_coma.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"
#include "ra8_time.h"

/** @enum ehb_const_t @brief Console + demo knobs (no magic numbers). */
typedef enum : uint32_t {
  k_ehb_baud = 115200U, /**< SCI8 J-Link VCOM console baud. */
} ehb_const_t;

/** @enum ehb_fmt_t @brief Decimal-serialiser fields. */
typedef enum : uint8_t {
  k_ehb_radix       = 10U, /**< Decimal serialiser radix.          */
  k_ehb_dec_u32_max = 10U, /**< Max decimal digits for a uint32_t. */
} ehb_fmt_t;

/* Console line fragments (short so each write is one shift-register fill). */
static const uint8_t k_ehb_msg_boot[]    = "eth-hal: boot\r\n";
static const uint8_t k_ehb_msg_eswclk[]  = "eth-hal: eswclk_init rc=";
static const uint8_t k_ehb_msg_mstp[]    = "eth-hal: mstp_eswm rc=";
static const uint8_t k_ehb_msg_coma[]    = "eth-hal: coma_bringup rc=";
static const uint8_t k_ehb_msg_rgmii[]   = "eth-hal: rgmii_select rc=";
static const uint8_t k_ehb_msg_pass[]    = "eth-hal: bringup PASS\r\n";
static const uint8_t k_ehb_msg_fail[]    = "eth-hal: bringup FAIL\r\n";
static const uint8_t k_ehb_msg_hw_fail[] = "eth-hal: hw_init_failed\r\n";
static const uint8_t k_ehb_msg_crlf[]    = "\r\n";

/**
 * @brief Write a byte span to the SCI8 console, discarding the status.
 *
 * @param[in] data Non-NULL byte span to transmit.
 * @param[in] len  Byte count (0 is a no-op).
 *
 * @pre ``ra8_board_uart_console_init`` has succeeded.
 * @pre ``data`` points at ``len`` readable bytes.
 * @post ``len`` bytes have been queued to the console UART.
 * @post No other state is modified.
 *
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void ehb_write(const uint8_t* data, uint32_t len)
{
  (void)ra8_board_uart_console_write(data, (size_t)len);
}

/**
 * @brief Log one unsigned 32-bit value as decimal ASCII plus CRLF.
 *
 * @details Serialises LSB-first into a local buffer then emits MSB-first;
 *          zero prints as a single ``0``. Used to narrate ``ra8_err_t`` codes.
 *
 * @param[in] val Value to print.
 *
 * @pre The console has been initialised.
 * @pre ``val`` fits in a uint32_t (always true).
 * @post The decimal digits of ``val`` followed by CRLF were queued.
 * @post No other state is modified.
 *
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void ehb_write_u32_line(uint32_t val)
{
  uint8_t  buf[k_ehb_dec_u32_max];
  uint32_t n = 0U;
  if (val == 0U) {
    buf[n] = (uint8_t)'0';
    n++;
  }
  while ((val > 0U) && (n < (uint32_t)k_ehb_dec_u32_max)) {
    buf[n] = (uint8_t)((uint32_t)'0' + (val % (uint32_t)k_ehb_radix));
    n++;
    val /= (uint32_t)k_ehb_radix;
  }
  for (uint32_t i = 0U; i < n; i++) {
    ehb_write(&buf[n - 1U - i], 1U);
  }
  ehb_write(k_ehb_msg_crlf, (uint32_t)sizeof(k_ehb_msg_crlf) - 1U);
}

/**
 * @brief Print the negative banner and park the CPU.
 *
 * @details Mirrors the sibling apps' panic idiom: one banner the HIL gate
 *          can match, then a debugger-visible stop.
 *
 * @pre Called only on an unrecoverable bring-up failure.
 * @pre The console is up (best effort -- the write may be dropped).
 * @post Never returns; the CPU parks in a WFI loop.
 * @post The negative banner was pushed to the console.
 *
 * @note Not thread-safe (terminal path).
 * @since 0.1.0
 */
[[noreturn]] static void ehb_panic_halt(void)
{
  ehb_write(k_ehb_msg_hw_fail, (uint32_t)sizeof(k_ehb_msg_hw_fail) - 1U);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up clocks, MSTP, SysTick and the SCI8 console; halt on failure.
 *
 * @details The minimum the media bring-up below depends on: a running MSTP
 *          controller (``ra8_mstp_init``) so the ESWM gate can be released,
 *          and the console for narration.
 *
 * @pre Running after Reset_Handler with ``.data`` / ``.bss`` initialised.
 * @pre The board wiring matches EK-RA8D2 (SCI8 J-Link VCOM).
 * @post The console accepts writes at ::k_ehb_baud.
 * @post On any init failure the CPU is parked via ::ehb_panic_halt.
 *
 * @note Not thread-safe; boot context only.
 * @since 0.1.0
 */
static void ehb_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    ehb_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    ehb_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    ehb_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    ehb_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_ehb_baud) != k_ra8_ok) {
    ehb_panic_halt();
  }
}

/**
 * @brief Drive the ESWCLK + ESWM prerequisites, then the two ETH HAL primitives.
 *
 * @details
 * Narrates every status code so a transcript distinguishes which step
 * refused: ESWCLK bring-up, the ESWM module-stop release, the COMA bring-up,
 * or the RGMII media-select. Port 1 is selected because the EK-RA8D2 RJ45 is
 * wired to RMAC1 / ETHA1.
 *
 * @return ra8_err_t Status of the first failing call, else ``k_ra8_ok``.
 * @retval k_ra8_ok             Every step succeeded.
 * @retval k_ra8_err_hw_timeout ESWCLK, MSTP readback, or CABPIRM.BPR timed out.
 * @retval k_ra8_err_invalid_arg ``ra8_eth_rgmii_select`` rejected the port.
 *
 * @pre ::ehb_setup_or_halt has run (clocks, MSTP, SysTick, console).
 * @pre No other agent owns the ESWM.
 * @post On success COMA is out of reset and port 1's media mux is RGMII.
 * @post Every step's status code has been narrated on the console.
 *
 * @note Not thread-safe (single-threaded app).
 *
 * @par MC/DC:
 * Decision: four sequential single-condition ``rc != k_ra8_ok`` guards, each
 * fully covered by two vectors (taken / not taken). The COMA and RGMII legs
 * are exercised in both directions on host by ``tests/test_ra8_eth_coma.c``
 * and ``tests/test_ra8_eth.c`` (happy path + timeout / bad-port).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t ehb_run_once(void)
{
  ra8_err_t rc = ra8_cgc_eswclk_init();
  ehb_write(k_ehb_msg_eswclk, (uint32_t)sizeof(k_ehb_msg_eswclk) - 1U);
  ehb_write_u32_line((uint32_t)rc);
  if (rc != k_ra8_ok) {
    return rc;
  }

  rc = ra8_mstp_enable(k_ra8_mstp_eswm);
  ehb_write(k_ehb_msg_mstp, (uint32_t)sizeof(k_ehb_msg_mstp) - 1U);
  ehb_write_u32_line((uint32_t)rc);
  if (rc != k_ra8_ok) {
    return rc;
  }

  rc = ra8_eth_coma_bringup();
  ehb_write(k_ehb_msg_coma, (uint32_t)sizeof(k_ehb_msg_coma) - 1U);
  ehb_write_u32_line((uint32_t)rc);
  if (rc != k_ra8_ok) {
    return rc;
  }

  rc = ra8_eth_rgmii_select(k_ra8_eth_mii_port_1);
  ehb_write(k_ehb_msg_rgmii, (uint32_t)sizeof(k_ehb_msg_rgmii) - 1U);
  ehb_write_u32_line((uint32_t)rc);
  return rc;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  ehb_setup_or_halt();
  ra8_isr_globals_enable();

  ehb_write(k_ehb_msg_boot, (uint32_t)sizeof(k_ehb_msg_boot) - 1U);

  if (ehb_run_once() != k_ra8_ok) {
    ehb_write(k_ehb_msg_fail, (uint32_t)sizeof(k_ehb_msg_fail) - 1U);
  } else {
    ehb_write(k_ehb_msg_pass, (uint32_t)sizeof(k_ehb_msg_pass) - 1U);
  }

  while (1) {
    __asm__ volatile("wfi");
  }
  return 0;
}
#pragma GCC diagnostic pop
