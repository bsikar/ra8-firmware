/**
 * @file ra8_cache_store_mount.c
 * @brief ra8_cache_store mount/recovery path + shared on-flash helpers (#201).
 *
 * @par Tag
 * [Ring 4 / Storage] {World: NS}
 *
 * @details
 * Owns ::ra8_cache_store_init and the crash-recovery machinery: superblock
 * read/write, directory-checkpoint save/load, and the append-log scan that
 * replays after an unclean shutdown. Also defines the low-level helpers
 * (CRC-32, LevelX sector read/write/release, index find/add) shared with the
 * runtime TU. Runs LevelX through its public `lx_nor_flash_*` API only.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "lx_api.h"
#include "ra8_cache_store.h"
#include "ra8_cache_store_internal.h"
#include "ra8_check.h"
#include "ra8_err.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_cache_store";

/**
 * @brief One-shot latch so LevelX's global open-list is initialised once.
 * @details `lx_nor_flash_initialize()` clears LevelX's opened-flash registry;
 *          calling it more than once would disturb an already-open partition.
 *          The store calls it behind this latch on the first init.
 * @warning Written only by ::ra8_cache_store_init; not for other modules.
 * @since 0.1.0
 */
static bool s_cs_lx_system_inited = false;

/**
 * @enum ra8_cs_shift_t
 * @brief Small fixed quantities for CRC folding and dir packing.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_cs_crc_bits = 8U,   /**< Bits folded per input byte.               */
  k_ra8_cs_pct_full = 100U, /**< Whole-percent denominator for the margin. */
} ra8_cs_shift_t;

/* ------------------------------------------------------------------------- */
/* Shared low-level helpers (declared in ra8_cache_store_internal.h) */
/* ------------------------------------------------------------------------- */

uint32_t ra8_cs_crc32(const uint8_t* data, uint32_t len)
{
  if (data == nullptr) {
    return 0U;
  }
  if (len == 0U) {
    return 0U;
  }
  uint32_t crc = (uint32_t)k_ra8_cs_crc32_seed;
  for (uint32_t i = 0U; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0U; b < (uint8_t)k_ra8_cs_crc_bits; b++) {
      bool lsb = (crc & 1U) != 0U;
      crc >>= 1U;
      if (lsb) {
        crc ^= (uint32_t)k_ra8_cs_crc32_poly;
      }
    }
  }
  return crc ^ (uint32_t)k_ra8_cs_crc32_seed;
}

ra8_err_t ra8_cs_sector_read(const ra8_cache_store_t* store, uint32_t sector, uint8_t* out512)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(out512, s_tag, "out512");
  UINT rc = lx_nor_flash_sector_read(store->flash, (ULONG)sector, out512);
  if (rc == (UINT)LX_SUCCESS) {
    return k_ra8_ok;
  }
  if (rc == (UINT)LX_SECTOR_NOT_FOUND) {
    return k_ra8_err_not_found;
  }
  return k_ra8_err_hw_init_failed;
}

ra8_err_t ra8_cs_sector_write(ra8_cache_store_t* store, uint32_t sector, const uint8_t* in512)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(in512, s_tag, "in512");
  /* LevelX writes take a mutable buffer pointer but do not modify it. */
  UINT rc = lx_nor_flash_sector_write(store->flash, (ULONG)sector, (VOID*)(uintptr_t)in512);
  if (rc != (UINT)LX_SUCCESS) {
    return k_ra8_err_hw_init_failed;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_cs_sector_release(ra8_cache_store_t* store, uint32_t sector)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(store->flash, s_tag, "flash");
  UINT rc = lx_nor_flash_sector_release(store->flash, (ULONG)sector);
  if (rc != (UINT)LX_SUCCESS) {
    return k_ra8_err_hw_init_failed;
  }
  return k_ra8_ok;
}

