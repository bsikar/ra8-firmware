/**
 * @file examples/ek_ra8d2/hw_validated/hil/eth_open_probe/main.c
 * @brief Bare-metal ra8_eth_open() bring-up probe (tracker issue #524).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The smallest program that reaches `ra8_eth_open()` on real silicon:
 * clocks, module-stop, SysTick, the SCI8 J-Link VCOM console, the board
 * Ethernet pin/PHY bring-up, then one `ra8_eth_open()` on channel 1 --
 * exactly the sequence `port/netxduo/src/nx_ether_driver_ra8_eth.c`
 * performs from `NX_LINK_INITIALIZE`, with no RTOS, no IP stack and no
 * packet pool in between.
 *
 * It exists because that sequence has been reported to take a HardFault
 * inside the GWCA TX-ring init, with the descriptor-chain pointer the
 * caller loaded out of the HAL's GWCA state block arriving with address
 * bit 25 clear (`0x220664B0` -> `0x200664B0`), while the SRAM word
 * itself reads back correct over J-Link. Bit 25 is the bit that
 * separates the Cortex-M85 DTCM window at `0x20000000` from system SRAM
 * at `0x22000000` (HUM Ch 5 "Address Space" pp 236-243), so a cleared
 * bit 25 turns an SRAM pointer into an address just above the 64 KiB
 * DTCM -- unimplemented, and therefore a precise BusFault.
 *
 * The app narrates every step and registers the console as the
 * `ra8_log` byte sink, so if the fault does occur the shared decoder in
 * `libs/ra8_core/src/ra8_exception.c` prints the stacked frame
 * (`pc`, `r0`..`r3`) plus `cfsr` / `bfar` on the bench UART instead of
 * dropping them into ITM. Those are the numbers the report turns on:
 * `r0` is the descriptor-chain pointer as the CPU actually loaded it.
 *
 * Sequence:
 *   1. `ra8_cgc_init` + `ra8_cgc_get_clock_hz` + `ra8_mstp_init` +
 *      `ra8_time_init` + `ra8_board_uart_console_init`.
 *   2. `ra8_log_set_byte_sink` -> console, so a fault dump is visible.
 *   3. `ra8_board_ethernet_init` (pins, PHY reset, RMII/RGMII select).
 *   4. `ra8_eth_open` with channel 1 -- the EK-RA8D2 RJ45 is wired to
 *      ETHA1 / RMAC1.
 *   5. Print the verdict line and idle.
 *
 * Reaching the verdict line proves the open path completed; a HardFault
 * dump instead of it is the #524 signature.
 *
 * @note **Headless-emulator status.** `tools/ra8_emulator` models enough
 * of the ESWM register file for the open path to run, so an EIL run
 * reaches the PASS line. It does NOT model SRAM access timing, so it
 * never reproduced the fault; only the bench could, which is exactly
 * why the wire gate and this app both matter.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_eth.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @enum eop_consts_t @brief Console + probe knobs (no magic numbers). */
typedef enum : uint32_t {
  k_eop_uart_baud = 115200U, /**< SCI8 J-Link VCOM console baud.            */
  k_eop_channel   = 1U,      /**< EK-RA8D2 RJ45 is ETHA1 / RMAC1.           */
  k_eop_hex_chars = 8U,      /**< Hex digits printed per 32-bit value.      */
  k_eop_hex_shift = 4U,      /**< Bits consumed per emitted hex digit.      */
  k_eop_hex_mask  = 0xFU,    /**< Nibble mask for one hex digit.            */
  k_eop_dec_base  = 10U,     /**< Decimal base for the status-code printer. */
  k_eop_dec_max   = 10U,     /**< Digits in the widest uint32 (4294967295). */
} eop_consts_t;

/**
 * @enum eop_pad_t
 * @brief Size of the leading `.bss` pad that steers where the HAL's
 *        Ethernet statics land in SRAM.
 *
 * @details
 * `RA8_ETH_PROBE_PAD_BYTES` is a build-time knob (`-D...` from CMake, see
 * this app's CMakeLists.txt) and the whole point of the app as an
 * instrument. `libs/ra8_board_ek_ra8d2/ld/linker_script.ld` emits
 * `*(.bss)` before `*(.bss.*)`, and `-fdata-sections` puts every ordinary
 * variable in its own `.bss.<name>`; so a pad forced into the *plain*
 * `.bss` section lands ahead of every library static, and growing it
 * slides the whole GWCA block -- `s_gwca_state`, the descriptor chains
 * and the buffer pools -- up SRAM by exactly that many bytes with no
 * other change to the program.
 *
 * That makes "where the Ethernet DMA structures live" a single
 * build-time variable, which is what tracker issue #499 needs and could
 * not get by growing an application array (that moves the rings *and*
 * every unrelated static, and pulling the rings out of `.bss` into a
 * placed section moves everything that followed them instead).
 *
 * @invariant Value is a multiple of 16 so the pad cannot itself change
 *            the alignment of anything behind it.
 *
 * @see eop_bss_pad_anchor()
 */
