/**
 * @file ns_main.c
 * @brief Non-Secure main entry point: launches ThreadX and the e-reader UI.
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * This file is executed after the Secure world bootloader configures the SAU
 * and jumps to ns_reset_handler. It operates entirely in Non-Secure state,
 * leveraging ThreadX for multitasking and calling Secure services via
 * NSC veneers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_err.h"
#include "ra_nsc.h"
#include "tx_api.h"

/* Linker symbols for BSS and Stack */
extern uint32_t g_ra_ls_ns_bss_start;
extern uint32_t g_ra_ls_ns_bss_end;
extern uint32_t g_ra_ls_ns_stack_top;
extern uint32_t g_ra_ls_ns_run_start;

/** @brief Address of NS VTOR register. */
typedef enum : uintptr_t {
  k_ns_scb_vtor_addr = 0xE000ED08U,
} ns_scb_addr_t;

/**
 * @enum ns_thread_stack_t
 * @brief Stack sizes for Non-Secure ThreadX threads.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_thread_stack_size  = 4096U, /**< Stack size for UI thread.     */
  k_ns_sys_thread_stack_size = 2048U, /**< Stack size for System thread. */
} ns_thread_stack_t;

static TX_THREAD s_ui_thread;
static TX_THREAD s_sys_thread;

static uint8_t s_ui_thread_stack[k_ns_ui_thread_stack_size];
static uint8_t s_sys_thread_stack[k_ns_sys_thread_stack_size];

/**
 * @enum ns_log_cadence_t
 * @brief Heartbeat-log cadence per thread (iterations between log lines).
 * @details Each thread logs a startup line once, then a periodic heartbeat so
 *          the serial / SWO log shows the system is alive without flooding it.
 *          The UI thread sleeps ~16 ms (~60 FPS) and the system thread ~100
 *          ticks, so both cadences land near a one-second heartbeat.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_heartbeat_frames = 60U, /**< UI heartbeat every ~60 frames (~1 s).    */
  k_ns_sys_heartbeat_iters = 10U, /**< System heartbeat every ~10 loops (~1 s). */
} ns_log_cadence_t;

/**
 * @enum ns_thread_sleep_t
 * @brief Per-thread loop sleep period, in ThreadX ticks.
 * @details The UI thread sleeps ~16 ticks (~16 ms at a 1 kHz tick, ~60 FPS); the
 *          system thread sleeps ~100 ticks (~100 ms) since it only supervises.
 *          Every loop must sleep so the other threads get to run.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_frame_ticks = 16U,  /**< UI loop sleep (~16 ms, ~60 FPS).     */
  k_ns_sys_poll_ticks = 100U, /**< System loop sleep (~100 ms cadence). */
} ns_thread_sleep_t;

/**
 * @enum ns_thread_prio_t
 * @brief ThreadX priority + preemption threshold for each Non-Secure thread.
 * @details Lower numbers are higher priority. The UI thread runs ahead of the
 *          system thread so input/rendering stays responsive; each thread uses
 *          the same value for its priority and its preemption threshold.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_priority  = 10U, /**< UI thread priority + preemption threshold.     */
  k_ns_sys_priority = 15U, /**< System thread priority + preemption threshold. */
} ns_thread_prio_t;

/* External declarations for thread tick hooks */
extern void              PendSV_Handler(void);
extern void              _tx_timer_interrupt(void);
extern volatile uint32_t g_ra_threadx_systick_ready;

/**
 * @brief SysTick exception handler for the Non-Secure ThreadX OS.
 * @details Drives the ThreadX timer: once the kernel is up
 *          (@c g_ra_threadx_systick_ready set), each SysTick forwards into
 *          @c _tx_timer_interrupt to service timeouts and the time-slice. Before
 *          the kernel is ready the tick is ignored so an early SysTick cannot
 *          enter the scheduler.
 * @return Nothing.
 * @pre Installed as the SysTick vector in the NS vector table.
 * @pre @c g_ra_threadx_systick_ready is set only after @c tx_kernel_enter.
 * @post When the kernel is ready, one ThreadX timer tick has been serviced.
 * @post When the kernel is not ready, no scheduler state is touched.
 * @note Runs in NS handler mode; not callable from thread context.
 * @since 0.1.0
 */
