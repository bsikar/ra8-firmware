/**
 * @file ra8_reconcile.h
 * @brief Desired-state boot-time companion firmware version reconciler
 *
 * @par Tag
 * [Ring 4 / Service] {World: Any}
 *
 * @details
 * Implements desired-state convergence across independent processor power
 * domains (`"RA8U"` multi-image bundles). Instead of attempting a distributed
 * two-phase commit across the RA8 and companions (ESP32-C6, display controller),
 * every boot inspects the committed `"RA8U"` bundle descriptors, queries each
 * companion's active version, and idempotently applies any mismatching target.
 *
 * Power loss at any point leaves non-volatile retry state (`RA8R`) consistent;
 * the next boot re-queries every companion, skips converged targets without
 * re-flashing, enforces bounded exponential backoff, and latches an explicit
 * safe-halted state once retries exhaust for a bundle version.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_bundle.h"
#include "ra8_err.h"

/**
 * @enum ra8_reconcile_const_t
 * @brief Wire magic, revision, and retry bounds for `ra8_reconcile`.
 */
typedef enum : uint32_t {
  k_ra8_reconcile_nv_magic          = 0x52413852U, /**< ASCII "RA8R" NV state magic.        */
  k_ra8_reconcile_nv_version        = 1U,          /**< Persisted NV schema version.        */
  k_ra8_reconcile_default_retries   = 3U,          /**< Default per-target retry cap.       */
  k_ra8_reconcile_max_retries_limit = 16U,         /**< Hard upper cap on max_retries.      */
  k_ra8_reconcile_max_backoff_shift = 10U,         /**< Maximum exponential shift exponent. */
} ra8_reconcile_const_t;

/**
 * @enum ra8_reconcile_state_t
 * @brief Overall desired-state reconciliation outcome stored in NV and reports.
 */
typedef enum : uint8_t {
  k_ra8_reconcile_state_idle          = 0U, /**< Initialized, no reconciliation executed.    */
  k_ra8_reconcile_state_converged     = 1U, /**< All companions match desired state.         */
  k_ra8_reconcile_state_pending_retry = 2U, /**< At least one companion pending/retrying.    */
  k_ra8_reconcile_state_safe_halted   = 3U, /**< Retries exhausted; further applies blocked. */
} ra8_reconcile_state_t;

/**
 * @enum ra8_reconcile_target_status_t
 * @brief Per-companion persisted reconciliation progress status.
 */
typedef enum : uint8_t {
  k_ra8_reconcile_target_idle        = 0U, /**< No apply attempted yet for bundle.       */
  k_ra8_reconcile_target_converged   = 1U, /**< Companion matched desired version/proto. */
  k_ra8_reconcile_target_retrying    = 2U, /**< Mismatch observed; retry budget active.  */
  k_ra8_reconcile_target_safe_halted = 3U, /**< Retry budget exhausted; latched safe.    */
} ra8_reconcile_target_status_t;

/**
 * @struct ra8_reconcile_policy_t
 * @brief Bounded retry and exponential backoff policy configuration.
 */
typedef struct {
  uint32_t base_backoff_ms;     /**< Delay in ms before second attempt (retry 1). */
  uint32_t max_backoff_ms;      /**< Upper clamp on computed backoff delay in ms. */
  uint8_t  max_retries;         /**< Max apply attempts per target (`1..16`).     */
  uint8_t  backoff_shift;       /**< Max exponential shift exponent (`0..10`).    */
  bool     reprobe_after_apply; /**< Re-query companion immediately after apply.  */
  uint8_t  reserved0;           /**< Reserved, must be zero.                      */
} ra8_reconcile_policy_t;

static_assert(sizeof(ra8_reconcile_policy_t) == 12U, "ra8_reconcile_policy_t must remain 12 bytes");
static_assert(alignof(ra8_reconcile_policy_t) == 4U, "ra8_reconcile_policy_t align must be 4");

/**
 * @struct ra8_reconcile_target_nv_t
 * @brief 16-byte persisted reconciliation state for one companion target.
 */
typedef struct {
  uint32_t target;                /**< Target ID (`ra8_bundle_target_t` or `0`).    */
  uint32_t desired_version;       /**< Desired `image_version` from bundle desc.    */
  uint32_t last_reported_version; /**< Last observed companion version (`0` unset). */
  uint8_t  retries_used;          /**< Number of apply attempts persisted so far.   */
  uint8_t  status;                /**< Status (`ra8_reconcile_target_status_t`).    */
  uint16_t reserved0;             /**< Reserved, must be zero.                      */
} ra8_reconcile_target_nv_t;

