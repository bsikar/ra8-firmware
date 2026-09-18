/**
 * @file examples/ek_ra8d2/hw_pending/cache_store_paged_demo/src/main.c
 * @brief Maintained consumer for ra8_cache_store: paged reads + crash replay (#937).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `libs/ra8_cache_store` shipped with no maintained application driving it: the
 * only caller on `dev` lived under `examples/ek_ra8d2/hil_needs_revalidation/`,
 * and `threadx_levelx_demo` links the lib without ever calling it (#937). This
 * app is that missing consumer, and it deliberately drives the three parts of
 * the public surface the parked demo never touched:
 *
 *   1. **paged** -- ::ra8_cache_store_read used as the `ra8_vsource_read_fn`
 *      shaped random-access reader: head, tail, windows that straddle a
 *      512-byte logical-sector boundary, a whole-blob read, and the
 *      out-of-range / NULL guards.
 *   2. **replay** -- ::ra8_cache_store_sync (checkpoint, clean marker left
 *      unset) followed by another put and then a simulated power loss: a fresh
 *      LevelX control block is mounted over the same media with `format=false`
 *      and no ::ra8_cache_store_close in between, so mount must take the dirty
 *      path and rebuild the index by replaying the append log. Both the
 *      checkpointed entries and the post-checkpoint one must come back.
 *   3. **guards / unwind** -- write-once refusal, zero-length and NULL puts,
 *      not-found lookups, pinned-entry eviction refusal, and every call after
 *      ::ra8_cache_store_close reporting `k_ra8_err_not_initialized`, plus the
 *      three ::ra8_cache_store_init argument rejections.
 *
 * Every leg self-checks. On success the app prints the success-only banner
 * `[csp] cache_store paged demo PASS ...`; on any failure it prints
 * `[csp] cache_store paged demo FAIL stage=S status=C` instead.
 *
 * ## Why a RAM-backed NOR driver
 * ra8_cache_store's physical-flash bind is an injected callback. Production
 * binds the Octo-SPI driver; this app binds the RAM driver below so the whole
 * path runs in SRAM with no MMIO, which makes an emulated run byte-identical to
 * an on-silicon run. The backing array persists across a LevelX close/open in
 * one boot, which is exactly the "control state lost, media survives" model the
 * replay leg needs.
 *
 * @author Brighton Sikarskie
 * @date 2026-09-16
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "lx_api.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cache_store.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_time.h"

/**
 * @enum csp_config_t
 * @brief Compile-time settings and fixture geometry for the app.
 * @details `k_csp_blob_bytes` is deliberately not a multiple of the 512-byte
 *          LevelX logical sector, so the paged leg has real sector-straddling
 *          and partial-tail windows to read.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_csp_baud            = 115200U, /**< Console baud (matches the other demos).   */
  k_csp_index_cap       = 8U,      /**< cache_store index slots.                  */
  k_csp_staging_bytes   = 512U,    /**< One LevelX logical sector.                */
  k_csp_logical_sectors = 128U,    /**< Usable LevelX logical-sector span.        */
  k_csp_blob_bytes      = 1300U,   /**< Big blob: 3 payload sectors + remainder.  */
  k_csp_small_bytes     = 100U,    /**< Sub-sector blob (one payload sector).     */
  k_csp_late_bytes      = 300U,    /**< Blob appended after the checkpoint.       */
  k_csp_window_bytes    = 16U,     /**< Largest partial read window.              */
  k_csp_reemit_ms       = 1000U,   /**< Steady-state banner re-emit cadence (ms). */
  k_csp_radix_dec       = 10U,     /**< Decimal radix for the u32 printer.        */
  k_csp_u32_buf_size    = 12U,     /**< u32 printer buffer (digits + NUL).        */
  k_csp_pattern_mul     = 31U,     /**< Position-dependent payload multiplier.    */
  k_csp_pattern_add     = 7U,      /**< Position-dependent payload offset.        */
  k_csp_byte_mask       = 0xFFU,   /**< Byte mask for the payload pattern.        */
  k_csp_bad_overprov    = 91U,     /**< Above k_ra8_cache_store_max_overprov.     */
  k_csp_tiny_staging    = 64U,     /**< Below one logical sector (init reject).   */
} csp_config_t;

