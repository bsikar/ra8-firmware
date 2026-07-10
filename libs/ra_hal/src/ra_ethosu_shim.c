/**
 * @file ra_ethosu_shim.c
 * @brief Arm ethos-u-core-driver C API shim over the `ra_npu` driver (RA8P1-only)
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Implementation of the `ra_ethosu_shim.h` contract. The whole translation unit
 * is gated behind `RA_HAS_NPU` so it compiles to nothing on the RA8D2 (which has
 * no NPU) and is only built for the RA8P1 (`-DRA_DEVICE_RA8P1`), mirroring
 * `ra_npu.c`. It touches NO hardware registers directly -- every NPU access
 * flows through the `ra_npu` HAL, so this file carries no HUM / TRM citations.
 *
 * The three functions provide exactly the Arm `ethos-u-core-driver` symbols
 * TFLite-micro's Ethos-U operator kernel links against; see the header for how
 * they map onto `ra_npu_init` / `ra_npu_submit` / `ra_npu_run` / `ra_npu_wait`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra_device.h"

#ifdef RA_HAS_NPU

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_ethosu_shim.h"
#include "ra_log.h"
#include "ra_npu.h"
#include "ra_npu_regs.h"

/**
 * @var s_tag
 * @brief Log component tag for this adapter.
 * @details Passed to the `ra_log_*` macros as the source tag.
 * @note Read-only literal; never modified.
 * @since 0.1.0
 */
static const char* s_tag = "ETHOSU";

/**
 * @enum ra_ethosu_ret_t
 * @brief Integer return codes of the Arm `ethosu_invoke_v3` contract.
 *
 * @details The Arm driver returns `int`: 0 on success, else a negative error
 *          code. TFLite-micro's kernel treats any non-zero value as failure, so
 *          this shim collapses every failure onto a single ::k_ra_ethosu_ret_err
 *          value rather than leaking the internal `ra_err_t` taxonomy across the
 *          ABI boundary. Named so the codes are not magic numbers.
 *
 * @invariant ::k_ra_ethosu_ret_ok is 0; every failure code is non-zero.
 * @since 0.1.0
 */
typedef enum : int32_t {
  k_ra_ethosu_ret_ok  = 0,  /**< Invoke succeeded (command stream ran to end). */
  k_ra_ethosu_ret_err = -1, /**< Invoke failed (bad argument or `ra_npu` error). */
} ra_ethosu_ret_t;

/**
 * @struct ethosu_driver
 * @brief Minimal concrete definition of the opaque Arm NPU driver handle.
 *
 * @details The Arm `ethos-u-core-driver` `struct ethosu_driver` is a large
 *          device-context aggregate. TFLite-micro's kernel never reads a field,
 *          so this shim keeps a single static instance with just the `reserved`
 *          flag the real struct also carries -- enough to hand back a stable,
 *          non-NULL, self-describing handle.
 *
 * @invariant Exactly one instance (::s_ethosu_driver) exists; the NPU is singular.
 * @see ethosu_reserve_driver
 * @since 0.1.0
 */
struct ethosu_driver {
  bool reserved; /**< True while a caller holds this handle (Arm-compatible field). */
};

/**
 * @var s_ethosu_driver
 * @brief The single persistent NPU driver handle handed out by the shim.
 * @details The RA8P1 has one Ethos-U55; ::ethosu_reserve_driver returns
 *          `&s_ethosu_driver` on every successful call.
 * @warning Do not modify directly; owned by the reserve / release logic.
 * @since 0.1.0
 */
static struct ethosu_driver s_ethosu_driver = {};

/**
 * @var s_ethosu_driver_ready
 * @brief True once the shim has lazily run `ra_npu_init()` for the NPU.
 * @details Gates the one-time `ra_npu_init()` in ::ethosu_reserve_driver so the
 *          NPU is brought up exactly once for the life of the program.
 * @warning Do not modify directly; owned by ::ethosu_reserve_driver.
 * @since 0.1.0
 */
