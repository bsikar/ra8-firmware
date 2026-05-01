/**
 * @file port/levelx/lx_nor_driver_ra_xspi.c
 * @brief LevelX NOR driver implementation backed by the RA8D2 ``ra_xspi`` HAL
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Implements ``lx_nor_driver_ra_xspi_initialize`` and the four LevelX
 * NOR driver callbacks (read / write / block_erase / block_erased_verify)
 * by translating each LevelX ``ULONG``-word request into a matching
 * byte-oriented call into ``libs/ra_hal/inc/ra_xspi.h`` running against
 * the on-board EK-RA8D2 Macronix MX25LM512 octal-SPI flash chip.
 *
 * Address translation:
 *
 * LevelX hands the read / write callbacks a ``ULONG *flash_address``
 * which is a pointer into ``lx_nor_flash_base_address``. We pick a
 * non-zero synthetic base (``k_ra_lx_nor_base``) so that LevelX can
 * use the pointer as an opaque cookie, then convert back to a flash
 * byte offset via ``(addr - base) * sizeof(ULONG)`` in the read /
 * write paths and ``block * 4 KiB`` in the block-erase path.
 *
 * Sector / block geometry:
 *
 * - LevelX physical sector size = ``LX_NOR_SECTOR_SIZE * sizeof(ULONG)``
 *   = 512 bytes (LevelX default).
 * - LevelX block size = MX25LM512 erase-sector size = 4 KiB.
 * - Words per block = 4096 / sizeof(ULONG) = 1024.
 * - We expose ``k_ra_lx_nor_total_blocks`` blocks (256 KiB total) -- the
 *   first 256 KiB of the 64 MiB flash is reserved for LevelX's
 *   wear-levelled partition. The remainder of the chip stays untouched
 *   for XIP code or other firmware uses.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "lx_nor_driver_ra_xspi.h"

#include <stdint.h>

#include "lx_api.h"
#include "ra_err.h"
#include "ra_xspi.h"

/**
 * @brief Compile-time constants for the xSPI <-> LevelX bridge.
 */
typedef enum : uint32_t {
  /** @brief xSPI instance routed to the EK-RA8D2 MX25LM512 flash chip. */
  k_ra_lx_xspi_instance = 0U,

  /** @brief Number of LevelX blocks exposed to wear-levelling.
   *
   *  64 blocks * 4 KiB/block = 256 KiB partition. Sized to be small
   *  enough that ``lx_nor_flash_format()`` finishes quickly during the
   *  demo and large enough that ``LX_NOR_SECTOR_MAPPING_CACHE_SIZE``
   *  (16) does not dominate.
   */
  k_ra_lx_nor_total_blocks = 64U,

  /** @brief MX25LM512 erase-sector size (opcode 0x20 / sector erase). */
  k_ra_lx_nor_block_bytes = 4096U,

  /** @brief LevelX words per block (= block_bytes / sizeof(ULONG)). */
  k_ra_lx_nor_words_per_block = 1024U,

  /** @brief Bytes per ULONG word (LevelX uses 4-byte ULONGs on this target). */
  k_ra_lx_nor_word_bytes = 4U,
} ra_lx_nor_constants_t;

/**
 * @brief Synthetic flash base address handed to LevelX.
 *
 * @details
 * LevelX treats ``lx_nor_flash_base_address`` as an opaque pointer
 * cookie -- it adds offsets to it in word units when computing the
 * ``flash_address`` argument to the driver callbacks, but never
 * dereferences it directly. We therefore pick an arbitrary non-NULL
 * value (the first 256 KiB of the chip is the partition we reserve
 * for LevelX) and recover the byte offset inside each callback via
 * ``(addr - base) * sizeof(ULONG)``.
 */
typedef enum : uintptr_t {
  /** @brief Synthetic base for LevelX-managed window inside the flash. */
  k_ra_lx_nor_base_addr = 0x10000000U,
} ra_lx_nor_base_t;

/**
 * @brief Per-driver scratch buffer LevelX uses for sector mapping reads.
 *
 * @details
 * LevelX requires ``lx_nor_flash_sector_buffer`` to point to RAM at
 * least ``LX_NOR_SECTOR_SIZE * sizeof(ULONG)`` (512 bytes) wide. We
 * give it a 4-byte-aligned static buffer so it lines up with the
 * ULONG word stride LevelX expects.
 *
 * @note Static-storage; not thread-safe but LevelX serialises driver
 *       calls itself.
 *
 * @since 0.1.0
 */
static ULONG s_ra_lx_nor_sector_buffer[LX_NOR_SECTOR_SIZE];

/**
 * @brief Convert a LevelX ``ULONG*`` flash pointer to a byte offset.
 *
 * @param[in] flash_address LevelX-supplied pointer into the flash window.
 *
 * @return Byte offset from the start of the LevelX-managed partition.
 *
 * @pre ``flash_address != NULL``.
 * @post Returned offset is < total partition size in bytes.
 *
 * @note Pure helper, no side effects.
 *
 * @since 0.1.0
 */
