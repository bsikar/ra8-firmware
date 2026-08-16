/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_fw_version/main.c
 * @brief Complete an esp-hosted RPC round-trip against the ESP32-C6.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The second application built on ``port/esp-hosted/``, and the one that
 * proves the protocol rather than the wire. ``c6_spi_probe`` established the
 * physical link and ``c6_hosted_init`` established that the port can clock a
 * framed transaction; both stop at "bytes moved, and their shape is sane".
 * This one sends a request the co-processor must parse and answer, then
 * checks the answer's contents:
 *
 *   1. bring the port up under ThreadX;
 *   2. send the ``ESP_PRIV_IF`` host-capabilities event the reference host
 *      sends first, draining any boot ``ESP_PRIV_EVENT_INIT`` the
 *      co-processor still holds -- whose firmware-version TLV becomes an
 *      independent second reading of the answer;
 *   3. send ``RPC_ID__Req_GetCoprocessorFwVersion`` on ``ESP_SERIAL_IF``,
 *      protobuf-encoded by the vendored generated codec inside the TLV
 *      envelope the serial endpoint expects;
 *   4. keep clocking transactions until the matching response arrives;
 *   5. decode it and compare its version against
 *      ``esp_hosted_host_fw_ver.h`` -- the vendored host driver's own version
 *      -- printing one PASS or FAIL line.
 *
 * @par Why that expectation
 * The co-processor image and the vendored host driver come from one pinned
 * upstream commit, so their versions must agree exactly. Checking the answer
 * against a literal would only prove the co-processor said something; checking
 * it against the host's own version makes the test the host/co-processor
 * version lock, and a vendor bump without a co-processor reflash fails here
 * instead of failing silently in the field.
 *
 * @par Bench requirements
 * SW4 1=OFF 2=OFF 3=ON 4=OFF, the C6 harness on J26, and the C6 powered from
 * its own USB. SW4-3 ON is what connects J26-1..J26-4 to the MCU at all; with
 * it wrong the board and the co-processor both look healthy and the link
 * simply does not exist.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_fwver.h"
#include "esp_hosted_interface.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "tx_api.h"

/**
 * @enum c6_fwver_pump_budget_t
 * @brief Per-phase transaction budgets, so each phase bounds its own wait.
 * @details The capabilities exchange only has to drain whatever the
 * co-processor already queued, so it is short. The request phase has to
 * survive the co-processor scheduling a reply, so it gets the full budget.
 * @invariant Both values are non-zero, which ::c6_fwver_link_pump requires.
 * @invariant ::k_c6_fwver_caps_transfers is well below
 *            ::k_c6_fwver_max_transfers, so a silent co-processor spends most
 *            of its budget where the answer would actually come from.
 * @par Example:
 * @code
 * (void)c6_fwver_link_pump(..., (uint16_t)k_c6_fwver_caps_transfers, ...);
 * @endcode
 * @see c6_fwver_link_pump
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_c6_fwver_caps_transfers = 8U, /**< Transactions the capabilities phase clocks. */
} c6_fwver_pump_budget_t;

/**
 * @var s_c6_fwver_cpuclk_hz
 * @brief Cached CPUCLK0 rate, used to programme SysTick.
 * @details Read once during bring-up, before ThreadX starts.
 * @note Single-threaded; written once from ``main``.
 * @warning Zero until ::c6_fwver_setup_or_halt has run.
 * @since 0.1.0
 */
static uint32_t s_c6_fwver_cpuclk_hz;

/**
 * @var s_c6_fwver_pclka_hz
 * @brief Cached PCLKA rate, the SCI baud-clock source the port divides.
 * @details Handed to the port as ``ra8_esp_hosted_port_cfg_t::pclk_hz``; a
 * stale value silently mis-programs the SPI bit rate, so it is read from the
 * CGC rather than assumed.
 * @note Single-threaded; written once from ``main``.
 * @warning Zero until ::c6_fwver_setup_or_halt has run.
 * @since 0.1.0
 */
static uint32_t s_c6_fwver_pclka_hz;

