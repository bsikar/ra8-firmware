/* Host (macOS) GUIX port header.
 *
 * Adapted from libs/third_party/guix/ports/linux/gnu/inc/gx_port.h (Microsoft
 * / Eclipse ThreadX, MIT), but the ThreadX caller-checking macros are emptied
 * because the host build uses the generic RTOS binding
 * (GX_DISABLE_THREADX_BINDING) and never links ThreadX.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef GX_PORT_H
#define GX_PORT_H

#ifdef GX_INCLUDE_USER_DEFINE_FILE
#include "gx_user.h"
#endif

typedef INT   GX_BOOL;
typedef SHORT GX_VALUE;

#define GX_VALUE_MAX 0x7FFF

#ifndef GX_THREAD_STACK_SIZE
#define GX_THREAD_STACK_SIZE 4096
#endif

#define GX_CONST const

#define GX_INCLUDE_DEFAULT_COLORS

#define GX_MAX_ACTIVE_TIMERS  32
#define GX_MAX_VIEWS          32
#define GX_MAX_DISPLAY_HEIGHT 1024
#define GX_MAX_DISPLAY_WIDTH  1280

/* Generic single-threaded host bind: no ThreadX, so caller-checking is a no-op. */
#define GX_CALLER_CHECKING_EXTERNS
#define GX_THREADS_ONLY_CALLER_CHECKING
#define GX_INIT_AND_THREADS_CALLER_CHECKING
#define GX_NOT_ISR_CALLER_CHECKING
#define GX_THREAD_WAIT_CALLER_CHECKING

#ifdef GX_SYSTEM_INIT
CHAR _gx_version_id[] = "GUIX host (macOS) generic single-threaded bind";
#else
extern CHAR _gx_version_id[];
#endif

#endif /* GX_PORT_H */
