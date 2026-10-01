/**
 * @file examples/ek_ra8d2/hw_pending/cache_store_paged_demo/src/csp_nor.c
 * @brief RAM-backed LevelX NOR model for the cache_store paged demo.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The media model behind ::csp_nor_init: a static ULONG array standing in for
 * a NOR part, plus the four LevelX driver callbacks (read, write, block erase,
 * erased verify) that operate on it. Nothing here touches a peripheral, so the
 * demo's whole store path is deterministic in SRAM.
 *
 * The four callbacks keep internal linkage; only the bind entry point and the
 * wipe helper cross the translation-unit boundary (see csp_nor.h).
 *
 * @author Brighton Sikarskie
 * @date 2026-09-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "csp_nor.h"

#include <stdint.h>

#include "lx_api.h"
#include "ra8_attributes.h"

/**
 * @enum csp_nor_geom_t
 * @brief Geometry and sentinel constants for the RAM-backed NOR model.
 * @details 64 blocks x 512 ULONG words is well over the logical-sector span the
 *          store asks for, while keeping the SRAM backing at 128 KiB on target.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_csp_nor_blocks        = 64U,         /**< NOR blocks.         */
  k_csp_nor_words_per_blk = 512U,        /**< ULONG words/block.  */
  k_csp_nor_total_words   = 64U * 512U,  /**< Backing word count. */
  k_csp_nor_erased        = 0xFFFFFFFFU, /**< LevelX erased word. */
} csp_nor_geom_t;

/** @brief The fake NOR media; survives the simulated power loss. */
static ULONG s_nor_backing[k_csp_nor_total_words];
/** @brief LevelX per-open sector scratch (one logical sector wide). */
static ULONG s_nor_sector_buf[LX_NOR_SECTOR_SIZE];

/**
 * @brief Read @p words ULONGs from the backing pointer into @p destination.
 * @details Copies the requested contiguous range without allocating storage or
 *          disturbing the emulated NOR contents.
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
/* cppcheck-suppress constParameterCallback -- bound to the LevelX read callback typedef; the ULONG* signature is fixed by the driver seam. */
/* NOLINTNEXTLINE(readability-non-const-parameter) -- LevelX driver callback signature is fixed by the vendor seam. */
RA8_INTERNAL static UINT
internal_csp_nor_read(ULONG* flash_address, ULONG* destination, ULONG words)
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
 * @details Programs the requested contiguous range in the RAM-backed NOR model
 *          without allocating storage or touching neighbouring words.
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
/* cppcheck-suppress constParameterCallback -- bound to the LevelX write callback typedef; the ULONG* signature is fixed by the driver seam. */
/* NOLINTNEXTLINE(readability-non-const-parameter) -- LevelX driver callback signature is fixed by the vendor seam. */
RA8_INTERNAL static UINT internal_csp_nor_write(ULONG* flash_address, ULONG* source, ULONG words)
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
 * @details Computes the block base word and overwrites exactly one
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
RA8_INTERNAL static UINT internal_csp_nor_block_erase(ULONG block, ULONG erase_count)
{
  LX_PARAMETER_NOT_USED(erase_count);
  if (block >= (ULONG)k_csp_nor_blocks) {
    return (UINT)LX_ERROR;
  }
  ULONG base = block * (ULONG)k_csp_nor_words_per_blk;
  for (ULONG i = 0U; i < (ULONG)k_csp_nor_words_per_blk; i++) {
    s_nor_backing[base + i] = (ULONG)k_csp_nor_erased;
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief Verify one block reads as fully erased.
 * @details Bounds-checks the block index, then scans every word in the block for
 *          the LevelX erased pattern without modifying the backing.
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
RA8_INTERNAL static UINT internal_csp_nor_block_erased_verify(ULONG block)
{
  if (block >= (ULONG)k_csp_nor_blocks) {
    return (UINT)LX_ERROR;
  }
  ULONG base = block * (ULONG)k_csp_nor_words_per_blk;
  for (ULONG i = 0U; i < (ULONG)k_csp_nor_words_per_blk; i++) {
    if (s_nor_backing[base + i] != (ULONG)k_csp_nor_erased) {
      return (UINT)LX_ERROR;
    }
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief LevelX driver-initialise callback over the static SRAM backing.
 * @details Signature-compatible with ::ra8_cache_store_nor_init_fn: programs the
 *          geometry, the four driver callbacks, and the per-open sector buffer
 *          against the shared backing array.
 * @param[in,out] nor_flash LevelX control block to populate.
 * @return `LX_SUCCESS`, or `LX_ERROR` when @p nor_flash is NULL.
 * @retval 0 Control block programmed.
 * @retval 1 @p nor_flash was NULL.
 * @pre @p nor_flash points at caller-owned LevelX control-block storage.
 * @pre The static backing array is available (static storage; always).
 * @post On success the geometry and all four callbacks are set.
 * @post The backing bytes are left as they were (format erases, open does not).
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
unsigned int csp_nor_init(struct LX_NOR_FLASH_STRUCT* nor_flash)
{
  if (nor_flash == LX_NULL) {
    return (UINT)LX_ERROR;
  }
  nor_flash->lx_nor_flash_base_address               = &s_nor_backing[0];
  nor_flash->lx_nor_flash_total_blocks               = (ULONG)k_csp_nor_blocks;
  nor_flash->lx_nor_flash_words_per_block            = (ULONG)k_csp_nor_words_per_blk;
  nor_flash->lx_nor_flash_driver_read                = internal_csp_nor_read;
  nor_flash->lx_nor_flash_driver_write               = internal_csp_nor_write;
  nor_flash->lx_nor_flash_driver_block_erase         = internal_csp_nor_block_erase;
  nor_flash->lx_nor_flash_driver_block_erased_verify = internal_csp_nor_block_erased_verify;
  nor_flash->lx_nor_flash_sector_buffer              = &s_nor_sector_buf[0];
  return (UINT)LX_SUCCESS;
}

/**
 * @brief Blank the whole SRAM backing to the erased pattern.
 * @details Puts the fake media in the state a never-written NOR part would be
 *          in, so the first mount sees a blank device regardless of BSS content.
 * @return Nothing.
 * @pre The backing is static storage (always available).
 * @pre No LevelX operation is mid-flight against the backing.
 * @post Every backing word reads as the erased pattern.
 * @post A subsequent open sees a blank (unformatted) device.
 * @note Not thread-safe; called once before the first mount.
 * @since 0.1.0
 */
void csp_nor_wipe(void)
{
  for (ULONG i = 0U; i < (ULONG)k_csp_nor_total_words; i++) {
    s_nor_backing[i] = (ULONG)k_csp_nor_erased;
  }
}
