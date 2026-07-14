/**
 * @file ra8_ethosu_shim.h
 * @brief Arm ethos-u-core-driver C API shim over the `ra8_npu` driver (RA8P1-only)
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * This is the ADAPTER that lets the vendored TensorFlow Lite for
 * Microcontrollers (TFLite-micro) Ethos-U operator kernel drive the RA8P1's
 * Arm Ethos-U55 NPU through this project's first-party `ra8_npu` HAL, instead of
 * the upstream Arm `ethos-u-core-driver` library (which is NOT vendored in this
 * tree). It re-implements the MINIMAL slice of the Arm driver C API that the
 * kernel calls -- three functions -- on top of `ra8_npu_init()` /
 * `ra8_npu_submit()` / `ra8_npu_run()` / `ra8_npu_wait()`:
 *
 *  - ::ethosu_reserve_driver  -- lazily bring the single NPU up and hand back a
 *                                persistent driver handle.
 *  - ::ethosu_invoke_v3       -- translate a (command stream + base-pointer
 *                                array) call into an ::ra8_npu_job_t and run it.
 *  - ::ethosu_release_driver  -- a no-op: the one NPU stays initialised.
 *
 * The signatures here are transcribed byte-for-byte from the Arm
 * `ethosu_driver.h` public prototypes (SPDX Apache-2.0, reference-only) so this
 * shim is a drop-in symbol provider: if the real TFLite-micro Ethos-U kernel
 * (which `#include`s the Arm header and calls these symbols) is ever linked, it
 * resolves against this shim with no source change. The names deliberately do
 * NOT carry this project's `ra8_` / `k_` prefixes -- they are an external ABI
 * contract owned by Arm, and renaming them would defeat the drop-in purpose.
 *
 * ## Status / gating
 *
 * RA8P1-only: the whole surface is guarded behind `RA8_HAS_NPU` (defined only
 * for `-DRA8_DEVICE_RA8P1`, see `ra8_device.h`), exactly like `ra8_npu.h`. There
 * is no RA8P1 board yet, so the adapter is host-tested for the exact NPU
 * register-programming contract only (see `tests/test_ra8_ethosu_shim.c`); a real
 * Vela-compiled inference on silicon is a follow-up on the RA8P1 NPU epic. This
 * shim intentionally does NOT enable the dormant TFLite-micro C++ stack
 * (`RA8_USE_TFLITE_MICRO`, OFF by default): it only PROVIDES the symbols that
 * stack would need.
 *
 * ## Threading
 *
 * Not thread-safe. The NPU is a single-job engine and the shim keeps one static
 * driver context; the caller owns serialisation, the same contract as `ra8_npu`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_device.h"

/* RA8P1-only. On a device without an NPU this header declares nothing (its
 * matching ra8_ethosu_shim.c is an empty translation unit), so it stays safe to
 * include anywhere: using an ethosu_* symbol on the RA8D2 is then a link error,
 * not a compile-time #error that would trip whole-tree tooling (clang-tidy). */
#ifdef RA8_HAS_NPU

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct ethosu_driver
 * @brief Opaque Arm ethos-u-core-driver handle (concrete definition private).
 *
 * @details
 * The Arm ethos-u-core-driver models each NPU instance as a
 * `struct ethosu_driver`. TFLite-micro's Ethos-U kernel treats it purely as an
 * OPAQUE token: it reserves one, threads it through ::ethosu_invoke_v3, then
 * releases it, and never reads a field. This shim therefore only forward-
 * declares the tag here and keeps its minimal concrete definition private to
 * `ra8_ethosu_shim.c`. Keeping the type incomplete in this header means the
 * header stays compatible with the real Arm `ethosu_driver.h` -- an incomplete
 * type is compatible with that header's complete definition, so nothing
 * conflicts if both are ever visible.
 *
 * @invariant Only ever used through a pointer; never dereferenced by callers.
 * @see ethosu_reserve_driver
 * @since 0.1.0
 */
struct ethosu_driver;

