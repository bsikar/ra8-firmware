/* Host (macOS) GUIX configuration.
 *
 * Runs GUIX single-threaded with NO ThreadX: GX_DISABLE_THREADX_BINDING
 * switches GUIX onto its generic RTOS surface (the gx_generic_* functions
 * we implement in gx_generic_rtos_host.c). Error checking is disabled so
 * the gx_* macros resolve straight to the non-checked _gx_* entry points
 * and never reference ThreadX caller-check state.
 *
 * GX_DISABLE_THREADX_BINDING is tested in gx_api.h before this file is
 * included, so the host build also sets it on the command line; the guard
 * below keeps that from being a macro redefinition.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#ifndef GX_USER_H
#define GX_USER_H

#ifndef GX_DISABLE_THREADX_BINDING
#define GX_DISABLE_THREADX_BINDING
#endif
#define GX_DISABLE_ERROR_CHECKING
#define GX_SYSTEM_TIMER_MS 20

#endif /* GX_USER_H */
