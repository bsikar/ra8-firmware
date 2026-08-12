/**
 * @file ra8_cache_store.h
 * @brief Persistent key(CRC32)->blob cache for compiled `.rabook` containers,
 * @ingroup grp_storage
 *        built on the vendored, HIL-validated LevelX NOR wear-levelling layer
 *        (#201).
 *
 * @details
 * `ra8_cache_store` is the persistent-cache tier from the multi-GB book-streaming
 * design (Decision 2): a small, flat `put` / `get` / `evict` store keyed by the
 * source CRC-32, log-structured, living on the OSPI/NAND tier -- NOT a second
 * `ra8_fs` mount. It is the on-flash backing for a compiled `.rabook` so the
 * reader can page a book straight out of NOR flash without touching the SD card
 * (fast resume + a power-savings win on a battery handheld).
 *
 * ## Not bespoke wear-levelling -- LevelX underneath
 * The wear-levelling / block-remapping / power-safe-sector-write machinery is
 * the already-vendored Azure RTOS LevelX (`libs/third_party/levelx/`), run in
 * standalone mode (`LX_STANDALONE_ENABLE`, no ThreadX). This module is only the
 * thin `key -> LevelX-logical-sector` index + log/checkpoint bookkeeping on top
 * of `lx_nor_flash_sector_write` / `_read` / `_release`. See issue #201 and
 * `recon/reports/disk-cache-design-review.md` for the full rationale.
 *
 * ## Dependency-inversion seam (host-testable)
 * The bind to a physical flash is the injected @ref ra8_cache_store_nor_init_fn
 * driver-initialise callback -- `lx_nor_driver_ra8_xspi_initialize` on the
 * EK-RA8D2 Octo-SPI flash, or a RAM NOR fake under the host tests. This
 * header therefore never includes `lx_api.h`; the `LX_NOR_FLASH` control block
 * is passed as an opaque, forward-declared pointer the caller owns.
 *
 * ## Zero heap (NASA P10 Rule 3)
 * The caller supplies **all** storage: the `LX_NOR_FLASH` control block, this
 * ::ra8_cache_store_t handle, the ::ra8_cache_store_entry_t index array, and a
 * one-sector (>= @ref k_ra8_cache_store_sector_bytes) staging buffer. The module
 * allocates nothing.
 *
 * ## Streaming a hit (feeds ra8_vsource)
 * ::ra8_cache_store_get fills a ::ra8_cache_store_reader_t and the caller wires
 * ::ra8_cache_store_read (an `ra8_vsource_read_fn`-shaped `read(ctx,off,buf,len)`)
 * into `ra8_vsource_add_paged`, so a cached `.rabook` demand-pages through the
 * #204/#205 paged path -- full residency is never required.
 *
 * ## Write-once invariant
 * An entry is sealed by a single ::ra8_cache_store_put; there is deliberately no
 * in-place-update call (the Liskov-clean shape). Because every cached blob is
 * re-derivable from its SD source, eviction never writes anything back -- it
 * only releases sectors. GC is internal to LevelX plus this module's sector
 * accounting.
 *
 * @note Not thread-safe: one reader core, callers serialise access.
 * @see ra8_cache_store_put   Seal a new entry.
 * @see ra8_cache_store_get   Open an entry for streaming reads.
 * @see ra8_cache_store_evict Drop an entry and reclaim its sectors.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 *
 * @par Tag
 * [Ring 4 / Storage] {World: NS}
 *
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/**
 * @brief Opaque forward declaration of the LevelX NOR control block.
 * @details Lets this header expose the LevelX-backed API without dragging in
 *          `lx_api.h` (and its `LX_STANDALONE_ENABLE` build coupling). The
 *          caller owns the concrete `LX_NOR_FLASH` object; only the `.c`
 *          implementation dereferences it.
 */
struct LX_NOR_FLASH_STRUCT;

/**
 * @typedef ra8_cache_store_nor_init_fn
 * @brief LevelX NOR driver-initialise seam (the injected physical-flash bind).
 *
 * @details Passed straight to `lx_nor_flash_open()` / `lx_nor_flash_format()`.
 *          Production binds `lx_nor_driver_ra8_xspi_initialize`; host tests bind
 *          a RAM NOR fake. The `unsigned int` return mirrors LevelX's
 *          `UINT` (`LX_SUCCESS` == 0) without needing the LevelX typedefs here.
 *
 * @param[in,out] nor_flash LevelX control block to populate (geometry +
 *                          driver callbacks + sector buffer).
 * @return `0` (`LX_SUCCESS`) on success; any non-zero LevelX status on failure.
 * @since 0.1.0
 */
