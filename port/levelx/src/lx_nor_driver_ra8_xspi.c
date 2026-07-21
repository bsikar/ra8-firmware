/**
 * @file port/levelx/src/lx_nor_driver_ra8_xspi.c
 * @brief LevelX NOR driver implementation backed by the RA8 ``ra8_xspi`` HAL
 *
 * @par Tag
 * [Ring 4 / PORT] {World: NS}
 *
 * @details
 * Implements ``lx_nor_driver_ra8_xspi_initialize`` and the four LevelX
 * NOR driver callbacks (read / write / block_erase / block_erased_verify)
 * by translating each LevelX ``ULONG``-word request into a matching
 * byte-oriented call into ``libs/ra8_hal/inc/ra8_xspi.h`` running against
 * the on-board EK-RA8D2 ISSI IS25LX512M octal-SPI flash chip.
 *
 * Address translation:
 *
 * LevelX hands the read / write callbacks a ``ULONG *flash_address``
 * which is a pointer into ``lx_nor_flash_base_address``. We pick a
 * non-zero synthetic base (``k_ra8_lx_nor_base``) so that LevelX can
 * use the pointer as an opaque cookie, then convert back to a flash
 * byte offset via ``(addr - base) * sizeof(ULONG)`` in the read /
 * write paths and ``block * 4 KiB`` in the block-erase path.
 *
 * Sector / block geometry:
 *
 * - LevelX physical sector size = ``LX_NOR_SECTOR_SIZE * sizeof(ULONG)``
 *   = 512 bytes (LevelX default).
 * - LevelX block size = IS25LX512M erase-sector size = 4 KiB.
 * - Words per block = 4096 / sizeof(ULONG) = 1024.
 * - We expose ``k_ra8_lx_nor_total_blocks`` blocks (256 KiB total) -- the
 *   first 256 KiB of the 64 MiB flash is reserved for LevelX's
 *   wear-levelled partition. The remainder of the chip stays untouched
 *   for XIP code or other firmware uses.
 *
 * Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "lx_nor_driver_ra8_xspi.h"

#include <stdint.h>

#include "lx_api.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_pfs_regs.h"
#include "ra8_port_constants.h"
#include "ra8_time.h"
#include "ra8_xspi.h"

/** @brief Logging tag for the LevelX <-> ra8_xspi bridge. */
static const char* s_lx_xspi_tag = "LX_XSPI";

/**
 * @enum ra8_lx_jedec_id_t
 * @brief Expected JEDEC ID triplet for the EK-RA8D2 IS25LX512M-JHLE chip.
 *
 * @details
 * The IS25LX512M datasheet Ch 8.13 "Read Identification (RDID)" lists:
 *   - manufacturer ID = 0x9D (ISSI)
 *   - memory type    = 0x5A (IS25LX family)
 *   - capacity code  = 0x1A (512 Mbit)
 * The ``ra8_xspi_flash_read_id`` helper packs these as
 * ``(mfr << 16) | (type << 8) | cap``, giving 0x9D5A1A. We assert on
 * the packed value during bring-up and surface the actual value over
 * the log so a wrong-chip / wrong-protocol bring-up is loud rather
 * than a silent ``LX_ERROR`` from ``lx_nor_flash_format``.
 */
typedef enum : uint32_t {
  k_ra8_lx_jedec_mfr      = 0x9DU,      /**< ISSI manufacturer ID.    */
  k_ra8_lx_jedec_type     = 0x5AU,      /**< IS25LX memory-type code. */
  k_ra8_lx_jedec_capacity = 0x1AU,      /**< 512 Mbit capacity code.  */
  k_ra8_lx_jedec_expected = 0x9D5A1AUL, /**< Packed RDID expected.    */
} ra8_lx_jedec_id_t;

/**
 * @brief Compile-time constants for the xSPI <-> LevelX bridge.
 */
typedef enum : uint32_t {
  /** @brief xSPI instance routed to the EK-RA8D2 IS25LX512M flash chip. */
  k_ra8_lx_xspi_instance = 0U,

  /** @brief Number of LevelX blocks exposed to wear-levelling.
   *
   *  64 blocks * 4 KiB/block = 256 KiB partition. Sized to be small
   *  enough that ``lx_nor_flash_format()`` finishes quickly during the
   *  demo and large enough that ``LX_NOR_SECTOR_MAPPING_CACHE_SIZE``
   *  (16) does not dominate.
   */
  k_ra8_lx_nor_total_blocks = 64U,

  /** @brief IS25LX512M erase-sector size (opcode 0x20 / sector erase). */
  k_ra8_lx_nor_block_bytes = 4096U,

  /** @brief LevelX words per block (= block_bytes / sizeof(ULONG)). */
  k_ra8_lx_nor_words_per_block = 1024U,

  /** @brief Bytes per ULONG word (LevelX uses 4-byte ULONGs on this target). */
  k_ra8_lx_nor_word_bytes = 4U,
} ra8_lx_nor_constants_t;

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
  k_ra8_lx_nor_base_addr = 0x10000000U,
} ra8_lx_nor_base_t;

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
static ULONG s_ra8_lx_nor_sector_buffer[LX_NOR_SECTOR_SIZE];

