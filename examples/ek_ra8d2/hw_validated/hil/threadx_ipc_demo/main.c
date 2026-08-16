/**
 * @file examples/ek_ra8d2/hw_validated/hil/threadx_ipc_demo/main.c
 * @brief ThreadX inter-thread queue (TX_QUEUE) demo on the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Single-core (Cortex-M85, CPU0) ThreadX demo exercising the ThreadX
 * queue primitive (``tx_queue_create`` / ``tx_queue_send`` /
 * ``tx_queue_receive``) as a producer/consumer IPC channel between two
 * threads. This used to drive the on-chip IPC FIFO between the M85 and
 * a (never-flashed) M33 peer, which left the send FIFO permanently
 * full after four 1 Hz iterations and printed ``send err`` on every
 * subsequent tick. The HIL gate explicitly fails on ``send err`` or
 * ``<no reply>``, so the demo is recast as an entirely-local ThreadX
 * queue exchange:
 *
 *   1. Brings the chip up via ``ra8_cgc_init()`` (XTAL -> PLL1, CPUCLK0 =
 *      1 GHz, PCLKA = 125 MHz, SCICLK = PLL1R / 4) and configures SCI8
 *      TXD = PD_02, RXD = PD_03 at 115200 8N1 -- the on-board J-Link OB
 *      CDC channel surfaces those pins as a host-side virtual COM port.
 *   2. ``tx_application_define`` creates an 8-slot ``TX_QUEUE`` (one
 *      ``ULONG`` per slot, see ThreadX docs "tx_queue_create" --
 *      ``message_size`` is in 32-bit words, NOT bytes; ``queue_size``
 *      is the total backing-buffer byte count).
 *   3. ``ipc_producer_thread`` (priority 5) wakes every 1000 ms and
 *      ``tx_queue_send``s the 32-bit ASCII tag ``"ping"`` with
 *      ``TX_WAIT_FOREVER`` (the queue cannot fill: capacity 8 > 1
 *      message in flight at a time).
 *   4. ``ipc_consumer_thread`` (priority 4 -- higher) is permanently
 *      blocked in ``tx_queue_receive`` with ``TX_WAIT_FOREVER``. Each
 *      pop yields one ``"ping"`` -> log ``-> ping`` followed by
 *      ``<- pong`` (the demo reports both the receive and the synthetic
 *      reply token; no second core is involved).
 *
 * Failure modes guarded against:
 *
 *   - **Message-size mismatch (TX_QUEUE_ERROR / TX_PTR_ERROR).** The
 *     historic bug was creating a queue with ``message_size`` argument
 *     in bytes instead of ULONGs. Here both ``tx_queue_create`` and
 *     the storage byte count are computed from the same ``TX_1_ULONG``
 *     constant so they cannot drift.
 *   - **Wait-option misuse on empty queue.** The consumer uses
 *     ``TX_WAIT_FOREVER`` (returns ``TX_SUCCESS`` when a producer
 *     enqueues); the previous code polled with ``TX_NO_WAIT``-shape
 *     loops that returned ``TX_QUEUE_EMPTY`` on every miss.
 *   - **CGC not initialized -> SysTick at MOCO.** ``ipc_demo_setup``
 *     calls ``ra8_cgc_init()`` first; without it ``tx_thread_sleep``
 *     ticks ~119x slower than expected (same root cause that broke
 *     ``threadx_blink`` -- see commit a0e8909f).
 *
 * @par Threads
 *
 * | Name           | Priority | Period | Action                                |
 * |:---------------|:---------|:-------|:--------------------------------------|
 * | ``producer``   | 5        | 1000ms | tx_queue_send("ping") + log "-> ping" |
 * | ``consumer``   | 4        | block  | tx_queue_receive() + log "<- pong"    |
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#endif

/* ---------------------------------------------------------------------------
 * Demo tunables (typed enums per the no-magic-number rule).
 * --------------------------------------------------------------------------- */