int32_t ra8_cs_index_find(const ra8_cache_store_t* store, uint32_t key)
{
  if (store == nullptr) {
    return -1;
  }
  if (store->index == nullptr) {
    return -1;
  }
  for (uint16_t i = 0U; i < store->index_cap; i++) {
    const ra8_cache_store_entry_t* e = &store->index[i];
    if ((e->flags & (uint8_t)k_ra8_cache_store_flag_in_use) == 0U) {
      continue;
    }
    if (e->key == key) {
      return (int32_t)i;
    }
  }
  return -1;
}

int32_t ra8_cs_index_add(ra8_cache_store_t* store,
                         uint32_t           key,
                         uint32_t           start_sector,
                         uint16_t           sector_count,
                         uint32_t           byte_len,
                         bool               pinned)
{
  if (store == nullptr) {
    return -1;
  }
  if (store->index == nullptr) {
    return -1;
  }
  for (uint16_t i = 0U; i < store->index_cap; i++) {
    ra8_cache_store_entry_t* e = &store->index[i];
    if ((e->flags & (uint8_t)k_ra8_cache_store_flag_in_use) != 0U) {
      continue;
    }
    uint8_t flags = (uint8_t)k_ra8_cache_store_flag_in_use;
    if (pinned) {
      flags |= (uint8_t)k_ra8_cache_store_flag_pinned;
    }
    *e = (ra8_cache_store_entry_t){.key          = key,
                                   .start_sector = start_sector,
                                   .byte_len     = byte_len,
                                   .sector_count = sector_count,
                                   .flags        = flags,
                                   .reserved     = 0U};
    return (int32_t)i;
  }
  return -1;
}

/* ------------------------------------------------------------------------- */
/* Superblock */
/* ------------------------------------------------------------------------- */

ra8_err_t ra8_cs_super_write(ra8_cache_store_t* store, uint32_t clean)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(store->staging, s_tag, "staging");
  ra8_cs_super_t sb = {.magic           = (uint32_t)k_ra8_cs_super_magic,
                       .version         = (uint32_t)k_ra8_cs_format_version,
                       .seq             = store->next_seq,
                       .clean           = clean,
                       .entry_count     = 0U,
                       .live_sectors    = store->live_sectors,
                       .next_seq        = store->next_seq,
                       .log_start       = store->log_start,
                       .data_capacity   = store->data_capacity,
                       .logical_sectors = store->logical_sectors,
                       .crc             = 0U};
  for (uint16_t i = 0U; i < store->index_cap; i++) {
    if ((store->index[i].flags & (uint8_t)k_ra8_cache_store_flag_in_use) != 0U) {
      sb.entry_count++;
    }
  }
  sb.crc = ra8_cs_crc32((const uint8_t*)&sb, (uint32_t)(sizeof(sb) - sizeof(sb.crc)));
  (void)memset(store->staging, 0, (size_t)k_ra8_cache_store_sector_bytes);
  (void)memcpy(store->staging, &sb, sizeof(sb));
  return ra8_cs_sector_write(store, 0U, store->staging);
}

/**
 * @brief Read + parse the superblock (sector 0) into @p out_sb.
 * @details An unreadable sector 0 (the rare LevelX allocation-failure miss) is
 *          treated as an invalid superblock -- @p out_sb is zeroed so the caller
 *          falls back to a log replay. On a formatted device LevelX returns a
 *          real record (a clean one written at format time, or 0xFF on a raw
 *          device), so this never needs a separate "absent" case.
 * @param[in]  store  Store to read from.
 * @param[out] out_sb Receives the parsed (or zeroed) superblock.
 * @return Error code.
 * @retval k_ra8_ok Sector 0 parsed, or zeroed on an unreadable miss.
 * @pre `store->staging` covers one sector.
 * @pre @p out_sb is writable.
 * @post `*out_sb` is always populated (real record or all-zero).
 * @post No flash sector is written.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
static ra8_err_t cs_super_read(ra8_cache_store_t* store, ra8_cs_super_t* out_sb)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(out_sb, s_tag, "out_sb");
  if (ra8_cs_sector_read(store, 0U, store->staging) != k_ra8_ok) {
    (void)memset(out_sb, 0, sizeof(*out_sb));
    return k_ra8_ok;
  }
  (void)memcpy(out_sb, store->staging, sizeof(*out_sb));
  return k_ra8_ok;
}

/**
 * @brief True when @p sb is a valid, clean-shutdown superblock.
 * @details Nested single-condition checks (magic, CRC, clean marker) so there is
 *          no compound decision to MC/DC.
 * @param[in] sb Parsed superblock candidate.
 * @return Whether the checkpoint directory may be trusted.
 * @retval true  Magic + CRC valid and the clean marker is set.
 * @retval false Any check failed.
 * @pre @p sb points at a fully-read record.
 * @pre The caller confirmed the sector was present.
 * @post @p sb is unmodified.
 * @post No I/O is performed.
 * @note Thread-safe: pure over its argument (no I/O).
 * @since 0.1.0
 */