static_assert(sizeof(ra8_reconcile_target_nv_t) == 16U,
              "ra8_reconcile_target_nv_t must remain 16 bytes");

/**
 * @struct ra8_reconcile_nv_state_t
 * @brief 88-byte CRC-32-protected persisted reconciler state (`RA8R`).
 */
typedef struct {
  uint32_t magic;          /**< Must equal ::k_ra8_reconcile_nv_magic.   */
  uint16_t format_version; /**< Must equal ::k_ra8_reconcile_nv_version. */
  uint16_t flags;          /**< Reserved, must be zero.                  */
  uint32_t bundle_version; /**< Bundle version owning retry budgets.     */
  uint32_t seq;            /**< Monotonic persisted write sequence.      */
  uint8_t  overall_state;  /**< State (`ra8_reconcile_state_t`).         */
  uint8_t  target_count;   /**< Active companion entries (`0..4`).       */
  uint8_t  reserved[2];    /**< Reserved, must be zero.                  */
  /** @brief Per-companion persisted retry and status records. */
  ra8_reconcile_target_nv_t targets[k_ra8_bundle_max_images];
  /** @brief IEEE 802.3 CRC-32 over bytes `[0..84)`. */
  uint32_t crc32;
} ra8_reconcile_nv_state_t;

static_assert(sizeof(ra8_reconcile_nv_state_t) == 88U,
              "ra8_reconcile_nv_state_t must remain 88 bytes");
static_assert(alignof(ra8_reconcile_nv_state_t) == 4U, "ra8_reconcile_nv_state_t align must be 4");
static_assert(offsetof(ra8_reconcile_nv_state_t, targets) == 20U, "nv targets offset must be 20");
static_assert(offsetof(ra8_reconcile_nv_state_t, crc32) == 84U, "nv crc32 offset must be 84");

/**
 * @struct ra8_reconcile_report_t
 * @brief Diagnostic summary emitted by ::ra8_reconcile_boot.
 */
typedef struct {
  uint32_t  bundle_version;       /**< Active bundle version reconciled.           */
  uint32_t  last_backoff_ms;      /**< Backoff delay applied on latest retry.      */
  uint32_t  applies_attempted;    /**< Companion applies started during call.      */
  uint32_t  applies_succeeded;    /**< Companion applies returning `k_ra8_ok`.     */
  uint8_t   overall_state;        /**< Resulting ::ra8_reconcile_state_t.          */
  uint8_t   companions_total;     /**< Companion count in committed bundle.        */
  uint8_t   companions_converged; /**< Companions matching desired version.        */
  uint8_t   companions_pending;   /**< Companions still awaiting convergence.      */
  uint8_t   halted_target_count;  /**< Companions currently in safe-halted state.  */
  bool      nv_reset_applied;     /**< True if virgin/corrupt/old-bundle NV reset. */
  uint8_t   reserved0[2];         /**< Reserved padding bytes.                     */
  ra8_err_t last_err;             /**< Most recent companion/apply error observed. */
  uint16_t  reserved1;            /**< Reserved padding bytes.                     */
} ra8_reconcile_report_t;

static_assert(sizeof(ra8_reconcile_report_t) == 28U, "ra8_reconcile_report_t must remain 28 bytes");
static_assert(alignof(ra8_reconcile_report_t) == 4U, "ra8_reconcile_report_t align must be 4");

/**
 * @struct ra8_reconcile_ops_t
 * @brief Injected NV persistence, companion probe/apply, backoff, and safe-state hooks.
 */
typedef struct {
  /** @brief Load persisted NV state (`k_ra8_err_not_found` if blank, `k_ra8_err_crc_mismatch` if torn). */
  ra8_err_t (*nv_load)(void* ctx, ra8_reconcile_nv_state_t* out_state);
  /** @brief Persist updated CRC-32-stamped NV state atomically before hazardous apply/halt. */
  ra8_err_t (*nv_save)(void* ctx, const ra8_reconcile_nv_state_t* state);
  /** @brief Query companion active @p out_version and link @p out_proto_ver (nullable). */
  ra8_err_t (*query_companion)(void*               ctx,
                               ra8_bundle_target_t target,
                               uint32_t*           out_version,
                               uint32_t*           out_proto_ver);
  /** @brief Idempotently stream/apply image described by @p desc to companion target. */
  ra8_err_t (*apply_companion)(void* ctx, const ra8_bundle_image_desc_t* desc);
  /** @brief Optional bounded delay hook invoked prior to retry attempts (`attempt >= 2`). */
  ra8_err_t (*backoff_delay)(void*               ctx,
                             ra8_bundle_target_t target,
                             uint8_t             attempt,
                             uint32_t            delay_ms);
  /** @brief Optional callback notified when a target exhausts retries and enters safe state. */
  void (*on_safe_state)(void*                           ctx,
                        ra8_bundle_target_t             failed_target,
                        const ra8_reconcile_nv_state_t* state);
  /** @brief Caller-owned context passed verbatim to every hook. */
  void* ctx;
} ra8_reconcile_ops_t;

