/**
 * @file examples/ek_ra8d2/threadx_ipc_demo/main.c
 * @brief Inter-Processor Communication (IPC) demo on the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Cortex-M85 (CPU0) hosts a single ThreadX thread that exercises the
 * ``ra_ipc`` HAL against the on-chip IPC mailbox unit:
 *
 *   1. Brings the chip up via ``ra_cgc_init()`` (XTAL -> PLL1, CPUCLK0 =
 *      1 GHz, PCLKA = 125 MHz, SCICLK = PLL1R / 4) and configures SCI8
 *      TXD = PD_02, RXD = PD_03 at 115200 8N1 -- the on-board J-Link OB
 *      CDC channel surfaces those pins as a host-side virtual COM port.
 *   2. Resolves the M85->M33 send channel via ``ra_ipc_channel_for_send``
 *      (CPU0 + pair 0 -> channel 2, FIFO10 of unit IPC1) and the
 *      M33->M85 receive channel via ``ra_ipc_channel_for_recv`` (CPU0 +
 *      pair 0 -> channel 0, FIFO00 of unit IPC0).
 *   3. Initialises both channels with FIFO reset + status clear, then
 *      installs the IPC unit-0 ISR (so a peer write on channel 0 raises
 *      an interrupt on the M85 side).
 *   4. Spawns ``ipc_demo_thread`` at priority 4. Each iteration:
 *        - Pushes one ``"ping"`` 32-bit token into the M85->M33 FIFO via
 *          ``ra_ipc_send_message_retry`` (bounded retry).
 *        - Drains every queued reply on the M33->M85 channel with
 *          ``ra_ipc_recv_message`` (best-effort -- if the M33 side has
 *          not produced a reply this iteration it just falls through).
 *        - Logs both events to SCI8 in printable ASCII so the host
 *          terminal can observe progress.
 *        - Sleeps 1000 ms.
 *
 * Channel pairing convention -- see ``ra_ipc.h`` table:
 *
 *   - M85 sends:    channel 2 (IPC1.FIFO10), payload tag ``"ping"``.
 *   - M85 receives: channel 0 (IPC0.FIFO00), payload tag ``"pong"``.
 *
 * The peer-side firmware that runs on the Cortex-M33 is **out of scope
 * for this repository**. To close the loop the M33 build must:
 *
 *   - Initialise channel 0 (its send side) and channel 2 (its receive
 *     side) with ``ra_ipc_init``.
 *   - In an IPC ISR or polled loop, read every word arriving on
 *     channel 2 and reply with the ASCII tag ``"pong"`` on channel 0
 *     via ``ra_ipc_send_message_retry``.
 *
 * Without that peer the demo still runs cleanly: ``ra_ipc_recv_message``
 * returns ``k_ra_err_no_data`` until the M33 side produces a token, and
 * the M85 side simply prints a ``"<no reply>"`` line every second.
 *
 * @par Threads
 *
 * | Name           | Priority | Period | Action                              |
 * |:---------------|:---------|:-------|:------------------------------------|
 * | ``ipc_demo``   | 4        | 1000ms | send "ping" + drain "pong" + log    |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_ipc_regs.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_ipc.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

#ifndef RA_SIMULATOR_MODE
#include "tx_api.h"
#endif

/* ---------------------------------------------------------------------------
 * Demo tunables (typed enums per the no-magic-number rule).
 * --------------------------------------------------------------------------- */

/**
 * @brief Compile-time configuration for the IPC demo.
 *
 * @details
 * SCI8 is the J-Link OB CDC channel on the EK-RA8D2 board. ``period_ms``
 * is the inter-iteration sleep used by the ThreadX thread. The thread
 * stack is sized comfortably above ``TX_MINIMUM_STACK`` so the FPU /
 * Helium register save area on a context switch does not overflow.
 */
