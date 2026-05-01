/***********************************************************************************************************************
 * @file    port/lwip/arch/sys_arch.c
 * @brief   ThreadX-backed implementation of lwIP's sys_arch port API.
 *
 * @details
 * Provides every function lwIP's `lwip/sys.h` requires from the platform
 * port:
 *   - Mutexes / semaphores / mailboxes (TX_MUTEX / TX_SEMAPHORE / TX_QUEUE)
 *   - sys_thread_new() (TX_THREAD allocated from a static pool)
 *   - sys_now() / sys_jiffies() (derived from tx_time_get + TX_TIMER_TICKS_PER_SECOND)
 *   - sys_msleep() (tx_thread_sleep)
 *   - sys_arch_protect() / sys_arch_unprotect() (recursive TX_MUTEX)
 *
 * Diagnostics produced by lwIP's LWIP_PLATFORM_DIAG / LWIP_PLATFORM_ASSERT
 * macros land in `ra_lwip_platform_diag` / `ra_lwip_platform_assert`,
 * which forward to ra_log_*.
 *
 * @par NASA Power of 10 Compliance
 * Rule 3 (no dynamic allocation after init) is intentionally relaxed for
 * the vendored lwIP stack. All runtime state -- mailbox slot arrays,
 * worker-thread TCBs and stacks -- is carved out of compile-time pools
 * declared in this file. Once the pool is exhausted sys_thread_new()
 * asserts (per the lwIP contract: "this function MUST NOT FAIL").
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * @license   SPDX-License-Identifier: MIT
 **********************************************************************************************************************/

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tx_api.h"

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "lwip/debug.h"

#include "ra_log.h"

/* ------------------------------------------------------------------------- */
/* Compile-time pool sizing                                                  */
/* ------------------------------------------------------------------------- */

#ifndef RA_LWIP_MAX_THREADS
#define RA_LWIP_MAX_THREADS (6U)
#endif

#ifndef RA_LWIP_THREAD_STACK_BYTES
#define RA_LWIP_THREAD_STACK_BYTES (12U * 1024U)
#endif

/* Threadx priorities are 0 (highest) .. 31 (lowest). Clamp here. */
typedef enum : uint32_t {
    k_ra_lwip_prio_min = 0U,
    k_ra_lwip_prio_max = 31U,
} ra_lwip_prio_t;

/**
 * @brief Static thread pool used by sys_thread_new().
 *
 * @details
 * lwIP requires sys_thread_new() to never fail. We pre-allocate
 * RA_LWIP_MAX_THREADS TCBs + stacks here so the call only fails (asserts)
 * when the firmware genuinely tries to spin up more lwIP threads than
 * the pool was sized for.
 */
typedef struct {
    TX_THREAD tcb;                              /**< ThreadX thread control block */
    uint8_t   stack[RA_LWIP_THREAD_STACK_BYTES];/**< Static stack */
    uint8_t   in_use;                           /**< 1 if allocated to a thread */
} ra_lwip_thread_slot_t;

static ra_lwip_thread_slot_t s_thread_pool[RA_LWIP_MAX_THREADS];

/**
 * @brief Recursive mutex backing SYS_ARCH_PROTECT / SYS_ARCH_UNPROTECT.
 */
static TX_MUTEX s_protect_mutex;
static uint8_t  s_protect_mutex_valid;

/* lwIP log tag forwarded to ra_log_*. */
static const char* const s_lwip_tag = "lwip";

/* ========================================================================= */
/* Diagnostics shim (referenced from arch/cc.h)                              */
/* ========================================================================= */

void ra_lwip_platform_diag(const char* fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    ra_log_info(s_lwip_tag, buf);
}

/**
 * @brief Pseudo-random uint32 used by LWIP_RAND().
 *
 * @details
 * Mixes ThreadX's tick count with a simple xorshift state. NOT
 * cryptographic; sufficient for DNS transaction IDs, ephemeral port
 * selection, and TCP ISN salt. When `ra_rsip_trng_read` lands this
 * implementation will be replaced.
 */