/* Convert a LevelX ``ULONG*`` flash pointer to a byte offset -- see implementation for details. */
static uint32_t priv_flash_offset_bytes(ULONG* flash_address)
{
  /* LevelX adds a number-of-ULONGs offset onto base_address and hands
   * the result back to us. Recover the byte offset by subtracting the
   * base in pointer-difference units (each unit is sizeof(ULONG) = 4
   * bytes on this target). */
  uintptr_t addr = (uintptr_t)flash_address;
  uintptr_t base = k_ra8_lx_nor_base_addr;
  return (uint32_t)(addr - base);
}

/**
 * @brief LevelX read callback: copy ``words`` ULONGs from flash to RAM.
 *
 * @details
 * Translates the word-oriented LevelX request into a single byte-wise
 * call to ``ra8_xspi_flash_read``. ``ra8_xspi_flash_read`` caps each
 * call at ``k_ra8_xspi_max_xfer`` (4096 bytes); LevelX never asks for
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
 *
 * @retval 0 Success or default value.
 * @post Side effects bounded to documented state.
 */
static UINT priv_nor_read(ULONG* flash_address, ULONG* destination, ULONG words)
{
  if ((flash_address == LX_NULL) || (destination == LX_NULL) || (words == 0U)) {
    return (UINT)LX_ERROR;
  }
  uint32_t  offset = priv_flash_offset_bytes(flash_address);
  uint32_t  bytes  = (uint32_t)words * k_ra8_lx_nor_word_bytes;
  ra8_err_t err =
    ra8_xspi_flash_read((uint8_t)k_ra8_lx_xspi_instance, offset, (uint8_t*)destination, bytes);
  if (err != k_ra8_ok) {
    return (UINT)LX_ERROR;
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief LevelX write callback: program ``words`` ULONGs from RAM to flash.
 *
 * @details
 * Forwards the word-oriented LevelX request to
 * ``ra8_xspi_flash_program``. The HAL handles the JEDEC
 * write-enable -> page-program -> WIP-poll dance internally and
 * chunks arbitrary lengths into per-page (256-byte) manual-command
 * transfers, so a full ``LX_NOR_SECTOR_SIZE * sizeof(ULONG)`` =
 * 512-byte sector write (two pages) round-trips intact; LevelX's
 * 4-byte metadata writes are a single chunk.
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
 *
 * @retval 0 Success or default value.
 * @post Side effects bounded to documented state.
 */
static UINT priv_nor_write(ULONG* flash_address, ULONG* source, ULONG words)
{
  if ((flash_address == LX_NULL) || (source == LX_NULL) || (words == 0U)) {
    return (UINT)LX_ERROR;
  }
  uint32_t  offset = priv_flash_offset_bytes(flash_address);
  uint32_t  bytes  = (uint32_t)words * k_ra8_lx_nor_word_bytes;
  ra8_err_t err =
    ra8_xspi_flash_program((uint8_t)k_ra8_lx_xspi_instance, offset, (const uint8_t*)source, bytes);
  if (err != k_ra8_ok) {
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
 * the IS25LX512M sector size (4 KiB) and call ``ra8_xspi_flash_erase_sector``.
 * The HAL handles WREN -> SE -> WIP-poll internally. The
 * ``erase_count`` argument is ignored -- LevelX only uses it for its
 * own bookkeeping in higher layers.
 *
 * @param[in] block       Block index (0..``k_ra8_lx_nor_total_blocks - 1``).
 * @param[in] erase_count LevelX-side erase counter (unused here).
 *
 * @return ``LX_SUCCESS`` on success, ``LX_ERROR`` on xSPI failure.
 *
 * @pre ``block < k_ra8_lx_nor_total_blocks``.
 *
 * @post On success the targeted 4 KiB sector reads as 0xFF on the
 *       next call.
 *
 * @note Not thread-safe; LevelX serialises driver calls.
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
static UINT priv_nor_block_erase(ULONG block, ULONG erase_count)
{
  LX_PARAMETER_NOT_USED(erase_count);
  if (block >= (ULONG)k_ra8_lx_nor_total_blocks) {
    return (UINT)LX_ERROR;
  }
  uint32_t  offset = (uint32_t)block * k_ra8_lx_nor_block_bytes;
  ra8_err_t err    = ra8_xspi_flash_erase_sector((uint8_t)k_ra8_lx_xspi_instance, offset);
  if (err != k_ra8_ok) {
    return (UINT)LX_ERROR;
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief LevelX block-erased-verify callback: confirm a sector is 0xFF.
 *
 * @details
 * Reads the block back through ``ra8_xspi_flash_read`` in
 * ``k_ra8_xspi_max_xfer`` chunks (one full sector fits in a single
 * 4 KiB transfer) into the per-driver scratch buffer and checks every
 * ULONG word equals ``LX_ALL_ONES`` (0xFFFFFFFF). Returns
 * ``LX_SUCCESS`` only if every word verifies erased.
 *
 * @param[in] block Block index to verify.
 *
 * @return ``LX_SUCCESS`` if the block is fully erased, otherwise
 *         ``LX_ERROR``.
 *
 * @pre ``block < k_ra8_lx_nor_total_blocks``.
 *
 * @post On success the entire block reads as 0xFFFFFFFF.
 *
 * @note Not thread-safe; LevelX serialises driver calls.
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
static UINT priv_nor_block_erased_verify(ULONG block)
{
  if (block >= (ULONG)k_ra8_lx_nor_total_blocks) {
    return (UINT)LX_ERROR;
  }
  uint32_t offset       = (uint32_t)block * k_ra8_lx_nor_block_bytes;
  uint32_t remain_words = k_ra8_lx_nor_words_per_block;
  while (remain_words > 0U) {
    uint32_t chunk_words = remain_words;
    if (chunk_words > (uint32_t)LX_NOR_SECTOR_SIZE) {
      chunk_words = (uint32_t)LX_NOR_SECTOR_SIZE;
    }
    uint32_t  chunk_bytes = chunk_words * k_ra8_lx_nor_word_bytes;
    ra8_err_t err         = ra8_xspi_flash_read((uint8_t)k_ra8_lx_xspi_instance,
                                                offset,
                                                (uint8_t*)s_ra8_lx_nor_sector_buffer,
                                                chunk_bytes);
    if (err != k_ra8_ok) {
      return (UINT)LX_ERROR;
    }
    for (uint32_t i = 0U; i < chunk_words; i++) {
      if (s_ra8_lx_nor_sector_buffer[i] != (ULONG)LX_ALL_ONES) {
        return (UINT)LX_ERROR;
      }
    }
    offset += chunk_bytes;
    remain_words -= chunk_words;
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @struct ra8_xspi_rdid_observation_t
 * @brief JLink-readable snapshot of the most recent RDID probe.
 *
 * @details
 * UART on the EK-RA8D2 will not reliably drain ``ra8_log_*`` output
 * before ``demo_panic_halt`` fires inside ``lx_nor_flash_format``,
 * so the operator cannot see the IS25LX512M JEDEC triplet via the
 * console. Stash the result in a known SRAM word that the developer
 * can read with ``mem32 <addr> 1`` from JLinkExe / Ozone after the
 * panic halt.
 *
 * Layout (little-endian on Cortex-M85):
 *   - ``magic``    -- ``k_ra8_xspi_rdid_magic`` once any probe has run
 *                     (``0`` on cold boot before the global has been
 *                     touched, so the operator can tell stale-RAM
 *                     values from a real reading).
 *   - ``call_count`` -- number of times ``priv_bus_init_once`` has
 *                       reached the RDID probe step. Bumped before
 *                       the call so even a hang inside the HAL leaves
 *                       a visible breadcrumb.
 *   - ``rid_err``  -- ``ra8_err_t`` returned by ``ra8_xspi_flash_read_id``
 *                     (cast to ``uint32_t``). ``0`` (= ``k_ra8_ok``)
 *                     means the call returned cleanly.
 *   - ``jedec_id`` -- packed ``(mfr << 16) | (type << 8) | capacity``
 *                     from the HAL. Expected ``0x009D5A1A`` for the
 *                     IS25LX512M-JHLE on EK-RA8D2 v1.
 *
 * @invariant ``magic`` is either ``0`` (untouched) or
 *            ``k_ra8_xspi_rdid_magic``; any other value indicates SRAM
 *            corruption / wrong address.
 *
 * @see g_ra8_xspi_rdid_observed
 *
 * @since 0.1.0
 */
typedef struct {
  uint32_t magic;        /**< ``k_ra8_xspi_rdid_magic`` once written.        */
  uint32_t call_count;   /**< Number of probe attempts since reset.          */
  uint32_t rid_err;      /**< Last ``ra8_err_t`` from ``read_id`` (uint).    */
  uint32_t jedec_id;     /**< Last raw RDID triplet from the HAL.            */
  uint32_t reset_8d_err; /**< ``ra8_err_t`` from 8D-form software reset.     */
  uint32_t reset_1s_err; /**< ``ra8_err_t`` from 1S-form software reset.     */
  uint32_t stage;        /**< Last stage reached (see ``ra8_xspi_stage_t``). */
  uint32_t reserved;     /**< Reserved for future diagnostics.               */
} ra8_xspi_rdid_observation_t;

/**
 * @enum ra8_xspi_stage_t
 * @brief Bring-up stage markers stamped into ``g_ra8_xspi_rdid_observed.stage``.
 *
 * @details
 * Lets a JLink operator tell at a glance how far the bring-up made it
 * before either succeeding (``k_ra8_xspi_stage_done``) or returning an
 * error.
 */
typedef enum : uint32_t {
  k_ra8_xspi_stage_pins     = 1U, /**< Pin routing in progress. */
  k_ra8_xspi_stage_init_8d  = 2U, /**< xSPI init in 8D mode.    */
  k_ra8_xspi_stage_reset_8d = 3U, /**< Software reset in 8D.    */
  k_ra8_xspi_stage_init_1s  = 4U, /**< xSPI init in 1S mode.    */
  k_ra8_xspi_stage_reset_1s = 5U, /**< Software reset in 1S.    */
  k_ra8_xspi_stage_rdid     = 6U, /**< RDID probe in flight.    */
  k_ra8_xspi_stage_done     = 7U, /**< Bring-up succeeded.      */
} ra8_xspi_stage_t;

/**
 * @enum ra8_xspi_rdid_magic_t
 * @brief Sentinel value distinguishing a written observation from
 *        cold-boot zero / stale RAM.
 *
 * @details
 * Picked to be ASCII ``"RDID"`` (little-endian read order) so the
 * operator can recognise the field in a ``mem32`` dump at a glance.
 */
typedef enum : uint32_t {
  k_ra8_xspi_rdid_magic = 0x44494452UL, /**< 'R','D','I','D' little-endian. */
} ra8_xspi_rdid_magic_t;

/**
 * @var g_ra8_xspi_rdid_observed
 * @brief JLink-readable observation of the last RDID probe.
 *
 * @details
 * Lives in normal ``.bss`` (zero-initialized by the C runtime), so on
 * cold boot the ``magic`` field reads ``0`` and the operator knows
 * the global has not yet been written. Updated unconditionally inside
 * ``priv_bus_init_once`` *before* any error-return path so that even
 * an immediate ``ra8_log_error`` -> ``demo_panic_halt`` sequence
 * leaves the actual JEDEC triplet visible in SRAM.
 *
 * Look up its address with::
 *
 *     arm-none-eabi-nm build/.../<app>.elf | grep g_ra8_xspi_rdid_observed
 *
 * Then in JLinkExe / Ozone::
 *
 *     mem32 <addr> 4
 *
 * to dump ``{magic, call_count, rid_err, jedec_id}``.
 *
 * @note Not ``static`` -- exported deliberately so external tools
 *       (``arm-none-eabi-nm``, JLink ELF symbol view) can locate it.
 * @warning Direct modification by other modules is forbidden; only
 *          ``priv_bus_init_once`` writes this global.
 *
 * @since 0.1.0
 */
ra8_xspi_rdid_observation_t g_ra8_xspi_rdid_observed;

/**
 * @enum ra8_xspi_pin_count_t
 * @brief Number of OCTA bus pin slots in ``g_ra8_xspi_pin_observed``.
 *
 * @details
 * 11 active rows (CS, CK, DQS, DQ0..DQ7) plus one reserved sentinel
 * row so the array size matches the comment in
 * ``ra8_board_ek_ra8d2.c::s_xspi_octa_pins``. RESET_L is excluded --
 * it is plain GPIO, not a peripheral function.
 */
typedef enum : uint8_t {
  k_ra8_xspi_pin_count = 12U, /**< CS + CK + DQS + DQ0..DQ7 + 1 spare. */
} ra8_xspi_pin_count_t;

/**
 * @struct ra8_xspi_pin_observation_t
 * @brief One row per OCTA pin captured AFTER ``ra8_board_xspi_pins_init``.
 *
 * @details
 * Stores both the (port, pin) pair the BSP claimed it routed and
 * the actual PFS readback. If ``psel_observed`` != ``k_ra8_psel_qspi``
 * (0x1C) or ``pmr_observed`` != 1 the BSP enum is wrong for that pin
 * and the chip will not see CS / CLK / DQ asserted on the right pad,
 * which is the most likely root cause for the all-ones RDID readout
 * documented in HARDWARE_BRINGUP.md.
 *
 * @invariant ``port <= k_ra8_port_max`` or 0xFF sentinel.
 *
 * @see g_ra8_xspi_pin_observed
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t  port;          /**< Port index from ``ra8_port_t``. */
  uint8_t  pin;           /**< Pin index within the port.      */
  uint8_t  psel_observed; /**< PFS PSEL[28..24] >> 24.         */
  uint8_t  pmr_observed;  /**< 1 if PFS.PMR == 1 after init.   */
  uint32_t pfs_raw;       /**< Full 32-bit PmnPFS readback.    */
} ra8_xspi_pin_observation_t;

/**
 * @var g_ra8_xspi_pin_observed
 * @brief JLink-readable PSEL readback for the 12 OCTA pin slots.
 *
 * @details
 * Updated unconditionally by ``priv_bus_init_once`` immediately
 * after ``ra8_board_xspi_pins_init`` returns. Cold-boot value is
 * all-zero (``.bss``). Look up the address with::
 *
 *     arm-none-eabi-nm <app>.elf | grep g_ra8_xspi_pin_observed
 *
 * Then in JLinkExe::
 *
 *     mem32 <addr> 24
 *
 * to dump 12 rows (2 words / 8 bytes per row). Each row's
 * ``psel_observed`` byte should equal 0x1C and ``pmr_observed``
 * should equal 1; any deviation flags a BSP enum error.
 *
 * @note Not ``static`` -- exported for ``nm`` / JLink lookup.
 * @warning Direct modification by other modules is forbidden.
 *
 * @since 0.1.0
 */
ra8_xspi_pin_observation_t g_ra8_xspi_pin_observed[k_ra8_xspi_pin_count];

/**
 * @struct ra8_xspi_pin_ref_t
 * @brief Board-side reference (port, pin) for the 12 OCTA pad slots.
 *
 * @details
 * Mirrors the order in
 * ``libs/ra8_board_ek_ra8d2/src/ra8_board_ek_ra8d2.c::s_xspi_octa_pins``
 * (CS, CK, DQS, DQ0..DQ7) so a JLink operator can match a row index
 * in ``g_ra8_xspi_pin_observed`` 1:1 with the BSP table comment.
 * Sourced from EK-RA8D2 v1 UM Table 29 (p 35).
 *
 * @invariant ``port`` and ``pin`` are valid PFS indices, or 0xFF
 *            sentinel for the reserved row.
 *
 * @since 0.1.0
 */
typedef struct {
  uint8_t port; /**< Port index. */
  uint8_t pin;  /**< Pin index.  */
} ra8_xspi_pin_ref_t;

/**
 * @var s_ra8_xspi_pin_refs
 * @brief Static reference list of OCTA (port, pin) pairs.
 *
 * @details
 * Indexed by row 0..11 of ``g_ra8_xspi_pin_observed``. The 12th slot
 * is a sentinel (0xFF, 0xFF) reserved for parity with the BSP table.
 *
 * @note File-scope; read-only.
 *
 * @since 0.1.0
 */
static const ra8_xspi_pin_ref_t s_ra8_xspi_pin_refs[k_ra8_xspi_pin_count] = {
  {(uint8_t)k_ra8_port_1, (uint8_t)k_ra8_pin_4}, /**< CS,  P104.         */
  {(uint8_t)k_ra8_port_8, (uint8_t)k_ra8_pin_8}, /**< CK,  P808.         */
  {(uint8_t)k_ra8_port_8, (uint8_t)k_ra8_pin_1}, /**< DQS, P801.         */
  {(uint8_t)k_ra8_port_1, (uint8_t)k_ra8_pin_0}, /**< DQ0, P100.         */
  {(uint8_t)k_ra8_port_8, (uint8_t)k_ra8_pin_3}, /**< DQ1, P803.         */
  {(uint8_t)k_ra8_port_1, (uint8_t)k_ra8_pin_3}, /**< DQ2, P103.         */
  {(uint8_t)k_ra8_port_1, (uint8_t)k_ra8_pin_1}, /**< DQ3, P101.         */
  {(uint8_t)k_ra8_port_1, (uint8_t)k_ra8_pin_2}, /**< DQ4, P102.         */
  {(uint8_t)k_ra8_port_8, (uint8_t)k_ra8_pin_0}, /**< DQ5, P800.         */
  {(uint8_t)k_ra8_port_8, (uint8_t)k_ra8_pin_2}, /**< DQ6, P802.         */
  {(uint8_t)k_ra8_port_8, (uint8_t)k_ra8_pin_4}, /**< DQ7, P804.         */
  {0xFFU, 0xFFU},                                /**< Reserved sentinel. */
};

/** @brief Constants for the diagnostic xSPI pin-state capture. */
typedef enum : uint8_t {
  k_ra8_lx_pin_unobserved = 0xFFU, /**< Sentinel before PFS is read.       */
  k_ra8_lx_pfs_psel_mask  = 0x1FU, /**< PSEL is the low 5 bits of the PFS. */
} ra8_lx_pin_diag_t;

/**
 * @brief Capture the post-init PFS state of every OCTA pin.
 *
 * @details
 * Iterates ``s_ra8_xspi_pin_refs`` and reads each pin's ``PmnPFS``
 * register, extracting the 5-bit PSEL field and the PMR bit. Called
 * AFTER ``ra8_board_xspi_pins_init`` so any mis-routed pin where
 * PSEL != 0x1C is recorded for JLink readout.
 *
 * @pre ``ra8_board_xspi_pins_init`` has been called.
 * @pre IOPORT module clock is on.
 *
 * @post ``g_ra8_xspi_pin_observed[0..k_ra8_xspi_pin_count-1]`` filled.
 * @post No state outside that array is modified.
 *
 * @note Not thread-safe; called once from boot.
 *
 * @since 0.1.0
 */
static void priv_capture_xspi_pin_state(void)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_xspi_pin_count; i++) {
    const uint8_t port                       = s_ra8_xspi_pin_refs[i].port;
    const uint8_t pin                        = s_ra8_xspi_pin_refs[i].pin;
    g_ra8_xspi_pin_observed[i].port          = port;
    g_ra8_xspi_pin_observed[i].pin           = pin;
    g_ra8_xspi_pin_observed[i].psel_observed = (uint8_t)k_ra8_lx_pin_unobserved;
    g_ra8_xspi_pin_observed[i].pmr_observed  = (uint8_t)k_ra8_lx_pin_unobserved;
    g_ra8_xspi_pin_observed[i].pfs_raw       = 0U;
    if ((port > (uint8_t)k_ra8_port_max) || (pin > (uint8_t)k_ra8_pin_max)) {
      continue;
    }
    volatile uint32_t* pfs = ra8_pfs_pmn((ra8_port_t)port, (ra8_pin_t)pin);
    if (pfs == nullptr) {
      continue;
    }
    const uint32_t raw                 = *pfs;
    g_ra8_xspi_pin_observed[i].pfs_raw = raw;
    g_ra8_xspi_pin_observed[i].psel_observed =
      (uint8_t)((raw >> (uint32_t)k_ra8_pfs_bit_psel0) & (uint32_t)k_ra8_lx_pfs_psel_mask);
    g_ra8_xspi_pin_observed[i].pmr_observed =
      (uint8_t)(((raw & (uint32_t)k_ra8_pfs_mask_pmr) != 0U) ? 1U : 0U);
  }
}

/**
 * @brief One-shot guard so we only run the bus bring-up once.
 *
 * @details
 * LevelX calls ``lx_nor_driver_ra8_xspi_initialize`` on every
 * ``lx_nor_flash_format`` and every ``lx_nor_flash_open``. Pin
 * routing and ``ra8_xspi_init`` must run exactly once -- a second
 * ``ra8_pfs_route_peripheral`` would trip the validator and return
 * ``k_ra8_err_gpio_conflict``. This file-static latches once the
 * first call succeeds.
 */
static bool s_xspi_bus_ready = false;

/**
 * @enum ra8_xspi_reset_timing_t
 * @brief Post-software-reset settle time required by the IS25LX512M.
 *
 * @details
 * IS25LX512M datasheet Ch 8.21 "Reset (RST)" p 39 specifies tRPH
 * (reset processing time) = 50 us max before the next chip-select
 * is allowed. We use a 1 ms ``ra8_delay_ms`` because that is the
 * smallest delay primitive available in ``ra8_core/ra8_time.h`` and
 * 1 ms >> 50 us with 20x headroom.
 */
typedef enum : uint32_t {
  k_ra8_xspi_reset_settle_ms = 1U, /**< >= tRPH (50 us). */
} ra8_xspi_reset_timing_t;

/**
 * @enum ra8_xspi_reset_form_t
 * @brief Width of the opcode pair shipped to ``ra8_xspi_software_reset``.
 *
 * @details
 * IS25LX512M Ch 7.3 "Operating Protocols" p 27 -- 1S-1S-1S accepts
 * 1-byte JEDEC opcodes; 8D-8D-8D OPI requires the opcode followed by
 * its 1's-complement (so the device sees 2 bytes per "opcode" on the
 * 8 DQ lines on both clock edges).
 */
typedef enum : uint8_t {
  k_ra8_xspi_reset_form_1s = 1U, /**< 1-byte opcode for 1S-1S-1S.       */
  k_ra8_xspi_reset_form_8d = 2U, /**< Opcode + complement for 8D-8D-8D. */
} ra8_xspi_reset_form_t;

/**
 * @brief Stamp the observation breadcrumb at the start of a bring-up call.
 *
 * @details
 * Writes the magic word, bumps ``call_count``, and zeroes the sub-stage
 * error fields BEFORE any HAL call so a JLink memory dump can show
 * exactly how far the bring-up progressed even if the controller hangs
 * mid-step.
 *
 * @pre ``g_ra8_xspi_rdid_observed`` is accessible (always; static SRAM).
 * @pre Called from the bus-init path before any HAL XSPI call.
 * @post ``g_ra8_xspi_rdid_observed.stage == k_ra8_xspi_stage_pins``.
 * @post ``g_ra8_xspi_rdid_observed.magic == k_ra8_xspi_rdid_magic`` and
 *       ``call_count`` is incremented by one.
 *
 * @note Not thread-safe; the LevelX NOR driver serialises all bus
 *       accesses through a single owner task.
 *
 * @since 0.1.0
 */
static void priv_stamp_observed_start(void)
{
  g_ra8_xspi_rdid_observed.magic = (uint32_t)k_ra8_xspi_rdid_magic;
  g_ra8_xspi_rdid_observed.call_count += 1U;
  g_ra8_xspi_rdid_observed.rid_err      = (uint32_t)k_ra8_ok;
  g_ra8_xspi_rdid_observed.jedec_id     = 0U;
  g_ra8_xspi_rdid_observed.reset_8d_err = (uint32_t)k_ra8_ok;
  g_ra8_xspi_rdid_observed.reset_1s_err = (uint32_t)k_ra8_ok;
  g_ra8_xspi_rdid_observed.stage        = (uint32_t)k_ra8_xspi_stage_pins;
}

/**
 * @brief Phase A -- punch the IS25LX512M out of any 8D-OPI mode.
 *
 * @details
 * The volatile-config register that selects 8D mode survives a
 * Cortex-M85 warm-reset because power was never removed, so even after
 * the PFS reset pulse the chip may still be in OPI. The only opcode it
 * understands in that state is the 8D RSTEN/RST pair (0x66 0x99 0x99
 * 0x66). Sending those first with the controller in 8D mode recovers
 * an OPI-stuck chip; a chip that was in 1S simply ignores the garbage
 * and gets recovered by Phase B.
 *
 * @return ``LX_SUCCESS`` if the controller switched to 8D and the
 *         reset opcodes were dispatched; ``LX_ERROR`` if
 *         ``ra8_xspi_init`` rejected the 8D mode switch.
 * @retval LX_SUCCESS Controller is in 8D mode; reset opcodes dispatched.
 * @retval LX_ERROR ``ra8_xspi_init`` rejected the 8D protocol switch.
 *
 * @pre ``priv_stamp_observed_start`` has been called for this bring-up.
 * @pre XSPI pins are configured (``ra8_board_xspi_pins_init`` succeeded).
 * @post ``g_ra8_xspi_rdid_observed.reset_8d_err`` records the HAL outcome.
 * @post Controller is left in 8D mode regardless of the reset result.
 *
 * @note ``ra8_xspi_software_reset`` may legitimately time out here when
 *       the chip is actually in 1S; we tolerate the failure and
 *       record it in ``reset_8d_err`` instead of bailing.
 * @note Not thread-safe; bus owner serialises all calls.
 *
 * @since 0.1.0
 */
static UINT priv_reset_phase_8d(void)
{
  g_ra8_xspi_rdid_observed.stage = (uint32_t)k_ra8_xspi_stage_init_8d;
  if (ra8_xspi_init((uint8_t)k_ra8_lx_xspi_instance, k_ra8_xspi_lio_8d8d8d) != k_ra8_ok) {
    ra8_log_error(s_lx_xspi_tag, "ra8_xspi_init 8d failed");
    return (UINT)LX_ERROR;
  }
  g_ra8_xspi_rdid_observed.stage = (uint32_t)k_ra8_xspi_stage_reset_8d;
  const ra8_err_t reset_8d =
    ra8_xspi_software_reset((uint8_t)k_ra8_lx_xspi_instance, (uint8_t)k_ra8_xspi_reset_form_8d);
  g_ra8_xspi_rdid_observed.reset_8d_err = (uint32_t)reset_8d;
  /* Tolerate timeouts here: if the chip is in 1S mode, the 8D bytes
   * never form a recognised opcode and CMDCMP may legitimately stall.
   * The Phase-B 1S reset will recover regardless. */
  ra8_delay_ms((uint32_t)k_ra8_xspi_reset_settle_ms);
  return (UINT)LX_SUCCESS;
}

/**
 * @brief Phase B -- switch the controller to 1S-1S-1S and reset again.
 *
 * @details
 * Restores the chip's power-on default protocol (the one the rest of
 * the driver uses) and issues the 1-byte RSTEN/RST pair. After this
 * any chip that survived Phase A in 1S mode is also in a known reset
 * state.
 *
 * @return ``LX_SUCCESS`` if both the controller switch and the reset
 *         opcodes succeeded; ``LX_ERROR`` if ``ra8_xspi_init`` failed.
 * @retval LX_SUCCESS Controller is in 1S mode; reset opcodes dispatched.
 * @retval LX_ERROR ``ra8_xspi_init`` rejected the 1S protocol switch.
 *
 * @pre Phase A (``priv_reset_phase_8d``) has been attempted.
 * @pre XSPI pins are configured (``ra8_board_xspi_pins_init`` succeeded).
 * @post ``g_ra8_xspi_rdid_observed.reset_1s_err`` records the HAL outcome.
 * @post Controller is left in 1S-1S-1S protocol mode.
 *
 * @note Not thread-safe; bus owner serialises all calls.
 *
 * @since 0.1.0
 */
static UINT priv_reset_phase_1s(void)
{
  g_ra8_xspi_rdid_observed.stage = (uint32_t)k_ra8_xspi_stage_init_1s;
  if (ra8_xspi_init((uint8_t)k_ra8_lx_xspi_instance, k_ra8_xspi_lio_1s1s1s) != k_ra8_ok) {
    ra8_log_error(s_lx_xspi_tag, "ra8_xspi_init 1s failed");
    return (UINT)LX_ERROR;
  }
  g_ra8_xspi_rdid_observed.stage = (uint32_t)k_ra8_xspi_stage_reset_1s;
  const ra8_err_t reset_1s =
    ra8_xspi_software_reset((uint8_t)k_ra8_lx_xspi_instance, (uint8_t)k_ra8_xspi_reset_form_1s);
  g_ra8_xspi_rdid_observed.reset_1s_err = (uint32_t)reset_1s;
  ra8_delay_ms((uint32_t)k_ra8_xspi_reset_settle_ms);
  return (UINT)LX_SUCCESS;
}

/**
 * @brief Probe RDID and confirm the IS25LX512M is responsive.
 *
 * @details
 * Reads the JEDEC triplet. If the controller still cannot read it back
 * after the dual-mode software reset, every downstream WREN / PP / SE
 * will also fail -- so we bail loudly. The expected value for the
 * IS25LX512M-JHLE on EK-RA8D2 v1 is ``{0x9D, 0x5A, 0x1A}`` per
 * datasheet Ch 8.13 p 36. A mismatch usually means a pin-routing issue
 * (CS / DQ not actually on PSEL=0x1C).
 *
 * @return ``LX_SUCCESS`` if RDID matches; ``LX_ERROR`` otherwise.
 * @retval LX_SUCCESS JEDEC ID equals ``k_ra8_lx_jedec_expected``.
 * @retval LX_ERROR ``ra8_xspi_flash_read_id`` failed or ID mismatched.
 *
 * @pre Controller is in 1S-1S-1S mode (Phase B completed).
 * @pre IS25LX512M had at least ``k_ra8_xspi_reset_settle_ms`` to settle.
 * @post ``g_ra8_xspi_rdid_observed.{rid_err,jedec_id}`` are updated.
 * @post On success, the chip is confirmed responsive and ready for
 *       erase/program/read opcodes.
 *
 * @note Not thread-safe; bus owner serialises all calls.
 *
 * @since 0.1.0
 */
static UINT priv_probe_rdid(void)
{
  g_ra8_xspi_rdid_observed.stage = (uint32_t)k_ra8_xspi_stage_rdid;
  uint32_t        jedec_id       = 0U;
  const ra8_err_t rid_err = ra8_xspi_flash_read_id((uint8_t)k_ra8_lx_xspi_instance, &jedec_id);
  g_ra8_xspi_rdid_observed.rid_err  = (uint32_t)rid_err;
  g_ra8_xspi_rdid_observed.jedec_id = jedec_id;
  if (rid_err != k_ra8_ok) {
    ra8_log_error_val(s_lx_xspi_tag, "RDID transfer failed err", (uint32_t)rid_err);
    return (UINT)LX_ERROR;
  }
  ra8_log_info_val(s_lx_xspi_tag, "RDID returned", jedec_id);
  if (jedec_id != (uint32_t)k_ra8_lx_jedec_expected) {
    ra8_log_error_val(s_lx_xspi_tag, "RDID mismatch (expected 0x9D5A1A); got", jedec_id);
    return (UINT)LX_ERROR;
  }
  return (UINT)LX_SUCCESS;
}

/**
 * @brief Bring the OCTA bus up (pins + controller) on first call.
 *
 * @details
 * Idempotent: the first call routes the OCTA pins, ungates the xSPI
 * module-stop bit, walks the IS25LX512M reset sequence and confirms the
 * device with an RDID probe, then latches ``s_xspi_bus_ready``. Every
 * later call returns ``LX_SUCCESS`` immediately, so each LevelX entry
 * point can open with this guard without tracking bring-up itself.
 *
 * @return ``LX_SUCCESS`` if the bus is ready, ``LX_ERROR`` otherwise.
 * @retval LX_SUCCESS Bus ready -- either just brought up, or already up.
 * @retval LX_ERROR   Pin routing, reset or the RDID probe failed; the
 *                    ready latch stays clear so a later call retries.
 *
 * @pre ``ra8_infrastructure_init`` has run (pin validator alive).
 * @pre Called from a single bring-up context (no concurrent LevelX entry).
 * @post On success the IS25LX512M is out of reset, OCTA pins are
 *       routed to PSEL=0x1C, the xSPI controller MSTP gate is open,
 *       ``LIOCFGCS[0]`` is in 1S-1S-1S mode, and the chip has
 *       reported the expected RDID triplet ``0x9D 0x5A 0x1A``.
 * @post On success ``s_xspi_bus_ready`` is set, so later calls short-circuit.
 *
 * @note Not thread-safe; LevelX drives this from a single context.
 * @since 0.1.0
 */
static UINT priv_bus_init_once(void)
{
  if (s_xspi_bus_ready) {
    return (UINT)LX_SUCCESS;
  }

  /* Stamp the magic + bump the counter BEFORE any HAL call so even a
   * hang inside the controller leaves a visible breadcrumb at the
   * known SRAM address. */
  priv_stamp_observed_start();

  if (ra8_board_xspi_pins_init() != k_ra8_ok) {
    /* Capture whatever pin state we have BEFORE bailing so JLink can
     * see how far the BSP got even on partial-failure paths. */
    priv_capture_xspi_pin_state();
    ra8_log_error(s_lx_xspi_tag, "xspi pin init failed");
    return (UINT)LX_ERROR;
  }
  /* Snapshot post-init PSEL on every OCTA pad. Any row where
   * psel_observed != 0x1C or pmr_observed != 1 means the BSP enum is
   * wrong for that pin -- the chip will not see that signal. */
  priv_capture_xspi_pin_state();

  const UINT phase_a = priv_reset_phase_8d();
  if (phase_a != (UINT)LX_SUCCESS) {
    return phase_a;
  }
  const UINT phase_b = priv_reset_phase_1s();
  if (phase_b != (UINT)LX_SUCCESS) {
    return phase_b;
  }
  const UINT probe = priv_probe_rdid();
  if (probe != (UINT)LX_SUCCESS) {
    return probe;
  }

  g_ra8_xspi_rdid_observed.stage = (uint32_t)k_ra8_xspi_stage_done;
  s_xspi_bus_ready               = true;
  return (UINT)LX_SUCCESS;
}

/* Lx nor driver ra xspi initialize -- see implementation for details. */
UINT lx_nor_driver_ra8_xspi_initialize(LX_NOR_FLASH* nor_flash)
{
  if (nor_flash == LX_NULL) {
    return (UINT)LX_ERROR;
  }

  /* Idempotent bus bring-up: pins + xSPI controller. Done here (not
   * in main) so the LevelX driver is self-contained and apps don't
   * have to remember the order of init calls. */
  const UINT bus = priv_bus_init_once();
  if (bus != (UINT)LX_SUCCESS) {
    return bus;
  }

  /* Programme the synthetic base + geometry. LevelX uses the base
   * pointer as a cookie when calling back into our read / write
   * callbacks; we recover the byte offset from it inside
   * priv_flash_offset_bytes(). */
  nor_flash->lx_nor_flash_base_address    = (ULONG*)(uintptr_t)k_ra8_lx_nor_base_addr;
  nor_flash->lx_nor_flash_total_blocks    = (ULONG)k_ra8_lx_nor_total_blocks;
  nor_flash->lx_nor_flash_words_per_block = (ULONG)k_ra8_lx_nor_words_per_block;

  /* Wire up the four LevelX driver callbacks. */
  nor_flash->lx_nor_flash_driver_read                = priv_nor_read;
  nor_flash->lx_nor_flash_driver_write               = priv_nor_write;
  nor_flash->lx_nor_flash_driver_block_erase         = priv_nor_block_erase;
  nor_flash->lx_nor_flash_driver_block_erased_verify = priv_nor_block_erased_verify;

  /* Hand LevelX a 512-byte ULONG-aligned RAM scratch buffer for its
   * sector-mapping reads. */
  nor_flash->lx_nor_flash_sector_buffer = &s_ra8_lx_nor_sector_buffer[0];

  return (UINT)LX_SUCCESS;
}