/**
 * @enum csp_key_t
 * @brief The content keys (source CRC-32 stand-ins) the app caches.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_csp_key_big     = 0xC0FFEE01U, /**< The paged blob.                    */
  k_csp_key_small   = 0xC0FFEE02U, /**< Sub-sector blob.                   */
  k_csp_key_late    = 0xC0FFEE03U, /**< Put after the sync checkpoint.     */
  k_csp_key_missing = 0xDEADBE01U, /**< Never cached (not-found probes).   */
} csp_key_t;

/**
 * @enum csp_stage_t
 * @brief Leg identifiers reported in the FAIL banner.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_csp_stage_none   = 0U, /**< No leg has failed.                       */
  k_csp_stage_mount  = 1U, /**< Format + mount over the RAM NOR driver.  */
  k_csp_stage_seed   = 2U, /**< Put the fixture blobs and open a reader. */
  k_csp_stage_paged  = 3U, /**< Random-access reads through the reader.  */
  k_csp_stage_replay = 4U, /**< Sync, late put, dirty remount, recover.  */
  k_csp_stage_guards = 5U, /**< Argument + write-once + pin refusals.    */
  k_csp_stage_unwind = 6U, /**< Close, post-close calls, init rejects.   */
} csp_stage_t;

/**
 * @enum csp_nor_geom_t
 * @brief Geometry and sentinel constants for the RAM-backed NOR model.
 * @details 64 blocks x 512 ULONG words is well over the logical-sector span the
 *          store asks for, while keeping the SRAM backing at 128 KiB on target.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_csp_nor_blocks         = 64U,         /**< NOR blocks.         */
  k_csp_nor_words_per_blk  = 512U,        /**< ULONG words/block.  */
  k_csp_nor_total_words    = 64U * 512U,  /**< Backing word count. */
  k_csp_nor_erased         = 0xFFFFFFFFU, /**< LevelX erased word. */
} csp_nor_geom_t;

/**
 * @struct csp_result_t
 * @brief Outcome of one full run, rendered by ::internal_csp_report.
 * @since 0.1.0
 */
typedef struct {
  csp_stage_t stage;          /**< Leg that failed, or ::k_csp_stage_none. */
  ra8_err_t   status;         /**< Failing status, or `k_ra8_ok`.          */
  uint32_t    bytes_verified; /**< Payload bytes byte-compared by reads.   */
  uint32_t    recovered;      /**< Entries recovered by the dirty remount. */
  uint32_t    guards_ok;      /**< Guard probes that returned as expected. */
} csp_result_t;

/** @brief LevelX control block for the first (pre-crash) session. */
static LX_NOR_FLASH s_nor_a;
/** @brief LevelX control block for the post-crash remount session. */
static LX_NOR_FLASH s_nor_b;
/** @brief The fake NOR media; survives the simulated power loss. */
static ULONG s_nor_backing[k_csp_nor_total_words];
/** @brief LevelX per-open sector scratch (one logical sector wide). */
static ULONG s_nor_sector_buf[LX_NOR_SECTOR_SIZE];
/** @brief cache_store handle (caller-owned -- NASA P10 Rule 3). */
static ra8_cache_store_t s_store;
/** @brief cache_store index array (caller-owned). */
static ra8_cache_store_entry_t s_index[k_csp_index_cap];
/** @brief One-sector staging buffer (caller-owned). */
static uint8_t s_staging[k_csp_staging_bytes];
/** @brief Source bytes for the paged blob. */
static uint8_t s_blob[k_csp_blob_bytes];
/** @brief Read-back buffer sized for a whole-blob read. */
static uint8_t s_read[k_csp_blob_bytes];
/** @brief Streaming handle for the paged blob. */
static ra8_cache_store_reader_t s_reader;

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
RA8_INTERNAL static UINT internal_csp_nor_read(ULONG* flash_address, ULONG* destination,
                                               ULONG words)
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
RA8_INTERNAL static unsigned int internal_csp_nor_init(struct LX_NOR_FLASH_STRUCT* nor_flash)
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
RA8_INTERNAL static void internal_csp_nor_wipe(void)
{
  for (ULONG i = 0U; i < (ULONG)k_csp_nor_total_words; i++) {
    s_nor_backing[i] = (ULONG)k_csp_nor_erased;
  }
}