/**
 * @brief Reserve the single Ethos-U55 NPU, bringing it up on first use.
 *
 * @details
 * The Arm contract blocks until an NPU driver is available and returns a handle
 * to it. On the RA8P1 there is exactly one Ethos-U55, so this shim keeps a
 * single static driver context: the first call lazily runs `ra8_npu_init()` (out
 * of module-stop + soft reset) and, on success, every call returns the same
 * non-NULL handle. A NULL return signals that the NPU could not be initialised
 * -- callers (and the Arm `ethosu_invoke_v2` compatibility inline) test the
 * handle for NULL before invoking.
 *
 * @return Pointer to the persistent NPU driver handle, or `nullptr` on failure.
 * @retval non-NULL The NPU is initialised and reserved for use.
 * @retval nullptr  `ra8_npu_init()` failed; no usable NPU handle.
 *
 * @pre The build targets the RA8P1 (`RA8_HAS_NPU` defined).
 * @pre NPUCLK is running (CGC clock tree already configured).
 * @post On a non-NULL return the NPU is out of module-stop and idle.
 * @post The returned handle stays valid until the program ends (never freed).
 *
 * @note Not thread-safe. Call from a single-threaded context or with the NPU
 *       IRQ masked, matching the `ra8_npu` ownership contract.
 * @see ethosu_invoke_v3
 * @see ethosu_release_driver
 * @since 0.1.0
 */
struct ethosu_driver* ethosu_reserve_driver(void);

/**
 * @brief Run one Ethos-U55 command stream through the `ra8_npu` submission path.
 *
 * @details
 * Translates the Arm invoke arguments into an ::ra8_npu_job_t and drives the
 * blocking submit -> run -> wait sequence: `QBASE`/`QSIZE` are pointed at
 * @p custom_data_ptr / @p custom_data_size and each of the first
 * @p num_base_addr entries of @p base_addr is programmed into `BASEPn`. The
 * per-region byte sizes @p base_addr_size are accepted for ABI compatibility
 * but unused -- the Ethos-U55 derives tensor extents from the command stream,
 * so `ra8_npu` programs only the region bases. @p user_arg (the Arm inference
 * begin/end callback cookie) is likewise unused: this shim has no callbacks.
 *
 * @p num_base_addr is bounded to `ra8_npu`'s region capacity
 * (`k_ra8_npu_region_count`); an out-of-range count is rejected before any
 * register is touched, so the fixed ::ra8_npu_job_t region array can never
 * overflow.
 *
 * @param[in] drv              NPU handle from ::ethosu_reserve_driver (non-NULL).
 * @param[in] custom_data_ptr  Vela command-stream base in NPU-visible SRAM.
 * @param[in] custom_data_size Command-stream length in bytes (> 0).
 * @param[in] base_addr        Array of @p num_base_addr 64-bit AXI region bases.
 * @param[in] base_addr_size   Per-region byte sizes; accepted but unused.
 * @param[in] num_base_addr    Region count, 0 .. `k_ra8_npu_region_count`.
 * @param[in] user_arg         Arm callback cookie; accepted but unused.
 *
 * @return 0 on success, non-zero (negative) on any error.
 * @retval 0  The command stream ran to completion with no NPU fault.
 * @retval -1 A bad argument, region-count overflow, or an `ra8_npu` submit /
 *            run / wait error (bus/parse/weight/ECC fault or timeout).
 *
 * @pre ::ethosu_reserve_driver returned @p drv successfully.
 * @pre @p custom_data_ptr describes a command stream resident in NPU-visible SRAM.
 * @post On a 0 return the output tensors in @p base_addr hold the result.
 * @post On a non-zero return no partial result is reported as success.
 *
 * @note Not thread-safe. Blocks until the job completes, faults, or times out.
 * @see ethosu_reserve_driver
 * @since 0.1.0
 */
int ethosu_invoke_v3(struct ethosu_driver* drv,
                     const void*           custom_data_ptr,
                     int                   custom_data_size,
                     const uint64_t*       base_addr,
                     const size_t*         base_addr_size,
                     int                   num_base_addr,
                     void*                 user_arg);

/**
 * @brief Release a driver handle previously reserved with ::ethosu_reserve_driver.
 *
 * @details
 * On multi-NPU Arm systems this returns the driver to the free pool. The RA8P1
 * has a single, persistent NPU that this shim keeps initialised for the life of
 * the program, so release is a no-op: the handle stays valid and a subsequent
 * ::ethosu_reserve_driver hands back the same context without re-initialising.
 * @p drv is validated (non-NULL) so a misuse is caught rather than ignored.
 *
 * @param[in] drv NPU handle from ::ethosu_reserve_driver (non-NULL).
 *
 * @return None (void).
 * @retval None This function returns no value.
 *
 * @pre @p drv was obtained from ::ethosu_reserve_driver.
 * @pre The NPU is not mid-job (no inference is running).
 * @post The NPU stays initialised and reservable again.
 * @post No NPU register state is modified.
 *
 * @note Not thread-safe.
 * @see ethosu_reserve_driver
 * @since 0.1.0
 */
void ethosu_release_driver(struct ethosu_driver* drv);

#ifdef __cplusplus
}
#endif

#endif /* RA8_HAS_NPU */
