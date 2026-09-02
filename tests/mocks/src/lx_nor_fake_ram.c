/**
 * @file lx_nor_fake_ram.c
 * @brief RAM-backed LevelX NOR driver for host tests -- implementation.
 *
 * @par Tag
 * [Ring 4 / Storage] {World: NS}
 *
 * @details
 * Implements a minimal LevelX NOR driver whose media is a static ULONG array.
 * The read/write callbacks dereference the `ULONG*` LevelX hands them directly
 * (LevelX derives it from `lx_nor_flash_base_address`, which points at the
 * backing), mirroring the vendored `lx_nor_flash_simulator.c` but with clean,
 * first-party style and a configurable, larger geometry.
 *
 * ULONG width note: on the LP64 test host `ULONG` is 8 bytes, so the erased
 * pattern is written as `(ULONG)0xFFFFFFFF` (== LevelX's `LX_ALL_ONES`) rather
 * than all-ones, matching how LevelX compares words against `0xFFFFFFFF`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "lx_nor_fake_ram.h"

#include "lx_api.h"
#include "ra8_attributes.h"

/**
 * @enum lx_nor_fake_const_t
 * @brief Geometry + sentinel constants for the RAM NOR fake.
 * @details 64 blocks * 1024 words/block gives LevelX an ample logical-sector
 *          space (hundreds of 512-byte sectors) so the cache-store tests never
 *          run the underlying device out of physical sectors.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lx_nor_fake_total_blocks    = 64U,         /**< NOR blocks.         */
  k_lx_nor_fake_words_per_block = 1024U,       /**< ULONG words/block.  */
  k_lx_nor_fake_total_words     = 64U * 1024U, /**< Backing word count. */
  k_lx_nor_fake_erased          = 0xFFFFFFFFU, /**< LevelX erased word. */
} lx_nor_fake_const_t;

/**
 * @var s_fake_backing
 * @brief The fake NOR media (persists across open/close within a process).
 * @warning Written only through the driver callbacks and ::lx_nor_fake_ram_wipe.
 * @since 0.1.0
 */
static ULONG s_fake_backing[k_lx_nor_fake_total_words];

/**
 * @var s_fake_sector_buf
 * @brief LevelX per-open sector scratch (one logical sector wide).
 * @warning Owned by LevelX between open and close; not for other use.
 * @since 0.1.0
 */
static ULONG s_fake_sector_buf[LX_NOR_SECTOR_SIZE];

/**
 * @var s_fake_fail_writes
 * @brief Countdown of write callbacks to fail (fault injection); 0 = off.
 * @warning Set only through ::lx_nor_fake_ram_fail_writes.
 * @since 0.1.0
 */
static unsigned int s_fake_fail_writes;

/* NOLINTBEGIN(readability-non-const-parameter) -- LevelX fixes both callback signatures. */
/**
 * @brief Read @p words ULONGs from the backing pointer into @p destination.
 * @details Implements the LevelX read callback directly over its pointer
 * cookie, copying exactly the requested number of vendor-width words.
 * @param[in]  flash_address Source pointer into the backing (LevelX cookie).
 * @param[out] destination   Destination buffer of @p words ULONGs.
 * @param[in]  words         Word count.
 * @return `LX_SUCCESS`, or `LX_ERROR` on a NULL argument.
 * @retval 0 Words copied.
 * @retval 1 A pointer argument was NULL.
 * @pre @p flash_address lies within the backing.
 * @pre @p destination covers @p words ULONGs.
 * @post @p destination holds the copied words on success.
 * @post The backing is unmodified.
 * @note The non-const source signature is fixed by the LevelX callback ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_read(ULONG* flash_address, ULONG* destination, ULONG words)
{
  if (flash_address == LX_NULL) {
    return (UINT)LX_ERROR;
  }
  if (destination == LX_NULL) {
    return (UINT)LX_ERROR;
  }
  for (ULONG i = 0U; i < words; i++) {
    destination[i] = flash_address[i];
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief Write @p words ULONGs from @p source to the backing pointer.
 * @details Implements the LevelX write callback and consumes one injected
 * failure before modifying any backing word.
 * @param[in] flash_address Destination pointer into the backing (LevelX cookie).
 * @param[in] source        Source buffer of @p words ULONGs.
 * @param[in] words         Word count.
 * @return `LX_SUCCESS`, or `LX_ERROR` on a NULL argument.
 * @retval 0 Words written.
 * @retval 1 A pointer argument was NULL.
 * @pre @p flash_address lies within the backing.
 * @pre @p source covers @p words ULONGs.
 * @post The targeted backing words hold @p source on success.
 * @post No word outside the range is changed.
 * @note NOR bit-transition rules are LevelX's responsibility in this RAM fake.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_write(ULONG* flash_address, ULONG* source, ULONG words)
{
  if (flash_address == LX_NULL) {
    return (UINT)LX_ERROR;
  }
  if (source == LX_NULL) {
    return (UINT)LX_ERROR;
  }
  if (s_fake_fail_writes != 0U) {
    s_fake_fail_writes--;
    return (UINT)LX_ERROR;
  }
  for (ULONG i = 0U; i < words; i++) {
    flash_address[i] = source[i];
  }
  return (UINT)LX_SUCCESS;
}
/* NOLINTEND(readability-non-const-parameter) */