static bool cs_super_is_clean(const ra8_cs_super_t* sb)
{
  if (sb == nullptr) {
    return false;
  }
  if (sb->magic != (uint32_t)k_ra8_cs_super_magic) {
    return false;
  }
  uint32_t want = ra8_cs_crc32((const uint8_t*)sb, (uint32_t)(sizeof(*sb) - sizeof(sb->crc)));
  if (sb->crc != want) {
    return false;
  }
  if (sb->clean != (uint32_t)k_ra8_cs_clean) {
    return false;
  }
  return true;
}

/* ------------------------------------------------------------------------- */
/* Directory checkpoint save / load */
/* ------------------------------------------------------------------------- */

/**
 * @brief Pack up to one sector of in-use index entries into @p out512.
 * @details Zeroes the sector, then serializes the next run of in-use slots
 *          (advancing @p slot past them) until the sector is full or the index
 *          is exhausted.
 * @param[in]     store  Store whose index is serialized.
 * @param[in,out] slot   Cursor into the index; advanced past packed slots.
 * @param[out]    out512 One-sector destination buffer.
 * @return Number of entries packed into this sector.
 * @retval 0 No more in-use entries (or a NULL argument).
 * @pre `store->index` covers `store->index_cap` slots.
 * @pre @p out512 covers one sector.
 * @post `*slot` points past the last packed entry.
 * @post @p out512 holds `<= k_ra8_cs_dir_per_sector` serialized entries.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static uint32_t cs_dir_pack_sector(ra8_cache_store_t* store, uint16_t* slot, uint8_t* out512)
{
  if (store == nullptr) {
    return 0U;
  }
  if (out512 == nullptr) {
    return 0U;
  }
  (void)memset(out512, 0, (size_t)k_ra8_cache_store_sector_bytes);
  uint32_t packed = 0U;
  for (uint32_t e = 0U; e < (uint32_t)k_ra8_cs_dir_per_sector; e++) {
    for (; *slot < store->index_cap; (*slot)++) {
      if ((store->index[*slot].flags & (uint8_t)k_ra8_cache_store_flag_in_use) != 0U) {
        break;
      }
    }
    if (*slot >= store->index_cap) {
      break;
    }
    const ra8_cache_store_entry_t* src = &store->index[*slot];
    ra8_cs_dir_ent_t               ent = {.key          = src->key,
                                          .start_sector = src->start_sector,
                                          .byte_len     = src->byte_len,
                                          .sector_count = src->sector_count,
                                          .flags =
                                            (uint16_t)(src->flags & (uint8_t)k_ra8_cache_store_flag_pinned)};
    (void)memcpy(&out512[(size_t)e * (uint32_t)k_ra8_cs_dir_ent_bytes], &ent, sizeof(ent));
    (*slot)++;
    packed++;
  }
  return packed;
}

ra8_err_t ra8_cs_dir_save(ra8_cache_store_t* store, uint32_t* out_entry_count)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(out_entry_count, s_tag, "out_entry_count");
  uint32_t count = 0U;
  uint16_t slot  = 0U;
  for (uint16_t d = 0U; d < store->checkpoint_dirs; d++) {
    count += cs_dir_pack_sector(store, &slot, store->staging);
    RA8_RETURN_ON_ERROR(ra8_cs_sector_write(store, 1U + (uint32_t)d, store->staging), s_tag, "dir");
  }
  *out_entry_count = count;
  return k_ra8_ok;
}

/**
 * @brief Unpack one directory sector into the index, up to @p entry_count total.
 * @details Deserializes each ::ra8_cs_dir_ent_t in @p in512, adds it to the index,
 *          accumulates its run into `live_sectors`, and advances @p loaded, until
 *          the sector is exhausted or @p entry_count entries are loaded.
 * @param[in,out] store       Store whose index gains entries.
 * @param[in]     in512       One directory sector read from flash.
 * @param[in,out] loaded      Running count of entries loaded so far.
 * @param[in]     entry_count Total entries the checkpoint recorded.
 * @return Nothing.
 * @pre `store->index` covers `store->index_cap` slots.
 * @pre @p in512 covers one sector; @p loaded is writable.
 * @post `*loaded <= entry_count` and grows by the entries in this sector.
 * @post `store->live_sectors` grows by the loaded runs.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static void cs_dir_unpack_sector(ra8_cache_store_t* store,
                                 const uint8_t*     in512,
                                 uint32_t*          loaded,
                                 uint32_t           entry_count)
{
  if (store == nullptr) {
    return;
  }
  if (in512 == nullptr) {
    return;
  }
  for (uint32_t e = 0U; e < (uint32_t)k_ra8_cs_dir_per_sector; e++) {
    if (*loaded >= entry_count) {
      break;
    }
    ra8_cs_dir_ent_t ent = {};
    (void)memcpy(&ent, &in512[(size_t)e * (uint32_t)k_ra8_cs_dir_ent_bytes], sizeof(ent));
    bool pinned = (ent.flags & (uint16_t)k_ra8_cache_store_flag_pinned) != 0U;
    (void)
      ra8_cs_index_add(store, ent.key, ent.start_sector, ent.sector_count, ent.byte_len, pinned);
    store->live_sectors += ent.sector_count;
    (*loaded)++;
  }
}

/**
 * @brief Load @p entry_count directory entries from the checkpoint into the index.
 * @details Reads each checkpoint directory sector and hands it to
 *          ::cs_dir_unpack_sector until the recorded entry count is loaded.
 * @param[in,out] store       Store whose index is rebuilt.
 * @param[in]     entry_count Entries the superblock recorded.
 * @return Error code.
 * @retval k_ra8_ok                Directory loaded; `live_sectors` accumulated.
 * @retval k_ra8_err_invalid_state `entry_count` exceeds the index capacity.
 * @retval k_ra8_err_hw_init_failed LevelX read error.
 * @pre `store->checkpoint_dirs` directory sectors exist on flash.
 * @pre `entry_count <= store->index_cap`.
 * @post On `k_ra8_ok` the index holds every checkpoint entry.
 * @post `store->live_sectors` equals the loaded run total.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
static ra8_err_t cs_dir_load(ra8_cache_store_t* store, uint32_t entry_count)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(store->staging, s_tag, "staging");
  if (entry_count > (uint32_t)store->index_cap) {
    return k_ra8_err_invalid_state;
  }
  store->live_sectors = 0U;
  uint32_t loaded     = 0U;
  for (uint16_t d = 0U; d < store->checkpoint_dirs; d++) {
    if (loaded >= entry_count) {
      break;
    }
    RA8_RETURN_ON_ERROR(ra8_cs_sector_read(store, 1U + (uint32_t)d, store->staging), s_tag, "dir");
    cs_dir_unpack_sector(store, store->staging, &loaded, entry_count);
  }
  return k_ra8_ok;
}

/* ------------------------------------------------------------------------- */
/* Append-log scan (unclean-shutdown replay) */
/* ------------------------------------------------------------------------- */