typedef enum : uint32_t {
  k_ipc_demo_baud         = 115200U,
  k_ipc_demo_period_ms    = 1000U,
  k_ipc_demo_sci_channel  = 8U,
  k_ipc_demo_thread_stack = 2048U,
  k_ipc_demo_thread_prio  = 4U,
  k_ipc_demo_thread_ticks = 1000U,
  k_ipc_demo_max_drain    = 8U,
  k_ipc_demo_send_retries = 16U,
} ipc_demo_cfg_t;

/**
 * @brief 32-bit ASCII payload tags exchanged across the IPC FIFO.
 *
 * @details
 * The IPC mailbox is 32-bit per word, so each four-byte ASCII tag fits
 * in a single FIFO entry. ``ping`` flows M85->M33, ``pong`` flows
 * M33->M85. The peer-side firmware (out of scope) reverses the mapping.
 */
typedef enum : uint32_t {
  k_ipc_demo_payload_ping = 0x676E6970U, /**< "ping" little-endian.    */
  k_ipc_demo_payload_pong = 0x676E6F70U, /**< "pong" little-endian.    */
} ipc_demo_payload_t;

/** @brief Pinout for the on-board J-Link OB CDC channel. */
static const ra_port_pin_t k_ipc_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_ipc_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Static log lines streamed over SCI8 (must remain ASCII). */
static const uint8_t k_ipc_demo_banner[]   = "[ipc_demo] M85 starting\r\n";
static const uint8_t k_ipc_demo_send_msg[] = "[ipc_demo] -> ping\r\n";
static const uint8_t k_ipc_demo_recv_msg[] = "[ipc_demo] <- pong\r\n";
static const uint8_t k_ipc_demo_idle_msg[] = "[ipc_demo] <no reply>\r\n";
static const uint8_t k_ipc_demo_err_send[] = "[ipc_demo] send err\r\n";

/**
 * @brief Resolved channel ids cached at boot.
 *
 * @details
 * Channel ids are derived from ``ra_ipc_channel_for_*`` so the demo
 * does not hard-code FIFO mapping. CPU0 + pair 0 produces channel 2
 * for send and channel 0 for receive (see ``ra_ipc.h``).
 */
static uint8_t s_ipc_send_channel;
static uint8_t s_ipc_recv_channel;

/* ---------------------------------------------------------------------------
 * SCI8 helpers.
 * --------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI on a fatal init failure.
 *
 * @return Never returns.
 *
 * @pre Called only after a non-recoverable boot error.
 * @post CPU is parked at WFI; only debugger / reset wakes it.
 *
 * @since 0.1.0
 */
static void ipc_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Write a static ASCII string to SCI8.
 *
 * @param[in] s   NUL-terminated source buffer (caller-owned, static).
 * @param[in] len Length in bytes excluding the NUL terminator.
 *
 * @pre ``s`` non-NULL and ``len`` > 0.
 * @pre ``ra_sci_init`` has been called for ``k_ipc_demo_sci_channel``.
 * @post Bytes are queued on the SCI8 TX path (best-effort polling).
 * @post Errors are dropped -- the log path is non-essential.
 *
 * @since 0.1.0
 */
static void ipc_demo_log(const uint8_t* s, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_ipc_demo_sci_channel, s, len);
}

/* ---------------------------------------------------------------------------
 * Boot path: CGC -> pins -> SCI8 -> IPC channels.
 * --------------------------------------------------------------------------- */