/**
 * @brief The position-dependent payload byte for a given payload offset.
 * @details A cheap deterministic pattern, so a read window can be verified from
 *          its offset alone without keeping a copy of the source bytes.
 * @param[in] offset Payload byte offset.
 * @param[in] salt   Per-blob salt (the content key's low byte).
 * @return The expected byte value at @p offset.
 * @retval 0 A legitimate pattern value, not an error.
 * @pre @p offset is within the blob being described.
 * @pre @p salt identifies the blob the offset belongs to.
 * @post No state is changed (pure function).
 * @post The same inputs always produce the same byte.
 * @note Thread-safe: no state.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_csp_pattern(uint32_t offset, uint32_t salt)
{
  uint32_t v = (offset * (uint32_t)k_csp_pattern_mul) + (uint32_t)k_csp_pattern_add + salt;
  return (uint8_t)(v & (uint32_t)k_csp_byte_mask);
}

/**
 * @brief Fill @p buf with the pattern for `[base, base + len)`.
 * @details Writes the deterministic payload the store is asked to cache.
 * @param[out] buf  Destination buffer of @p len bytes.
 * @param[in]  len  Byte count.
 * @param[in]  base Payload offset the first byte corresponds to.
 * @param[in]  salt Per-blob salt.
 * @return Nothing.
 * @pre @p buf covers @p len bytes.
 * @pre @p base + @p len does not overflow.
 * @post `buf[i]` holds the pattern byte for `base + i`.
 * @post Nothing outside @p buf is touched.
 * @note Not thread-safe with respect to @p buf.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_csp_fill(uint8_t* buf, uint32_t len, uint32_t base, uint32_t salt)
{
  for (uint32_t i = 0U; i < len; i++) {
    buf[i] = internal_csp_pattern(base + i, salt);
  }
}

/**
 * @brief Byte-compare @p buf against the pattern for `[base, base + len)`.
 * @details The read-side half of ::internal_csp_fill: proves a paged read landed
 *          the right bytes at the right payload offset, not merely `len` bytes.
 * @param[in] buf  Bytes read back.
 * @param[in] len  Byte count.
 * @param[in] base Payload offset the first byte should correspond to.
 * @param[in] salt Per-blob salt.
 * @return True when every byte matches.
 * @retval true  The window matches the pattern.
 * @retval false At least one byte differs.
 * @pre @p buf covers @p len bytes.
 * @pre @p base identifies where the window starts in the payload.
 * @post No state is changed.
 * @post The caller can treat false as a data-integrity failure.
 * @note Thread-safe with respect to the app's own state.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_csp_check(const uint8_t* buf, uint32_t len, uint32_t base,
                                            uint32_t salt)
{
  for (uint32_t i = 0U; i < len; i++) {
    if (buf[i] != internal_csp_pattern(base + i, salt)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Collapse an expected-status probe into a pass/fail code.
 * @details Guard legs assert on the exact status the API documents; a mismatch
 *          becomes `k_ra8_err_invalid_state` so the FAIL banner shows the leg.
 * @param[in]     got      Status the call actually returned.
 * @param[in]     want     Status the API documents for that call.
 * @param[in,out] counter  Guard-probe tally, incremented on a match.
 * @return `k_ra8_ok` when @p got equals @p want, else `k_ra8_err_invalid_state`.
 * @retval k_ra8_ok                The probe behaved as documented.
 * @retval k_ra8_err_invalid_state The probe returned something else.
 * @pre @p counter points at the run's guard tally.
 * @pre @p want is the documented status for the probe just made.
 * @post @p counter is incremented only on a match.
 * @post No store state is changed.
 * @note Not thread-safe with respect to @p counter.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_csp_expect(ra8_err_t got, ra8_err_t want, uint32_t* counter)
{
  if (got != want) {
    return k_ra8_err_invalid_state;
  }
  *counter = *counter + 1U;
  return k_ra8_ok;
}

/**
 * @brief Build the store configuration over this app's static fixture storage.
 * @details Binds the LevelX control block, the RAM NOR driver seam, the index
 *          array, and the staging buffer; `format` picks fresh media vs remount.
 * @param[in] nor    LevelX control block for this session.
 * @param[in] format True to format the media before opening it.
 * @return A fully populated, caller-owned configuration value.
 * @retval ra8_cache_store_cfg_t Configuration referencing static storage only.
 * @pre @p nor points at caller-owned LevelX control-block storage.
 * @pre The fixture arrays have their declared compile-time capacities.
 * @post The returned value references only firmware-lifetime storage.
 * @post No fixture byte is modified.
 * @note The value owns no memory and is copied by value.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_cache_store_cfg_t internal_csp_cfg(LX_NOR_FLASH* nor, bool format)
{
  return (ra8_cache_store_cfg_t){
    .nor_flash         = nor,
    .nor_driver_init   = internal_csp_nor_init,
    .name              = "ra8_cache_paged",
    .index             = s_index,
    .staging           = s_staging,
    .staging_bytes     = (uint32_t)sizeof(s_staging),
    .logical_sectors   = (uint32_t)k_csp_logical_sectors,
    .index_cap         = (uint16_t)k_csp_index_cap,
    .overprovision_pct = 0U,
    .format            = format,
  };
}

/**
 * @brief Format fresh media and mount a store over it.
 * @details Blanks the RAM NOR backing so the run is independent of BSS content,
 *          then formats and mounts, which stamps an empty clean superblock.
 * @return Error code.
 * @retval k_ra8_ok                 The store is mounted and empty.
 * @retval k_ra8_err_hw_init_failed LevelX format or open failed.
 * @retval k_ra8_err_invalid_state  The store reported itself uninitialised.
 * @pre The console is up (failures are reported by the caller).
 * @pre No store is currently mounted over the backing.
 * @post On `k_ra8_ok` `s_store` is mounted over freshly formatted media.
 * @post On error the store is left uninitialised.
 * @note Not thread-safe; single-threaded demo flow.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_csp_leg_mount(void)
{
  internal_csp_nor_wipe();
  s_store                        = (ra8_cache_store_t){};
  const ra8_cache_store_cfg_t cf = internal_csp_cfg(&s_nor_a, true);
  const ra8_err_t             rc = ra8_cache_store_init(&s_store, &cf);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (!s_store.inited) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Seal the fixture blobs and open a reader over the paged one.
 * @details Puts a 1300-byte blob (three payload sectors plus a partial tail) and
 *          a sub-sector blob, then opens the big one for streaming reads.
 * @param[in,out] res Run result; `bytes_verified` is left for the paged leg.
 * @return Error code.
 * @retval k_ra8_ok                Both blobs are sealed and `s_reader` is open.
 * @retval k_ra8_err_no_mem        Index or sector budget exhausted.
 * @retval k_ra8_err_invalid_state The reader reported the wrong payload length.
 * @pre The store is mounted and empty.
 * @pre Neither fixture key is already cached.
 * @post On `k_ra8_ok` both keys resolve and `s_reader` streams the big blob.
 * @post On error no partial entry is visible to a later get.
 * @note Not thread-safe; single-threaded demo flow.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_csp_leg_seed(csp_result_t* res)
{
  (void)res;
  internal_csp_fill(s_blob, (uint32_t)k_csp_blob_bytes, 0U, (uint32_t)k_csp_key_big);
  ra8_err_t rc =
    ra8_cache_store_put(&s_store, (uint32_t)k_csp_key_big, s_blob, (uint32_t)k_csp_blob_bytes);
  if (rc != k_ra8_ok) {
    return rc;
  }
  internal_csp_fill(s_blob, (uint32_t)k_csp_small_bytes, 0U, (uint32_t)k_csp_key_small);
  rc =
    ra8_cache_store_put(&s_store, (uint32_t)k_csp_key_small, s_blob, (uint32_t)k_csp_small_bytes);
  if (rc != k_ra8_ok) {
    return rc;
  }
  /* Restore the big blob's bytes: the buffer was reused for the small put. */
  internal_csp_fill(s_blob, (uint32_t)k_csp_blob_bytes, 0U, (uint32_t)k_csp_key_big);
  rc = ra8_cache_store_get(&s_store, (uint32_t)k_csp_key_big, &s_reader);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (s_reader.byte_len != (uint32_t)k_csp_blob_bytes) {
    return k_ra8_err_invalid_state;
  }
  return k_ra8_ok;
}