typedef unsigned int (*ra8_cache_store_nor_init_fn)(struct LX_NOR_FLASH_STRUCT* nor_flash);

/**
 * @enum ra8_cache_store_geom_t
 * @brief Fixed geometry + tuning constants for the store.
 * @details `k_ra8_cache_store_sector_bytes` is the LevelX logical-sector size the
 *          module reads/writes a sector at a time; the staging buffer and every
 *          media record are sized against it.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_cache_store_sector_bytes = 512U, /**< LevelX logical-sector size (bytes). */
  k_ra8_cache_store_min_sectors  = 8U,   /**< Minimum usable logical-sector span. */
  k_ra8_cache_store_overprov_pct = 20U,  /**< Default GC headroom margin (%).     */
  k_ra8_cache_store_max_overprov = 90U,  /**< Reject nonsensical margins above.   */
} ra8_cache_store_geom_t;

/**
 * @enum ra8_cache_store_flag_t
 * @brief Per-entry index-slot flag bits.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_cache_store_flag_none   = 0U,      /**< Empty slot.                         */
  k_ra8_cache_store_flag_in_use = 1U << 0, /**< Slot holds a live entry.            */
  k_ra8_cache_store_flag_pinned = 1U << 1, /**< Never-evict (open book / metadata). */
} ra8_cache_store_flag_t;

/**
 * @struct ra8_cache_store_entry_t
 * @brief One RAM index slot: `key -> {header sector, run length, payload len}`.
 *
 * @details Caller-owned array element (see ::ra8_cache_store_cfg_t::index).
 *          Treat as private -- populated and consumed only by the store. An
 *          entry's on-flash run is `[start_sector, start_sector + sector_count)`
 *          with the header at `start_sector` and the payload in the sectors
 *          after it.
 *
 * @invariant `sector_count >= 1` when @ref k_ra8_cache_store_flag_in_use is set.
 * @since 0.1.0
 */
typedef struct {
  uint32_t key;          /**< Content key (source CRC-32); valid iff in-use. */
  uint32_t start_sector; /**< Logical sector of the entry header.            */
  uint32_t byte_len;     /**< Payload length in bytes.                       */
  uint16_t sector_count; /**< Run length (header + payload sectors).         */
  uint8_t  flags;        /**< ::ra8_cache_store_flag_t bit set.              */
  uint8_t  reserved;     /**< Padding; keeps the slot 16 bytes.              */
} ra8_cache_store_entry_t;

/**
 * @struct ra8_cache_store_t
 * @brief Caller-owned store handle. Treat every field as private.
 *
 * @details Zero-initialise (`= {}`) and pass to ::ra8_cache_store_init. The
 *          handle out-lives every call and every ::ra8_cache_store_reader_t
 *          derived from it.
 *
 * @invariant `log_start < logical_sectors` after a successful init.
 * @since 0.1.0
 */
typedef struct {
  struct LX_NOR_FLASH_STRUCT* flash;           /**< LevelX control block (borrowed).        */
  ra8_cache_store_entry_t*    index;           /**< Caller-owned index slot array.          */
  uint8_t*                    staging;         /**< Caller-owned >=1-sector scratch.        */
  uint16_t                    index_cap;       /**< Number of index slots.                  */
  uint16_t                    checkpoint_dirs; /**< Directory sectors for the checkpoint.   */
  uint32_t                    logical_sectors; /**< Usable LevelX logical-sector span.      */
  uint32_t                    log_start;       /**< First append-log sector.                */
  uint32_t                    data_capacity;   /**< Overprovisioned live-sector budget.     */
  uint32_t                    live_sectors;    /**< Sectors currently held by live entries. */
  uint32_t                    next_seq;        /**< Monotonic append sequence number.       */
  uint8_t                     flash_state;     /**< RAM shadow of sector 0 (dirty/clean).   */
  bool                        inited;          /**< True between init and close.            */
} ra8_cache_store_t;

