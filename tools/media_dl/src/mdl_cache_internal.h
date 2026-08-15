/**
 * @file mdl_cache_internal.h
 * @brief Private binary-index and body I/O seams for mdl_cache.
 * @details Shares validated host paths and transaction helpers between the
 *          cache policy and persistent-I/O translation units.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */
#ifndef MDL_CACHE_INTERNAL_H
/** @brief One-definition guard for the private cache seam. */
#define MDL_CACHE_INTERNAL_H

#include "mdl_cache.h"
#include "ra8_attributes.h"

/** @brief Complete paths and host identity derived for one request. */
typedef struct {
  char     host[k_mdl_gov_host_max];     /**< Lowercase host and optional port. */
  char     directory[k_fw_fs_path_cap];  /**< Host-specific cache directory. */
  char     index_path[k_fw_fs_path_cap]; /**< Versioned host index path.     */
  uint64_t host_hash;                    /**< Stable host identity.           */
} mdl_cache_paths_t;

/**
 * @brief Prepare a host directory and load or recover its index.
 * @details Derives a host-bound namespace, authenticates an existing index,
 *          and discards only a corrupt regular index before resetting it.
 * @param[in,out] cache Cache binding and index workspace.
 * @param[in] url Request URL used to select the host.
 * @param[out] paths Derived host paths.
 * @param[out] rebuilt Whether a corrupt regular index was discarded.
 * @return Canonical path, namespace, or parse status.
 * @retval k_ra8_ok Paths and a valid or empty index are available.
 * @retval k_ra8_err_invalid_state A non-regular index or invalid namespace exists.
 * @retval other URL parsing, directory, file, or cleanup failed.
 * @pre All pointers are non-NULL and cache root is canonical.
 * @pre Cache storage and index workspace are exclusively owned.
 * @post Success initializes both @p paths and `cache->index`.
 * @post Success reports corrupt-index recovery through @p rebuilt.
 * @note A non-regular index fails closed and is never removed.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_cache_load(mdl_cache_t*       cache,
                                       const char*        url,
                                       mdl_cache_paths_t* paths,
                                       bool*              rebuilt);

/**
 * @brief Transactionally publish the current host index.
 * @details Encodes a checksummed generation directly into a portable storage
 *          transaction and commits only after every record is written.
 * @param[in,out] cache Cache binding containing the index.
 * @param[in] paths Matching host paths from ::priv_mdl_cache_load.
 * @return Canonical serialization or publication status.
 * @retval k_ra8_ok The complete index generation was published.
 * @retval k_ra8_err_invalid_arg The index and host paths do not match.
 * @retval other Validation, transaction writing, commit, or abort failed.
 * @pre Index invariants and host identity are valid.
 * @pre Cache storage and transaction workspace are exclusively owned.
 * @post Success publishes one complete checksummed generation.
 * @post Failure never exposes a partial index generation.
 * @note Existing indexes require truthful atomic-replace support.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_cache_save(mdl_cache_t* cache, const mdl_cache_paths_t* paths);

/**
 * @brief Read and hash-check one retained body into caller storage.
 * @details Requires a regular exact-sized file, reads it completely with
 *          bounded progress, checks EOF, then authenticates the content hash.
 * @param[in,out] storage Exclusive storage binding.
 * @param[in] paths Matching host directory.
 * @param[in] record Persistent body identity.
 * @param[out] buffer Destination storage.
 * @param[in] capacity Destination capacity.
 * @param[out] out_length Exact body extent.
 * @return Canonical file, size, or identity status.
 * @retval k_ra8_ok Exact authenticated bytes are available.
 * @retval k_ra8_err_not_found The retained body is absent.
 * @retval k_ra8_err_validation_failed The body hash differs.
 * @retval other Size, open, read, or close failed.
 * @pre Every pointer is non-NULL.
 * @pre @p buffer spans @p capacity nonzero writable bytes.
 * @post Success publishes only an exact hash-matching body.
 * @post Failure leaves @p out_length equal to zero.
 * @note A mismatched body is treated as validation failure.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_cache_read_body(mdl_storage_t*            storage,
                                            const mdl_cache_paths_t*  paths,
                                            const mdl_cache_record_t* record,
                                            char*                     buffer,
                                            size_t                    capacity,
                                            size_t*                   out_length);

/**
 * @brief Publish one new body under its immutable content-derived leaf.
 * @details Derives the leaf from both URL and content identities, streams the
 *          exact bytes into a transaction, and commits only on full success.
 * @param[in,out] storage Exclusive storage binding.
 * @param[in] paths Matching host directory.
 * @param[in] url_hash Stable URL identity.
 * @param[in] content_hash Exact body identity.
 * @param[in] buffer Complete body bytes.
 * @param[in] length Body extent.
 * @param[out] relative_path Published relative leaf.
 * @param[in] relative_capacity Writable leaf capacity.
 * @return Canonical transaction status.
 * @retval k_ra8_ok The immutable body is completely published.
 * @retval k_ra8_err_invalid_arg A pointer or length contract is invalid.
 * @retval k_ra8_err_invalid_size The leaf or full path exceeded its bound.
 * @retval other Transaction begin, write, commit, or abort failed.
 * @pre Body is nonempty and pointers/capacities are valid.
 * @pre Storage and transaction workspace are exclusively owned.
 * @post Success publishes exactly @p length bytes atomically.
 * @post Success returns the exact relative leaf in @p relative_path.
 * @note Orphaned immutable bodies are harmless after an index-save failure.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_mdl_cache_publish_body(mdl_storage_t*           storage,
                                               const mdl_cache_paths_t* paths,
                                               uint64_t                 url_hash,
                                               uint64_t                 content_hash,
                                               const char*              buffer,
                                               size_t                   length,
                                               char*                    relative_path,
                                               size_t                   relative_capacity);

#endif