/**
 * @brief Read one window through the reader and byte-check it.
 * @details Wraps ::ra8_cache_store_read plus ::internal_csp_check so each paged
 *          probe is one call, and tallies the bytes actually compared.
 * @param[in]     offset Payload offset to read from.
 * @param[in]     len    Window length in bytes.
 * @param[in,out] res    Run result whose `bytes_verified` tally grows.
 * @return Error code.
 * @retval k_ra8_ok                The window read back and matched.
 * @retval k_ra8_err_out_of_range  The window ran past the payload.
 * @retval k_ra8_err_invalid_state The bytes did not match the pattern.
 * @pre `s_reader` is open over the big blob.
 * @pre `offset + len` is within the blob.
 * @post On `k_ra8_ok` `res->bytes_verified` has grown by @p len.
 * @post `s_read` holds the window that was just checked.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_csp_window(uint32_t offset, uint32_t len, csp_result_t* res)
{
  const ra8_err_t rc = ra8_cache_store_read(&s_reader, (uint64_t)offset, s_read, len);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (!internal_csp_check(s_read, len, offset, (uint32_t)k_csp_key_big)) {
    return k_ra8_err_invalid_state;
  }
  res->bytes_verified += len;
  return k_ra8_ok;
}

/**
 * @brief Drive the random-access read path the parked demo never exercised.
 * @details Reads the whole blob, then a head window, two windows that straddle a
 *          512-byte logical-sector boundary, and the partial tail; finally
 *          probes the out-of-range and NULL guards.
 * @param[in,out] res Run result (byte tally and guard tally).
 * @return Error code.
 * @retval k_ra8_ok                Every window matched and every guard held.
 * @retval k_ra8_err_invalid_state A window mismatched or a guard misbehaved.
 * @retval k_ra8_err_hw_init_failed A LevelX sector read failed.
 * @pre `s_reader` is open over the big blob.
 * @pre The blob was sealed with the deterministic pattern.
 * @post On `k_ra8_ok` `res->bytes_verified` counts every byte compared.
 * @post No store content is modified by this leg.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_csp_leg_paged(csp_result_t* res)
{
  const uint32_t sector = (uint32_t)k_ra8_cache_store_sector_bytes;
  const uint32_t win    = (uint32_t)k_csp_window_bytes;
  const uint32_t total  = (uint32_t)k_csp_blob_bytes;

  ra8_err_t rc = internal_csp_window(0U, total, res); /* whole blob            */
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_window(0U, win, res); /* head, sub-sector      */
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_window(sector - (win / 2U), win, res); /* straddles sector 1->2 */
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_window((2U * sector) - 3U, win, res); /* straddles sector 2->3 */
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_window(total - 5U, 5U, res); /* partial tail          */
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(ra8_cache_store_read(&s_reader, (uint64_t)(total - 2U), s_read, 4U),
                           k_ra8_err_out_of_range,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(ra8_cache_store_read(nullptr, 0U, s_read, 4U),
                           k_ra8_err_null_ptr,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  return internal_csp_expect(ra8_cache_store_read(&s_reader, 0U, nullptr, 4U),
                             k_ra8_err_null_ptr,
                             &res->guards_ok);
}

/**
 * @brief Checkpoint, append one more entry, then survive a simulated power loss.
 * @details Calls ::ra8_cache_store_sync (directory written, clean marker left
 *          unset), puts one more blob so the append log holds an entry the
 *          checkpoint never saw, then abandons the session without a close and
 *          mounts a fresh LevelX control block over the same media with
 *          `format=false`. Mount must take the dirty path and replay the log,
 *          recovering all three keys.
 * @param[in,out] res Run result; `recovered` counts the keys that came back.
 * @return Error code.
 * @retval k_ra8_ok                All three keys resolved after the remount.
 * @retval k_ra8_err_invalid_state A recovered entry had the wrong length or data.
 * @retval k_ra8_err_not_found     Replay lost an entry the log contained.
 * @pre The store is mounted with the two fixture blobs sealed.
 * @pre The RAM backing survives the simulated power loss (static storage).
 * @post On `k_ra8_ok` `s_store` is mounted on the post-crash session.
 * @post `s_reader` is re-opened against the post-crash store.
 * @note Not thread-safe; single-threaded demo flow.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_csp_leg_replay(csp_result_t* res)
{
  ra8_err_t rc = ra8_cache_store_sync(&s_store);
  if (rc != k_ra8_ok) {
    return rc;
  }
  internal_csp_fill(s_blob, (uint32_t)k_csp_late_bytes, 0U, (uint32_t)k_csp_key_late);
  rc = ra8_cache_store_put(&s_store, (uint32_t)k_csp_key_late, s_blob, (uint32_t)k_csp_late_bytes);
  if (rc != k_ra8_ok) {
    return rc;
  }

  /* Power loss: the control block and the handle are gone, the media is not.
   * No close runs, so the on-flash clean marker stays unset and the next mount
   * has to rebuild the index by scanning the append log. */
  s_nor_b                        = (LX_NOR_FLASH){};
  s_store                        = (ra8_cache_store_t){};
  const ra8_cache_store_cfg_t cf = internal_csp_cfg(&s_nor_b, false);
  rc                             = ra8_cache_store_init(&s_store, &cf);
  if (rc != k_ra8_ok) {
    return rc;
  }

  ra8_cache_store_reader_t late = {};
  rc = ra8_cache_store_get(&s_store, (uint32_t)k_csp_key_late, &late);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (late.byte_len != (uint32_t)k_csp_late_bytes) {
    return k_ra8_err_invalid_state;
  }
  rc = ra8_cache_store_read(&late, 0U, s_read, (uint32_t)k_csp_late_bytes);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (!internal_csp_check(s_read, (uint32_t)k_csp_late_bytes, 0U, (uint32_t)k_csp_key_late)) {
    return k_ra8_err_invalid_state;
  }
  res->bytes_verified += (uint32_t)k_csp_late_bytes;
  res->recovered += 1U;

  ra8_cache_store_reader_t small = {};
  rc = ra8_cache_store_get(&s_store, (uint32_t)k_csp_key_small, &small);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (small.byte_len != (uint32_t)k_csp_small_bytes) {
    return k_ra8_err_invalid_state;
  }
  res->recovered += 1U;

  rc = ra8_cache_store_get(&s_store, (uint32_t)k_csp_key_big, &s_reader);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (s_reader.byte_len != (uint32_t)k_csp_blob_bytes) {
    return k_ra8_err_invalid_state;
  }
  res->recovered += 1U;

  /* The recovered big blob still streams: re-read the straddling window. */
  return internal_csp_window((uint32_t)k_ra8_cache_store_sector_bytes - 4U, 8U, res);
}