/**
 * @var s_c6_fwver_init_err
 * @brief Exact result of ``ra8_esp_hosted_port_init``.
 * @details Recorded in ``tx_application_define`` and reported verbatim by the
 * worker, which is the first context that can print after the init.
 * @note Written once before the scheduler starts, read afterwards.
 * @warning A non-``k_ra8_ok`` value stops the exchange; the application never
 *          continues as though the port had come up.
 * @since 0.1.0
 */
static ra8_err_t s_c6_fwver_init_err = k_ra8_err_not_initialized;

/**
 * @var s_c6_fwver_worker
 * @brief Control block of the single application thread.
 * @details ThreadX requires statically allocated control blocks, as does NASA
 * Power of 10 Rule 3.
 * @note Owned by ThreadX once ``tx_thread_create`` succeeds.
 * @warning Never reused for a second thread.
 * @since 0.1.0
 */
static TX_THREAD s_c6_fwver_worker;

/**
 * @var s_c6_fwver_worker_name
 * @brief Thread name handed to ThreadX.
 * @details A mutable array rather than a string literal: ThreadX takes a
 * non-``const`` ``CHAR*``, and casting the qualifier away would trip the
 * project's ``-Wcast-qual``.
 * @note Read by ThreadX for as long as the thread exists.
 * @warning Never modified after creation; ThreadX keeps the pointer.
 * @since 0.1.0
 */
static CHAR s_c6_fwver_worker_name[] = "c6_fw_version";

/**
 * @var s_c6_fwver_worker_stack
 * @brief Stack backing ::s_c6_fwver_worker.
 * @details Sized by ::k_c6_fwver_worker_stack. The deepest call the worker
 * makes is the generated protobuf decoder, which walks nested messages.
 * @note Written only by ThreadX and the worker itself.
 * @warning Shrinking this without re-measuring risks a silent overflow.
 * @since 0.1.0
 */
static UCHAR s_c6_fwver_worker_stack[k_c6_fwver_worker_stack];

/**
 * @var s_c6_fwver_txbuf
 * @brief Staging buffer for the one payload each phase transmits.
 * @details Sized by ::k_c6_fwver_tx_max, which is two orders of magnitude
 * above the packed request; static because this image has no heap.
 * @note Written only by the worker thread.
 * @warning Reused between phases; a phase must finish before the next fills
 *          it.
 * @since 0.1.0
 */
static uint8_t s_c6_fwver_txbuf[k_c6_fwver_tx_max];

/**
 * @brief Park the CPU forever after an unrecoverable bring-up failure.
 * @return Never returns.
 * @pre A bring-up step has failed and the application cannot proceed.
 * @pre The failure was already reported if the console was up.
 * @post The CPU is parked in a wait-for-interrupt loop.
 * @post No further application state changes.
 * @note Never returns; not callable from interrupt context.
 * @since 0.1.0
 */
static void c6_fwver_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring clocks, module-stop state, SysTick and the console up.
 * @return Nothing; panic-halts instead of returning on any failure.
 * @pre ``Reset_Handler`` has copied ``.data`` and zeroed ``.bss``.
 * @pre ``SystemInit`` has completed and the console SCI is unclaimed.
 * @post ::s_c6_fwver_cpuclk_hz and ::s_c6_fwver_pclka_hz are non-zero and the
 *       console transmits at ::k_c6_fwver_uart_baud, 8N1.
 * @post SysTick runs and every peripheral this app uses is out of
 *       module-stop.
 * @note Never returns on error; a clock failure happens before the console
 *       exists, so it is visible only to a debugger.
 * @since 0.1.0
 */
static void c6_fwver_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    c6_fwver_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &s_c6_fwver_cpuclk_hz) != k_ra8_ok) {
    c6_fwver_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &s_c6_fwver_pclka_hz) != k_ra8_ok) {
    c6_fwver_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    c6_fwver_panic_halt();
  }
  if (ra8_time_init(s_c6_fwver_cpuclk_hz) != k_ra8_ok) {
    c6_fwver_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_c6_fwver_uart_baud) != k_ra8_ok) {
    c6_fwver_panic_halt();
  }
}