typedef enum : uint32_t {
  k_eop_pad_bytes = RA8_ETH_PROBE_PAD_BYTES, /**< Leading `.bss` pad, bytes. */
} eop_pad_t;

static_assert((k_eop_pad_bytes % 16U) == 0U, "pad must be a multiple of 16");
static_assert(k_eop_pad_bytes >= 16U, "pad must be non-empty");

/**
 * @var s_eop_bss_pad
 * @brief Leading `.bss` filler that positions every library static behind it.
 *
 * @details
 * Forced into the plain `.bss` input section (not the `-fdata-sections`
 * `.bss.s_eop_bss_pad` it would otherwise get) so the linker emits it in
 * the `*(.bss)` group, ahead of every `*(.bss.*)`. `volatile` plus the
 * single write in ::eop_bss_pad_anchor keeps `--gc-sections` from
 * discarding it.
 *
 * @note Read-only to everything except ::eop_bss_pad_anchor.
 * @warning Never size this from a runtime value -- the address sweep it
 *          exists for is only meaningful if the layout is static.
 * @since 0.1.0
 */
[[gnu::section(".bss")]] static volatile uint8_t s_eop_bss_pad[k_eop_pad_bytes];

/** @enum eop_mac_t @brief Locally-administered MAC the probe opens with. */
typedef enum : uint8_t {
  k_eop_mac_0 = 0x02U, /**< Octet 0 -- locally administered, unicast. */
  k_eop_mac_1 = 0x00U, /**< Octet 1.                                  */
  k_eop_mac_2 = 0x00U, /**< Octet 2.                                  */
  k_eop_mac_3 = 0x00U, /**< Octet 3.                                  */
  k_eop_mac_4 = 0x00U, /**< Octet 4.                                  */
  k_eop_mac_5 = 0x01U, /**< Octet 5.                                  */
} eop_mac_t;

static const uint8_t k_eop_msg_boot[]     = "eth-open-probe: boot\r\n";
static const uint8_t k_eop_msg_clock[]    = "eth-open-probe: cpuclk0=";
static const uint8_t k_eop_msg_pad[]      = " bss_pad=";
static const uint8_t k_eop_msg_padlen[]   = " pad_bytes=";
static const uint8_t k_eop_msg_board[]    = "eth-open-probe: board_eth rc=";
static const uint8_t k_eop_msg_opening[]  = "eth-open-probe: calling ra8_eth_open ch=1\r\n";
static const uint8_t k_eop_msg_open_rc[]  = "eth-open-probe: eth_open rc=";
static const uint8_t k_eop_msg_pass[]     = "eth-open-probe: PASS\r\n";
static const uint8_t k_eop_msg_fail[]     = "eth-open-probe: FAIL\r\n";
static const uint8_t k_eop_msg_hw_fail[]  = "eth-open-probe: hw_init_failed\r\n";
static const uint8_t k_eop_msg_newline[]  = "\r\n";
static const uint8_t k_eop_hex_digits[]   = "0123456789ABCDEF";
static const uint8_t k_eop_msg_hex_lead[] = "0x";

/**
 * @brief Push bytes at the SCI8 console, discarding the write status.
 *
 * @details Console loss is acceptable for a narration helper -- the HIL
 *          gate re-checks the transcript it actually received.
 *
 * @param[in] msg Bytes to send (not NUL-inspected).
 * @param[in] len Number of bytes to send.
 *
 * @pre `msg` points at `len` readable bytes.
 * @pre `ra8_board_uart_console_init` succeeded.
 * @post `len` bytes were pushed to the console (or dropped on error).
 * @post No other state is modified.
 *
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void eop_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/**
 * @brief `ra8_log` byte sink forwarding every log byte to the console.
 *
 * @details
 * `ra8_exception_report()` narrates the decoded fault through `ra8_log`,
 * whose default ITM backend deliberately drops every byte in fault
 * context. A registered byte sink bypasses that gate, so the dump
 * (`pc`, `r0`..`r3`, `cfsr`, `bfar`) lands on the bench UART where the
 * transcript can be read without a debugger attached.
 *
 * @param[in] ctx  Unused opaque cookie (sink ABI).
 * @param[in] byte Log byte to emit.
 *
 * @pre `ra8_board_uart_console_init` succeeded.
 * @pre Registered via `ra8_log_set_byte_sink`.
 * @post The byte was pushed to the console (or dropped on error).
 * @post No other state is modified.
 *
 * @note Callable from fault context -- the console write is polled.
 * @since 0.1.0
 */
static void eop_log_sink(void* ctx, uint8_t byte)
{
  (void)ctx;
  (void)ra8_board_uart_console_write(&byte, 1U);
}

