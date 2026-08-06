/**
 * @file main.c
 * @brief Minimal ThreadX module - proves module isolation works.
 *
 * @details
 * This is the module-side entry point. It runs inside the MPU sandbox
 * managed by the kernel-side Module Manager. The module creates a single
 * thread that increments a counter and sleeps, proving it can execute
 * ThreadX syscalls via the module dispatch mechanism.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#define TXM_MODULE

#include "txm_module.h"

/* Module-local pool for thread stacks. */
static ULONG s_pool_space[512U / sizeof(ULONG)];

/* ThreadX objects - allocated from kernel object pool via txm_module_object_allocate. */
static TX_THREAD*    s_thread;
static TX_BYTE_POOL* s_byte_pool;

/* Counter incremented by the module thread. */
static volatile ULONG s_tick_count;

/**
 * @brief Module thread entry - sleeps in a loop, incrementing a counter.
 */
static void hello_thread_entry(ULONG input)
{
  (void)input;
  for (;;) {
    s_tick_count++;
    tx_thread_sleep(100U); /* 100 ticks ~= 1 second at default tick rate */
  }
}

/**
 * @brief Module entry point - called by the Module Manager when the module starts.
 *
 * @param[in] id  Module ID passed by the Module Manager.
 */
void demo_module_start(ULONG id);

void demo_module_start(ULONG id)
{
  (void)id;
  CHAR* stack_ptr;

  /* Allocate control blocks from the kernel's object pool. */
  txm_module_object_allocate((void**)&s_thread, sizeof(TX_THREAD));
  txm_module_object_allocate((void**)&s_byte_pool, sizeof(TX_BYTE_POOL));

  /* Create a byte pool for the thread stack. */
  tx_byte_pool_create(s_byte_pool, "mod_pool", (UCHAR*)s_pool_space, sizeof(s_pool_space));

  /* Allocate stack from the pool. */
  tx_byte_allocate(s_byte_pool, (VOID**)&stack_ptr, 256U, TX_NO_WAIT);

  /* Create the module's thread. */
  tx_thread_create(s_thread,
                   "mod_hello",
                   hello_thread_entry,
                   0U,
                   stack_ptr,
                   256U,
                   15U,
                   15U, /* Priority 15 (lower than kernel threads) */
                   TX_NO_TIME_SLICE,
                   TX_AUTO_START);
}
