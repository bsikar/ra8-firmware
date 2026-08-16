/**
 * @file reader_vmem_workspace_test.c
 * @brief Allocation-free two-instance tests for reader_vmem workspace planning
 *
 * @details Binds two distinct aligned backings, proves typed views never alias,
 * and verifies budget and capacity rejection without process-global ownership.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "reader_vmem_internal.h"

/** @brief Small test-only backing bound independently twice. */
typedef struct {
  alignas(max_align_t) uint8_t bytes[16384U]; /**< Test-owned workspace bytes. */
} test_backing_t;

/** @brief First explicit test composition backing. */
static test_backing_t s_first_backing;
/** @brief Second explicit test composition backing. */
static test_backing_t s_second_backing;

/**
 * @brief Check one typed view starts within its owning backing.
 * @details Compares numeric half-open address bounds without dereferencing the view.
 * @param[in] backing Test backing that should own the pointer.
 * @param[in] pointer Candidate typed workspace view.
 * @return Whether the address begins inside the backing byte array.
 * @retval true Pointer lies in `[backing->bytes, backing->bytes + size)`.
 * @retval false Pointer lies outside that half-open address range.
 * @pre @p backing is non-null and alive.
 * @pre @p pointer may be any non-dereferenced pointer value.
 * @post Neither backing nor pointer target is changed.
 * @post Candidate pointer is never dereferenced.
 * @note Test-only and thread-safe for immutable arguments.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_contains(const test_backing_t* backing, const void* pointer)
{
  const uintptr_t begin = (uintptr_t)backing->bytes;
  const uintptr_t end   = begin + sizeof(backing->bytes);
  const uintptr_t value = (uintptr_t)pointer;
  return value >= begin && value < end;
}

int main(void)
{
  rv_workspace_need_t need = {};
  if (!priv_rv_workspace_require(1U, &need) || need.total_bytes > sizeof(test_backing_t)) {
    return 1;
  }
  rv_workspace_t first_view  = {};
  rv_workspace_t second_view = {};
  if (!priv_rv_workspace_bind(s_first_backing.bytes,
                              sizeof(s_first_backing.bytes),
                              &need,
                              &first_view) ||
      !priv_rv_workspace_bind(s_second_backing.bytes,
                              sizeof(s_second_backing.bytes),
                              &need,
                              &second_view)) {
    return 2;
  }
  if (!internal_contains(&s_first_backing, first_view.frame_mem) ||
      !internal_contains(&s_first_backing, first_view.meta) ||
      !internal_contains(&s_first_backing, first_view.keys) ||
      !internal_contains(&s_first_backing, first_view.buckets) ||
      !internal_contains(&s_second_backing, second_view.frame_mem) ||
      first_view.frame_mem == second_view.frame_mem) {
    return 3;
  }
  if (priv_rv_workspace_bind(s_first_backing.bytes, need.total_bytes - 1U, &need, &first_view) ||
      priv_rv_workspace_require(0U, &need)) {
    return 4;
  }
  return 0;
}