/**
 * @brief Route PD_02 / PD_03 to SCI8 TXD / RXD.
 *
 * @return ``k_ra_ok`` on success, or the first failing PFS error code.
 *
 * @pre IOPORT module reachable, single-threaded init context.
 * @pre PD_02 and PD_03 are not already claimed by another peripheral.
 * @post On success both pins are in SCI async mode.
 * @post On failure no further pins are routed.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t ipc_demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_ipc_demo_pin_txd, k_ra_psel_sci_async, "ipc_demo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_ipc_demo_pin_rxd, k_ra_psel_sci_async, "ipc_demo.rxd8");
}

/**
 * @brief Bring CGC + SCI8 + IPC channels up. Panic-halts on any failure.
 *
 * @details
 * Resolves the M85->M33 send channel via ``ra_ipc_channel_for_send`` and
 * the M33->M85 receive channel via ``ra_ipc_channel_for_recv``, then
 * initialises both with FIFO reset + status clear and installs the IPC
 * unit-0 ISR so peer writes on the receive channel raise an interrupt
 * on the M85 NVIC.
 *
 * @pre Reset_Handler has run.
 * @pre ``ra_isr_init`` has been called by Reset_Handler / SystemInit.
 * @post On success, ``s_ipc_send_channel`` and ``s_ipc_recv_channel``
 *       hold valid channel ids.
 * @post On any failure, the function does not return.
 *
 * @since 0.1.0
 */
static void ipc_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    ipc_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    ipc_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    ipc_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    ipc_demo_panic_halt();
  }
  if (ipc_demo_pins_init() != k_ra_ok) {
    ipc_demo_panic_halt();
  }

  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_ipc_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_ipc_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    ipc_demo_panic_halt();
  }

  /* Resolve send / recv channels from the pair-convention helper so
   * the demo does not hard-code FIFO ids. */
  if (ra_ipc_channel_for_send(k_ra_ipc_core_cpu0, 0U, &s_ipc_send_channel) != k_ra_ok) {
    ipc_demo_panic_halt();
  }
  if (ra_ipc_channel_for_recv(k_ra_ipc_core_cpu0, 0U, &s_ipc_recv_channel) != k_ra_ok) {
    ipc_demo_panic_halt();
  }

  const ra_ipc_config_t send_cfg = {
    .channel      = s_ipc_send_channel,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra_ipc_event_fifo_full,
  };
  if (ra_ipc_init(&send_cfg) != k_ra_ok) {
    ipc_demo_panic_halt();
  }

  const ra_ipc_config_t recv_cfg = {
    .channel      = s_ipc_recv_channel,
    .reset_fifo   = true,
    .clear_status = true,
    .event_mask   = (uint32_t)k_ra_ipc_event_msg_ready,
  };
  if (ra_ipc_init(&recv_cfg) != k_ra_ok) {
    ipc_demo_panic_halt();
  }
}

/* ---------------------------------------------------------------------------
 * ThreadX bring-up.
 * --------------------------------------------------------------------------- */

#ifndef RA_SIMULATOR_MODE
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */
extern void _tx_timer_interrupt(void);
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,readability-identifier-naming) */

/** @brief ThreadX control block + stack for the IPC demo thread. */
static TX_THREAD s_ipc_thread;
static uint8_t   s_ipc_thread_stack[k_ipc_demo_thread_stack] __attribute__((aligned(8)));

/**
 * @brief Route SysTick to the ThreadX timer interrupt.
 *
 * @pre Called from exception context (IPSR == 15).
 * @pre ``_tx_initialize_low_level`` has programmed SysTick.
 * @post One ThreadX tick has elapsed.
 * @post PendSV may be pending if scheduler picked a new thread.
 */
void SysTick_Handler(void);
void SysTick_Handler(void)
{
  _tx_timer_interrupt();
}

/**
 * @brief IPC demo thread body.
 *
 * @details
 * One iteration per second:
 *   - Pushes the ``"ping"`` token into the M85->M33 FIFO with a bounded
 *     retry so a transient FIFO-full does not drop the message.
 *   - Drains the M33->M85 FIFO up to ``k_ipc_demo_max_drain`` words.
 *     Each successful read is logged as ``"<- pong"``. If the FIFO was
 *     empty for the whole window, ``"<no reply>"`` is logged instead so
 *     the operator can see the loop is alive even before the M33 peer
 *     firmware is flashed.
 *   - Sleeps ``k_ipc_demo_thread_ticks`` ticks.
 *
 * @param[in] thread_input Unused ThreadX cookie.
 *
 * @return Never returns.
 *
 * @pre ``ipc_demo_setup_or_halt`` has succeeded.
 * @pre ThreadX scheduler is running.
 * @post Each loop iteration writes at least one log line over SCI8.
 * @post FIFO state on both channels is consumer-clean at the bottom of
 *       each iteration (RX drain bounded by the loop budget).
 *
 * @since 0.1.0
 */