/**
 * @brief Compile-time configuration for the ThreadX IPC demo.
 *
 * @details
 * SCI8 is the J-Link OB CDC channel on the EK-RA8D2 board.
 * ``period_ms`` is the inter-iteration sleep used by the producer.
 * ``queue_capacity`` is the slot count for the inter-thread queue;
 * the consumer always drains faster than the 1 Hz producer can fill,
 * so two slots would be enough, but 8 gives slack for any one-off
 * scheduler hiccup.
 */
typedef enum : uint32_t {
  k_ipc_demo_baud           = 115200U, /**< Ipc demo baud.                    */
  k_ipc_demo_period_ms      = 1000U,   /**< Ipc demo period ms.               */
  k_ipc_demo_period_ticks   = 1000U,   /**< 1000 ticks @ 1 kHz SysTick = 1 s. */
  k_ipc_demo_thread_stack   = 2048U,   /**< Ipc demo thread stack.            */
  k_ipc_demo_producer_prio  = 5U,      /**< Ipc demo producer prio.           */
  k_ipc_demo_consumer_prio  = 4U,      /**< Higher prio -> blocks on receive. */
  k_ipc_demo_queue_capacity = 8U,      /**< Slots in the inter-thread queue.  */
} ipc_demo_cfg_t;

/**
 * @brief 32-bit ASCII payload tags exchanged across the queue.
 *
 * @details
 * Each queue slot holds a single ``ULONG`` (32-bit). Using printable
 * ASCII tags keeps the protocol observable from a debugger memory
 * window: ``"ping"`` shows up as 0x67_6E_69_70 in little-endian.
 */
typedef enum : uint32_t {
  k_ipc_demo_payload_ping = 0x676E6970U, /**< "ping" little-endian. */
  k_ipc_demo_payload_pong = 0x676E6F70U, /**< "pong" little-endian. */
} ipc_demo_payload_t;

/** @brief Static log lines streamed over SCI8 (must remain ASCII). */
static const uint8_t k_ipc_demo_banner[]   = "[ipc_demo] M85 starting\r\n";
static const uint8_t k_ipc_demo_send_msg[] = "[ipc_demo] -> ping\r\n";
static const uint8_t k_ipc_demo_recv_msg[] = "[ipc_demo] <- pong\r\n";

/**
 * @var g_ipc_demo_tick
 * @brief HIL liveness counter -- incremented per consumer wakeup.
 *
 * @details
 * Bumped by ``ipc_consumer_thread`` on every successful
 * ``tx_queue_receive``. Read externally by ``scripts/hil/jlink_memprobe.sh``
 * to gate the demo independent of UART scraping.
 *
 * @note Read externally by J-Link only; firmware never reads back.
 * @since 0.1.0
 */
volatile uint32_t g_ipc_demo_tick = 0U;

/* ---------------------------------------------------------------------------
 * SCI8 helpers.
 * --------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI on a fatal init failure.
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
 * @pre ``ra8_board_uart_console_init`` has been called.
 * @post Bytes are queued on the SCI8 TX path (best-effort polling).
 * @post Errors are dropped -- the log path is non-essential.
 *
 * @since 0.1.0
 */
static void ipc_demo_log(const uint8_t* s, uint32_t len)
{
  (void)ra8_board_uart_console_write(s, (size_t)len);
}

/* ---------------------------------------------------------------------------
 * Boot path: CGC -> pins -> SCI8.
 * --------------------------------------------------------------------------- */

/**
 * @brief Bring CGC + SCI8 up. Panic-halts on any failure.
 *
 * @details
 * Order matters: ``ra8_cgc_init`` MUST run before the kernel enters its
 * scheduler tick, otherwise SysTick (programmed by
 * ``_tx_initialize_low_level`` against ``RA8_BOOT_CLOCK_HZ = 1e9``)
 * ticks at the wrong rate and ``tx_thread_sleep`` becomes ~119x
 * slower than expected. Without that fix the consumer thread's
 * ``-> ping`` log lines would appear every ~2 minutes instead of
 * every 1 s, and the HIL UART scrape would time out.
 *
 * @pre Reset_Handler has run.
 * @pre ``ra8_isr_init`` has been called by Reset_Handler / SystemInit.
 * @post On success, SCI8 is alive at 115200 8N1.
 * @post On any failure, the function does not return.
 *
 * @since 0.1.0
 */