/**
 * @brief Print a 32-bit value as `0x` + eight upper-case hex digits.
 *
 * @details Fixed width so a transcript diff lines up column-wise; the
 *          addresses this probe reports are only legible in hex.
 *
 * @param[in] value Value to print.
 *
 * @pre The console is up (best effort -- bytes may be dropped).
 * @pre `value` is any uint32 (full range supported).
 * @post Ten characters were pushed to the console.
 * @post No other state is modified.
 *
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void eop_print_hex(uint32_t value)
{
  eop_print(k_eop_msg_hex_lead, (uint32_t)sizeof(k_eop_msg_hex_lead) - 1U);
  for (uint32_t i = 0U; i < (uint32_t)k_eop_hex_chars; i++) {
    const uint32_t shift = ((uint32_t)k_eop_hex_chars - 1U - i) * (uint32_t)k_eop_hex_shift;
    const uint32_t digit = (value >> shift) & (uint32_t)k_eop_hex_mask;
    eop_print(&k_eop_hex_digits[digit], 1U);
  }
}

/**
 * @brief Print an unsigned 32-bit value in decimal.
 *
 * @details Digits are pushed LSB-first into a local buffer then emitted
 *          MSB-first; zero prints as a single `0`.
 *
 * @param[in] value Value to print.
 *
 * @pre The console is up (best effort -- bytes may be dropped).
 * @pre `value` is any uint32 (full range supported).
 * @post The decimal text was pushed to the console.
 * @post No other state is modified.
 *
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static void eop_print_uint(uint32_t value)
{
  uint8_t  buf[k_eop_dec_max];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = (uint8_t)'0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_eop_dec_max)) {
    buf[n] = (uint8_t)((uint32_t)'0' + (value % (uint32_t)k_eop_dec_base));
    n++;
    value /= (uint32_t)k_eop_dec_base;
  }
  for (uint32_t i = 0U; i < n; i++) {
    eop_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Touch ::s_eop_bss_pad and report where it starts.
 *
 * @details
 * Two jobs in one place. The write keeps `--gc-sections` from dropping
 * the pad, which would silently turn the address sweep into a no-op; and
 * the returned address anchors the transcript to the link map, so a run
 * log says which layout produced it without anyone having to guess which
 * build it came from. Every library static sits at a fixed offset from
 * this address, so it identifies the layout completely.
 *
 * @return uint32_t Address of the first pad byte.
 * @retval 0x22000030..0x2219FFFF Somewhere in the `.bss` output section.
 *
 * @pre `.bss` has been zero-filled by the reset handler.
 * @pre The pad is at least one byte long (static_assert above).
 * @post The first pad byte reads back as written.
 * @post No other state is modified.
 *
 * @note Not thread-safe (single-threaded app).
 * @since 0.1.0
 */
static uint32_t eop_bss_pad_anchor(void)
{
  enum : uint8_t {
    k_eop_pad_probe_idx  = 0U,    /**< Byte written to defeat --gc-sections. */
    k_eop_pad_probe_byte = 0xA5U, /**< Arbitrary non-zero witness value.     */
  };
  s_eop_bss_pad[k_eop_pad_probe_idx] = (uint8_t)k_eop_pad_probe_byte;
  return (uint32_t)(uintptr_t)&s_eop_bss_pad[k_eop_pad_probe_idx];
}

/**
 * @brief Print the negative banner and park the CPU.
 *
 * @details Mirrors the sibling apps' panic idiom: one banner the HIL
 *          gate can match, then a debugger-visible stop.
 *
 * @pre Called only on an unrecoverable bring-up failure.
 * @pre The console is up (best effort -- the write may be dropped).
 * @post Never returns; the CPU parks in a BKPT + WFI loop.
 * @post The negative banner was pushed to the console.
 *
 * @note Not thread-safe (terminal path).
 * @since 0.1.0
 */
[[noreturn]] static void eop_panic_halt(void)
{
  eop_print(k_eop_msg_hw_fail, (uint32_t)sizeof(k_eop_msg_hw_fail) - 1U);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring up clocks, MSTP, SysTick and the SCI8 console; halt on failure.
 *
 * @details The minimum the Ethernet bring-up below depends on:
 *          `ra8_board_ethernet_init` needs a running SysTick for its PHY
 *          reset delays, and `ra8_eth_open` needs the ESWM module clock
 *          `ra8_mstp_init` releases.
 *
 * @param[out] out_cpuclk_hz Receives the measured CPUCLK0 frequency in Hz.
 *
 * @pre Running after Reset_Handler with `.data` / `.bss` initialised.
 * @pre `out_cpuclk_hz` points at writable storage.
 * @post The console accepts writes at ::k_eop_uart_baud.
 * @post On any init failure the CPU is parked via ::eop_panic_halt.
 *
 * @note Not thread-safe; boot context only.
 * @since 0.1.0
 */
static void eop_setup_or_halt(uint32_t* out_cpuclk_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    eop_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    eop_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    eop_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    eop_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_eop_uart_baud) != k_ra8_ok) {
    eop_panic_halt();
  }
  *out_cpuclk_hz = cpuclk0_hz;
}

