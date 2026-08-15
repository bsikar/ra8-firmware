/**
 * @file fs_exfat_dir_listing_test_util.h
 * @brief Bounded exFAT directory-listing assertions for host tests
 * @details Records names, attributes, and sizes in caller-visible bounded
 * storage and provides exact file-versus-directory membership predicates.
 * This listing responsibility is separate from the structural tree scanner.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "unity_minimal.h"

/** @brief Bounds and attribute bits used by listing assertions. */
typedef enum : uint32_t {
  k_exfat_listing_cap            = 32U,   /**< Maximum entries retained.      */
  k_exfat_listing_name_cap       = 80U,   /**< Bytes in each retained name.   */
  k_exfat_listing_attr_directory = 0x10U, /**< exFAT directory attribute bit. */
} exfat_listing_limit_t;

/**
 * @struct name_ctx_t
 * @brief Listdir context that records the names and attributes it is handed.
 * @since 0.1.0
 */
typedef struct {
  uint32_t count;                                                /**< Entries reported.         */
  char     names[k_exfat_listing_cap][k_exfat_listing_name_cap]; /**< Names in order.           */
  uint8_t  attrs[k_exfat_listing_cap];                           /**< Matching attribute bytes. */
  uint32_t sizes[k_exfat_listing_cap];                           /**< Matching sizes.           */
} name_ctx_t;

/**
 * @brief Record one listed name, attribute, and size.
 * @param[in] name Entry name.
 * @param[in] attr Entry attribute byte.
 * @param[in] size Entry size.
 * @param[in,out] ctx Pointer to a ::name_ctx_t.
 * @pre @p name and @p ctx are non-NULL.
 * @post The entry count advances; entries inside the cap are retained.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static inline void
internal_name_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  name_ctx_t* listing = (name_ctx_t*)ctx;
  if (listing->count < (uint32_t)k_exfat_listing_cap) {
    (void)snprintf(listing->names[listing->count], (size_t)k_exfat_listing_name_cap, "%s", name);
    listing->attrs[listing->count] = attr;
    listing->sizes[listing->count] = size;
  }
  listing->count++;
}

/**
 * @brief List @p path and return how many entries it reported.
 * @param[in] h Mounted exFAT volume.
 * @param[in] path Directory path.
 * @param[out] out Receives the recorded names and attributes.
 * @return The entry count.
 * @pre @p h, @p path, and @p out are non-NULL.
 * @post @p out holds the reported entries.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static inline uint32_t
internal_list_names(ra8_fs_mount_t* h, const char* path, name_ctx_t* out)
{
  *out = (name_ctx_t){};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, path, internal_name_cb, out));
  return out->count;
}

/**
 * @brief Test whether a listing contains a named directory.
 * @param[in] ctx Recorded listing.
 * @param[in] name Name to find.
 * @return 1 when present as a directory; otherwise 0.
 * @pre @p ctx and @p name are non-NULL.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static inline uint8_t internal_has_dir(const name_ctx_t* ctx, const char* name)
{
  for (uint32_t i = 0U; i < ctx->count; i++) {
    if (strcmp(ctx->names[i], name) != 0) {
      continue;
    }
    return ((ctx->attrs[i] & (uint8_t)k_exfat_listing_attr_directory) != 0U) ? 1U : 0U;
  }
  return 0U;
}

/**
 * @brief Test whether a listing contains a named plain file.
 * @param[in] ctx Recorded listing.
 * @param[in] name Name to find.
 * @return 1 when present as a plain file; otherwise 0.
 * @pre @p ctx and @p name are non-NULL.
 * @since 0.1.0

 * @details Performs one bounded, deterministic operation for this host test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
*/
RA8_INTERNAL static inline uint8_t internal_has_file(const name_ctx_t* ctx, const char* name)
{
  for (uint32_t i = 0U; i < ctx->count; i++) {
    if (strcmp(ctx->names[i], name) != 0) {
      continue;
    }
    return ((ctx->attrs[i] & (uint8_t)k_exfat_listing_attr_directory) == 0U) ? 1U : 0U;
  }
  return 0U;
}
