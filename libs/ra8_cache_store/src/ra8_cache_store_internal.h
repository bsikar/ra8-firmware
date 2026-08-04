/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_cache_store_internal.h
 * @brief Internal on-media format + cross-TU helpers for ra8_cache_store (#201).
 * @ingroup grp_storage
 *
 * @details
 * Private to `libs/ra8_cache_store/src/`. Declares the three fixed-width on-flash
 * records (superblock, entry header, directory entry) and the helpers shared
 * between the runtime TU (`ra8_cache_store.c`) and the mount/recovery TU
 * (`ra8_cache_store_mount.c`). Not a public interface -- consumers use
 * `ra8_cache_store.h`.
 *
 * On-flash layout over the LevelX logical-sector space:
 *
 *   | Logical sector(s)          | Contents                                     |
 *   |----------------------------|----------------------------------------------|
 *   | 0                          | ::ra8_cs_super_t (superblock + clean marker)  |
 *   | [1, log_start)             | directory checkpoint (::ra8_cs_dir_ent_t run) |
 *   | [log_start, logical)       | append log: ::ra8_cs_entry_hdr_t + payload    |
 *
 * Records are written little-endian and consumed only by the same firmware
 * build/platform that wrote them (host tests read back on the host; the target
 * reads back on the target), so no byte-order translation is performed. Each
 * record's trailing CRC-32 covers all preceding fields.
 *
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 *
 * @par Tag
 * [Ring 4 / Storage] {World: NS}
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_cache_store.h"
#include "ra8_err.h"

/**
 * @enum ra8_cs_media_const_t
 * @brief Magics + serialization sizing for the on-flash records.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_cs_super_magic    = 0x52435331U, /**< Superblock tag 'R','C','S','1'.        */
  k_ra8_cs_entry_magic    = 0x52435345U, /**< Entry-header tag 'R','C','S','E'.      */
  k_ra8_cs_format_version = 1U,          /**< On-flash layout revision.              */
  k_ra8_cs_dir_ent_bytes  = 16U,         /**< Serialized ::ra8_cs_dir_ent_t size.    */
  k_ra8_cs_dir_per_sector = 32U,         /**< Directory entries per 512-byte sector. */
  k_ra8_cs_crc32_poly     = 0xEDB88320U, /**< Reflected CRC-32/ISO-HDLC polynomial.  */
  k_ra8_cs_crc32_seed     = 0xFFFFFFFFU, /**< CRC-32 pre/post conditioning.          */
} ra8_cs_media_const_t;

/**
 * @enum ra8_cs_clean_t
 * @brief Values of ::ra8_cs_super_t::clean (the shutdown marker).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_cs_dirty = 0U, /**< Session open or crashed -> mount must replay the log. */
  k_ra8_cs_clean = 1U, /**< Clean shutdown -> the checkpoint directory is valid.  */
} ra8_cs_clean_t;

/**
 * @struct ra8_cs_super_t
 * @brief On-flash superblock at logical sector 0 (also the checkpoint commit).
 *
 * @details Written last on any checkpoint, so a torn checkpoint leaves the prior
 *          (or dirty) superblock and forces a safe replay. All fields are 4-byte
 *          and self-packing (no padding) for a deterministic CRC.
 *
 * @invariant `crc` == CRC-32 over the preceding fields for a valid superblock.
 * @since 0.1.0
 */
/* cppcheck-suppress-begin unusedStructMember */
typedef struct {
  uint32_t magic;           /**< ::k_ra8_cs_super_magic.               */
  uint32_t version;         /**< ::k_ra8_cs_format_version.            */
  uint32_t seq;             /**< Checkpoint sequence (monotonic).      */
  uint32_t clean;           /**< ::ra8_cs_clean_t shutdown marker.     */
  uint32_t entry_count;     /**< Directory entries in the checkpoint.  */
  uint32_t live_sectors;    /**< Live sector count at checkpoint time. */
  uint32_t next_seq;        /**< Next append sequence number.          */
  uint32_t log_start;       /**< First append-log logical sector.      */
  uint32_t data_capacity;   /**< Overprovisioned live-sector budget.   */
  uint32_t logical_sectors; /**< Usable logical-sector span.           */
  uint32_t crc;             /**< CRC-32 over the ten fields above.     */
} ra8_cs_super_t;
/* cppcheck-suppress-end unusedStructMember */

/**
 * @struct ra8_cs_entry_hdr_t
 * @brief On-flash entry header at a run's first logical sector.
 *
 * @details Written **last** in a put (after the payload sectors), so its
 *          presence certifies a complete append. Stores its own `start_sector`
 *          so a stray payload sector cannot masquerade as a header during a scan.
 *
 * @invariant `hdr_crc` == CRC-32 over the preceding fields; `sector_count >= 1`.
 * @since 0.1.0
 */