static_assert(sizeof(ra8_reconcile_ops_t) == sizeof(void*) * 7U,
              "ra8_reconcile_ops_t must stay 7 pointer slots");

/**
 * @brief Initialize a CRC-32-stamped NV state for a committed `"RA8U"` bundle.
 *
 * @details Calling context: `task-safe-reentrant`. Extracts non-RA8 companion
 * targets (`target != k_ra8_bundle_target_ra8_app`), zeroes unused slots, and
 * writes the IEEE CRC-32 checksum. Leaves @p out_state untouched on error.
 *
 * @param[in]  bundle_version Committed `"RA8U"` security/bundle version.
 * @param[in]  descs          Validated descriptor array from `"RA8U"`.
 * @param[in]  image_count    Number of entries in @p descs (`1..4`).
 * @param[out] out_state      Initialized NV state destination (non-NULL).
 *
 * @return ::ra8_err_t outcome (`k_ra8_ok`, `k_ra8_err_null_ptr`, or `k_ra8_err_invalid_arg`).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reconcile_nv_init(uint32_t                       bundle_version,
                                              const ra8_bundle_image_desc_t* descs,
                                              uint16_t                       image_count,
                                              ra8_reconcile_nv_state_t*      out_state);

/**
 * @brief Validate CRC-32 and structural invariants of a persisted NV state block.
 *
 * @details Calling context: `task-safe-reentrant`. Computes IEEE CRC-32 over
 * bytes `[0..84)` before verifying header magic, format version, and target slots.
 *
 * @param[in] state Persisted NV state block to check (non-NULL).
 * @return `k_ra8_ok`, `k_ra8_err_null_ptr`, `k_ra8_err_crc_mismatch`, or `k_ra8_err_validation_failed`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reconcile_nv_validate(const ra8_reconcile_nv_state_t* state);

/**
 * @brief Compute bounded exponential retry backoff delay in milliseconds.
 *
 * @details Calling context: `task-safe-reentrant`. Returns `0` when
 * `retries_used == 0` (first attempt), otherwise clamps
 * `base_backoff_ms << min(retries_used - 1, policy->backoff_shift)` at `max_backoff_ms`.
 *
 * @param[in]  policy       Reconciler retry/backoff policy (non-NULL).
 * @param[in]  retries_used Failed attempts already recorded for the target.
 * @param[out] out_delay_ms Destination receiving computed delay in ms (non-NULL).
 *
 * @return `k_ra8_ok`, `k_ra8_err_null_ptr`, or `k_ra8_err_invalid_arg`.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reconcile_compute_backoff_ms(const ra8_reconcile_policy_t* policy,
                                                         uint8_t                       retries_used,
                                                         uint32_t* out_delay_ms);

/**
 * @brief Execute one desired-state boot-time companion reconciliation pass.
 *
 * @details Calling context: `task-only-non-reentrant`. Loads persisted NV state
 * (resetting cleanly if blank, corrupt, or from a different `bundle_version`),
 * honors latched `safe_halted` state without issuing further applies, queries each
 * companion target, skips already-converged targets idempotently, persists retry
 * increments prior to applying mismatching companions, applies bounded backoff
 * on retries, and writes @p out_report on completion.
 *
 * @param[in]  bundle_hdr Validated committed `"RA8U"` bundle header (non-NULL).
 * @param[in]  descs      Validated descriptor array (`bundle_hdr->image_count` entries).
 * @param[in]  policy     Retry and exponential backoff policy (non-NULL).
 * @param[in]  ops        Injected NV/companion/backoff/safe hooks (non-NULL).
 * @param[out] out_report Diagnostic convergence summary (non-NULL, untouched on hard error).
 *
 * @return `k_ra8_ok` when boot pass completed (inspect `out_report->overall_state`), or explicit error.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_reconcile_boot(const ra8_bundle_hdr_t*        bundle_hdr,
                                           const ra8_bundle_image_desc_t* descs,
                                           const ra8_reconcile_policy_t*  policy,
                                           const ra8_reconcile_ops_t*     ops,
                                           ra8_reconcile_report_t*        out_report);

#ifdef __cplusplus
}
#endif