/**
 * @brief Read + validate the entry header at logical sector @p s.
 * @details Reads sector @p s and accepts it only as a self-anchored,
 *          CRC-sealed, in-bounds entry header; any failed check means no run
 *          starts here (an empty/torn sector or a stray payload sector).
 * @param[in]  store   Store to read from.
 * @param[in]  s       Candidate header sector.
 * @param[out] out_hdr Receives the parsed header when valid.
 * @return Whether a complete, self-consistent header lives at @p s.
 * @retval true  A valid header anchored at @p s was read.
 * @retval false The sector missed, or the record failed a check.
 * @pre `store->staging` covers one sector.
 * @pre @p out_hdr is writable.
 * @post On true, `out_hdr->start_sector == s`.
 * @post On false, no run is claimed at @p s.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
static bool cs_hdr_read(ra8_cache_store_t* store, uint32_t s, ra8_cs_entry_hdr_t* out_hdr)
{
  if (ra8_cs_sector_read(store, s, store->staging) != k_ra8_ok) {
    return false;
  }
  ra8_cs_entry_hdr_t h = {};
  (void)memcpy(&h, store->staging, sizeof(h));
  if (h.magic != (uint32_t)k_ra8_cs_entry_magic) {
    return false;
  }
  if (h.start_sector != s) {
    return false;
  }
  if (h.sector_count == 0U) {
    return false;
  }
  if (((uint64_t)s + (uint64_t)h.sector_count) > (uint64_t)store->logical_sectors) {
    return false;
  }
  uint32_t want = ra8_cs_crc32((const uint8_t*)&h, (uint32_t)(sizeof(h) - sizeof(h.hdr_crc)));
  if (h.hdr_crc != want) {
    return false;
  }
  *out_hdr = h;
  return true;
}

/**
 * @brief Fold one validated header into the index, tracking the max sequence.
 * @details Adds the entry when its key is new; a duplicate key or a full index is skipped so the scan stays bounded.
 * @param[in,out] store    Store whose index gains the entry.
 * @param[in]     h        Validated entry header.
 * @param[in,out] max_seq  Running maximum append sequence number.
 * @return Nothing.
 * @pre @p h passed ::cs_hdr_read.
 * @pre @p max_seq is writable.
 * @post The run is indexed and `store->live_sectors` grows by its length.
 * @post `*max_seq >= h.seq`.
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static void cs_scan_accept(ra8_cache_store_t* store, const ra8_cs_entry_hdr_t* h, uint32_t* max_seq)
{
  bool pinned = (h->flags & (uint16_t)k_ra8_cache_store_flag_pinned) != 0U;
  if (ra8_cs_index_find(store, h->key) >= 0) {
    return; /* write-once: a duplicate key on flash is ignored (keep the first). */
  }
  if (ra8_cs_index_add(store, h->key, h->start_sector, h->sector_count, h->byte_len, pinned) < 0) {
    return; /* index full: stop indexing further entries. */
  }
  store->live_sectors += h->sector_count;
  if (h->seq > *max_seq) {
    *max_seq = h->seq;
  }
}