/**
 * @brief Send the host-capabilities event and drain the co-processor's.
 * @return Nothing.
 * @pre The port is up and the console is ready.
 * @pre No other context is driving the SPI bus.
 * @post The capabilities event was transmitted, or the reason it was not is
 *       on the console.
 * @post Any queued boot INIT event has been decoded and reported.
 * @note Failure here is reported and not fatal: the RPC exchange does not
 *       depend on it, and a bring-up learns more from attempting the next
 *       phase than from stopping.
 * @since 0.1.0
 */
static void c6_fwver_phase_caps(void)
{
  uint16_t        len = 0U;
  const ra8_err_t err =
    c6_fwver_priv_host_caps(s_c6_fwver_txbuf, (uint16_t)k_c6_fwver_tx_max, &len);
  c6_fwver_puts("c6_fwver: host caps build=");
  c6_fwver_puts(ra8_err_to_str(err));
  c6_fwver_puts(" bytes=");
  c6_fwver_put_u32((uint32_t)len);
  c6_fwver_puts("\r\n");
  if (err != k_ra8_ok) {
    return;
  }

  c6_fwver_pump_stats_t stats = {};
  const ra8_err_t       ran   = c6_fwver_link_pump((uint8_t)ESP_PRIV_IF,
                                                   0U,
                                                   s_c6_fwver_txbuf,
                                                   len,
                                                   (uint16_t)k_c6_fwver_caps_transfers,
                                                   c6_fwver_dispatch,
                                                   &stats);
  c6_fwver_print_pump("caps", &stats);
  if (ran != k_ra8_ok) {
    c6_fwver_puts("c6_fwver: caps pump=");
    c6_fwver_puts(ra8_err_to_str(ran));
    c6_fwver_puts("\r\n");
  }
}

/**
 * @brief Send the firmware-version request and pump until it is answered.
 * @return Nothing.
 * @pre The port is up and the console is ready.
 * @pre The capabilities phase has finished, so the bus is free.
 * @post The request was transmitted, or the reason it was not is on the
 *       console.
 * @post The response, if one arrived, has been recorded for the verdict.
 * @note The pump stops as soon as the RPC layer accepts the response, so a
 *       healthy link spends only the transactions it needs.
 * @since 0.1.0
 */
static void c6_fwver_phase_request(void)
{
  uint16_t        len = 0U;
  const ra8_err_t err = c6_fwver_rpc_request(s_c6_fwver_txbuf, (uint16_t)k_c6_fwver_tx_max, &len);
  c6_fwver_puts("c6_fwver: request build=");
  c6_fwver_puts(ra8_err_to_str(err));
  c6_fwver_puts(" bytes=");
  c6_fwver_put_u32((uint32_t)len);
  c6_fwver_puts("\r\n");
  if (err != k_ra8_ok) {
    return;
  }

  c6_fwver_pump_stats_t stats = {};
  const ra8_err_t       ran   = c6_fwver_link_pump((uint8_t)ESP_SERIAL_IF,
                                                   0U,
                                                   s_c6_fwver_txbuf,
                                                   len,
                                                   (uint16_t)k_c6_fwver_max_transfers,
                                                   c6_fwver_dispatch,
                                                   &stats);
  c6_fwver_print_pump("rpc", &stats);
  if (ran != k_ra8_ok) {
    c6_fwver_puts("c6_fwver: rpc pump=");
    c6_fwver_puts(ra8_err_to_str(ran));
    c6_fwver_puts("\r\n");
  }
}

/**
 * @brief Print a liveness line forever, never returning.
 * @param[in] passed Verdict the run reached, echoed on every beat.
 * @return Never returns.
 * @pre The console is up and ThreadX is scheduling.
 * @pre The verdict has already been printed once.
 * @post A line is emitted every ::k_c6_fwver_heartbeat_ms milliseconds.
 * @post No application state is modified beyond the local beat counter.
 * @note This is the one deliberately unbounded loop in the application. It
 *       exists so a console attached after the run still learns the verdict.
 * @since 0.1.0
 */
static void c6_fwver_heartbeat(bool passed)
{
  uint32_t beat = 0U;
  while (1) {
    c6_fwver_puts("c6_fwver: heartbeat n=");
    c6_fwver_put_u32(beat);
    c6_fwver_puts(passed ? " verdict=PASS\r\n" : " verdict=FAIL\r\n");
    beat++;
    tx_thread_sleep((ULONG)k_c6_fwver_heartbeat_ms);
  }
}

