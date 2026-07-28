/**
 * @file examples/ek_ra8d2/hw_validated/c6/c6_hosted_init/main.c
 * @brief Bring the RA8D2 + ThreadX esp-hosted port up against the ESP32-C6.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * First application to consume ``port/esp-hosted/`` -- the first-party
 * RA8D2 + ThreadX port of the vendored esp-hosted host driver. It is the
 * port's buildable consumer and its bring-up harness: it initialises the
 * port, states the pin map and interrupt routing the port resolved, reads
 * both side-band lines through the OS-abstraction vtable, clocks exactly
 * one full-duplex transaction, decodes the frame the co-processor returned
 * and prints one PASS/FAIL verdict line.
 *
 * This file holds the bring-up sequence only; the reporting, the console
 * formatters and the frame work live in ``src/`` behind ``c6_hosted.h``.
 *
 * @par Flow
 *   1. ``ra8_cgc_init`` -- CPUCLK0 and PCLKA -- then ``ra8_mstp_init``,
 *      SysTick and the board console. Any failure panic-halts.
 *   2. Banner: port identity, clocks, SPI parameters, the resolved pin map,
 *      and which side-band pin takes the ICU edge path versus the port's
 *      software edge detector.
 *   3. ``tx_kernel_enter``. The port creates ThreadX byte pools, mutexes
 *      and timers, so ``ra8_esp_hosted_port_init`` runs from
 *      ``tx_application_define`` -- the context where ThreadX permits
 *      object creation -- and the worker thread reports what it returned.
 *   4. Worker: sample both side-band lines, run one transaction, dump pool
 *      occupancy, then heartbeat forever.
 *
 * @par Runtime status -- proven on silicon 2026-07-28
 * This application is the first thing built on ``port/esp-hosted/`` to have
 * run on the board. The port comes up, the vtable samples both side-band
 * lines, and a 5 MHz transaction returns the co-processor's idle filler frame
 * with the transfer reporting ``RET_OK``. ``make hil-c6`` re-runs it against
 * the app's own ``hil.conf``.
 *
 * The pin map comes from the probe
 * ``examples/ek_ra8d2/hw_validated/c6/c6_spi_probe``, which scope-qualified
 * every J26 hole on 2026-07-27 at SPI mode 3 / 1 MHz with zero bad checksums.
 *
 * That first run also found a defect in this application rather than in the
 * port: its verdict demanded a payload-header ``offset`` of twelve from a
 * frame that legitimately carries zero, so it reported FAIL at a healthy link.
 * The idle filler is now a pass in its own right -- see
 * ``src/c6_hosted_frame.c``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "c6_hosted.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "tx_api.h"

/**
 * @var s_c6_hosted_cpuclk_hz
 * @brief Cached CPUCLK0 rate, used to programme SysTick.
 * @details Read once during bring-up, before ThreadX starts.
 * @note Single-threaded; written once from ``main``.
 * @warning Zero until ::c6_hosted_setup_or_halt has run.
 * @since 0.1.0
 */
static uint32_t s_c6_hosted_cpuclk_hz;

/**
 * @var s_c6_hosted_pclka_hz
 * @brief Cached PCLKA rate, the SCI baud-clock source the port divides.
 * @details Handed to the port as ``ra8_esp_hosted_port_cfg_t::pclk_hz``; a
 * stale value silently mis-programs the SPI bit rate, so it is read from
 * the CGC rather than assumed.
 * @note Single-threaded; written once from ``main``.
 * @warning Zero until ::c6_hosted_setup_or_halt has run.
 * @since 0.1.0
 */
static uint32_t s_c6_hosted_pclka_hz;

/**
 * @var s_c6_hosted_init_err
 * @brief Exact result of ``ra8_esp_hosted_port_init``.
 * @details Recorded in ``tx_application_define`` and reported verbatim by
 * the worker, which is the first context that can print after the init.
 * @note Written once before the scheduler starts, read afterwards.
 * @warning A non-``k_ra8_ok`` value stops the transaction; the application
 *          never continues as though the port had come up.
 * @since 0.1.0
 */
