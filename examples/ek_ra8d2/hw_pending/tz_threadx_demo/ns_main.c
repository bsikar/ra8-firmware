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
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra_err.h"
#include "ra_nsc.h"
#include "tx_api.h"

/* Linker symbols for BSS, Stack, and Run memory */
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
  k_ns_ui_thread_stack_size   = 2048U, /**< Stack size for UI thread. */
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
  k_ns_ui_heartbeat_iters   = 50U,  /**< UI heartbeat log cadence. */
  k_ns_work_heartbeat_iters = 10U,  /**< Worker heartbeat log cadence. */
} ns_log_cadence_t;

/* External declarations for thread tick hooks */
extern void              PendSV_Handler(void);
extern void              _tx_timer_interrupt(void);
extern volatile uint32_t g_ra_threadx_systick_ready;

/**
 * @brief SysTick exception handler for the Non-Secure ThreadX OS.
 */
static void ns_systick_handler(void)
{
  if (g_ra_threadx_systick_ready != 0U) {
    _tx_timer_interrupt();
  }
}

/**
 * @brief Park the CPU in an idle loop if a fatal startup error occurs.
 */
static void __attribute__((noreturn)) ns_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief UI thread entry: logs periodically to simulate UI rendering loops.
 */
static void ui_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  (void)ra_nsc_log_emit("UI", "UI thread started -- Simulating screen/event loop");

  uint32_t count = 0U;
  for (;;) {
    count++;
    if ((count % (uint32_t)k_ns_ui_heartbeat_iters) == 0U) {
      (void)ra_nsc_log_emit("UI", "UI Loop: Refreshing screen layout...");
    }
    tx_thread_sleep(20U); /* ~20 ticks */
  }
}

/**
 * @brief Worker thread entry: simulates background sensors/storage processing.
 */
static void work_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  (void)ra_nsc_log_emit("WORK", "Worker thread started -- Processing background tasks");

  uint32_t count = 0U;
  for (;;) {
    count++;
    if ((count % (uint32_t)k_ns_work_heartbeat_iters) == 0U) {
      (void)ra_nsc_log_emit("WORK", "Worker Loop: Reading battery & sensor state via NSC veneers");
    }
    tx_thread_sleep(100U); /* ~100 ticks */
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
                         10U, /* Priority             */
                         10U, /* Preemption threshold */
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);

  /* Create the Worker thread */
  (void)tx_thread_create(&s_work_thread,
                         "Worker Thread",
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

  /* Announce that Non-Secure is online! */
  (void)ra_nsc_log_emit("BOOT", "tz_threadx_demo: Non-Secure world online!");

  /* Enter ThreadX RTOS kernel (never returns) */
  tx_kernel_enter();

  ns_panic_halt();
}

/* =============================================================================
 * Non-Secure Vector Table
 * =============================================================================
 */
typedef void (*ns_exc_handler_t)(void);

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
