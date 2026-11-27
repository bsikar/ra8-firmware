/**
 * @file port/nimble/src/nimble_port_alloc.c
 * @brief Fixed static SRAM block allocator backing NimBLE startup GATT tables.
 *
 * @par Tag
 * [Ring 4 / PORT] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nimble/nimble_npl.h"
#include "ra8_attributes.h"

/**
 * @brief Declaration matching ``port/nimble/inc/nimble_port_alloc.h`` without
 *        pulling in the ``malloc``/``realloc``/``free`` preprocessor redirects.
 */
void* nimble_port_malloc(size_t size);
void* nimble_port_realloc(void* ptr, size_t size);
void  nimble_port_free(void* ptr);

typedef enum : uint32_t {
  k_nimble_alloc_pool_bytes = 8192U, /**< Static SRAM pool bytes for GATT/ATT tables. */
  k_nimble_alloc_align      = 8U,    /**< Minimum block alignment (8-byte).           */
  k_nimble_alloc_max_blocks = 24U,   /**< Max concurrent tracked blocks/holes.        */
} nimble_port_alloc_const_t;

typedef struct nimble_alloc_block {
  uint32_t offset;
  uint32_t size;
  bool     used;
} nimble_alloc_block_t;

static alignas(8) uint8_t s_pool[k_nimble_alloc_pool_bytes];
static nimble_alloc_block_t s_blocks[k_nimble_alloc_max_blocks];
static uint32_t             s_block_count;
static bool                 s_inited;

RA8_INTERNAL static void internal_alloc_init_if_needed(void)
{
  if (!s_inited) {
    s_blocks[0] = (nimble_alloc_block_t){
      .offset = 0U,
      .size   = (uint32_t)k_nimble_alloc_pool_bytes,
      .used   = false,
    };
    s_block_count = 1U;
    s_inited      = true;
  }
}

RA8_INTERNAL static uint32_t internal_align_size(size_t size)
{
  if ((size == 0U) || (size > (size_t)k_nimble_alloc_pool_bytes)) {
    return (size == 0U) ? (uint32_t)k_nimble_alloc_align : 0U;
  }
  const uint32_t s = (uint32_t)size;
  const uint32_t m = (uint32_t)k_nimble_alloc_align - 1U;
  if (s > ((uint32_t)k_nimble_alloc_pool_bytes - m)) {
    return (uint32_t)k_nimble_alloc_pool_bytes;
  }
  return (s + m) & ~m;
}

RA8_INTERNAL static int32_t internal_find_block(const void* ptr)
{
  if (ptr == nullptr) {
    return -1;
  }
  for (uint32_t i = 0U; i < s_block_count; i++) {
    if (s_blocks[i].used && ((const void*)&s_pool[s_blocks[i].offset] == ptr)) {
      return (int32_t)i;
    }
  }
  return -1;
}

RA8_INTERNAL static void internal_split_block(uint32_t idx, uint32_t need)
{
  const uint32_t rem = s_blocks[idx].size - need;
  if ((rem < (uint32_t)k_nimble_alloc_align) ||
      (s_block_count >= (uint32_t)k_nimble_alloc_max_blocks)) {
    return;
  }
  for (uint32_t j = s_block_count; j > (idx + 1U); j--) {
    s_blocks[j] = s_blocks[j - 1U];
  }
  s_blocks[idx].size = need;
  s_blocks[idx + 1U] = (nimble_alloc_block_t){
    .offset = s_blocks[idx].offset + need,
    .size   = rem,
    .used   = false,
  };
  s_block_count++;
}

RA8_INTERNAL static void internal_remove_block(uint32_t idx)
{
  for (uint32_t j = idx; (j + 1U) < s_block_count; j++) {
    s_blocks[j] = s_blocks[j + 1U];
  }
  if (s_block_count > 0U) {
    s_block_count--;
  }
}

RA8_INTERNAL static void internal_coalesce(uint32_t idx)
{
  if (((idx + 1U) < s_block_count) && !s_blocks[idx + 1U].used) {
    s_blocks[idx].size += s_blocks[idx + 1U].size;
    internal_remove_block(idx + 1U);
  }
  if ((idx > 0U) && !s_blocks[idx - 1U].used) {
    s_blocks[idx - 1U].size += s_blocks[idx].size;
    internal_remove_block(idx);
  }
}

RA8_INTERNAL static void* internal_malloc_locked(uint32_t need)
{
  internal_alloc_init_if_needed();
  if (need == 0U) {
    return nullptr;
  }
  for (uint32_t i = 0U; i < s_block_count; i++) {
    if (!s_blocks[i].used && (s_blocks[i].size >= need)) {
      internal_split_block(i, need);
      s_blocks[i].used = true;
      return &s_pool[s_blocks[i].offset];
    }
  }
  return nullptr;
}

RA8_INTERNAL static void internal_free_locked(const void* ptr)
{
  const int32_t idx = internal_find_block(ptr);
  if (idx < 0) {
    return;
  }
  s_blocks[(uint32_t)idx].used = false;
  internal_coalesce((uint32_t)idx);
}

/* Allocate size bytes from the static NimBLE SRAM pool. */
void* nimble_port_malloc(size_t size)
{
  const uint32_t need = internal_align_size(size);
  const uint32_t crit = ble_npl_hw_enter_critical();
  void* const    out  = internal_malloc_locked(need);
  ble_npl_hw_exit_critical(crit);
  return out;
}

/* Return a block to the static NimBLE SRAM pool and coalesce neighbors. */
void nimble_port_free(void* ptr)
{
  if (ptr == nullptr) {
    return;
  }
  const uint32_t crit = ble_npl_hw_enter_critical();
  internal_free_locked(ptr);
  ble_npl_hw_exit_critical(crit);
}

/* Resize an allocation from nimble_port_malloc within the static SRAM pool. */
void* nimble_port_realloc(void* ptr, size_t size)
{
  if (ptr == nullptr) {
    return nimble_port_malloc(size);
  }
  if (size == 0U) {
    nimble_port_free(ptr);
    return nullptr;
  }

  const uint32_t need = internal_align_size(size);
  if (need == 0U) {
    return nullptr;
  }

  const uint32_t crit = ble_npl_hw_enter_critical();
  const int32_t  idx  = internal_find_block(ptr);
  if (idx < 0) {
    ble_npl_hw_exit_critical(crit);
    return nullptr;
  }
  const uint32_t u = (uint32_t)idx;
  if (s_blocks[u].size >= need) {
    internal_split_block(u, need);
    ble_npl_hw_exit_critical(crit);
    return ptr;
  }
  if (((u + 1U) < s_block_count) && !s_blocks[u + 1U].used &&
      ((s_blocks[u].size + s_blocks[u + 1U].size) >= need)) {
    s_blocks[u].size += s_blocks[u + 1U].size;
    internal_remove_block(u + 1U);
    internal_split_block(u, need);
    ble_npl_hw_exit_critical(crit);
    return ptr;
  }

  const uint32_t old_size = s_blocks[u].size;
  void* const    next     = internal_malloc_locked(need);
  if (next != nullptr) {
    const uint32_t copy_bytes = (old_size < need) ? old_size : need;
    (void)memcpy(next, ptr, (size_t)copy_bytes);
    internal_free_locked(ptr);
  }
  ble_npl_hw_exit_critical(crit);
  return next;
}