/**
 * @brief Run the board Ethernet bring-up and the one `ra8_eth_open` call.
 *
 * @details
 * Narrates both status codes so a transcript distinguishes "the board
 * layer refused" from "the HAL open path refused" from "the CPU faulted
 * inside the open path and never returned a code at all" -- the last of
 * which is the #524 signature and is why every line is flushed before
 * the call rather than after it.
 *
 * @return ra8_err_t Status of the last call attempted.
 * @retval k_ra8_ok               Both the board layer and `ra8_eth_open` succeeded.
 * @retval k_ra8_err_invalid_arg  `ra8_eth_open` rejected the channel or sizes.
 * @retval k_ra8_err_timeout      GWCA / RMAC mode change did not complete.
 *
 * @pre ::eop_setup_or_halt has run (clocks, MSTP, SysTick, console).
 * @pre No other agent owns the ESWM.
 * @post On success the GWCA is in OPERATION with both rings primed.
 * @post Both status codes have been narrated on the console.
 *
 * @note Not thread-safe (single-threaded app).
 *
 * @par MC/DC:
 * Decision: `board_rc != k_ra8_ok` then `open_rc != k_ra8_ok` -- two
 * independent single-condition decisions rather than one compound, so
 * two vectors each (taken / not taken) fully cover them. Both are
 * exercised on host in `tests/apps/test_app_eth_open_probe.c` through
 * the HAL fault-injection seam.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t eop_run_once(void)
{
  const ra8_err_t board_rc = ra8_board_ethernet_init();
  eop_print(k_eop_msg_board, (uint32_t)sizeof(k_eop_msg_board) - 1U);
  eop_print_uint((uint32_t)board_rc);
  eop_print(k_eop_msg_newline, (uint32_t)sizeof(k_eop_msg_newline) - 1U);
  if (board_rc != k_ra8_ok) {
    return board_rc;
  }

  const ra8_eth_cfg_t cfg = {
    .mac_address        = {(uint8_t)k_eop_mac_0,
                           (uint8_t)k_eop_mac_1,
                           (uint8_t)k_eop_mac_2,
                           (uint8_t)k_eop_mac_3,
                           (uint8_t)k_eop_mac_4,
                           (uint8_t)k_eop_mac_5},
    .channel            = (uint8_t)k_eop_channel,
    .num_tx_descriptors = 0U,
    .num_rx_descriptors = 0U,
    .buffer_size        = 0U,
  };

  eop_print(k_eop_msg_opening, (uint32_t)sizeof(k_eop_msg_opening) - 1U);
  const ra8_err_t open_rc = ra8_eth_open(&cfg);
  eop_print(k_eop_msg_open_rc, (uint32_t)sizeof(k_eop_msg_open_rc) - 1U);
  eop_print_uint((uint32_t)open_rc);
  eop_print(k_eop_msg_newline, (uint32_t)sizeof(k_eop_msg_newline) - 1U);
  return open_rc;
}

void main(void)
{
  uint32_t cpuclk0_hz = 0U;
  eop_setup_or_halt(&cpuclk0_hz);
  ra8_isr_globals_enable();

  /* Route the shared fault decoder's ra8_log dump onto the bench
   * console before anything that can fault runs. */
  ra8_log_set_byte_sink(eop_log_sink, nullptr);

  eop_print(k_eop_msg_boot, (uint32_t)sizeof(k_eop_msg_boot) - 1U);
  eop_print(k_eop_msg_clock, (uint32_t)sizeof(k_eop_msg_clock) - 1U);
  eop_print_hex(cpuclk0_hz);
  eop_print(k_eop_msg_pad, (uint32_t)sizeof(k_eop_msg_pad) - 1U);
  eop_print_hex(eop_bss_pad_anchor());
  eop_print(k_eop_msg_padlen, (uint32_t)sizeof(k_eop_msg_padlen) - 1U);
  eop_print_hex((uint32_t)k_eop_pad_bytes);
  eop_print(k_eop_msg_newline, (uint32_t)sizeof(k_eop_msg_newline) - 1U);

  if (eop_run_once() != k_ra8_ok) {
    eop_print(k_eop_msg_fail, (uint32_t)sizeof(k_eop_msg_fail) - 1U);
  } else {
    eop_print(k_eop_msg_pass, (uint32_t)sizeof(k_eop_msg_pass) - 1U);
  }

  while (1) {
    __asm__ volatile("wfi");
  }
}