static ra8_err_t s_c6_hosted_init_err = k_ra8_err_not_initialized;

/**
 * @var s_c6_hosted_worker
 * @brief Control block of the single application thread.
 * @details ThreadX requires statically allocated control blocks, as does
 * NASA Power of 10 Rule 3.
 * @note Owned by ThreadX once ``tx_thread_create`` succeeds.
 * @warning Never reused for a second thread.
 * @since 0.1.0
 */
static TX_THREAD s_c6_hosted_worker;

/**
 * @var s_c6_hosted_worker_name
 * @brief Thread name handed to ThreadX.
 * @details A mutable array rather than a string literal: ThreadX takes a
 * non-``const`` ``CHAR*``, and casting the qualifier away would trip the
 * project's ``-Wcast-qual``.
 * @note Read by ThreadX for as long as the thread exists.
 * @warning Never modified after creation; ThreadX keeps the pointer.
 * @since 0.1.0
 */
static CHAR s_c6_hosted_worker_name[] = "c6_hosted_init";

/**
 * @var s_c6_hosted_worker_stack
 * @brief Stack backing ::s_c6_hosted_worker.
 * @details Sized by ::k_c6_hosted_worker_stack; the deepest call the worker
 * makes is a formatter with a small on-stack digit array.
 * @note Written only by ThreadX and the worker itself.
 * @warning Shrinking this without re-measuring risks a silent overflow.
 * @since 0.1.0
 */
static UCHAR s_c6_hosted_worker_stack[k_c6_hosted_worker_stack];

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
static void c6_hosted_panic_halt(void)
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
 * @post ::s_c6_hosted_cpuclk_hz and ::s_c6_hosted_pclka_hz are non-zero and
 *       the console transmits at ::k_c6_hosted_uart_baud, 8N1.
 * @post SysTick runs and every peripheral this app uses is out of
 *       module-stop.
 * @note Never returns on error; a clock failure happens before the console
 *       exists, so it is visible only to a debugger.
 * @since 0.1.0
 */
static void c6_hosted_setup_or_halt(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    c6_hosted_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &s_c6_hosted_cpuclk_hz) != k_ra8_ok) {
    c6_hosted_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &s_c6_hosted_pclka_hz) != k_ra8_ok) {
    c6_hosted_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    c6_hosted_panic_halt();
  }
  if (ra8_time_init(s_c6_hosted_cpuclk_hz) != k_ra8_ok) {
    c6_hosted_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_c6_hosted_uart_baud) != k_ra8_ok) {
    c6_hosted_panic_halt();
  }
}

/**
 * @brief Print heartbeat lines forever, never returning.
 * @return Never returns.
 * @pre The console is up and ThreadX is scheduling.
 * @pre The caller has finished every one-shot bring-up step.
 * @post A line is emitted every ::k_c6_hosted_heartbeat_ms milliseconds.
 * @post No application state is modified beyond the local beat counter.
 * @note This is the one deliberately unbounded loop in the application. It
 *       exists so a bench with no scope can still see whether the
 *       co-processor ever raises a side-band line.
 * @since 0.1.0
 */
static void c6_hosted_heartbeat(void)
{
  uint32_t beat = 0U;
  while (1) {
    c6_hosted_puts("c6_hosted_init: heartbeat n=");
    c6_hosted_put_u32(beat);
    c6_hosted_puts(" events=");
    c6_hosted_put_u32(c6_hosted_event_count());
    c6_hosted_puts("\r\n");
    if (ra8_esp_hosted_port_is_ready()) {
      c6_hosted_report_sideband();
    } else {
      c6_hosted_puts("c6_hosted_init: port down -- side-band cannot be read\r\n");
    }
    beat++;
    tx_thread_sleep((ULONG)k_c6_hosted_heartbeat_ms);
  }
}

