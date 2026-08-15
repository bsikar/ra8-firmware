/**
 * @file mdl_fetch_body_internal.h
 * @brief Portable single-transaction response-body sink for media fetches.
 * @details Declares the caller-owned state and lifecycle used to stage, verify,
 *          commit, or abort one bounded response body.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "mdl_net.h"
#include "mdl_state.h"
#include "mdl_storage.h"
#include "ra8_attributes.h"
#include "ra8_err.h"

/** @brief Bounded signature extent retained before image publication begins. */
typedef enum : uint8_t {
  k_mdl_fetch_magic_bytes = 16U, /**< Largest supported image signature prefix. */
} mdl_fetch_body_limit_t;

/** @brief Destination policy for one response body. */
typedef enum : uint8_t {
  k_mdl_fetch_body_exact = 0U, /**< Publish under the supplied exact path. */
  k_mdl_fetch_body_image = 1U, /**< Derive the suffix from image magic.    */
} mdl_fetch_body_mode_t;

/**
 * @struct mdl_fetch_body_t
 * @brief Caller-owned state for one retryable streamed fetch.
 * @invariant An active writer owns the bound storage transaction workspace.
 * @since 0.1.0
 */
typedef struct {
  mdl_storage_t*        storage;                         /**< Exclusive storage binding.        */
  const char*           target_abs;                      /**< Requested absolute destination.   */
  const char*           target_rel;                      /**< Requested relative path or NULL.  */
  mdl_fetch_body_mode_t mode;                            /**< Exact or magic-typed publication. */
  mdl_storage_txn_t     writer;                          /**< Single private output stage.      */
  uint8_t               prefix[k_mdl_fetch_magic_bytes]; /**< Deferred signature bytes.         */
  uint8_t               prefix_bytes;                    /**< Bytes retained in @ref prefix.    */
  uint64_t              received;                        /**< Network bytes accepted.           */
  char                  actual_abs[PATH_MAX];            /**< Magic-derived final path.         */
  char                  actual_rel[k_mdl_relpath_max];   /**< Magic-derived relative path.      */
} mdl_fetch_body_t;

/**
 * @brief Initialize an exact-path transactional body sink.
 * @details Binds the caller's storage and destination without filesystem I/O.
 * @param[out] body Caller-owned sink state.
 * @param[in,out] storage Initialized exclusive storage binding.
 * @param[in] target_abs Canonical exact destination.
 * @return Canonical argument status.
 * @retval k_ra8_ok The body is ready to bind to the network seam.
 * @retval k_ra8_err_invalid_arg A required pointer/path is invalid.
 * @pre @p body is inactive and writable.
 * @pre @p target_abs remains valid through the synchronous fetch.
 * @post No filesystem transaction has begun.
 * @post Success initializes an empty accepted-byte count.
 * @note The first nonempty body chunk begins the transaction.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_body_init_exact(mdl_fetch_body_t* body,
                                                  mdl_storage_t*    storage,
                                                  const char*       target_abs);

/**
 * @brief Initialize a magic-typed image body sink.
 * @details Binds requested paths while deferring suffix selection to body magic.
 * @param[out] body Caller-owned sink state.
 * @param[in,out] storage Initialized exclusive storage binding.
 * @param[in] target_abs URL-derived absolute destination.
 * @param[in] target_rel URL-derived relative destination.
 * @return Canonical argument status.
 * @retval k_ra8_ok The body is ready with no transaction created.
 * @retval k_ra8_err_invalid_arg A required pointer/path is invalid.
 * @pre Paths remain valid through the synchronous fetch.
 * @pre Each path contains a replaceable suffix.
 * @post At most sixteen leading bytes are buffered before transaction begin.
 * @post No filesystem transaction has begun.
 * @note Content-Type never overrides unsupported or conflicting bytes.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_body_init_image(mdl_fetch_body_t* body,
                                                  mdl_storage_t*    storage,
                                                  const char*       target_abs,
                                                  const char*       target_rel);

/**
 * @brief Return the network sink view of one body state.
 * @details Exposes reset/write callbacks borrowing the supplied body context.
 * @param[in,out] body Initialized body state.
 * @return Reset/write callback binding borrowing @p body.
 * @retval mdl_net_body_sink_t A complete synchronous callback view.
 * @pre @p body remains live until the network call returns.
 * @pre @p body is initialized and exclusively owned.
 * @post No state is changed by constructing the view.
 * @post The returned view points back to @p body.
 * @note The returned context is not retained by the dispatcher.
 * @since 0.1.0
 */
RA8_PRIV mdl_net_body_sink_t priv_mdl_fetch_body_sink(mdl_fetch_body_t* body);

/**
 * @brief Finish prefix classification and make a nonempty stage commit-ready.
 * @details Flushes a short retained signature after a successful response.
 * @param[in,out] body Completed successful non-304 response body.
 * @return Canonical size, type, path, or transaction status.
 * @retval k_ra8_ok The actual paths and complete private stage are ready.
 * @retval k_ra8_err_invalid_size The body was empty.
 * @retval k_ra8_err_validation_failed Image magic was unsupported.
 * @retval other Path or transaction failure propagated.
 * @pre The network call returned ::k_ra8_ok with a status other than 304.
 * @pre @p body is initialized and exclusively owned.
 * @post Success does not validate, publish, or mutate persistent state.
 * @post Failure leaves any active private stage available for explicit abort.
 * @note Callers may perform state-capacity checks before commit.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_body_prepare(mdl_fetch_body_t* body);

/**
 * @brief Validate exact size/hash and publish one prepared body.
 * @details Independently rereads the private stage before one durable commit.
 * @param[in,out] body Prepared body transaction.
 * @return Canonical validation, commit, or cleanup status.
 * @retval k_ra8_ok The independently verified body was published.
 * @retval other Validation, publication, or cleanup failed.
 * @pre ::priv_mdl_fetch_body_prepare succeeded.
 * @pre Persistent-state capacity and destination policy have been checked.
 * @post Success publishes exactly once and leaves no active transaction.
 * @post A prepublication failure attempts cleanup and preserves any prior destination.
 * @note A backend may truthfully return a postpublication durability error;
 *       the body's identity/path fields remain readable in either case.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_body_commit(mdl_fetch_body_t* body);

/**
 * @brief Abort any private stage owned by a body sink.
 * @details Delegates stage cleanup and clears accepted bytes on success.
 * @param[in,out] body Initialized body state.
 * @return Canonical cleanup status.
 * @retval k_ra8_ok No private stage remains.
 * @retval k_ra8_err_invalid_arg The body pointer is NULL.
 * @pre @p body is non-NULL and exclusively owned.
 * @pre The body's storage binding remains live if a stage is active.
 * @post Success leaves no active transaction or accepted response bytes.
 * @post Failure preserves diagnostic state and publishes no new destination.
 * @note Required after network, status, or state failures.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_fetch_body_abort(mdl_fetch_body_t* body);
