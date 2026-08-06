/**
 * @file tz_stubs.c
 * @brief Weak stubs for TrustZone secure-stack symbols.
 *
 * @details
 * When tx_user.h defines TX_SINGLE_MODE_NON_SECURE, the ThreadX secure
 * stack implementation is compiled out. But the module manager's scheduler
 * assembly unconditionally references these symbols. Provide empty weak
 * definitions so the linker succeeds in non-TZ builds.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "tx_api.h"

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,clang-diagnostic-missing-prototypes)
__attribute__((weak)) UINT _tx_thread_secure_mode_stack_allocate(TX_THREAD* thread_ptr,
                                                                 ULONG      stack_size)
{
  (void)thread_ptr;
  (void)stack_size;
  return TX_FEATURE_NOT_ENABLED;
}

__attribute__((weak)) UINT _tx_thread_secure_mode_stack_free(TX_THREAD* thread_ptr)
{
  (void)thread_ptr;
  return TX_FEATURE_NOT_ENABLED;
}

__attribute__((weak)) void _tx_thread_secure_stack_context_save(TX_THREAD* thread_ptr)
{
  (void)thread_ptr;
}

__attribute__((weak)) void _tx_thread_secure_stack_context_restore(TX_THREAD* thread_ptr)
{
  (void)thread_ptr;
}
// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,clang-diagnostic-missing-prototypes)
/* ---- main() -------------------------------------------------------------- */
/* The board's Reset_Handler calls main(). ThreadX apps enter the kernel
 * via tx_kernel_enter(), which never returns. */
int main(void)
{
  tx_kernel_enter();
  return 0;
}

/* ---- Linker symbol stub -------------------------------------------------- */
/* Normally emitted by the app linker script. The board's default LD
 * doesn't define it, so provide a fallback pointing at the end of BSS. */
extern uint8_t g_ra8_ls_ebss[];
uint8_t*       g_ra8_threadx_unused_memory_start = g_ra8_ls_ebss;
