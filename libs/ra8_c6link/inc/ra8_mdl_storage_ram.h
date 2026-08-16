/**
 * @file ra8_mdl_storage_ram.h
 * @brief Caller-buffer transaction adapter for verified C6 media sources.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / PAL] {World: NS}
 *
 * @details
 * Binds ::ra8_mdl_storage_iface_t to one caller-owned byte span. Downloaded
 * source bytes stay private until commit, then become available through a
 * read-only view. Abort discards the logical extent. This adapter is intended
 * for bounded source material that the RA8 will transform before publishing;
 * it owns no heap, filesystem, transport, or static workspace.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_c6link_mdl_transfer.h"
#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct ra8_mdl_storage_ram_t
 * @brief Mutable transaction over one caller-owned destination span.
 * @details Treat members as private after ::ra8_mdl_storage_ram_init; obtain
 *          committed bytes only through ::ra8_mdl_storage_ram_view.
 * @invariant `length <= capacity` and nonzero capacity implies non-NULL data.
 * @invariant `active` and `committed` are never true simultaneously.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* data;      /**< Caller-owned backing bytes.      */
  size_t   capacity;  /**< Writable backing capacity.       */
  size_t   length;    /**< Private or committed byte count. */
  bool     active;    /**< A transfer may append bytes.     */
  bool     committed; /**< A read-only view may be exposed. */
} ra8_mdl_storage_ram_t;

/**
 * @brief Bind a caller buffer as one transactional media destination.
 * @param[out] storage Caller-owned adapter state.
 * @param[out] output Bound coordinator interface.
 * @param[in,out] data Writable source-buffer backing.
 * @param[in] capacity Backing capacity in bytes.
 * @return Initialization status.
 * @retval k_ra8_ok The idle adapter and complete interface are ready.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_size @p capacity is zero.
 * @pre @p data spans @p capacity writable bytes.
 * @pre No transfer is using @p storage or @p data.
 * @post Success leaves zero committed bytes and every interface callback bound.
 * @post Failure leaves @p output unchanged.
 * @note Not thread-safe; one context serves one transfer at a time.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mdl_storage_ram_init(ra8_mdl_storage_ram_t*   storage,
                                                 ra8_mdl_storage_iface_t* output,
                                                 uint8_t*                 data,
                                                 size_t                   capacity);

/**
 * @brief Expose the complete committed source as an immutable byte view.
 * @param[in] storage Initialized adapter state.
 * @param[out] data Receives the first committed byte.
 * @param[out] length Receives the exact committed byte count.
 * @return View status.
 * @retval k_ra8_ok A nonempty committed view was returned.
 * @retval k_ra8_err_null_ptr A required pointer is NULL.
 * @retval k_ra8_err_invalid_state No completed transaction is committed.
 * @pre @p storage is not concurrently mutated by a transfer.
 * @pre Output pointers are writable.
 * @post Success returns a view wholly inside the initialized backing span.
 * @post Failure leaves both outputs unchanged.
 * @note The view remains valid until the next successful begin or reinitialize.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mdl_storage_ram_view(const ra8_mdl_storage_ram_t* storage,
                                                 const uint8_t**              data,
                                                 size_t*                      length);

#ifdef __cplusplus
} /* extern "C" */
#endif