uint32_t ra_lwip_rand(void) {
    static uint32_t s_state = 0xA5A5C3C3U;
    uint32_t        x       = s_state ^ (uint32_t)tx_time_get();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_state = x;
    return x;
}

void ra_lwip_platform_assert(const char* msg, const char* file, int line) {
    ra_log_error_val(s_lwip_tag, msg, (uint32_t)line);
    ra_log_error(s_lwip_tag, file);
    /* Per lwIP convention, an assertion failure is fatal. */
    for (;;) {
        /* Halt; debugger or watchdog takes over. */
    }
}

/* ========================================================================= */
/* sys_init                                                                  */
/* ========================================================================= */

void sys_init(void) {
    if (!s_protect_mutex_valid) {
        UINT st = tx_mutex_create(&s_protect_mutex, "lwip_prot", TX_INHERIT);
        if (st == TX_SUCCESS) {
            s_protect_mutex_valid = 1U;
        }
    }
    (void)memset(s_thread_pool, 0, sizeof(s_thread_pool));
}

/* ========================================================================= */
/* Time                                                                      */
/* ========================================================================= */

u32_t sys_jiffies(void) {
    return (u32_t)tx_time_get();
}

u32_t sys_now(void) {
    /* Convert ThreadX ticks -> ms. TX_TIMER_TICKS_PER_SECOND is provided by
     * the project's tx_user.h (port/threadx/tx_user.h). */
    const uint32_t ticks = (uint32_t)tx_time_get();
    const uint32_t hz    = (uint32_t)TX_TIMER_TICKS_PER_SECOND;
    return (u32_t)((uint64_t)ticks * 1000U / (hz != 0U ? hz : 1U));
}

void sys_msleep(u32_t ms) {
    if (ms == 0U) {
        return;
    }
    const uint32_t hz    = (uint32_t)TX_TIMER_TICKS_PER_SECOND;
    uint32_t       ticks = (ms * hz + 999U) / 1000U;
    if (ticks == 0U) {
        ticks = 1U;
    }
    (void)tx_thread_sleep((ULONG)ticks);
}

/* Helper: convert a ms timeout to ThreadX ticks (0 means TX_NO_WAIT, 0xff..f
 * is reserved by lwIP to mean wait-forever -- but lwIP itself never passes
 * that to sys_arch_*; 0 means wait-forever in lwIP semantics). */
static ULONG ms_to_ticks(u32_t ms) {
    if (ms == 0U) {
        return TX_WAIT_FOREVER;
    }
    const uint32_t hz    = (uint32_t)TX_TIMER_TICKS_PER_SECOND;
    uint32_t       ticks = (ms * hz + 999U) / 1000U;
    if (ticks == 0U) {
        ticks = 1U;
    }
    return (ULONG)ticks;
}

/* ========================================================================= */
/* SYS_LIGHTWEIGHT_PROT                                                      */
/* ========================================================================= */

sys_prot_t sys_arch_protect(void) {
    if (s_protect_mutex_valid) {
        (void)tx_mutex_get(&s_protect_mutex, TX_WAIT_FOREVER);
    }
    return (sys_prot_t)0;
}

void sys_arch_unprotect(sys_prot_t pval) {
    (void)pval;
    if (s_protect_mutex_valid) {
        (void)tx_mutex_put(&s_protect_mutex);
    }
}

/* ========================================================================= */
/* Semaphores                                                                */
/* ========================================================================= */

err_t sys_sem_new(sys_sem_t* sem, u8_t count) {
    if (sem == NULL) {
        return ERR_ARG;
    }
    UINT st = tx_semaphore_create(&sem->tx, "lwip_sem", (ULONG)count);
    if (st != TX_SUCCESS) {
        sem->valid = 0U;
        return ERR_MEM;
    }
    sem->valid = 1U;
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t* sem) {
    if (sem != NULL && sem->valid != 0U) {
        (void)tx_semaphore_put(&sem->tx);
    }
}

