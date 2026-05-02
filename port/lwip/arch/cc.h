/***********************************************************************************************************************
 * @file    port/lwip/arch/cc.h
 * @brief   Compiler / architecture glue for the vendored lwIP on RA8D2.
 *
 * @details
 * lwIP includes `arch/cc.h` from `lwip/arch.h`. This file is responsible
 * for telling the stack:
 *   - the byte order of the target,
 *   - how to emit a debug / assertion message
 *     (LWIP_PLATFORM_DIAG / LWIP_PLATFORM_ASSERT),
 *   - any compiler-specific structure-packing hooks.
 *
 * Diagnostics are routed through a small extern shim
 * (`ra_lwip_platform_diag` / `ra_lwip_platform_assert`) implemented in
 * `arch/sys_arch.c` so that this header has no firmware include
 * dependencies and the lwIP static library can be built standalone.
 * The shim, in turn, calls `ra_log_*` from `libs/ra_core`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * @license   SPDX-License-Identifier: MIT
 **********************************************************************************************************************/

#pragma once

#include <stdint.h>
#include <stddef.h>

/* RA8D2 (Cortex-M85) is little-endian. */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* ---------------------------------------------------------------------- */
/* GCC / Clang structure packing                                          */
/* ---------------------------------------------------------------------- */
#define PACK_STRUCT_FIELD(x)    x
#define PACK_STRUCT_STRUCT      __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

/* ---------------------------------------------------------------------- */
/* Diagnostics                                                            */
/*                                                                        */
/* lwIP's debug + assert macros expand to a printf-style call. We route   */
/* both through these tiny extern shims so that the formatting backend    */
/* lives in sys_arch.c (where it can talk to ra_log_* without forcing a   */
/* link-time dependency on this header).                                  */
/* ---------------------------------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif

void ra_lwip_platform_diag(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void ra_lwip_platform_assert(const char* msg, const char* file, int line) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#define LWIP_PLATFORM_DIAG(x)   do { ra_lwip_platform_diag x; } while (0)

#define LWIP_PLATFORM_ASSERT(x) ra_lwip_platform_assert((x), __FILE__, __LINE__)

/* ---------------------------------------------------------------------- */
/* Random number generator                                                */
/*                                                                        */
/* lwIP's `arch.h` only sketches LWIP_RAND() inside a `#ifdef __DOXYGEN__` */
/* block, so the platform port MUST supply a real definition or          */
/* `dns.c` (DNS transaction-id randomization), `udp.c` (ephemeral port    */
/* allocation), and a couple of TCP paths fail to compile under          */
/* -Werror=implicit-function-declaration. The shim function is           */
/* implemented in `arch/sys_arch.c`; until a real entropy source         */
/* (RSIP TRNG) is wired in, it returns a per-call mix of                 */
/* tx_time_get() and an xorshift counter -- adequate for protocol IDs   */
/* but NOT cryptographic.                                                */
/* ---------------------------------------------------------------------- */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Ra lwip rand.
 *
 * @details See implementation for details.
 *
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 *
 * @pre Caller has validated arguments.
 * @pre Module has been initialised.
 * @post Side effects bounded to documented state.
 * @post Returned value reflects current state.
 *
 * @note Not thread-safe unless documented otherwise.
 *
 * @since 0.1.0
 */
uint32_t ra_lwip_rand(void);

#ifdef __cplusplus
}
#endif

#define LWIP_RAND() ra_lwip_rand()

/* No special LWIP_NO_* typedefs required: <stdint.h> is enough. */