static bool s_ethosu_driver_ready = false;

struct ethosu_driver* ethosu_reserve_driver(void)
{
  if (!s_ethosu_driver_ready) {
    const ra_err_t err = ra_npu_init();
    if (ra_err_is_error(err)) {
      ra_log_error(s_tag, "reserve: npu_init failed");
      return nullptr;
    }
    s_ethosu_driver_ready = true;
  }
  s_ethosu_driver.reserved = true;
  return &s_ethosu_driver;
}

int ethosu_invoke_v3(struct ethosu_driver* drv,
                     const void*           custom_data_ptr,
                     const int             custom_data_size,
                     uint64_t* const       base_addr,
                     const size_t*         base_addr_size,
                     const int             num_base_addr,
                     void*                 user_arg)
{
  /* base_addr_size (per-region byte sizes) and user_arg (Arm callback cookie)
   * are part of the ABI but have no analogue in the ra_npu job model. */
  (void)base_addr_size;
  (void)user_arg;

  if (drv == nullptr) {
    ra_log_error(s_tag, "invoke: null driver");
    return (int)k_ra_ethosu_ret_err;
  }
  if (custom_data_ptr == nullptr) {
    ra_log_error(s_tag, "invoke: null command stream");
    return (int)k_ra_ethosu_ret_err;
  }
  if (custom_data_size <= 0) {
    ra_log_error(s_tag, "invoke: empty command stream");
    return (int)k_ra_ethosu_ret_err;
  }
  if (num_base_addr < 0) {
    ra_log_error(s_tag, "invoke: negative region count");
    return (int)k_ra_ethosu_ret_err;
  }
  /* Bound the region count to ra_npu's fixed region_base[] capacity BEFORE the
   * copy loop, so an oversized count can never overflow the job descriptor. */
  if ((uint32_t)num_base_addr > (uint32_t)k_ra_npu_region_count) {
    ra_log_error(s_tag, "invoke: too many regions");
    return (int)k_ra_ethosu_ret_err;
  }
  if (num_base_addr > 0) {
    if (base_addr == nullptr) {
      ra_log_error(s_tag, "invoke: null base_addr array");
      return (int)k_ra_ethosu_ret_err;
    }
  }

  ra_npu_job_t job     = {};
  job.cmd_stream       = custom_data_ptr;
  job.cmd_stream_bytes = (uint32_t)custom_data_size;
  job.region_count     = (uint8_t)num_base_addr;
  for (uint8_t i = 0U; i < job.region_count; i++) {
    job.region_base[i] = base_addr[i];
  }

  const ra_err_t serr = ra_npu_submit(&job);
  if (ra_err_is_error(serr)) {
    ra_log_error(s_tag, "invoke: submit");
    return (int)k_ra_ethosu_ret_err;
  }
  const ra_err_t rerr = ra_npu_run();
  if (ra_err_is_error(rerr)) {
    ra_log_error(s_tag, "invoke: run");
    return (int)k_ra_ethosu_ret_err;
  }
  const ra_err_t werr = ra_npu_wait();
  if (ra_err_is_error(werr)) {
    ra_log_error(s_tag, "invoke: wait");
    return (int)k_ra_ethosu_ret_err;
  }
  return (int)k_ra_ethosu_ret_ok;
}

void ethosu_release_driver(struct ethosu_driver* drv)
{
  if (drv == nullptr) {
    ra_log_error(s_tag, "release: null driver");
    return;
  }
  /* Single persistent NPU: the block stays initialised so a later
   * ethosu_reserve_driver() re-hands the same context. Only clear the
   * caller-visible reserved flag. */
  drv->reserved = false;
}

#else
/* RA8D2 (no NPU): this translation unit is intentionally empty. */
typedef int ra_ethosu_shim_not_on_this_device_t;
#endif /* RA_HAS_NPU */