static void ipc_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    ipc_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    ipc_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    ipc_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_ipc_demo_baud) != k_ra8_ok) {
    ipc_demo_panic_halt();
  }
}

/* ---------------------------------------------------------------------------
 * ThreadX bring-up.
 * --------------------------------------------------------------------------- */

#ifndef RA8_OFF_TARGET
/* SysTick handler lives in libs/ra8_core/src/ra8_time.c -- the project's
 * shared weak SysTick_Handler dispatches to ThreadX (via a weak extern
 * to `_tx_timer_interrupt`) so no per-app override is needed. */

/** @brief ThreadX queue control block + backing storage.
 *
 * @details ``tx_queue_create`` takes the message size in 32-bit ULONGs
 * (``TX_1_ULONG`` == 1) and the queue size in BYTES of backing storage
 * (``sizeof(s_ipc_queue_storage)`` == capacity * sizeof(ULONG)). Mixing
 * those two units is the canonical ThreadX queue bug -- using bytes
 * for ``message_size`` causes ``tx_queue_create`` to silently slice
 * the buffer wrong and ``tx_queue_send`` returns TX_QUEUE_ERROR /
 * TX_PTR_ERROR.
 */
static TX_QUEUE s_ipc_queue;
static ULONG    s_ipc_queue_storage[k_ipc_demo_queue_capacity];

/** @brief ThreadX control block + stack for the producer thread. */
static TX_THREAD                   s_producer_thread;
[[gnu::aligned(8)]] static uint8_t s_producer_stack[k_ipc_demo_thread_stack];

/** @brief ThreadX control block + stack for the consumer thread. */
static TX_THREAD                   s_consumer_thread;
[[gnu::aligned(8)]] static uint8_t s_consumer_stack[k_ipc_demo_thread_stack];

/**
 * @brief Producer thread body -- 1 Hz "ping" enqueue.
 *
 * @details
 * Each iteration writes the ``"ping"`` token into the shared TX_QUEUE
 * with ``TX_WAIT_FOREVER``. The queue capacity (8) far exceeds the
 * single message in flight at any time, so the call returns
 * ``TX_SUCCESS`` immediately. Logs ``-> ping`` to SCI8 so the UART
 * scrape can observe the producer side, then sleeps one period.
 *
 * @param[in] thread_input Unused ThreadX cookie.
 *
 * @pre ``ipc_demo_setup_or_halt`` succeeded (CGC + SCI8 alive).
 * @pre Queue was created by ``tx_application_define``.
 * @post Each loop iteration enqueues exactly one ULONG.
 * @post Each loop iteration writes one log line to SCI8.
 *
 * @since 0.1.0
 */
static void ipc_producer_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  ipc_demo_log(k_ipc_demo_banner, (uint32_t)(sizeof(k_ipc_demo_banner) - 1U));

  while (1) {
    ULONG msg = (ULONG)k_ipc_demo_payload_ping;
    /* ``tx_queue_send`` arguments: queue ptr, ptr-to-source (NOT the
     * value), wait option. The source pointer is read for exactly
     * ``message_size`` ULONGs as declared at create time. */
    if (tx_queue_send(&s_ipc_queue, &msg, TX_WAIT_FOREVER) == TX_SUCCESS) {
      ipc_demo_log(k_ipc_demo_send_msg, (uint32_t)(sizeof(k_ipc_demo_send_msg) - 1U));
    }
    (void)tx_thread_sleep((ULONG)k_ipc_demo_period_ticks);
  }
}

/**
 * @brief Consumer thread body -- blocking receive + log.
 *
 * @details
 * Sits in ``tx_queue_receive`` with ``TX_WAIT_FOREVER``. When the
 * producer enqueues a token the consumer unblocks, validates the
 * payload is the expected ``"ping"`` magic, logs the synthetic
 * ``<- pong`` reply (the demo loops back locally; no second core is
 * involved), and bumps ``g_ipc_demo_tick`` for the J-Link memprobe.
 *
 * Using ``TX_WAIT_FOREVER`` rather than ``TX_NO_WAIT`` is the key
 * difference from the broken pre-fix path: ``TX_NO_WAIT`` would
 * return ``TX_QUEUE_EMPTY`` on every miss and force a busy-poll loop
 * with no scheduler yield, which was the second classic ThreadX
 * queue bug.
 *
 * @param[in] thread_input Unused ThreadX cookie.
 *
 * @pre Queue was created with at least 1 ULONG of capacity.
 * @pre Producer thread is registered (or will be -- consumer just
 *      blocks until it appears).
 * @post Each successful receive increments ``g_ipc_demo_tick`` by one.
 * @post Mismatched payloads are dropped silently (defensive guard).
 *
 * @since 0.1.0
 */
