/***********************************************************************************************************************
 * @file    port/lwip/arch/sys_arch.h
 * @brief   ThreadX-backed typedefs for lwIP's sys_arch port.
 *
 * @details
 * lwIP's `lwip/sys.h` requires the platform port to define the following
 * types before it is included:
 *   - sys_sem_t   : counting semaphore
 *   - sys_mutex_t : mutex
 *   - sys_mbox_t  : bounded message queue (pointer-sized messages)
 *   - sys_thread_t: thread handle
 *   - sys_prot_t  : protection cookie for SYS_LIGHTWEIGHT_PROT
 *
 * Each typedef wraps the corresponding ThreadX primitive
 * (TX_SEMAPHORE / TX_MUTEX / TX_QUEUE / TX_THREAD).
 *
 * @par Mailbox storage
 * ThreadX queues do not allocate their backing buffer; the caller hands
 * one in via tx_queue_create. To avoid any heap use the mailbox struct
 * carries the slot array inline, sized at compile time by
 * RA_LWIP_MBOX_SIZE (default 32 pointers per mbox).
 *
 * @par NASA Power of 10 Compliance
 * Rule 3 deviation -- this is a vendored-stack shim. The mailbox slot
 * array is statically sized, so no runtime allocation is performed.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * @license   SPDX-License-Identifier: MIT
 **********************************************************************************************************************/

#pragma once

#include <stdint.h>
#include "tx_api.h"

#ifndef RA_LWIP_MBOX_SIZE
#define RA_LWIP_MBOX_SIZE (32U)
#endif

/**
 * @struct ra_lwip_sem_s
 * @brief  ThreadX-backed counting semaphore wrapper.
 */
typedef struct ra_lwip_sem_s {
    TX_SEMAPHORE tx;       /**< Underlying ThreadX semaphore object */
    uint8_t      valid;    /**< Non-zero when sem has been created  */
} sys_sem_t;

/**
 * @struct ra_lwip_mutex_s
 * @brief  ThreadX-backed mutex wrapper (priority-inherit enabled).
 */
typedef struct ra_lwip_mutex_s {
    TX_MUTEX tx;           /**< Underlying ThreadX mutex object */
    uint8_t  valid;        /**< Non-zero when mutex has been created */
} sys_mutex_t;

/**
 * @struct ra_lwip_mbox_s
 * @brief  ThreadX-backed bounded message queue holding ULONG-sized slots.
 *
 * @details
 * ThreadX queues store fixed-size messages. lwIP only ever posts a single
 * `void*`, so each slot is one ULONG (TX_1_ULONG). The backing storage is
 * carried inline so the stack never calls malloc.
 */
typedef struct ra_lwip_mbox_s {
    TX_QUEUE tx;                            /**< Underlying ThreadX queue object */
    ULONG    storage[RA_LWIP_MBOX_SIZE];    /**< Inline slot array (ULONG per msg) */
    uint8_t  valid;                         /**< Non-zero when queue has been created */
} sys_mbox_t;

/**
 * @typedef sys_thread_t
 * @brief   Pointer to a ThreadX thread control block.
 *
 * @details
 * The TCB and thread stack are allocated by `sys_thread_new()` from a
 * static pool inside `sys_arch.c` (see RA_LWIP_MAX_THREADS).
 */
typedef TX_THREAD* sys_thread_t;

/**
 * @typedef sys_prot_t
 * @brief   Cookie returned by sys_arch_protect() / consumed by sys_arch_unprotect().
 *
 * @details
 * SYS_LIGHTWEIGHT_PROT is implemented as a recursive ThreadX mutex
 * acquired with TX_WAIT_FOREVER. The cookie is the recursion depth at
 * the point of acquire (carried for diagnostic purposes only).
 */
typedef uint32_t sys_prot_t;