static void ns_systick_handler(void)
{
  if (g_ra_threadx_systick_ready != 0U) {
    _tx_timer_interrupt();
  }
}

/**
 * @brief Park the CPU in an idle loop if a fatal startup error occurs.
 * @details Spins forever issuing @c wfi so a Non-Secure bring-up failure halts
 *          deterministically (low power) instead of executing undefined state.
 *          Recovery requires an external reset.
 * @return Does not return (@c noreturn).
 * @retval None The function never returns to its caller.
 * @pre Called only on an unrecoverable NS startup error.
 * @pre Interrupts that could resume normal flow are not relied upon.
 * @post The core remains parked in a low-power wait loop until reset.
 * @post No further application code executes.
 * @note Not thread-safe; terminal error path only.
 * @since 0.1.0
 */
static void __attribute__((noreturn)) ns_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief UI thread entry: runs the e-reader UI frame loop.
 * @details The Non-Secure UI thread. By the time ThreadX schedules it the Secure
 *          substrate, the TrustZone boundary, and logging (SCI8) are all up, so it
 *          drives the frame loop for the lifetime of the system: each iteration
 *          advances the frame counter, emits a heartbeat on the configured
 *          cadence, and yields so the other threads run. Never returns.
 * @param[in] thread_input ThreadX entry argument (unused; reserved by the API).
 * @return Nothing (runs for the lifetime of the system).
 * @pre Registered as the UI thread's entry in ::tx_application_define.
 * @pre @c ra_nsc_periph_init has completed (Secure services available).
 * @post The UI loop runs continuously, yielding each iteration.
 * @post Heartbeat log lines are emitted on the configured cadence.
 * @note Runs on the NS UI thread; not thread-safe across threads.
 * @since 0.1.0
 */
static void ui_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  (void)ra_nsc_log_emit("UI", "UI thread started");

  uint32_t frame = 0U;
  for (;;) {
    frame++;
    if ((frame % (uint32_t)k_ns_ui_heartbeat_frames) == 0U) {
      (void)ra_nsc_log_emit("UI", "UI loop: frame heartbeat");
    }
    tx_thread_sleep((uint32_t)k_ns_ui_frame_ticks); /* ~16 ms; yields to the others. */
  }
}

/**
 * @brief System supervisor and storage background thread.
 * @details Lower-priority background worker for long-running, non-UI duties --
 *          SD card / filesystem setup, input polling, battery status, and OTA
 *          state. It runs an infinite supervisor loop, emitting a periodic
 *          heartbeat and sleeping each iteration so the UI thread stays
 *          responsive. It never returns.
 * @param[in] thread_input ThreadX entry argument (unused; reserved by the API).
 * @return Nothing (runs for the lifetime of the system).
 * @pre Registered as the system thread's entry in ::tx_application_define.
 * @pre ThreadX is running (the scheduler invoked this entry).
 * @post The supervisor loop runs continuously, yielding each iteration.
 * @post Heartbeat log lines are emitted on the configured cadence.
 * @note Runs on the NS system thread; not thread-safe across threads.
 * @since 0.1.0
 */
static void sys_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  (void)ra_nsc_log_emit("SYS", "System thread started (storage + supervisor)");
  uint32_t tick = 0U;
  for (;;) {
    tick++;
    if ((tick % (uint32_t)k_ns_sys_heartbeat_iters) == 0U) {
      (void)ra_nsc_log_emit("SYS", "System heartbeat: supervisor loop");
    }
    tx_thread_sleep((uint32_t)k_ns_sys_poll_ticks);
  }
}