static uint32_t priv_flash_offset_bytes(ULONG* flash_address)
{
  /* LevelX adds a number-of-ULONGs offset onto base_address and hands
   * the result back to us. Recover the byte offset by subtracting the
   * base in pointer-difference units (each unit is sizeof(ULONG) = 4
   * bytes on this target). */
  uintptr_t addr = (uintptr_t)flash_address;
  uintptr_t base = k_ra_lx_nor_base_addr;
  return (uint32_t)(addr - base);
}

/**
 * @brief LevelX read callback: copy ``words`` ULONGs from flash to RAM.
 *
 * @details
 * Translates the word-oriented LevelX request into a single byte-wise
 * call to ``ra_xspi_flash_read``. ``ra_xspi_flash_read`` caps each
 * call at ``k_ra_xspi_max_xfer`` (4096 bytes); LevelX never asks for
 * more than one block (4 KiB) per callback, so the cap is never
 * exceeded.
 *
 * @param[in]  flash_address Source address (LevelX cookie).
 * @param[out] destination   Destination RAM buffer.
 * @param[in]  words         Number of ULONG words to copy.
 *
 * @return ``LX_SUCCESS`` on success, ``LX_ERROR`` on xSPI failure.
 *
 * @pre ``flash_address != NULL`` and ``destination != NULL``.
 * @pre ``words > 0``.
 *
 * @post On success the destination buffer holds ``words * 4`` bytes
 *       of flash data.
 *
 * @note Not thread-safe; LevelX serialises driver calls.
 *
 * @since 0.1.0
 */