/**
 * @brief Replay the append log to rebuild the index after an unclean shutdown.
 * @details Walks `[log_start, logical_sectors)`; a valid header claims its run
 *          (skip its payload), anything else advances one sector. A torn tail
 *          (payload written, header not) has no valid header and is discarded.
 * @param[in,out] store Store to rebuild.
 * @return Error code.
 * @retval k_ra8_ok           Index rebuilt from surviving entries.
 * @retval k_ra8_err_null_ptr `store` NULL.
 * @pre `store` geometry (`log_start`, `logical_sectors`) is set.
 * @pre `store->staging` covers one sector.
 * @post `store->live_sectors` / `store->next_seq` reflect the survivors.
 * @post Partially-appended entries are not indexed.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
static ra8_err_t cs_scan_log(ra8_cache_store_t* store)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(store->staging, s_tag, "staging");
  store->live_sectors = 0U;
  uint32_t max_seq    = 0U;
  uint32_t s          = store->log_start;
  for (uint32_t guard = store->log_start; guard < store->logical_sectors; guard++) {
    if (s >= store->logical_sectors) {
      break;
    }
    ra8_cs_entry_hdr_t h = {};
    if (!cs_hdr_read(store, s, &h)) {
      s += 1U;
      continue;
    }
    cs_scan_accept(store, &h, &max_seq);
    s += h.sector_count;
  }
  store->next_seq = max_seq + 1U;
  return k_ra8_ok;
}

/* ------------------------------------------------------------------------- */
/* init */
/* ------------------------------------------------------------------------- */