/* cppcheck-suppress-begin unusedStructMember */
typedef struct {
  uint32_t magic;        /**< ::k_ra8_cs_entry_magic.                         */
  uint32_t seq;          /**< Append sequence number (monotonic).             */
  uint32_t key;          /**< Content key (source CRC-32).                    */
  uint32_t byte_len;     /**< Payload length in bytes.                        */
  uint32_t start_sector; /**< This header's own logical sector (self-anchor). */
  uint16_t sector_count; /**< Run length (header + payload sectors).          */
  uint16_t flags;        /**< Persisted flags (pinned bit).                   */
  uint32_t hdr_crc;      /**< CRC-32 over the six fields above.               */
} ra8_cs_entry_hdr_t;
/* cppcheck-suppress-end unusedStructMember */

/**
 * @struct ra8_cs_dir_ent_t
 * @brief One serialized directory entry in the checkpoint region (16 bytes).
 * @invariant `sector_count >= 1`.
 * @since 0.1.0
 */
/* cppcheck-suppress-begin unusedStructMember */
typedef struct {
  uint32_t key;          /**< Content key.                   */
  uint32_t start_sector; /**< Entry header logical sector.   */
  uint32_t byte_len;     /**< Payload length in bytes.       */
  uint16_t sector_count; /**< Run length (header + payload). */
  uint16_t flags;        /**< Persisted flags (pinned bit).  */
} ra8_cs_dir_ent_t;
/* cppcheck-suppress-end unusedStructMember */

static_assert(sizeof(ra8_cs_dir_ent_t) == (uint32_t)k_ra8_cs_dir_ent_bytes,
              "directory entry layout pinned at 16 bytes");
static_assert(sizeof(ra8_cs_entry_hdr_t) <= (uint32_t)k_ra8_cache_store_sector_bytes,
              "entry header must fit one sector");
static_assert(sizeof(ra8_cs_super_t) <= (uint32_t)k_ra8_cache_store_sector_bytes,
              "superblock must fit one sector");

/**
 * @brief Fold a byte block into a CRC-32/ISO-HDLC value (seeded + finalised).
 *
 * @details Standalone bitwise reflected CRC over one contiguous block: seeds and
 *          XOR-finalises with ::k_ra8_cs_crc32_seed internally, so the caller
 *          passes raw bytes and gets the final CRC. Used to seal every on-flash
 *          record.
 *
 * @param[in] data Bytes to fold (may be NULL only when @p len is 0).
 * @param[in] len  Byte count.
 * @return The finalised CRC-32 over @p data.
 * @retval uint32_t CRC over the @p len bytes; the empty-input CRC when `len==0`.
 *
 * @pre @p data covers @p len readable bytes, or @p len is 0.
 * @pre The caller wants ISO-HDLC (zlib-compatible) conditioning.
 * @post @p data is unmodified.
 * @post The result depends only on the input bytes (pure function).
 * @note Thread-safe: pure over its arguments.
 * @since 0.1.0
 */
RA8_PRIV uint32_t ra8_cs_crc32(const uint8_t* data, uint32_t len);