/**
 * @brief Probe the documented refusals on a live store.
 * @details Write-once, zero-length and NULL puts, not-found lookups, and the
 *          pin/evict interlock (a pinned entry refuses eviction until unpinned).
 * @param[in,out] res Run result; `guards_ok` counts the probes that held.
 * @return Error code.
 * @retval k_ra8_ok                Every probe returned its documented status.
 * @retval k_ra8_err_invalid_state A probe returned something else.
 * @pre The store is mounted with all three fixture keys present.
 * @pre No reader is mid-stream over the key this leg evicts.
 * @post The small blob has been unpinned and evicted; the others remain.
 * @post `res->guards_ok` counts every probe that behaved.
 * @note Not thread-safe; single-threaded demo flow.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_csp_leg_guards(csp_result_t* res)
{
  ra8_cache_store_reader_t probe = {};

  ra8_err_t rc = internal_csp_expect(
    ra8_cache_store_put(&s_store, (uint32_t)k_csp_key_big, s_blob, (uint32_t)k_csp_small_bytes),
    k_ra8_err_exists,
    &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(
    ra8_cache_store_put(&s_store, (uint32_t)k_csp_key_missing, s_blob, 0U),
    k_ra8_err_invalid_size,
    &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(
    ra8_cache_store_put(&s_store, (uint32_t)k_csp_key_missing, nullptr, 4U),
    k_ra8_err_null_ptr,
    &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(ra8_cache_store_get(&s_store, (uint32_t)k_csp_key_missing, &probe),
                           k_ra8_err_not_found,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(ra8_cache_store_evict(&s_store, (uint32_t)k_csp_key_missing),
                           k_ra8_err_not_found,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = ra8_cache_store_pin(&s_store, (uint32_t)k_csp_key_small, true);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(ra8_cache_store_evict(&s_store, (uint32_t)k_csp_key_small),
                           k_ra8_err_busy,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = ra8_cache_store_pin(&s_store, (uint32_t)k_csp_key_small, false);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = ra8_cache_store_evict(&s_store, (uint32_t)k_csp_key_small);
  if (rc != k_ra8_ok) {
    return rc;
  }
  return internal_csp_expect(ra8_cache_store_get(&s_store, (uint32_t)k_csp_key_small, &probe),
                             k_ra8_err_not_found,
                             &res->guards_ok);
}

/**
 * @brief Close the store, then prove every post-close call refuses.
 * @details A clean close stamps the clean marker; after it the handle must
 *          report `k_ra8_err_not_initialized` for every operation. The leg then
 *          probes the three documented ::ra8_cache_store_init rejections.
 * @param[in,out] res Run result; `guards_ok` counts the probes that held.
 * @return Error code.
 * @retval k_ra8_ok                Close succeeded and every probe refused.
 * @retval k_ra8_err_invalid_state A probe returned something else.
 * @retval k_ra8_err_hw_init_failed The LevelX close failed.
 * @pre The store is mounted and no reader is mid-stream.
 * @pre The guards leg has finished with the store.
 * @post The store is closed and the media carries the clean marker.
 * @post No further operation is attempted on the closed handle.
 * @note Not thread-safe; single-threaded demo flow.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_csp_leg_unwind(csp_result_t* res)
{
  ra8_cache_store_reader_t probe = {};

  ra8_err_t rc = ra8_cache_store_close(&s_store);
  if (rc != k_ra8_ok) {
    return rc;
  }
  if (s_store.inited) {
    return k_ra8_err_invalid_state;
  }
  rc = internal_csp_expect(
    ra8_cache_store_put(&s_store, (uint32_t)k_csp_key_missing, s_blob, 4U),
    k_ra8_err_not_initialized,
    &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(ra8_cache_store_get(&s_store, (uint32_t)k_csp_key_big, &probe),
                           k_ra8_err_not_initialized,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(ra8_cache_store_evict(&s_store, (uint32_t)k_csp_key_big),
                           k_ra8_err_not_initialized,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(ra8_cache_store_pin(&s_store, (uint32_t)k_csp_key_big, true),
                           k_ra8_err_not_initialized,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(ra8_cache_store_sync(&s_store),
                           k_ra8_err_not_initialized,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = internal_csp_expect(ra8_cache_store_close(&s_store),
                           k_ra8_err_not_initialized,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }

  /* Init argument rejections, probed last so a mistake here cannot disturb a
   * live store. None of these reach LevelX: config validation runs first. */
  ra8_cache_store_t     spare = {};
  ra8_cache_store_cfg_t bad   = internal_csp_cfg(&s_nor_b, false);
  rc = internal_csp_expect(ra8_cache_store_init(&spare, nullptr),
                           k_ra8_err_null_ptr,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  bad.staging_bytes = (uint32_t)k_csp_tiny_staging;
  rc                = internal_csp_expect(ra8_cache_store_init(&spare, &bad),
                           k_ra8_err_invalid_size,
                           &res->guards_ok);
  if (rc != k_ra8_ok) {
    return rc;
  }
  bad.staging_bytes     = (uint32_t)sizeof(s_staging);
  bad.overprovision_pct = (uint8_t)k_csp_bad_overprov;
  return internal_csp_expect(ra8_cache_store_init(&spare, &bad),
                             k_ra8_err_invalid_arg,
                             &res->guards_ok);
}