u32_t sys_arch_sem_wait(sys_sem_t* sem, u32_t timeout_ms) {
    if (sem == NULL || sem->valid == 0U) {
        return SYS_ARCH_TIMEOUT;
    }
    const uint32_t start_ms = sys_now();
    UINT st = tx_semaphore_get(&sem->tx, ms_to_ticks(timeout_ms));
    if (st == TX_NO_INSTANCE) {
        return SYS_ARCH_TIMEOUT;
    }
    if (st != TX_SUCCESS) {
        return SYS_ARCH_TIMEOUT;
    }
    const uint32_t end_ms = sys_now();
    return (u32_t)(end_ms - start_ms);
}

void sys_sem_free(sys_sem_t* sem) {
    if (sem != NULL && sem->valid != 0U) {
        (void)tx_semaphore_delete(&sem->tx);
        sem->valid = 0U;
    }
}

int sys_sem_valid(sys_sem_t* sem) {
    return (sem != NULL && sem->valid != 0U) ? 1 : 0;
}

void sys_sem_set_invalid(sys_sem_t* sem) {
    if (sem != NULL) {
        sem->valid = 0U;
    }
}

/* ========================================================================= */
/* Mutexes                                                                   */
/* ========================================================================= */

err_t sys_mutex_new(sys_mutex_t* mutex) {
    if (mutex == NULL) {
        return ERR_ARG;
    }
    UINT st = tx_mutex_create(&mutex->tx, "lwip_mtx", TX_INHERIT);
    if (st != TX_SUCCESS) {
        mutex->valid = 0U;
        return ERR_MEM;
    }
    mutex->valid = 1U;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t* mutex) {
    if (mutex != NULL && mutex->valid != 0U) {
        (void)tx_mutex_get(&mutex->tx, TX_WAIT_FOREVER);
    }
}

void sys_mutex_unlock(sys_mutex_t* mutex) {
    if (mutex != NULL && mutex->valid != 0U) {
        (void)tx_mutex_put(&mutex->tx);
    }
}

void sys_mutex_free(sys_mutex_t* mutex) {
    if (mutex != NULL && mutex->valid != 0U) {
        (void)tx_mutex_delete(&mutex->tx);
        mutex->valid = 0U;
    }
}

int sys_mutex_valid(sys_mutex_t* mutex) {
    return (mutex != NULL && mutex->valid != 0U) ? 1 : 0;
}

void sys_mutex_set_invalid(sys_mutex_t* mutex) {
    if (mutex != NULL) {
        mutex->valid = 0U;
    }
}

/* ========================================================================= */
/* Mailboxes                                                                 */
/* ========================================================================= */

err_t sys_mbox_new(sys_mbox_t* mbox, int size) {
    if (mbox == NULL) {
        return ERR_ARG;
    }
    if (size <= 0 || (uint32_t)size > RA_LWIP_MBOX_SIZE) {
        size = (int)RA_LWIP_MBOX_SIZE;
    }
    UINT st = tx_queue_create(&mbox->tx,
                              "lwip_mbx",
                              TX_1_ULONG,
                              mbox->storage,
                              (ULONG)((uint32_t)size * sizeof(ULONG)));
    if (st != TX_SUCCESS) {
        mbox->valid = 0U;
        return ERR_MEM;
    }
    mbox->valid = 1U;
    return ERR_OK;
}

void sys_mbox_post(sys_mbox_t* mbox, void* msg) {
    if (mbox == NULL || mbox->valid == 0U) {
        return;
    }
    ULONG slot = (ULONG)(uintptr_t)msg;
    (void)tx_queue_send(&mbox->tx, &slot, TX_WAIT_FOREVER);
}

