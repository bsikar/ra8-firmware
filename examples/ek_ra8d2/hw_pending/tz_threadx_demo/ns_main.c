/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ns_main.c
 * @brief Non-Secure main entry point: launches ThreadX and demo worker threads.
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
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_nsc.h"
#include "tx_api.h"

/* Linker symbols for BSS, Stack, and Run memory */
extern uint32_t g_ra8_ls_ns_bss_start;
extern uint32_t g_ra8_ls_ns_bss_end;
extern uint32_t g_ra8_ls_ns_stack_top;
extern uint32_t g_ra8_ls_ns_run_start;

/** @brief Address of NS VTOR register. */
typedef enum : uintptr_t {
  k_ns_scb_vtor_addr = 0xE000ED08U, /**< Ns scb vtor address. */
} ns_scb_addr_t;

/**
 * @enum ns_thread_stack_t
 * @brief Stack sizes for Non-Secure ThreadX threads.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_thread_stack_size   = 2048U, /**< Stack size for UI thread.     */
  k_ns_work_thread_stack_size = 2048U, /**< Stack size for Worker thread. */
} ns_thread_stack_t;

static TX_THREAD s_ui_thread;
static TX_THREAD s_work_thread;

static uint8_t s_ui_thread_stack[k_ns_ui_thread_stack_size];
static uint8_t s_work_thread_stack[k_ns_work_thread_stack_size];

/**
 * @enum ns_log_cadence_t
 * @brief Heartbeat-log cadence per thread (iterations between log lines).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_heartbeat_iters   = 50U, /**< UI heartbeat log cadence.     */
  k_ns_work_heartbeat_iters = 10U, /**< Worker heartbeat log cadence. */
} ns_log_cadence_t;

/* External declarations for thread tick hooks */
extern void              PendSV_Handler(void);
extern void              _tx_timer_interrupt(void);
extern volatile uint32_t g_ra8_threadx_systick_ready;

/**
 * @brief SysTick exception handler for the Non-Secure ThreadX OS.
 */
static void ns_systick_handler(void)
{
  if (g_ra8_threadx_systick_ready != 0U) {
    _tx_timer_interrupt();
  }
}

/**
 * @brief Park the CPU in an idle loop if a fatal startup error occurs.
 */
[[noreturn]] static void ns_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @enum ns_thread_cadence_t
 * @brief Per-thread sleep cadence, in ThreadX ticks.
 * @details The UI thread runs the more responsive of the two so screen
 *          refreshes stay ahead of the background worker's batch work.
 * @invariant Both values are non-zero: a zero sleep would spin the scheduler.
 * @see ui_thread_entry
 * @see work_thread_entry
 */
typedef enum : uint16_t {
  k_ns_ui_sleep_ticks   = 20U,  /**< UI thread sleep between refreshes.   */
  k_ns_work_sleep_ticks = 100U, /**< Worker thread sleep between batches. */
} ns_thread_cadence_t;

/**
 * @brief UI thread entry: logs periodically to simulate UI rendering loops.
 */
static void ui_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  (void)ra8_nsc_log_emit("UI", "UI thread started -- Simulating screen/event loop");

  uint32_t count = 0U;
  for (;;) {
    count++;
    if ((count % (uint32_t)k_ns_ui_heartbeat_iters) == 0U) {
      (void)ra8_nsc_log_emit("UI", "UI Loop: Refreshing screen layout...");
    }
    tx_thread_sleep((ULONG)k_ns_ui_sleep_ticks);
  }
}

/**
 * @brief Worker thread entry: simulates background sensors/storage processing.
 */
static void work_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  (void)ra8_nsc_log_emit("WORK", "Worker thread started -- Processing background tasks");

  uint32_t count = 0U;
  for (;;) {
    count++;
    if ((count % (uint32_t)k_ns_work_heartbeat_iters) == 0U) {
      (void)ra8_nsc_log_emit("WORK", "Worker Loop: Reading battery & sensor state via NSC veneers");
    }
    tx_thread_sleep((ULONG)k_ns_work_sleep_ticks);
  }
}