/**
 * @struct ra8_cache_store_reader_t
 * @brief Handle to an open entry that streams through ::ra8_cache_store_read.
 *
 * @details Filled by ::ra8_cache_store_get. Pass its address as the `ctx` to
 *          `ra8_vsource_add_paged` together with ::ra8_cache_store_read and
 *          `size == byte_len`. Must out-live the vsource object that references
 *          it. Treat fields as private.
 *
 * @invariant `data_start >= store->log_start + 1` for a valid reader.
 * @since 0.1.0
 */
typedef struct {
  const ra8_cache_store_t* store;        /**< Parent store (flash + staging).   */
  uint32_t                 data_start;   /**< First payload logical sector.     */
  uint32_t                 data_sectors; /**< Payload sector count.             */
  uint32_t                 byte_len;     /**< Payload length (== vsource size). */
} ra8_cache_store_reader_t;

/**
 * @struct ra8_cache_store_cfg_t
 * @brief Injected dependencies + geometry for ::ra8_cache_store_init.
 *
 * @details Open/Closed seam: the store owns no globals; the LevelX control
 *          block, the physical-flash driver bind, and all RAM buffers are
 *          passed in. `logical_sectors` is how many LevelX logical sectors the
 *          store may use (the caller's reserved partition); `overprovision_pct`
 *          is the GC headroom margin reserved off that span.
 *
 * @invariant `nor_flash`, `nor_driver_init`, `index`, and `staging` are
 *            non-NULL; `index_cap > 0`; `staging_bytes >= sector size`.
 * @since 0.1.0
 */
typedef struct {
  struct LX_NOR_FLASH_STRUCT* nor_flash;         /**< Caller-owned LevelX control block.   */
  ra8_cache_store_nor_init_fn nor_driver_init;   /**< Injected physical-flash bind.        */
  const char*                 name;              /**< LevelX partition name (may be NULL). */
  ra8_cache_store_entry_t*    index;             /**< Caller-owned index array.            */
  uint8_t*                    staging;           /**< Caller-owned >=1-sector scratch.     */
  uint32_t                    staging_bytes;     /**< Bytes at @p staging (>= sector).     */
  uint32_t                    logical_sectors;   /**< Usable LevelX logical-sector span.   */
  uint16_t                    index_cap;         /**< Index slots (max live entries).      */
  uint8_t                     overprovision_pct; /**< GC headroom margin (0 => default).   */
  bool                        format;            /**< Format (wipe) before open.           */
} ra8_cache_store_cfg_t;