/**
 * @brief Run every leg in order, recording the first failure.
 * @details Mount, seed, paged, replay, guards, unwind. The first leg to fail
 *          stamps its stage and status into @p res and stops the run.
 * @param[out] res Zero-initialised run result to populate.
 * @return Error code.
 * @retval k_ra8_ok The whole sequence passed.
 * @retval Any      The status the failing leg returned.
 * @pre @p res is zero-initialised.
 * @pre The console is up so the verdict can be printed afterwards.
 * @post @p res carries the tallies and, on failure, the failing stage.
 * @post The store is closed on the success path.
 * @note Not thread-safe; single-threaded demo flow.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_csp_run(csp_result_t* res)
{
  res->stage    = k_csp_stage_mount;
  ra8_err_t rc  = internal_csp_leg_mount();
  if (rc == k_ra8_ok) {
    res->stage = k_csp_stage_seed;
    rc         = internal_csp_leg_seed(res);
  }
  if (rc == k_ra8_ok) {
    res->stage = k_csp_stage_paged;
    rc         = internal_csp_leg_paged(res);
  }
  if (rc == k_ra8_ok) {
    res->stage = k_csp_stage_replay;
    rc         = internal_csp_leg_replay(res);
  }
  if (rc == k_ra8_ok) {
    res->stage = k_csp_stage_guards;
    rc         = internal_csp_leg_guards(res);
  }
  if (rc == k_ra8_ok) {
    res->stage = k_csp_stage_unwind;
    rc         = internal_csp_leg_unwind(res);
  }
  res->status = rc;
  if (rc == k_ra8_ok) {
    res->stage = k_csp_stage_none;
  }
  return rc;
}

/**
 * @brief Park the CPU forever after draining the console TX FIFO.
 * @details Flushes pending diagnostics once, then executes wait-for-interrupt
 *          indefinitely so fatal startup state stays observable on a debugger.
 * @return Nothing (does not return).
 * @pre A required startup step has failed irrecoverably.
 * @pre The board console may hold pending diagnostic bytes.
 * @post The console FIFO has been drained once.
 * @post The processor remains in a low-activity wait loop.
 * @note The terminal loop preserves fixture state for inspection.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_csp_panic_halt(void)
{
  (void)ra8_board_uart_console_flush();
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC, the time base, and the J-Link VCOM console up, or halt.
 * @details Initialises the clock generator, resolves CPUCLK0, starts SysTick,
 *          and configures the board console, in dependency order.
 * @return Nothing.
 * @pre Reset startup has initialised .data and zeroed .bss.
 * @pre The board clock and console register mappings are accessible.
 * @post On return, delays and console diagnostics are available.
 * @post Any setup failure transfers to ::internal_csp_panic_halt.
 * @note Call once from the single-threaded startup path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_csp_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    internal_csp_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_csp_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_csp_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_csp_baud) != k_ra8_ok) {
    internal_csp_panic_halt();
  }
}

/**
 * @brief Write a NUL-terminated ASCII string to the console.
 * @details Ignores NULL input and hands non-NULL bytes to the board console
 *          without allocating or retrying on a diagnostic-sink error.
 * @param[in] s NUL-terminated string, or NULL (ignored).
 * @return Nothing.
 * @pre The console is initialised.
 * @pre @p s is NUL-terminated when non-NULL.
 * @post A non-NULL @p s has its bytes queued to the console TX FIFO.
 * @post A NULL @p s is a no-op.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_csp_print(const char* s)
{
  if (s == nullptr) {
    return;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)s, strlen(s));
}

/**
 * @brief Print an unsigned 32-bit value in decimal to the console.
 * @details Converts from the least significant digit into a bounded local
 *          buffer, then emits the forward substring through the string helper.
 * @param[in] value Integer to print.
 * @return Nothing.
 * @pre The console is initialised.
 * @pre None on @p value (the full range is accepted).
 * @post The decimal text of @p value is queued to the console.
 * @post No other state changes.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_csp_print_u32(uint32_t value)
{
  char     buf[k_csp_u32_buf_size];
  uint32_t i = (uint32_t)sizeof(buf);
  buf[--i]   = '\0';
  if (value == 0U) {
    buf[--i] = '0';
  } else {
    while ((value != 0U) && (i > 0U)) {
      buf[--i] = (char)('0' + (value % (uint32_t)k_csp_radix_dec));
      value /= (uint32_t)k_csp_radix_dec;
    }
  }
  internal_csp_print(&buf[i]);
}

/**
 * @brief Emit the one-line verdict banner for a completed run.
 * @details PASS only when the run returned `k_ra8_ok`; the banner carries the
 *          byte, recovery, and guard tallies so a scrape can see real work ran.
 * @param[in] res Populated run result.
 * @return Nothing.
 * @pre The console is initialised.
 * @pre @p res reflects a completed ::internal_csp_run.
 * @post Exactly one PASS or FAIL line is queued to the console.
 * @post The PASS banner is never printed for a failed run.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_csp_report(const csp_result_t* res)
{
  if (res->status == k_ra8_ok) {
    internal_csp_print("[csp] cache_store paged demo PASS bytes=");
    internal_csp_print_u32(res->bytes_verified);
    internal_csp_print(" recovered=");
    internal_csp_print_u32(res->recovered);
    internal_csp_print(" guards=");
    internal_csp_print_u32(res->guards_ok);
    internal_csp_print("\r\n");
    return;
  }
  internal_csp_print("[csp] cache_store paged demo FAIL stage=");
  internal_csp_print_u32((uint32_t)res->stage);
  internal_csp_print(" status=");
  internal_csp_print_u32((uint32_t)res->status);
  internal_csp_print("\r\n");
}

/**
 * @brief Application entry: bring the board up, run the legs, report forever.
 * @return Nothing (does not return).
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre The shared board boot files installed the vector table.
 * @post The demo has run once and its verdict banner is streaming steadily.
 * @post The CPU idles re-emitting the banner (or halts on a fatal init error).
 * @since 0.1.0
 */
void main(void)
{
  internal_csp_setup_or_halt();
  ra8_isr_globals_enable();
  internal_csp_print("[csp] cache_store paged demo: formatting RAM-backed LevelX...\r\n");

  csp_result_t res = {};
  (void)internal_csp_run(&res);
  internal_csp_report(&res);

  /* Idle, re-emitting the verdict so a STOP_ON scrape always sees the
   * steady-state banner and the run reaches its budget cleanly. */
  while (1) {
    ra8_delay_ms((uint32_t)k_csp_reemit_ms);
    internal_csp_report(&res);
  }
}