/**
 * @brief Setup ThreadX threads and resources.
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  /* Create the UI thread */
  (void)tx_thread_create(&s_ui_thread,
                         "UI Thread",
                         ui_thread_entry,
                         0UL,
                         s_ui_thread_stack,
                         k_ns_ui_thread_stack_size,
                         k_ns_ui_priority, /* Priority             */
                         k_ns_ui_priority, /* Preemption threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);

  /* Create the System/Storage thread */
  (void)tx_thread_create(&s_sys_thread,
                         "System Thread",
                         sys_thread_entry,
                         0UL,
                         s_sys_thread_stack,
                         k_ns_sys_thread_stack_size,
                         k_ns_sys_priority, /* Priority             */
                         k_ns_sys_priority, /* Preemption threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}

/**
 * @brief Non-Secure Reset handler: entered via Secure-to-NS transition.
 */
void __attribute__((noreturn)) ns_reset_handler(void)
{
  /* Zero the NS BSS section */
  const uintptr_t bss_start = (uintptr_t)&g_ra_ls_ns_bss_start;
  const uintptr_t bss_end   = (uintptr_t)&g_ra_ls_ns_bss_end;
  for (uintptr_t addr = bss_start; addr < bss_end; addr += sizeof(uint32_t)) {
    *(volatile uint32_t*)addr = 0U;
  }

  /* Set the NS VTOR so exceptions vector correctly to NS handlers */
  *(volatile uint32_t*)k_ns_scb_vtor_addr = (uint32_t)(uintptr_t)&g_ra_ls_ns_run_start;

  /* Call Secure-side substrate initialization via NSC veneer gateway */
  if (ra_nsc_periph_init() != k_ra_ok) {
    ns_panic_halt();
  }

  /* Announce the Non-Secure world is live before handing off to ThreadX.
   * Logs flow NS -> ra_nsc_log_emit veneer -> Secure ra_log_info -> ITM
   * (visible on the J-Link SWO console). */
  (void)ra_nsc_log_emit("BOOT", "ra8d2-ereader: Non-Secure world online");

  /* Enter ThreadX RTOS kernel (never returns) */
  tx_kernel_enter();

  ns_panic_halt();
}

/* =============================================================================
 * Non-Secure Vector Table
 * =============================================================================
 */
typedef void (*ns_exc_handler_t)(void);

/**
 * @brief Default Non-Secure fault/NMI handler: park the CPU in a wfi halt.
 * @details Installed on the NMI, HardFault, and the other unhandled Non-Secure
 *          exception vectors. There is no recovery path, so it spins in a @c wfi
 *          loop and leaves the faulting context intact for a debugger.
 * @return Nothing (noreturn; control never leaves the halt loop).
 * @retval None This function does not return.
 * @pre Reached only via a Non-Secure exception vector.
 * @pre Recovery, if any, is the debugger's or watchdog's responsibility.
 * @post The CPU is parked in a wfi loop and runs no further NS code.
 * @post The faulting context is left unmodified for inspection.
 * @note Runs in NS exception context.
 * @since 0.1.0
 */
static void __attribute__((noreturn)) ns_nmi_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

__attribute__((section(".ns_vectors"), used)) const ns_exc_handler_t g_ra_ns_vector_table[16] = {
  (ns_exc_handler_t)&g_ra_ls_ns_stack_top, /* 0 Initial MSP_NS */
  ns_reset_handler,                        /* 1 Reset          */
  ns_nmi_halt,                             /* 2 NMI            */
  ns_nmi_halt,                             /* 3 HardFault      */
  ns_nmi_halt,                             /* 4 MemManage      */
  ns_nmi_halt,                             /* 5 BusFault       */
  ns_nmi_halt,                             /* 6 UsageFault     */
  ns_nmi_halt,                             /* 7 SecureFault    */
  0,                                       /* 8 Reserved       */
  0,                                       /* 9 Reserved       */
  0,                                       /* 10 Reserved      */
  ns_nmi_halt,                             /* 11 SVCall        */
  ns_nmi_halt,                             /* 12 DebugMonitor  */
  0,                                       /* 13 Reserved      */
  PendSV_Handler,                          /* 14 PendSV        */
  ns_systick_handler,                      /* 15 SysTick       */
};