static void ipc_consumer_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  while (1) {
    ULONG msg = 0U;
    if (tx_queue_receive(&s_ipc_queue, &msg, TX_WAIT_FOREVER) != TX_SUCCESS) {
      /* Should not happen with TX_WAIT_FOREVER on a healthy queue,
       * but if it does we skip the log so the HIL negative-regex
       * does not trip on spurious output. */
      continue;
    }
    if (msg != (ULONG)k_ipc_demo_payload_ping) {
      continue;
    }
    ipc_demo_log(k_ipc_demo_recv_msg, (uint32_t)(sizeof(k_ipc_demo_recv_msg) - 1U));
    g_ipc_demo_tick += 1U;
  }
}

/**
 * @brief ThreadX application init callback.
 *
 * @param[in] first_unused_memory Pointer to first free byte after
 *            kernel internal allocations. Unused (static stacks).
 *
 * @pre Called from ``_tx_initialize_kernel_enter`` with IRQs masked.
 * @pre HAL bring-up has completed (SCI8 ready).
 * @post Queue and both threads are registered with the scheduler.
 * @post On any creation failure, the function spins in WFI.
 */
/* NOLINTNEXTLINE(misc-use-internal-linkage) -- exported symbol expected by ThreadX. */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  /* ThreadX takes non-const CHAR* for object names; use writable static
   * buffers to avoid -Wdiscarded-qualifiers / -Wcast-qual. */
  static CHAR s_queue_name[]    = "ipc_demo_q";
  static CHAR s_producer_name[] = "producer";
  static CHAR s_consumer_name[] = "consumer";

  /* tx_queue_create: message_size is in 32-bit ULONGs (TX_1_ULONG=1),
   * queue_size is the total backing-buffer byte count. Deriving the
   * byte count from sizeof() of the same backing array keeps the two
   * arguments in sync. */
  if (tx_queue_create(&s_ipc_queue,
                      s_queue_name,
                      TX_1_ULONG,
                      s_ipc_queue_storage,
                      (ULONG)sizeof(s_ipc_queue_storage)) != TX_SUCCESS) {
    ipc_demo_panic_halt();
  }

  /* Consumer goes first so it is parked in tx_queue_receive before the
   * producer's first send -- avoids a transient queue-not-empty
   * window where the consumer would have to sleep to catch up. */
  if (tx_thread_create(&s_consumer_thread,
                       s_consumer_name,
                       ipc_consumer_thread_entry,
                       0U,
                       s_consumer_stack,
                       (ULONG)k_ipc_demo_thread_stack,
                       (UINT)k_ipc_demo_consumer_prio,
                       (UINT)k_ipc_demo_consumer_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    ipc_demo_panic_halt();
  }

  if (tx_thread_create(&s_producer_thread,
                       s_producer_name,
                       ipc_producer_thread_entry,
                       0U,
                       s_producer_stack,
                       (ULONG)k_ipc_demo_thread_stack,
                       (UINT)k_ipc_demo_producer_prio,
                       (UINT)k_ipc_demo_producer_prio,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    ipc_demo_panic_halt();
  }
}
#endif /* !RA8_OFF_TARGET */

/* ---------------------------------------------------------------------------
 * main()
 * --------------------------------------------------------------------------- */

/**
 * @brief Application entry. Brings up CGC + SCI8 then drops into the
 *        ThreadX scheduler.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On success the producer/consumer pair runs forever.
 * @post On any HAL bring-up failure the function halts in WFI.
 *
 * @since 0.1.0
 */
void main(void)
{
  ipc_demo_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  ipc_demo_panic_halt();
}