/**
 * @brief Erase one block to the LevelX erased pattern.
 * @details Converts the validated block index into its backing-array span and
 * rewrites every word to LevelX's erased sentinel.
 * @param[in] block       Block index.
 * @param[in] erase_count LevelX erase counter (unused).
 * @return `LX_SUCCESS`, or `LX_ERROR` when @p block is out of range.
 * @retval 0 Block erased.
 * @retval 1 @p block is out of range.
 * @pre @p block indexes a real block.
 * @pre The backing is static host storage.
 * @post Every word of the block reads as the erased pattern.
 * @post No other block is touched.
 * @note The vendor-supplied erase counter is intentionally ignored by the fake.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_block_erase(ULONG block, ULONG erase_count)
{
  LX_PARAMETER_NOT_USED(erase_count);
  if (block >= (ULONG)k_lx_nor_fake_total_blocks) {
    return (UINT)LX_ERROR;
  }
  ULONG base = block * (ULONG)k_lx_nor_fake_words_per_block;
  for (ULONG i = 0U; i < (ULONG)k_lx_nor_fake_words_per_block; i++) {
    s_fake_backing[base + i] = (ULONG)k_lx_nor_fake_erased;
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief Verify one block is fully erased.
 * @details Scans the validated block span and fails on the first word that no
 * longer matches LevelX's erased sentinel.
 * @param[in] block Block index.
 * @return `LX_SUCCESS` when erased, else `LX_ERROR`.
 * @retval 0 Every word matches the erased pattern.
 * @retval 1 @p block is out of range, or a word is not erased.
 * @pre @p block indexes a real block.
 * @pre The backing is static host storage.
 * @post The backing is unmodified.
 * @post A success result means the block may be programmed.
 * @note The early failure keeps corrupted-block checks bounded by one block.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_block_erased_verify(ULONG block)
{
  if (block >= (ULONG)k_lx_nor_fake_total_blocks) {
    return (UINT)LX_ERROR;
  }
  ULONG base = block * (ULONG)k_lx_nor_fake_words_per_block;
  for (ULONG i = 0U; i < (ULONG)k_lx_nor_fake_words_per_block; i++) {
    if (s_fake_backing[base + i] != (ULONG)k_lx_nor_fake_erased) {
      return (UINT)LX_ERROR;
    }
  }
  return (UINT)LX_SUCCESS;
}

unsigned int lx_nor_fake_ram_init(struct LX_NOR_FLASH_STRUCT* nor_flash)
{
  if (nor_flash == LX_NULL) {
    return (UINT)LX_ERROR;
  }
  nor_flash->lx_nor_flash_base_address               = &s_fake_backing[0];
  nor_flash->lx_nor_flash_total_blocks               = (ULONG)k_lx_nor_fake_total_blocks;
  nor_flash->lx_nor_flash_words_per_block            = (ULONG)k_lx_nor_fake_words_per_block;
  nor_flash->lx_nor_flash_driver_read                = internal_read;
  nor_flash->lx_nor_flash_driver_write               = internal_write;
  nor_flash->lx_nor_flash_driver_block_erase         = internal_block_erase;
  nor_flash->lx_nor_flash_driver_block_erased_verify = internal_block_erased_verify;
  nor_flash->lx_nor_flash_sector_buffer              = &s_fake_sector_buf[0];
  return (UINT)LX_SUCCESS;
}

void lx_nor_fake_ram_wipe(void)
{
  s_fake_fail_writes = 0U;
  for (ULONG i = 0U; i < (ULONG)k_lx_nor_fake_total_words; i++) {
    s_fake_backing[i] = (ULONG)k_lx_nor_fake_erased;
  }
}

void lx_nor_fake_ram_fail_writes(unsigned int count)
{
  s_fake_fail_writes = count;
}
