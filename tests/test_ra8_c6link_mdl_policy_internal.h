/**
 * @file test_ra8_c6link_mdl_policy_internal.h
 * @brief Deterministic media backend shared by the two service test units.
 * @details The backend record and its three callbacks live beside the
 * request-policy and response-metadata vectors that exercise them hardest, and
 * are published here so the primary service suite binds the same one
 * implementation to its own independent instance. One runner entry point keeps
 * both hand-authored translation units below the repository size cap.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_c6link_mdl.h"
#include "ra8_c6link_mdl_msg.h"

/**
 * @enum t_mdl_backend_const_t
 * @brief Fixed sentinels published by the deterministic media backend.
 * @details One octet value identifies every digest the backend publishes, so a
 * test asserting on a terminal digest compares against the same constant the
 * backend wrote.
 * @invariant The fill octet is distinguishable from a zeroed buffer.
 * @code
 * TEST_ASSERT_EQ(k_t_mdl_digest_fill, chunk->sha256.data[0]);
 * @endcode
 * @see ra8_test_mdl_backend_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_t_mdl_digest_fill = 0xA5U, /**< Deterministic digest test octet. */
} t_mdl_backend_const_t;

/**
 * @enum t_mdl_read_fault_t
 * @brief Single backend metadata fault selected by one test vector.
 * @details Each value makes exactly one backend-coherence operand incoherent,
 * so a vector that selects it varies that operand alone.
 * @invariant Exactly one fault is active on a backend at a time.
 * @code
 * backend.read_fault = k_t_mdl_read_fault_oversize;
 * @endcode
 * @see ra8_test_mdl_backend_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_t_mdl_read_fault_none = 0U,     /**< Return canonical backend metadata. */
  k_t_mdl_read_fault_oversize,      /**< Report more data than requested.   */
  k_t_mdl_read_fault_empty_active,  /**< Report no data without completing. */
  k_t_mdl_read_fault_terminal_data, /**< Report terminal state with body data.
                                     */
} t_mdl_read_fault_t;

/**
 * @struct ra8_test_mdl_backend_t
 * @brief State behind the deterministic media backend.
 * @details Holds the borrowed artifact bytes, the observed request policy, the
 * callback counters a vector asserts on, and the single injected fault.
 * @invariant `at` never exceeds `len`.
 * @invariant A nonzero `read_fault` mutates exactly one response field.
 * @code
 * ra8_test_mdl_backend_t backend = {.bytes = body, .len = sizeof(body)};
 * @endcode
 * @see priv_test_mdl_backend_read
 * @since 0.1.0
 */
typedef struct {
  const uint8_t*   bytes;                                      /**< Modelled response body.  */
  size_t           len;                                        /**< Complete body length.    */
  size_t           at;                                         /**< Next body byte offset.   */
  uint32_t         begins;                                     /**< Successful begin count.  */
  uint32_t         cancels;                                    /**< Successful cancel count. */
  ra8_mdl_format_t format;                                     /**< Requested format.        */
  uint32_t         timeout_ms;                                 /**< Requested timeout.       */
  char             user_agent[k_ra8_mdl_user_agent_max];       /**< Copied User-Agent.       */
  char             referer[k_ra8_mdl_referer_max];             /**< Copied Referer.          */
  char             if_none_match[k_ra8_mdl_etag_max];          /**< Copied ETag condition.   */
  char             if_modified_since[k_ra8_mdl_http_date_max]; /**< Copied date condition.   */
  bool             terminal_total_zero;                        /**< Bad terminal total.      */
  bool             response_override;                          /**< Publish `response`.      */
  ra8_mdl_http_response_t response;                            /**< Terminal metadata.       */
  t_mdl_read_fault_t      read_fault;                          /**< Selected metadata fault. */
} ra8_test_mdl_backend_t;

/**
 * @brief Accept one job for the deterministic fixture URL.
 * @details Rewinds the artifact cursor and records the complete forwarded
 * request policy so a vector can assert the service passed it through.
 * @param[in,out] ctx ::ra8_test_mdl_backend_t bound by the service.
 * @param[in] request Validated portable media request.
 * @return Backend status.
 * @retval k_ra8_ok The fixture URL was accepted and the cursor rewound.
 * @retval k_ra8_err_invalid_arg The URL is not the deterministic fixture URL.
 * @pre @p ctx points at initialised backend state.
 * @pre @p request is already bounded by the portable service.
 * @post Success records the format, timeout and four request headers.
 * @post Failure leaves backend state unchanged.
 * @note Test-target-private; the fake models a single stable origin.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_test_mdl_backend_begin(void* ctx, const ra8_mdl_request_t* request);

/**
 * @brief Publish the next bounded artifact slice or the terminal record.
 * @details Applies the selected ::t_mdl_read_fault_t, then either serves the
 * next slice or, at end of body, the terminal digest and response metadata.
 * @param[in,out] ctx ::ra8_test_mdl_backend_t bound by the service.
 * @param[out] out Caller-owned body-byte destination.
 * @param[in] cap Writable capacity of @p out.
 * @param[out] got Exact body bytes published.
 * @param[out] total_bytes Complete artifact length.
 * @param[out] complete Whether this response is terminal.
 * @param[out] sha256 Terminal digest destination.
 * @param[out] response Terminal status and selected headers.
 * @return Backend status.
 * @retval k_ra8_ok The deterministic backend operation completed.
 * @pre Every output pointer is non-null and @p out spans @p cap bytes.
 * @pre @p ctx points at initialised backend state.
 * @post A data response advances the cursor by @p got.
 * @post A terminal response publishes the digest and response record.
 * @note Test-target-private; `response_override` replaces the canonical record.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_test_mdl_backend_read(void*     ctx,
                                              uint8_t*  out,
                                              uint16_t  cap,
                                              uint16_t* got,
                                              uint64_t* total_bytes,
                                              bool*     complete,
                                              uint8_t   sha256[k_ra8_mdl_sha256_bytes],
                                              ra8_mdl_http_response_t* response);

/**
 * @brief Count one cancellation of the deterministic backend job.
 * @param[in,out] ctx ::ra8_test_mdl_backend_t bound by the service.
 * @return Backend status.
 * @retval k_ra8_ok The deterministic backend operation completed.
 * @pre @p ctx points at initialised backend state.
 * @pre The service owns the backend exclusively.
 * @post The cancellation counter advances by exactly one.
 * @post The bound artifact and digest are unchanged.
 * @note Test-target-private; cancellation is idempotent for the fixture.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_test_mdl_backend_cancel(void* ctx);

/**
 * @brief Run the service request-policy and response-metadata vectors.
 * @details Exercises every field operand of the decoded StartRequest and every
 * operand of the terminal response record the backend publishes.
 * @return Nothing.
 * @pre The unity-minimal assertion process is initialized.
 * @pre No other service instance is mid-dispatch.
 * @post Normal return means every malformed field was rejected.
 * @post Every job this runner started is terminal or cancelled.
 * @note Test-target-private and synchronous.
 * @since 0.1.0
 */
RA8_PRIV void priv_test_mdl_policy_run(void);