/**
 * @brief Worker thread: report the init, run both phases, judge, heartbeat.
 * @param[in] thread_input ThreadX entry argument; unused.
 * @return Never returns.
 * @pre ``tx_application_define`` has recorded ::s_c6_fwver_init_err.
 * @pre The console is up.
 * @post On a successful init both phases ran and exactly one verdict was
 *       printed.
 * @post On a failed init no transaction was attempted and the error was
 *       named.
 * @note The failure path deliberately does not continue: a port that did not
 *       come up has an unpopulated vtable, and calling through it would fault
 *       rather than report.
 * @since 0.1.0
 */
static void c6_fwver_worker_entry(ULONG thread_input)
{
  (void)thread_input;

  c6_fwver_puts("c6_fwver: port_init=");
  c6_fwver_puts(ra8_err_to_str(s_c6_fwver_init_err));
  c6_fwver_puts("\r\n");

  if (s_c6_fwver_init_err != k_ra8_ok) {
    c6_fwver_puts("c6_fwver: FAIL port init failed -- no transaction attempted\r\n");
    c6_fwver_heartbeat(false);
    return;
  }

  ra8_delay_ms((uint32_t)k_c6_fwver_boot_wait_ms);
  c6_fwver_phase_caps();
  c6_fwver_phase_request();
  const bool passed = c6_fwver_rpc_report();
  ra8_esp_hosted_mem_dump("c6_fw_version");
  c6_fwver_heartbeat(passed);
}

/**
 * @brief ThreadX define hook: bring the port up and start the worker.
 * @param[in] first_unused_memory Free RAM handed over by the ThreadX port;
 *                                unused, every object here is static.
 * @return Nothing.
 * @pre ``tx_kernel_enter`` has been called and the clock cache is valid.
 * @pre No vendored esp-hosted entry point has been called yet.
 * @post ::s_c6_fwver_init_err holds the exact port-init result.
 * @post Exactly one worker thread was created.
 * @note The port creates ThreadX byte pools, mutexes and timers, so its init
 *       must run where ThreadX permits object creation -- here, not from
 *       ``main``.
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  const ra8_esp_hosted_port_cfg_t cfg = {
    .pclk_hz      = s_c6_fwver_pclka_hz,
    .sck_hz       = (uint32_t)k_c6_fwver_sck_hz,
    .edge_poll_ms = (uint16_t)k_c6_fwver_edge_poll_ms,
    .sci_channel  = (uint8_t)k_ra8_board_pmod1_sci_channel,
  };
  s_c6_fwver_init_err = ra8_esp_hosted_port_init(&cfg);

  if (tx_thread_create(&s_c6_fwver_worker,
                       s_c6_fwver_worker_name,
                       c6_fwver_worker_entry,
                       0U,
                       s_c6_fwver_worker_stack,
                       (ULONG)sizeof(s_c6_fwver_worker_stack),
                       (UINT)k_c6_fwver_worker_prio,
                       (UINT)k_c6_fwver_worker_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    c6_fwver_puts("c6_fwver: worker thread creation failed\r\n");
  }
}

/**
 * @brief Application entry: clocks, console, banner, then ThreadX.
 * @pre ``Reset_Handler`` has copied ``.data`` and zeroed ``.bss``.
 * @pre ``SystemInit`` has set VTOR, the FPU and the priority grouping.
 * @post The banner was printed exactly once.
 * @post Control passed to ThreadX and never came back.
 * @note Everything after ``tx_kernel_enter`` happens on ThreadX; the
 *       panic-halt below is reached only if the kernel refuses to start.
 * @since 0.1.0
 */
void main(void)
{
  c6_fwver_setup_or_halt();
  ra8_isr_globals_enable();

  c6_fwver_print_banner(s_c6_fwver_cpuclk_hz, s_c6_fwver_pclka_hz);
  c6_fwver_puts("c6_fwver: entering ThreadX; port init runs from tx_application_define\r\n");

  tx_kernel_enter();

  c6_fwver_panic_halt();
}