/**
 * @brief Bind a LevelX NOR flash instance and mount (or format) the store.
 *
 * @details
 * Opens the injected LevelX NOR partition (formatting first when
 * `cfg->format`), then recovers the key index:
 *  - a **clean** shutdown marker plus a valid on-flash directory checkpoint is
 *    loaded directly (fast path);
 *  - otherwise the append log is **replayed** (scanned) to rebuild the index and
 *    discard any entry whose append never completed.
 * The store is then marked dirty on flash so a later crash forces a replay.
 *
 * @param[out] store Zero-initialised handle to populate.
 * @param[in]  cfg   Injected dependencies + geometry (see the struct).
 *
 * @return Error code.
 * @retval k_ra8_ok                Store mounted (or formatted) and ready.
 * @retval k_ra8_err_null_ptr      `store`, `cfg`, or a required `cfg` member NULL.
 * @retval k_ra8_err_invalid_size  `staging_bytes` too small, `index_cap` zero, or
 *                                `logical_sectors` too small for the layout.
 * @retval k_ra8_err_invalid_arg   `overprovision_pct` out of range.
 * @retval k_ra8_err_hw_init_failed LevelX open/format failed.
 * @retval k_ra8_err_invalid_state  On-flash checkpoint/superblock is corrupt.
 *
 * @pre `store`, `cfg`, and every non-optional `cfg` member are non-NULL.
 * @pre `cfg->nor_driver_init` brings a working LevelX NOR flash up.
 * @post On `k_ra8_ok`, `store->inited` is true and the index reflects flash.
 * @post On any error the store is left uninitialised.
 *
 * @note Not thread-safe.
 * @see ra8_cache_store_close
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cache_store_init(ra8_cache_store_t*           store,
                                             const ra8_cache_store_cfg_t* cfg);

/**
 * @brief Seal a new entry once: append `data` under `key`, atomically.
 *
 * @details
 * Log-structured append with no in-place-update path: the payload sectors are
 * written first, then the entry header last, so a power loss mid-append leaves
 * an unreferenced (reclaimable) tail rather than a visible corrupt entry. Fails
 * if `key` already exists (write-once; evict first to replace) or if the
 * overprovisioned budget cannot fit the run.
 *
 * @param[in,out] store Initialised store.
 * @param[in]     key   Content key (source CRC-32).
 * @param[in]     data  Payload bytes (`len` readable).
 * @param[in]     len   Payload length in bytes (> 0).
 *
 * @return Error code.
 * @retval k_ra8_ok               Entry sealed and indexed.
 * @retval k_ra8_err_null_ptr     `store` or `data` NULL.
 * @retval k_ra8_err_invalid_size `len` is zero.
 * @retval k_ra8_err_not_initialized Store not initialised.
 * @retval k_ra8_err_exists       `key` already present (write-once).
 * @retval k_ra8_err_no_mem       Index full or budget/free-run exhausted.
 * @retval k_ra8_err_hw_init_failed LevelX sector write failed.
 *
 * @pre `store->inited` is true.
 * @pre `data` covers `len` bytes.
 * @post On `k_ra8_ok` a subsequent ::ra8_cache_store_get on `key` streams `data`.
 * @post On any error no partial entry is visible to ::ra8_cache_store_get.
 *
 * @note Not thread-safe.
 * @see ra8_cache_store_get
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_cache_store_put(ra8_cache_store_t* store, uint32_t key, const uint8_t* data, uint32_t len);

/**
 * @brief Open a sealed entry for random reads through ::ra8_cache_store_read.
 *
 * @details Looks `key` up in the index and fills @p out_reader so the caller can
 *          register it with `ra8_vsource_add_paged(vs, ra8_cache_store_read,
 *          out_reader, 0, out_reader->byte_len, &id)`. No data is copied here.
 *
 * @param[in]  store      Initialised store.
 * @param[in]  key        Content key to open.
 * @param[out] out_reader Receives the streaming handle (out-lives the vsource).
 *
 * @return Error code.
 * @retval k_ra8_ok                 Entry found; `out_reader` populated.
 * @retval k_ra8_err_null_ptr       `store` or `out_reader` NULL.
 * @retval k_ra8_err_not_initialized Store not initialised.
 * @retval k_ra8_err_not_found      `key` is not cached.
 *
 * @pre `store->inited` is true.
 * @pre `out_reader` is writable.
 * @post On `k_ra8_ok`, `out_reader->byte_len` is the entry's payload length.
 * @post On any error `out_reader` is untouched.
 *
 * @note Not thread-safe.
 * @see ra8_cache_store_read
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cache_store_get(const ra8_cache_store_t*  store,
                                            uint32_t                  key,
                                            ra8_cache_store_reader_t* out_reader);

/**
 * @brief `ra8_vsource_read_fn`-shaped random read over an open entry.
 *
 * @details Reads `len` bytes at `offset` from the entry's payload sectors, one
 *          LevelX logical sector at a time through the store staging buffer.
 *          Signature matches `ra8_vsource_read_fn` so it binds straight into
 *          `ra8_vsource_add_paged`.
 *
 * @param[in]  ctx    A ::ra8_cache_store_reader_t populated by ::ra8_cache_store_get.
 * @param[in]  offset Byte offset within the payload (`< byte_len`).
 * @param[out] buf    Destination (`len` writable bytes).
 * @param[in]  len    Bytes to read.
 *
 * @return Error code.
 * @retval k_ra8_ok                Bytes copied.
 * @retval k_ra8_err_null_ptr      `ctx` or `buf` NULL.
 * @retval k_ra8_err_out_of_range  `offset + len` exceeds `byte_len`.
 * @retval k_ra8_err_hw_init_failed LevelX sector read failed.
 *
 * @pre `ctx` is a reader from ::ra8_cache_store_get on a still-mounted store.
 * @pre `buf` covers `len` bytes.
 * @post On `k_ra8_ok`, `buf[0..len)` holds the payload slice.
 * @post On any error `buf` content is unspecified.
 *
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
ra8_err_t ra8_cache_store_read(void* ctx, uint64_t offset, uint8_t* buf, uint32_t len);

/**
 * @brief Drop an entry and reclaim its sectors (no write-back).
 *
 * @details Releases the entry's LevelX logical sectors (returning them to the
 *          free pool for GC) and clears its index slot. Because every cached
 *          blob is re-derivable from its SD source, **nothing is ever written
 *          back on eviction** -- the write-once invariant makes eviction
 *          unconditionally cheap. A pinned entry is refused.
 *
 * @param[in,out] store Initialised store.
 * @param[in]     key   Content key to drop.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Entry dropped and reclaimed.
 * @retval k_ra8_err_null_ptr       `store` NULL.
 * @retval k_ra8_err_not_initialized Store not initialised.
 * @retval k_ra8_err_not_found      `key` is not cached.
 * @retval k_ra8_err_busy           Entry is pinned (unpin first).
 * @retval k_ra8_err_hw_init_failed LevelX sector release failed.
 *
 * @pre `store->inited` is true.
 * @pre `key` is not pinned.
 * @post On `k_ra8_ok` a later ::ra8_cache_store_get on `key` returns not-found.
 * @post On `k_ra8_ok` the freed sectors are available to a later put.
 *
 * @note Not thread-safe.
 * @see ra8_cache_store_pin
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cache_store_evict(ra8_cache_store_t* store, uint32_t key);

/**
 * @brief Pin or unpin an entry (pinned entries are never evicted).
 *
 * @details A pin marks the currently-open book's container or the shelf/library
 *          metadata as never-evict, buying fast resume without an SD-card wake.
 *          The pin state is persisted at the next ::ra8_cache_store_sync /
 *          ::ra8_cache_store_close.
 *
 * @param[in,out] store Initialised store.
 * @param[in]     key   Content key to (un)pin.
 * @param[in]     pin   True to pin, false to unpin.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Pin state updated.
 * @retval k_ra8_err_null_ptr       `store` NULL.
 * @retval k_ra8_err_not_initialized Store not initialised.
 * @retval k_ra8_err_not_found      `key` is not cached.
 *
 * @pre `store->inited` is true.
 * @pre `key` is cached.
 * @post On `k_ra8_ok` the slot's pinned flag matches @p pin.
 * @post ::ra8_cache_store_evict on a pinned `key` returns `k_ra8_err_busy`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cache_store_pin(ra8_cache_store_t* store, uint32_t key, bool pin);

/**
 * @brief Checkpoint the index to flash (directory + clean marker not set).
 *
 * @details Serialises the live index into the on-flash directory region and
 *          rewrites the superblock. Called internally by ::ra8_cache_store_close;
 *          exposed so a caller can periodically shorten the replay a future
 *          crash would need. The clean marker stays unset until close.
 *
 * @param[in,out] store Initialised store.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Checkpoint written.
 * @retval k_ra8_err_null_ptr       `store` NULL.
 * @retval k_ra8_err_not_initialized Store not initialised.
 * @retval k_ra8_err_hw_init_failed LevelX write failed.
 *
 * @pre `store->inited` is true.
 * @pre The index reflects the intended live set.
 * @post On `k_ra8_ok` a clean remount can rebuild the index without a full scan.
 * @post The on-flash clean marker remains unset (dirty).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cache_store_sync(ra8_cache_store_t* store);

/**
 * @brief Checkpoint, set the clean-shutdown marker, and close the store.
 *
 * @details Writes the directory checkpoint and stamps the clean marker so the
 *          next ::ra8_cache_store_init takes the fast (no-scan) mount path, then
 *          closes the LevelX partition. After this the handle must be re-init'd
 *          before reuse.
 *
 * @param[in,out] store Initialised store.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Store checkpointed, marked clean, and closed.
 * @retval k_ra8_err_null_ptr       `store` NULL.
 * @retval k_ra8_err_not_initialized Store not initialised.
 * @retval k_ra8_err_hw_init_failed LevelX write/close failed.
 *
 * @pre `store->inited` is true.
 * @pre No reader is mid-stream against this store.
 * @post On `k_ra8_ok`, `store->inited` is false and the flash marker is clean.
 * @post A subsequent ::ra8_cache_store_init loads the checkpoint directly.
 *
 * @note Not thread-safe.
 * @see ra8_cache_store_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_cache_store_close(ra8_cache_store_t* store);

#ifdef __cplusplus
}
#endif
