/**
 * @file txm_trampolines.c
 * @brief Auto-generated description.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 *
 * Wrapper that compiles all module manager notify trampolines with
 * TX_DISABLE_NOTIFY_CALLBACKS forcibly undefined, regardless of
 * what tx_user.h says.
 *
 * tx_user.h defines TX_DISABLE_NOTIFY_CALLBACKS to save code space
 * in non-module builds. The module manager's dispatch table references
 * these trampolines unconditionally, so they must be compiled in.
 * This file includes the upstream .c files AFTER #undef-ing the guard.
 *
 * The include path for this TU must contain the upstream source dir:
 *   libs/third_party/threadx/common_modules/module_manager/src/
 * (added in cmake/threadx_modules.cmake).
 */

/* Pull in all the ThreadX headers the trampolines expect. */
#define TX_SOURCE_CODE
#include "tx_api.h"
#include "tx_event_flags.h"
#include "tx_queue.h"
#include "tx_semaphore.h"
#include "tx_thread.h"
#include "tx_timer.h"
#include "txm_module.h"

/* NOW undo the tx_user.h suppression. */
#ifdef TX_DISABLE_NOTIFY_CALLBACKS
#undef TX_DISABLE_NOTIFY_CALLBACKS
#endif

/* Include the trampoline .c files - these sit in the search path added by
 * cmake/threadx_modules.cmake (PRIVATE include of the src/ directory). */
#include "txm_module_manager_event_flags_notify_trampoline.c"
#include "txm_module_manager_queue_notify_trampoline.c"
#include "txm_module_manager_semaphore_notify_trampoline.c"
#include "txm_module_manager_thread_notify_trampoline.c"
#include "txm_module_manager_timer_notify_trampoline.c"