/**
 * @brief Read one LevelX logical sector into the store staging buffer region.
 * @details Wraps `lx_nor_flash_sector_read`, mapping the LevelX status onto an `ra8_err_t`.
 *
 * @param[in]  store  Store whose LevelX flash is read.
 * @param[in]  sector Logical sector index.
 * @param[out] out512 Destination of at least one sector.
 * @return Error code.
 * @retval k_ra8_ok               Sector read.
 * @retval k_ra8_err_not_found    Sector is unmapped/released (LevelX miss).
 * @retval k_ra8_err_hw_init_failed LevelX read error.
 *
 * @pre `store->flash` is an open LevelX NOR partition.
 * @pre @p out512 covers one sector.
 * @post On `k_ra8_ok`, @p out512 holds the sector bytes.
 * @post On any error @p out512 content is unspecified.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_cs_sector_read(const ra8_cache_store_t* store,
                                      uint32_t                 sector,
                                      uint8_t*                 out512);

/**
 * @brief Write one LevelX logical sector from a one-sector source buffer.
 * @details Wraps `lx_nor_flash_sector_write`, mapping a LevelX failure onto a hardware error.
 *
 * @param[in,out] store  Store whose LevelX flash is written.
 * @param[in]     sector Logical sector index.
 * @param[in]     in512  Source of exactly one sector.
 * @return Error code.
 * @retval k_ra8_ok                Sector written.
 * @retval k_ra8_err_hw_init_failed LevelX write error (e.g. no free sectors).
 *
 * @pre `store->flash` is an open LevelX NOR partition.
 * @pre @p in512 covers one sector.
 * @post On `k_ra8_ok`, a later ::ra8_cs_sector_read returns @p in512.
 * @post On any error the sector mapping is unchanged.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_cs_sector_write(ra8_cache_store_t* store,
                                       uint32_t           sector,
                                       const uint8_t*     in512);

/**
 * @brief Release a LevelX logical sector back to the free pool.
 * @details Wraps `lx_nor_flash_sector_release`, returning the logical sector to the free pool.
 *
 * @param[in,out] store  Store whose LevelX flash is released.
 * @param[in]     sector Logical sector index.
 * @return Error code.
 * @retval k_ra8_ok                Sector released (or already free).
 * @retval k_ra8_err_hw_init_failed LevelX release error.
 *
 * @pre `store->flash` is an open LevelX NOR partition.
 * @pre @p sector is within the partition.
 * @post On `k_ra8_ok`, a later read of @p sector misses until rewritten.
 * @post The sector becomes available for a future allocation.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_cs_sector_release(ra8_cache_store_t* store, uint32_t sector);

/**
 * @brief Find the index slot holding @p key.
 * @details Linear scan of the in-use index slots for a matching key.
 *
 * @param[in] store Store to search.
 * @param[in] key   Content key.
 * @return Slot index, or -1 when @p key is not present.
 * @retval -1 No in-use slot matches @p key.
 *
 * @pre `store->index` covers `store->index_cap` slots.
 * @pre The index reflects the live set.
 * @post The store is unmodified (pure lookup).
 * @post A non-negative result indexes an in-use slot with `key == @p key`.
 * @note Thread-safe with respect to a quiescent store (pure read).
 * @since 0.1.0
 */
RA8_PRIV int32_t ra8_cs_index_find(const ra8_cache_store_t* store, uint32_t key);

/**
 * @brief Claim a free index slot and populate it for @p key.
 * @details Claims the first free slot and records the entry's location, run length, and flags.
 *
 * @param[in,out] store        Store whose index gains an entry.
 * @param[in]     key          Content key.
 * @param[in]     start_sector Entry header logical sector.
 * @param[in]     sector_count Run length (header + payload).
 * @param[in]     byte_len     Payload length in bytes.
 * @param[in]     pinned       Persist the pinned flag.
 * @return Slot index, or -1 when the index is full.
 * @retval -1 No free slot.
 *
 * @pre `store->index` covers `store->index_cap` slots.
 * @pre @p key is not already present (caller checked).
 * @post On success the slot is in-use with the given fields.
 * @post On failure the index is unchanged.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
RA8_PRIV int32_t ra8_cs_index_add(ra8_cache_store_t* store,
                                  uint32_t           key,
                                  uint32_t           start_sector,
                                  uint16_t           sector_count,
                                  uint32_t           byte_len,
                                  bool               pinned);

/**
 * @brief Write the superblock (sector 0) with the given clean marker.
 *
 * @details Snapshots the store's live geometry (`entry_count`, `live_sectors`,
 *          `next_seq`, ...) plus @p clean into ::ra8_cs_super_t, seals it with a
 *          CRC, and writes sector 0 -- the commit point of a checkpoint.
 *
 * @param[in,out] store Store to snapshot.
 * @param[in]     clean ::ra8_cs_clean_t marker to stamp.
 * @return Error code.
 * @retval k_ra8_ok                Superblock written.
 * @retval k_ra8_err_hw_init_failed LevelX write error.
 *
 * @pre `store->inited` is true (or mid-init with geometry set).
 * @pre `store->staging` covers one sector.
 * @post On `k_ra8_ok`, sector 0 holds a valid superblock with @p clean.
 * @post On any error sector 0 is unchanged.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_cs_super_write(ra8_cache_store_t* store, uint32_t clean);

/**
 * @brief Serialize the live index into the on-flash checkpoint directory.
 *
 * @details Packs every in-use slot as an ::ra8_cs_dir_ent_t across the directory
 *          region `[1, log_start)`, then returns the count so the caller can put
 *          it in the superblock. Does not touch sector 0.
 *
 * @param[in,out] store         Store whose index is serialized.
 * @param[out]    out_entry_count Receives the number of entries written.
 * @return Error code.
 * @retval k_ra8_ok                Directory written.
 * @retval k_ra8_err_hw_init_failed LevelX write error.
 *
 * @pre `store->checkpoint_dirs` directory sectors were reserved at init.
 * @pre @p out_entry_count is writable.
 * @post On `k_ra8_ok`, `*out_entry_count` in-use entries are on flash.
 * @post The superblock is untouched (caller commits it).
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_cs_dir_save(ra8_cache_store_t* store, uint32_t* out_entry_count);

#ifdef __cplusplus
}
#endif
