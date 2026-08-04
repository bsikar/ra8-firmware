/**
 * @file ra8_dual_core_job.h
 * @brief Cross-core compile-job dispatch seam + status contract (#149)
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The #149 EPUB->`.rabook` offload runs the heavy compile on the Cortex-M33
 * secondary core while the Cortex-M85 keeps the UI live. This header is the
 * reusable Dependency-Inversion seam between the part that decides WHAT to
 * compile (the import library) and the cross-core transport that moves the work
 * to the secondary core: the import adapter calls a
 * @ref ra8_dual_core_compile_dispatch_fn with the source `.epub` bytes and an
 * output buffer; the production implementation stages the bytes into shared
 * memory, releases CPU1 (see `ra8_cpu1_release`), posts the job to the shared
 * mailbox, waits for completion and returns the finalized RABOOK1 blob. A host
 * test binds a mock dispatch returning a known blob, so the import adapter is
 * exercised without a second core.
 *
 * The concrete shared-memory mailbox layout (addresses, magics, the M85<->M33
 * handshake) is owned by the transport (the compile_on_m33 example); only the
 * dispatch contract and the job-status vocabulary are shared here.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_dual_core_job_status_t
 * @brief Outcome the secondary core reports for a dispatched compile job.
 *
 * @details Published in the shared mailbox by the worker core and read back by
 *          the primary core once the job's `done` flag is set; a dispatch seam
 *          maps any non-`ok` status to a non-zero @ref ra8_err_t for its caller.
 *
 * @invariant Exactly one status is live per job; ::k_ra8_dual_core_job_running is
 *            the initial value the primary core writes before posting the job.
 *
 * @code
 * if (mb->status == (uint32_t)k_ra8_dual_core_job_ok) { use_blob(mb); }
 * @endcode
 *
 * @see ra8_dual_core_compile_dispatch_fn
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_dual_core_job_running    = 0U, /**< Worker core is still building the blob.   */
  k_ra8_dual_core_job_ok         = 1U, /**< Compile finalized a valid RABOOK1 blob.   */
  k_ra8_dual_core_job_build_fail = 2U, /**< A compile stage overflowed or failed.     */
  k_ra8_dual_core_job_open_fail  = 3U, /**< The source `.epub` could not be parsed.   */
  k_ra8_dual_core_job_no_request = 4U, /**< No job was posted (request magic absent). */
} ra8_dual_core_job_status_t;

/**
 * @typedef ra8_dual_core_compile_dispatch_fn
 * @brief DI seam: dispatch one EPUB compile to the secondary core, return the blob.
 *
 * @details Hands the source `.epub` bytes to the secondary core and returns the
 *          finalized RABOOK1 blob it produces. The production binding stages the
 *          bytes to shared memory, releases CPU1, posts the job, waits for `done`
 *          and copies the blob into @p out_buf; a host test binds a mock that
 *          returns a precomputed blob. The caller (the import adapter) validates
 *          the returned blob with `ra8_book_validate` before trusting it.
 *
 * @param[in]  ctx      Opaque transport cookie (the shared-memory layout), or
 *                      NULL for a stateless mock.
 * @param[in]  epub     Source `.epub` bytes (non-NULL; @p epub_len readable).
 * @param[in]  epub_len Length of @p epub in bytes (> 0).
 * @param[out] out_buf  Destination for the finalized blob (non-NULL).
 * @param[in]  out_cap  Capacity of @p out_buf in bytes (> 0).
 * @param[out] out_len  Receives the produced blob length on @c k_ra8_ok.
 *
 * @return Error code.
 * @retval k_ra8_ok           A valid-status blob of @p *out_len bytes is in @p out_buf.
 * @retval k_ra8_err_null_ptr A required pointer argument is NULL.
 * @retval k_ra8_err_no_mem   The produced blob did not fit @p out_cap.
 * @retval k_ra8_err_hw_error The worker reported a non-ok status or never finished.
 *
 * @note Implementations run single-threaded from the primary core's request path.
 * @see ra8_dual_core_job_status_t
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_dual_core_compile_dispatch_fn)(void*          ctx,
                                                       const uint8_t* epub,
                                                       uint32_t       epub_len,
                                                       uint8_t*       out_buf,
                                                       uint32_t       out_cap,
                                                       uint32_t*      out_len);