err_t sys_mbox_trypost(sys_mbox_t* mbox, void* msg) {
    if (mbox == NULL || mbox->valid == 0U) {
        return ERR_ARG;
    }
    ULONG slot = (ULONG)(uintptr_t)msg;
    UINT  st   = tx_queue_send(&mbox->tx, &slot, TX_NO_WAIT);
    return (st == TX_SUCCESS) ? ERR_OK : ERR_MEM;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t* mbox, void* msg) {
    /* tx_queue_send is ISR-safe in ThreadX so long as TX_NO_WAIT is used. */
    return sys_mbox_trypost(mbox, msg);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t* mbox, void** msg, u32_t timeout_ms) {
    if (mbox == NULL || mbox->valid == 0U) {
        return SYS_ARCH_TIMEOUT;
    }
    const uint32_t start_ms = sys_now();
    ULONG          slot     = 0U;
    UINT           st       = tx_queue_receive(&mbox->tx, &slot, ms_to_ticks(timeout_ms));
    if (st == TX_QUEUE_EMPTY) {
        if (msg != NULL) {
            *msg = NULL;
        }
        return SYS_ARCH_TIMEOUT;
    }
    if (st != TX_SUCCESS) {
        if (msg != NULL) {
            *msg = NULL;
        }
        return SYS_ARCH_TIMEOUT;
    }
    if (msg != NULL) {
        *msg = (void*)(uintptr_t)slot;
    }
    return (u32_t)(sys_now() - start_ms);
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t* mbox, void** msg) {
    if (mbox == NULL || mbox->valid == 0U) {
        return SYS_MBOX_EMPTY;
    }
    ULONG slot = 0U;
    UINT  st   = tx_queue_receive(&mbox->tx, &slot, TX_NO_WAIT);
    if (st != TX_SUCCESS) {
        if (msg != NULL) {
            *msg = NULL;
        }
        return SYS_MBOX_EMPTY;
    }
    if (msg != NULL) {
        *msg = (void*)(uintptr_t)slot;
    }
    return 0U;
}

void sys_mbox_free(sys_mbox_t* mbox) {
    if (mbox != NULL && mbox->valid != 0U) {
        (void)tx_queue_delete(&mbox->tx);
        mbox->valid = 0U;
    }
}

int sys_mbox_valid(sys_mbox_t* mbox) {
    return (mbox != NULL && mbox->valid != 0U) ? 1 : 0;
}

void sys_mbox_set_invalid(sys_mbox_t* mbox) {
    if (mbox != NULL) {
        mbox->valid = 0U;
    }
}

/* ========================================================================= */
/* Threads                                                                   */
/* ========================================================================= */

/**
 * @brief Allocate a thread slot from the static pool.
 *
 * @return Pointer to a free slot or NULL when the pool is exhausted.
 */
static ra_lwip_thread_slot_t* alloc_thread_slot(void) {
    for (uint32_t i = 0U; i < RA_LWIP_MAX_THREADS; ++i) {
        if (s_thread_pool[i].in_use == 0U) {
            s_thread_pool[i].in_use = 1U;
            return &s_thread_pool[i];
        }
    }
    return NULL;
}

sys_thread_t sys_thread_new(const char*     name,
                            lwip_thread_fn  thread,
                            void*           arg,
                            int             stacksize,
                            int             prio) {
    (void)stacksize; /* stack is fixed-size out of the static pool */

    ra_lwip_thread_slot_t* slot = alloc_thread_slot();
    LWIP_ASSERT("sys_thread_new: thread pool exhausted", slot != NULL);
    if (slot == NULL) {
        return NULL;
    }

    UINT tx_prio = (UINT)prio;
    if (tx_prio > k_ra_lwip_prio_max) {
        tx_prio = k_ra_lwip_prio_max;
    }

    UINT st = tx_thread_create(&slot->tcb,
                               (CHAR*)(name != NULL ? name : "lwip_thr"),
                               (VOID (*)(ULONG))thread,
                               (ULONG)(uintptr_t)arg,
                               slot->stack,
                               (ULONG)sizeof(slot->stack),
                               tx_prio,
                               tx_prio, /* preempt threshold == prio */
                               TX_NO_TIME_SLICE,
                               TX_AUTO_START);
    LWIP_ASSERT("sys_thread_new: tx_thread_create failed", st == TX_SUCCESS);
    if (st != TX_SUCCESS) {
        slot->in_use = 0U;
        return NULL;
    }
    return &slot->tcb;
}