static void ipc_demo_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  ipc_demo_log(k_ipc_demo_banner, (uint32_t)(sizeof(k_ipc_demo_banner) - 1U));

  while (1) {
    /* Send "ping" with bounded retry. */
    if (ra_ipc_send_message_retry(s_ipc_send_channel,
                                  (uint32_t)k_ipc_demo_payload_ping,
                                  (uint16_t)k_ipc_demo_send_retries) == k_ra_ok) {
      ipc_demo_log(k_ipc_demo_send_msg, (uint32_t)(sizeof(k_ipc_demo_send_msg) - 1U));
    } else {
      ipc_demo_log(k_ipc_demo_err_send, (uint32_t)(sizeof(k_ipc_demo_err_send) - 1U));
    }

    /* Drain up to N "pong" replies. NASA Rule 2: bounded loop. */
    bool got_any = false;
    for (uint32_t i = 0U; i < (uint32_t)k_ipc_demo_max_drain; ++i) {
      uint32_t       word = 0U;
      const ra_err_t r    = ra_ipc_recv_message(s_ipc_recv_channel, &word);
      if (r != k_ra_ok) {
        break;
      }
      if (word == (uint32_t)k_ipc_demo_payload_pong) {
        ipc_demo_log(k_ipc_demo_recv_msg, (uint32_t)(sizeof(k_ipc_demo_recv_msg) - 1U));
      }
      got_any = true;
    }
    if (!got_any) {
      ipc_demo_log(k_ipc_demo_idle_msg, (uint32_t)(sizeof(k_ipc_demo_idle_msg) - 1U));
    }

    (void)tx_thread_sleep((ULONG)k_ipc_demo_thread_ticks);
  }
}

/**
 * @brief ThreadX application init callback.
 *
 * @param[in] first_unused_memory Pointer to first free byte after
 *            kernel internal allocations. Unused (static stack).
 *
 * @pre Called from ``_tx_initialize_kernel_enter`` with IRQs masked.
 * @pre HAL bring-up has completed (SCI8 + IPC ready).
 * @post ``s_ipc_thread`` is registered with the scheduler.
 * @post On creation failure, the function spins in WFI.
 */
/* NOLINTNEXTLINE(misc-use-internal-linkage) -- exported symbol expected by ThreadX. */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  /* ThreadX takes a non-const CHAR* for the thread name; use a writable
   * static buffer to avoid -Wdiscarded-qualifiers / -Wcast-qual. */
  static CHAR s_ipc_thread_name[] = "ipc_demo";
  UINT        err                 = tx_thread_create(&s_ipc_thread,
                                                     s_ipc_thread_name,
                                                     ipc_demo_thread_entry,
                                                     0U,
                                                     s_ipc_thread_stack,
                                                     (ULONG)k_ipc_demo_thread_stack,
                                                     (UINT)k_ipc_demo_thread_prio,
                                                     (UINT)k_ipc_demo_thread_prio,
                                                     TX_NO_TIME_SLICE,
                                                     TX_AUTO_START);
  if (err != TX_SUCCESS) {
    ipc_demo_panic_halt();
  }
}
#endif /* !RA_SIMULATOR_MODE */

/* ---------------------------------------------------------------------------
 * main()
 * --------------------------------------------------------------------------- */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up CGC + SCI8 + IPC then drops into
 *        the ThreadX scheduler.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On success the IPC demo thread runs forever.
 * @post On any HAL bring-up failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  ipc_demo_setup_or_halt();

  ra_isr_globals_enable();

#ifndef RA_SIMULATOR_MODE
  tx_kernel_enter();
#endif

  ipc_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