/**
 * @brief Validate the init config's required members and sizes.
 * @details Checks each required pointer and the size / margin bounds in turn, before any flash is touched.
 * @param[in] cfg Config to validate.
 * @return Error code.
 * @retval k_ra8_ok               Config is usable.
 * @retval k_ra8_err_null_ptr     A required pointer member is NULL.
 * @retval k_ra8_err_invalid_size `index_cap` zero or `staging_bytes` too small.
 * @retval k_ra8_err_invalid_arg  `overprovision_pct` out of range.
 * @pre @p cfg is non-NULL.
 * @pre The caller wants the store bound to `cfg->nor_flash`.
 * @post No state is modified (pure validation).
 * @post On `k_ra8_ok` every dependency needed by init is present.
 * @note Thread-safe: pure validation (no I/O).
 * @since 0.1.0
 */
static ra8_err_t cs_validate_cfg(const ra8_cache_store_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  RA8_CHECK_NULL_PTR(cfg->nor_flash, s_tag, "nor_flash");
  if (cfg->nor_driver_init == nullptr) {
    ra8_log_error(s_tag, "nor_driver_init");
    return k_ra8_err_null_ptr;
  }
  RA8_CHECK_NULL_PTR(cfg->index, s_tag, "index");
  RA8_CHECK_NULL_PTR(cfg->staging, s_tag, "staging");
  if (cfg->index_cap == 0U) {
    return k_ra8_err_invalid_size;
  }
  if (cfg->staging_bytes < (uint32_t)k_ra8_cache_store_sector_bytes) {
    return k_ra8_err_invalid_size;
  }
  if (cfg->overprovision_pct > (uint8_t)k_ra8_cache_store_max_overprov) {
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Derive the store geometry (checkpoint span, log start, budget).
 * @details Derives the checkpoint span from `index_cap`, the log-start sector, and the overprovisioned live-sector budget from the margin.
 * @param[out] store Store whose geometry fields are set.
 * @param[in]  cfg   Validated config.
 * @return Error code.
 * @retval k_ra8_ok               Geometry derived and stored.
 * @retval k_ra8_err_invalid_size `logical_sectors` too small for the layout.
 * @pre @p store and @p cfg are non-NULL.
 * @pre @p cfg passed ::cs_validate_cfg.
 * @post On `k_ra8_ok`, `log_start < logical_sectors` and `data_capacity > 0`.
 * @post On error @p store geometry is left partially set (init aborts).
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static ra8_err_t cs_geometry(ra8_cache_store_t* store, const ra8_cache_store_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  uint32_t dirs = ((uint32_t)cfg->index_cap + (uint32_t)k_ra8_cs_dir_per_sector - 1U) /
                  (uint32_t)k_ra8_cs_dir_per_sector;
  store->checkpoint_dirs = (uint16_t)dirs;
  store->log_start       = 1U + dirs;
  store->logical_sectors = cfg->logical_sectors;
  if (cfg->logical_sectors < (store->log_start + (uint32_t)k_ra8_cache_store_min_sectors)) {
    return k_ra8_err_invalid_size;
  }
  uint32_t op     = (cfg->overprovision_pct != 0U) ? (uint32_t)cfg->overprovision_pct
                                                   : (uint32_t)k_ra8_cache_store_overprov_pct;
  uint32_t usable = cfg->logical_sectors - store->log_start;
  store->data_capacity =
    (usable * ((uint32_t)k_ra8_cs_pct_full - op)) / (uint32_t)k_ra8_cs_pct_full;
  if (store->data_capacity == 0U) {
    store->data_capacity = 1U;
  }
  return k_ra8_ok;
}

/**
 * @brief Bring the injected LevelX NOR partition up (format on request, open).
 * @details Initialises the LevelX system once (latched), optionally formats, then opens the partition through the injected driver.
 * @param[in,out] store Store holding the LevelX control block pointer.
 * @param[in]     cfg   Validated config (driver-init seam, name, format flag).
 * @return Error code.
 * @retval k_ra8_ok                Partition open.
 * @retval k_ra8_err_hw_init_failed LevelX format or open failed.
 * @pre `cfg->nor_driver_init` populates the LevelX control block.
 * @pre `store->flash` points at caller-owned LevelX storage.
 * @post On `k_ra8_ok` the partition is open for sector I/O.
 * @post On error the partition is not usable.
 * @note Not thread-safe; single-threaded bring-up.
 * @since 0.1.0
 */
static ra8_err_t cs_open_levelx(ra8_cache_store_t* store, const ra8_cache_store_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  if (!s_cs_lx_system_inited) {
    (void)lx_nor_flash_initialize();
    s_cs_lx_system_inited = true;
  }
  /* LevelX takes a mutable CHAR* name but never writes it. */
  CHAR* nm = (cfg->name != nullptr) ? (CHAR*)(uintptr_t)cfg->name : (CHAR*)(uintptr_t) "ra8_cache";
  if (cfg->format) {
    if (lx_nor_flash_format(store->flash, nm, cfg->nor_driver_init, LX_NULL) != (UINT)LX_SUCCESS) {
      return k_ra8_err_hw_init_failed;
    }
  }
  if (lx_nor_flash_open(store->flash, nm, cfg->nor_driver_init) != (UINT)LX_SUCCESS) {
    return k_ra8_err_hw_init_failed;
  }
  /* Stamp an empty clean superblock at format time so a fresh mount loads the
   * (empty) checkpoint instead of scanning the whole -- as-yet unmapped -- log
   * region, which on LevelX would allocate a physical sector per read. */
  if (cfg->format) {
    RA8_RETURN_ON_ERROR(ra8_cs_super_write(store, (uint32_t)k_ra8_cs_clean), s_tag, "format super");
  }
  return k_ra8_ok;
}

/**
 * @brief Rebuild the index from a parsed superblock: checkpoint load or replay.
 * @details A clean, valid superblock loads its checkpoint directory; anything
 *          else replays the append log. Sets `flash_state` to match.
 * @param[in,out] store Store whose index is rebuilt.
 * @param[in]     sb    Parsed superblock from sector 0.
 * @return Error code.
 * @retval k_ra8_ok                Index rebuilt.
 * @retval k_ra8_err_invalid_state Checkpoint directory inconsistent.
 * @retval k_ra8_err_hw_init_failed LevelX I/O error.
 * @pre @p sb was populated by ::cs_super_read.
 * @pre The LevelX partition is open.
 * @post On `k_ra8_ok` the index reflects flash and `flash_state` is set.
 * @post No flash sector is written.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
static ra8_err_t cs_recover(ra8_cache_store_t* store, const ra8_cs_super_t* sb)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(sb, s_tag, "sb");
  if (cs_super_is_clean(sb)) {
    store->next_seq    = sb->next_seq;
    store->flash_state = (uint8_t)k_ra8_cs_clean;
    return cs_dir_load(store, sb->entry_count);
  }
  store->flash_state = (uint8_t)k_ra8_cs_dirty;
  return cs_scan_log(store);
}

/**
 * @brief Mount an open partition: read sector 0, then recover the index.
 * @details Reads the superblock (read-only) and hands it to ::cs_recover. Because
 *          `init` stamps a clean superblock at format time, a fresh or cleanly
 *          closed store loads its checkpoint; only a genuine unclean shutdown
 *          replays the log.
 * @param[in,out] store Open store to mount.
 * @return Error code.
 * @retval k_ra8_ok                Index recovered; `flash_state` set.
 * @retval k_ra8_err_hw_init_failed LevelX I/O error.
 * @retval k_ra8_err_invalid_state  Checkpoint directory inconsistent.
 * @pre The LevelX partition is open.
 * @pre `store` geometry and staging are set.
 * @post On `k_ra8_ok` the index reflects flash; no flash sector was written.
 * @post On error the store is not usable.
 * @note Not thread-safe; shares the store staging buffer.
 * @since 0.1.0
 */
static ra8_err_t cs_mount(ra8_cache_store_t* store)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(store->staging, s_tag, "staging");
  ra8_cs_super_t sb = {};
  RA8_RETURN_ON_ERROR(cs_super_read(store, &sb), s_tag, "super");
  return cs_recover(store, &sb);
}

/**
 * @brief Bind the caller-owned buffers into the store and reset its counters.
 * @details Zeroes the handle, records the LevelX control block, index, and
 *          staging, clears every index slot, and seeds the sequence + live
 *          counters. Sets no geometry (::cs_geometry does that next).
 * @param[out] store Store handle to populate.
 * @param[in]  cfg   Validated config.
 * @return Nothing.
 * @pre @p cfg passed ::cs_validate_cfg (buffers non-NULL, `index_cap > 0`).
 * @pre @p store is the caller-owned handle.
 * @post `store->index` is fully cleared and the counters are seeded.
 * @post `store->inited` is false (init sets it last).
 * @note Not thread-safe; the store serialises access.
 * @since 0.1.0
 */
static void cs_init_fields(ra8_cache_store_t* store, const ra8_cache_store_cfg_t* cfg)
{
  if (store == nullptr) {
    return;
  }
  if (cfg == nullptr) {
    return;
  }
  *store           = (ra8_cache_store_t){};
  store->flash     = cfg->nor_flash;
  store->index     = cfg->index;
  store->staging   = cfg->staging;
  store->index_cap = cfg->index_cap;
  for (uint16_t i = 0U; i < store->index_cap; i++) {
    store->index[i] = (ra8_cache_store_entry_t){};
  }
  store->next_seq     = 1U;
  store->live_sectors = 0U;
}

/**
 * @brief Derive geometry, open the LevelX partition, and mount the index.
 * @details The three-step bring-up after the store buffers are bound: geometry,
 *          LevelX open (with the format-time clean superblock), then mount.
 * @param[in,out] store Store with its buffers already bound.
 * @param[in]     cfg   Validated config.
 * @return Error code.
 * @retval k_ra8_ok                 Geometry set, partition open, index mounted.
 * @retval k_ra8_err_null_ptr       `store` or `cfg` NULL.
 * @retval k_ra8_err_invalid_size   `logical_sectors` too small for the layout.
 * @retval k_ra8_err_hw_init_failed LevelX open/format/I-O error.
 * @retval k_ra8_err_invalid_state  Checkpoint directory inconsistent.
 * @pre `store` buffers are bound (::cs_init_fields ran).
 * @pre @p cfg passed ::cs_validate_cfg.
 * @post On `k_ra8_ok` the store is ready except for `inited`.
 * @post On error the store is not usable.
 * @note Not thread-safe; single-threaded bring-up.
 * @since 0.1.0
 */
static ra8_err_t cs_bringup(ra8_cache_store_t* store, const ra8_cache_store_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg");
  RA8_RETURN_ON_ERROR(cs_geometry(store, cfg), s_tag, "geometry");
  RA8_RETURN_ON_ERROR(cs_open_levelx(store, cfg), s_tag, "levelx open");
  RA8_RETURN_ON_ERROR(cs_mount(store), s_tag, "mount");
  return k_ra8_ok;
}

ra8_err_t ra8_cache_store_init(ra8_cache_store_t* store, const ra8_cache_store_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(store, s_tag, "store");
  RA8_RETURN_ON_ERROR(cs_validate_cfg(cfg), s_tag, "cfg");
  cs_init_fields(store, cfg);
  RA8_RETURN_ON_ERROR(cs_bringup(store, cfg), s_tag, "bringup");
  store->inited = true;
  return k_ra8_ok;
}
