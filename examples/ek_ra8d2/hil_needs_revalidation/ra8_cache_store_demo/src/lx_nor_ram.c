/**
 * @file lx_nor_ram.c
 * @brief RAM-backed standalone-LevelX NOR driver -- implementation.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Implements a minimal LevelX NOR driver whose media is a static `ULONG` array.
 * The read/write callbacks dereference the `ULONG*` LevelX hands them directly
 * (LevelX derives it from `lx_nor_flash_base_address`, which points at the
 * backing), mirroring the vendored `lx_nor_flash_simulator.c` but first-party
 * and heap-free. No peripheral register is touched, so the emulator and silicon
 * run identical code.
 *
 * ULONG width note: under `LX_STANDALONE_ENABLE`, `ULONG` is `unsigned long`
 * (4 bytes on the arm-none-eabi ILP32 target, 8 bytes on the LP64 test host).
 * The erased pattern is written as `(ULONG)0xFFFFFFFF` (== LevelX's
 * `LX_ALL_ONES`), matching how LevelX compares words against `0xFFFFFFFF`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "lx_nor_ram.h"

#include "lx_api.h"
#include "ra8_attributes.h"

/**
 * @enum lx_nor_ram_const_t
 * @brief Geometry + sentinel constants for the RAM NOR driver.
 * @details 64 blocks * 512 words/block is an ample logical-sector space for the
 *          demo (well over a hundred usable 512-byte sectors on both the 4-byte
 *          target and 8-byte host) while keeping the SRAM backing modest
 *          (128 KiB on the target).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lx_nor_ram_total_blocks    = 64U,         /**< NOR blocks.         */
  k_lx_nor_ram_words_per_block = 512U,        /**< ULONG words/block.  */
  k_lx_nor_ram_total_words     = 64U * 512U,  /**< Backing word count. */
  k_lx_nor_ram_erased          = 0xFFFFFFFFU, /**< LevelX erased word. */
} lx_nor_ram_const_t;

/**
 * @var s_ram_backing
 * @brief The fake NOR media (persists across open/close within one boot).
 * @warning Written only through the driver callbacks and ::lx_nor_ram_wipe.
 * @since 0.1.0
 */
static ULONG s_ram_backing[k_lx_nor_ram_total_words];

/**
 * @var s_ram_sector_buf
 * @brief LevelX per-open sector scratch (one logical sector wide).
 * @warning Owned by LevelX between open and close; not for other use.
 * @since 0.1.0
 */
static ULONG s_ram_sector_buf[LX_NOR_SECTOR_SIZE];

/* Signature is fixed by the LevelX driver-read callback type (non-const). */
/* cppcheck-suppress constParameterCallback -- bound to the LevelX LX_NOR_FLASH read callback typedef; the ULONG* signature is fixed by the driver seam. */
/* NOLINTNEXTLINE(readability-non-const-parameter) -- LevelX driver callback signature is fixed by the vendor seam. */
/**
 * @brief Read @p words ULONGs from the backing pointer into @p destination.
 * @details Copies the requested contiguous range without allocating storage or
 *          changing the emulated NOR contents.
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
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_ram_read(ULONG* flash_address, ULONG* destination, ULONG words)
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

/* Signature is fixed by the LevelX driver-write callback type (non-const). */
/* cppcheck-suppress constParameterCallback -- bound to the LevelX LX_NOR_FLASH write callback typedef; the ULONG* signature is fixed by the driver seam. */
/* NOLINTNEXTLINE(readability-non-const-parameter) -- LevelX driver callback signature is fixed by the vendor seam. */
/**
 * @brief Write @p words ULONGs from @p source to the backing pointer.
 * @details Programs the requested contiguous range directly in the RAM-backed
 *          NOR model without allocating storage or touching other words.
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
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_ram_write(ULONG* flash_address, ULONG* source, ULONG words)
{
  if (flash_address == LX_NULL) {
    return (UINT)LX_ERROR;
  }
  if (source == LX_NULL) {
    return (UINT)LX_ERROR;
  }
  for (ULONG i = 0U; i < words; i++) {
    flash_address[i] = source[i];
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief Erase one block to the LevelX erased pattern.
 * @details Computes the selected block's base word and overwrites exactly one
 *          geometry-defined block with the erased value.
 * @param[in] block       Block index.
 * @param[in] erase_count LevelX erase counter (unused).
 * @return `LX_SUCCESS`, or `LX_ERROR` when @p block is out of range.
 * @retval 0 Block erased.
 * @retval 1 @p block is out of range.
 * @pre @p block indexes a real block.
 * @pre The backing is static storage.
 * @post Every word of the block reads as the erased pattern.
 * @post No other block is touched.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_ram_block_erase(ULONG block, ULONG erase_count)
{
  LX_PARAMETER_NOT_USED(erase_count);
  if (block >= (ULONG)k_lx_nor_ram_total_blocks) {
    return (UINT)LX_ERROR;
  }
  ULONG base = block * (ULONG)k_lx_nor_ram_words_per_block;
  for (ULONG i = 0U; i < (ULONG)k_lx_nor_ram_words_per_block; i++) {
    s_ram_backing[base + i] = (ULONG)k_lx_nor_ram_erased;
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief Verify one block is fully erased.
 * @details Bounds-checks the block index, then scans every word in the block
 *          for the LevelX erased pattern without modifying the backing.
 * @param[in] block Block index.
 * @return `LX_SUCCESS` when erased, else `LX_ERROR`.
 * @retval 0 Every word matches the erased pattern.
 * @retval 1 @p block is out of range, or a word is not erased.
 * @pre @p block indexes a real block.
 * @pre The backing is static storage.
 * @post The backing is unmodified.
 * @post A success result means the block may be programmed.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
RA8_INTERNAL static UINT internal_ram_block_erased_verify(ULONG block)
{
  if (block >= (ULONG)k_lx_nor_ram_total_blocks) {
    return (UINT)LX_ERROR;
  }
  ULONG base = block * (ULONG)k_lx_nor_ram_words_per_block;
  for (ULONG i = 0U; i < (ULONG)k_lx_nor_ram_words_per_block; i++) {
    if (s_ram_backing[base + i] != (ULONG)k_lx_nor_ram_erased) {
      return (UINT)LX_ERROR;
    }
  }
  return (UINT)LX_SUCCESS;
}

unsigned int lx_nor_ram_init(struct LX_NOR_FLASH_STRUCT* nor_flash)
{
  if (nor_flash == LX_NULL) {
    return (UINT)LX_ERROR;
  }
  nor_flash->lx_nor_flash_base_address               = &s_ram_backing[0];
  nor_flash->lx_nor_flash_total_blocks               = (ULONG)k_lx_nor_ram_total_blocks;
  nor_flash->lx_nor_flash_words_per_block            = (ULONG)k_lx_nor_ram_words_per_block;
  nor_flash->lx_nor_flash_driver_read                = internal_ram_read;
  nor_flash->lx_nor_flash_driver_write               = internal_ram_write;
  nor_flash->lx_nor_flash_driver_block_erase         = internal_ram_block_erase;
  nor_flash->lx_nor_flash_driver_block_erased_verify = internal_ram_block_erased_verify;
  nor_flash->lx_nor_flash_sector_buffer              = &s_ram_sector_buf[0];
  return (UINT)LX_SUCCESS;
}

void lx_nor_ram_wipe(void)
{
  for (ULONG i = 0U; i < (ULONG)k_lx_nor_ram_total_words; i++) {
    s_ram_backing[i] = (ULONG)k_lx_nor_ram_erased;
  }
}