/**
 * @brief Setup ThreadX threads and resources.
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  /* Create the UI thread. (CHAR*)(uintptr_t): the vendored ThreadX API takes
   * a non-const CHAR* for the thread name; the uintptr_t hop launders the
   * string-literal const without tripping -Wcast-qual. */
  (void)tx_thread_create(&s_ui_thread,
                         (CHAR*)(uintptr_t)"UI Thread",
                         ui_thread_entry,
                         0UL,
                         s_ui_thread_stack,
                         k_ns_ui_thread_stack_size,
                         10U, /* Priority             */
                         10U, /* Preemption threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);

  /* Create the Worker thread */
  (void)tx_thread_create(&s_work_thread,
                         (CHAR*)(uintptr_t)"Worker Thread",
                         work_thread_entry,
                         0UL,
                         s_work_thread_stack,
                         k_ns_work_thread_stack_size,
                         15U, /* Priority             */
                         15U, /* Preemption threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}

/**
 * @brief Non-Secure Reset handler: entered via Secure-to-NS transition.
 *
 * @details
 * Slot 1 of the NS vector table and the NS image's linker entry symbol; the
 * Secure boot BLXNS-es here. No header declares it because the only callers
 * are the hardware vector fetch and the linker -- the prototype below
 * satisfies -Wmissing-prototypes for this externally-linked boot symbol.
 *
 * @pre The Secure boot copied/armed the NS image and programmed VTOR_NS.
 * @pre Executes in NS Thread mode with MSP_NS from vector slot 0.
 * @post .bss is zeroed; ThreadX never returns.
 * @post Interrupt state is whatever tx_kernel_enter establishes.
 *
 * @note Not thread-safe; single-threaded NS boot only.
 * @since 0.1.0
 */
[[noreturn]] void ns_reset_handler(void);

[[noreturn]] void ns_reset_handler(void)
{
  /* Zero the NS BSS section */
  const uintptr_t bss_start = (uintptr_t)&g_ra8_ls_ns_bss_start;
  const uintptr_t bss_end   = (uintptr_t)&g_ra8_ls_ns_bss_end;
  for (uintptr_t addr = bss_start; addr < bss_end; addr += sizeof(uint32_t)) {
    *(volatile uint32_t*)addr = 0U;
  }

  /* Set the NS VTOR so exceptions vector correctly to NS handlers */
  *(volatile uint32_t*)k_ns_scb_vtor_addr = (uint32_t)(uintptr_t)&g_ra8_ls_ns_run_start;

  /* Call Secure-side substrate initialization via NSC veneer gateway */
  if (ra8_nsc_periph_init() != k_ra8_ok) {
    ns_panic_halt();
  }

  /* Announce that Non-Secure is online! */
  (void)ra8_nsc_log_emit("BOOT", "tz_threadx_demo: Non-Secure world online!");

  /* Enter ThreadX RTOS kernel (never returns) */
  tx_kernel_enter();

  ns_panic_halt();
}

/* =============================================================================
 * Non-Secure Vector Table
 * =============================================================================
 */
typedef void (*ns_exc_handler_t)(void);

[[noreturn]] static void ns_nmi_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[gnu::section(".ns_vectors"), gnu::used]] const ns_exc_handler_t g_ra8_ns_vector_table[16] = {
  (ns_exc_handler_t)&g_ra8_ls_ns_stack_top, /* 0 Initial MSP_NS */
  ns_reset_handler,                         /* 1 Reset          */
  ns_nmi_halt,                              /* 2 NMI            */
  ns_nmi_halt,                              /* 3 HardFault      */
  ns_nmi_halt,                              /* 4 MemManage      */
  ns_nmi_halt,                              /* 5 BusFault       */
  ns_nmi_halt,                              /* 6 UsageFault     */
  ns_nmi_halt,                              /* 7 SecureFault    */
  0,                                        /* 8 Reserved       */
  0,                                        /* 9 Reserved       */
  0,                                        /* 10 Reserved      */
  ns_nmi_halt,                              /* 11 SVCall        */
  ns_nmi_halt,                              /* 12 DebugMonitor  */
  0,                                        /* 13 Reserved      */
  PendSV_Handler,                           /* 14 PendSV        */
  ns_systick_handler,                       /* 15 SysTick       */
};