/**
 * @brief Worker thread: report the init, exercise the link, then heartbeat.
 * @param[in] thread_input ThreadX entry argument; unused.
 * @return Never returns.
 * @pre ``tx_application_define`` has recorded ::s_c6_hosted_init_err.
 * @pre The console is up.
 * @post On a successful init exactly one transaction ran and one verdict
 *       was printed.
 * @post On a failed init no transaction ran and the error was named.
 * @note The failure path deliberately does not continue: a port that did
 *       not come up has an unpopulated vtable, and calling through it would
 *       fault rather than report.
 * @since 0.1.0
 */
static void c6_hosted_worker_entry(ULONG thread_input)
{
  (void)thread_input;

  c6_hosted_puts("c6_hosted_init: port_init=");
  c6_hosted_puts(ra8_err_to_str(s_c6_hosted_init_err));
  c6_hosted_puts("\r\n");

  if (s_c6_hosted_init_err != k_ra8_ok) {
    c6_hosted_puts("c6_hosted_init: FAIL port init failed -- no transaction attempted\r\n");
    c6_hosted_heartbeat();
    return;
  }

  ra8_delay_ms((uint32_t)k_c6_hosted_boot_wait_ms);
  c6_hosted_report_sideband();
  c6_hosted_run_transaction();
  ra8_esp_hosted_mem_dump("c6_hosted_init");
  c6_hosted_heartbeat();
}

/**
 * @brief ThreadX define hook: bring the port up and start the worker.
 * @param[in] first_unused_memory Free RAM handed over by the ThreadX port;
 *                                unused, every object here is static.
 * @return Nothing.
 * @pre ``tx_kernel_enter`` has been called and the clock cache is valid.
 * @pre No vendored esp-hosted entry point has been called yet.
 * @post ::s_c6_hosted_init_err holds the exact port-init result.
 * @post Exactly one worker thread was created.
 * @note The port creates ThreadX byte pools, mutexes and timers, so its
 *       init must run where ThreadX permits object creation -- here, not
 *       from ``main``.
 * @since 0.1.0
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  if (ra8_esp_hosted_port_set_event_cb(c6_hosted_on_event, nullptr) != k_ra8_ok) {
    c6_hosted_puts("c6_hosted_init: event handler registration refused\r\n");
  }

  const ra8_esp_hosted_port_cfg_t cfg = {
    .pclk_hz      = s_c6_hosted_pclka_hz,
    .sck_hz       = (uint32_t)k_c6_hosted_sck_hz,
    .edge_poll_ms = (uint16_t)k_c6_hosted_edge_poll_ms,
    .sci_channel  = (uint8_t)k_ra8_board_pmod1_sci_channel,
  };
  s_c6_hosted_init_err = ra8_esp_hosted_port_init(&cfg);

  if (tx_thread_create(&s_c6_hosted_worker,
                       s_c6_hosted_worker_name,
                       c6_hosted_worker_entry,
                       0U,
                       s_c6_hosted_worker_stack,
                       (ULONG)sizeof(s_c6_hosted_worker_stack),
                       (UINT)k_c6_hosted_worker_prio,
                       (UINT)k_c6_hosted_worker_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    c6_hosted_puts("c6_hosted_init: worker thread creation failed\r\n");
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: clocks, console, banner, then ThreadX.
 * @return Never returns.
 * @pre ``Reset_Handler`` has copied ``.data`` and zeroed ``.bss``.
 * @pre ``SystemInit`` has set VTOR, the FPU and the priority grouping.
 * @post The banner and the resolved pin map were printed exactly once.
 * @post Control passed to ThreadX and never came back.
 * @note Everything after ``tx_kernel_enter`` happens on ThreadX; the
 *       panic-halt below is reached only if the kernel refuses to start.
 * @since 0.1.0
 */
int32_t main(void)
{
  c6_hosted_setup_or_halt();
  ra8_isr_globals_enable();

  c6_hosted_print_banner(s_c6_hosted_cpuclk_hz, s_c6_hosted_pclka_hz);
  c6_hosted_print_pin_map();
  c6_hosted_puts("c6_hosted_init: entering ThreadX; port init runs from "
                 "tx_application_define\r\n");

  tx_kernel_enter();

  c6_hosted_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