static UINT priv_nor_read(ULONG* flash_address, ULONG* destination, ULONG words)
{
  if ((flash_address == LX_NULL) || (destination == LX_NULL) || (words == 0U)) {
    return (UINT)LX_ERROR;
  }
  uint32_t offset = priv_flash_offset_bytes(flash_address);
  uint32_t bytes  = (uint32_t)words * k_ra_lx_nor_word_bytes;
  ra_err_t err =
    ra_xspi_flash_read((uint8_t)k_ra_lx_xspi_instance, offset, (uint8_t*)destination, bytes);
  if (err != k_ra_ok) {
    return (UINT)LX_ERROR;
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief LevelX write callback: program ``words`` ULONGs from RAM to flash.
 *
 * @details
 * Forwards the word-oriented LevelX request to
 * ``ra_xspi_flash_program``. The HAL handles the JEDEC
 * write-enable -> page-program -> WIP-poll dance internally and
 * caps each call at one full erase sector (4 KiB), which matches the
 * largest single LevelX call. ``ra_xspi_flash_program`` does NOT
 * cross page boundaries internally, so the caller (LevelX) is
 * implicitly responsible for staying within one 256-byte page.
 * LevelX's metadata writes are always 4-byte ULONGs; bulk sector
 * writes are at most one ``LX_NOR_SECTOR_SIZE * sizeof(ULONG)`` =
 * 512 bytes which fits in two pages.
 *
 * @param[in] flash_address Destination address (LevelX cookie).
 * @param[in] source        Source RAM buffer.
 * @param[in] words         Number of ULONG words to program.
 *
 * @return ``LX_SUCCESS`` on success, ``LX_ERROR`` on xSPI failure.
 *
 * @pre ``flash_address != NULL`` and ``source != NULL``.
 * @pre ``words > 0``.
 *
 * @post On success the targeted byte range has been programmed.
 *
 * @note Not thread-safe; LevelX serialises driver calls.
 *
 * @since 0.1.0
 */
static UINT priv_nor_write(ULONG* flash_address, ULONG* source, ULONG words)
{
  if ((flash_address == LX_NULL) || (source == LX_NULL) || (words == 0U)) {
    return (UINT)LX_ERROR;
  }
  uint32_t offset = priv_flash_offset_bytes(flash_address);
  uint32_t bytes  = (uint32_t)words * k_ra_lx_nor_word_bytes;
  ra_err_t err =
    ra_xspi_flash_program((uint8_t)k_ra_lx_xspi_instance, offset, (const uint8_t*)source, bytes);
  if (err != k_ra_ok) {
    return (UINT)LX_ERROR;
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief LevelX block-erase callback: erase one 4 KiB sector by index.
 *
 * @details
 * LevelX numbers blocks 0..(``lx_nor_flash_total_blocks`` - 1). We
 * convert the block index to a flash byte offset by multiplying by
 * the MX25LM512 sector size (4 KiB) and call ``ra_xspi_flash_erase_sector``.
 * The HAL handles WREN -> SE -> WIP-poll internally. The
 * ``erase_count`` argument is ignored -- LevelX only uses it for its
 * own bookkeeping in higher layers.
 *
 * @param[in] block       Block index (0..``k_ra_lx_nor_total_blocks - 1``).
 * @param[in] erase_count LevelX-side erase counter (unused here).
 *
 * @return ``LX_SUCCESS`` on success, ``LX_ERROR`` on xSPI failure.
 *
 * @pre ``block < k_ra_lx_nor_total_blocks``.
 *
 * @post On success the targeted 4 KiB sector reads as 0xFF on the
 *       next call.
 *
 * @note Not thread-safe; LevelX serialises driver calls.
 *
 * @since 0.1.0
 */
static UINT priv_nor_block_erase(ULONG block, ULONG erase_count)
{
  LX_PARAMETER_NOT_USED(erase_count);
  if (block >= (ULONG)k_ra_lx_nor_total_blocks) {
    return (UINT)LX_ERROR;
  }
  uint32_t offset = (uint32_t)block * k_ra_lx_nor_block_bytes;
  ra_err_t err    = ra_xspi_flash_erase_sector((uint8_t)k_ra_lx_xspi_instance, offset);
  if (err != k_ra_ok) {
    return (UINT)LX_ERROR;
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief LevelX block-erased-verify callback: confirm a sector is 0xFF.
 *
 * @details
 * Reads the block back through ``ra_xspi_flash_read`` in
 * ``k_ra_xspi_max_xfer`` chunks (one full sector fits in a single
 * 4 KiB transfer) into the per-driver scratch buffer and checks every
 * ULONG word equals ``LX_ALL_ONES`` (0xFFFFFFFF). Returns
 * ``LX_SUCCESS`` only if every word verifies erased.
 *
 * @param[in] block Block index to verify.
 *
 * @return ``LX_SUCCESS`` if the block is fully erased, otherwise
 *         ``LX_ERROR``.
 *
 * @pre ``block < k_ra_lx_nor_total_blocks``.
 *
 * @post On success the entire block reads as 0xFFFFFFFF.
 *
 * @note Not thread-safe; LevelX serialises driver calls.
 *
 * @since 0.1.0
 */
static UINT priv_nor_block_erased_verify(ULONG block)
{
  if (block >= (ULONG)k_ra_lx_nor_total_blocks) {
    return (UINT)LX_ERROR;
  }
  uint32_t offset       = (uint32_t)block * k_ra_lx_nor_block_bytes;
  uint32_t remain_words = k_ra_lx_nor_words_per_block;
  while (remain_words > 0U) {
    uint32_t chunk_words = remain_words;
    if (chunk_words > (uint32_t)LX_NOR_SECTOR_SIZE) {
      chunk_words = (uint32_t)LX_NOR_SECTOR_SIZE;
    }
    uint32_t chunk_bytes = chunk_words * k_ra_lx_nor_word_bytes;
    ra_err_t err         = ra_xspi_flash_read((uint8_t)k_ra_lx_xspi_instance,
                                              offset,
                                              (uint8_t*)s_ra_lx_nor_sector_buffer,
                                              chunk_bytes);
    if (err != k_ra_ok) {
      return (UINT)LX_ERROR;
    }
    for (uint32_t i = 0U; i < chunk_words; i++) {
      if (s_ra_lx_nor_sector_buffer[i] != (ULONG)LX_ALL_ONES) {
        return (UINT)LX_ERROR;
      }
    }
    offset += chunk_bytes;
    remain_words -= chunk_words;
  }
  return (UINT)LX_SUCCESS;
}

UINT lx_nor_driver_ra_xspi_initialize(LX_NOR_FLASH* nor_flash)
{
  if (nor_flash == LX_NULL) {
    return (UINT)LX_ERROR;
  }

  /* Programme the synthetic base + geometry. LevelX uses the base
   * pointer as a cookie when calling back into our read / write
   * callbacks; we recover the byte offset from it inside
   * priv_flash_offset_bytes(). */
  nor_flash->lx_nor_flash_base_address    = (ULONG*)(uintptr_t)k_ra_lx_nor_base_addr;
  nor_flash->lx_nor_flash_total_blocks    = (ULONG)k_ra_lx_nor_total_blocks;
  nor_flash->lx_nor_flash_words_per_block = (ULONG)k_ra_lx_nor_words_per_block;

  /* Wire up the four LevelX driver callbacks. */
  nor_flash->lx_nor_flash_driver_read                = priv_nor_read;
  nor_flash->lx_nor_flash_driver_write               = priv_nor_write;
  nor_flash->lx_nor_flash_driver_block_erase         = priv_nor_block_erase;
  nor_flash->lx_nor_flash_driver_block_erased_verify = priv_nor_block_erased_verify;

  /* Hand LevelX a 512-byte ULONG-aligned RAM scratch buffer for its
   * sector-mapping reads. */
  nor_flash->lx_nor_flash_sector_buffer = &s_ra_lx_nor_sector_buffer[0];

  return (UINT)LX_SUCCESS;
}
