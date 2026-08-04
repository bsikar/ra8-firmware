/**
 * @file ra8_fs_fat_alloc.c
 * @brief Where the free space is: next-free hint, FAT sector cache, FSInfo.
 *
 * @details
 * The state that turns cluster allocation from quadratic into amortised
 * constant, plus the FAT32 FSInfo maintenance that stops a card this firmware
 * wrote from reporting the wrong free space on a desktop.
 *
 * The three pieces are one mechanism seen from three angles -- see
 * `ra8_fs_fat_alloc_internal.h` for the argument. What lives here is the
 * storage and the accessors; the scan itself stays in `ra8_fs_fat.c` next to
 * the FAT entry decoding it drives.
 *
 * Two deliberate shapes are worth stating, because both are load-bearing:
 *
 * **The state is keyed by mount pointer, not held in the mount.** Every chain
 * walker in this adapter takes a `const ra8_fs_mount_t*`. Putting a mutable
 * cache in the mount would have meant either casting that `const` away or
 * propagating a non-const mount through a dozen call chains, to add something
 * that is not part of the volume at all.
 *
 * **One FAT sector cache is shared by every mount, not one per mount.** The
 * adapter is single-threaded by contract, so no operation interleaves two
 * volumes' FAT walks; a second buffer would cost another 512 bytes of `.bss`
 * to serve a case that cannot arise. The cache therefore records which mount
 * it belongs to, and a mount that finds someone else's sector simply misses.
 *
 * References (every shorthand citation in this file):
 *   - "MS FAT spec" = Microsoft Corp., "FAT: General Overview of On-Disk
 *     Format", v1.03, December 6 2000. FSInfo is sec 5.
 *
 * NASA Power-of-Ten compliance:
 *   - Rule 2: the only loops are over ::k_fs_alloc_slots (2).
 *   - Rule 3: zero malloc; all state lives in static arrays.
 *   - Rule 7: every backend call is checked.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"

/* =============================================================================
 * Module state
 * =============================================================================
 */

/**
 * @struct fat_alloc_state_t
 * @brief One mounted volume's answer to "where is the free space".
 *
 * @details Bound by ::priv_alloc_state_bind and released by
 *          ::priv_alloc_state_release, so it always describes the volume
 *          currently at `owner` and never a previous tenant of the same mount
 *          slot.
 *
 * @invariant `owner == nullptr` exactly when the slot is free.
 * @invariant `next_free >= k_cluster_first_data` in a bound slot.
 * @invariant `fsinfo_lba == k_fs_fsinfo_absent` unless the volume carries a
 *            fully validated FSInfo sector.
 * @see priv_alloc_state_bind()
 * @since 0.1.0
 */
typedef struct {
  const ra8_fs_mount_t* owner;      /**< Mount this slot describes; NULL = free. */
  uint32_t              next_free;  /**< Cluster the next scan starts at.        */
  uint32_t              free_count; /**< Free clusters, or ::k_fs_free_unknown.  */
  uint32_t              fsinfo_lba; /**< FSInfo sector, or ::k_fs_fsinfo_absent. */
  uint8_t               dirty;      /**< 1 = FSInfo needs a writeback.           */
} fat_alloc_state_t;

/**
 * @var s_alloc
 * @brief One allocator state slot per mount slot.
 * @details Indexed by search on `owner`, not by mount index, because this file
 *          has no visibility of the mount table in `ra8_fs_fat_mount.c`.
 * @note Not reentrant; the adapter is single-threaded by contract.
 * @warning Never index directly; go through ::priv_state_for.
 * @since 0.1.0
 */
static fat_alloc_state_t s_alloc[k_fs_alloc_slots] = {};

static_assert((uint32_t)k_fs_alloc_slots == (uint32_t)k_ra8_fs_max_mounts,
              "one allocator state slot per mount slot, or a bind can fail");

/**
 * @var s_fat_cache
 * @brief The one cached FAT sector, shared by every mount.
 * @details Valid only when `s_fat_cache_lba != k_fs_cache_empty`, and then
 *          only for `s_fat_cache_owner`.
 * @note Not reentrant; the adapter is single-threaded by contract.
 * @warning Never read directly; go through ::priv_fat_sector_read.
 * @since 0.1.0
 */
static uint8_t s_fat_cache[k_ra8_fs_bytes_per_sector] = {};

/**
 * @var s_fat_cache_lba
 * @brief Volume-relative LBA held in ::s_fat_cache, or ::k_fs_cache_empty.
 * @details `UINT32_MAX` is the empty sentinel because LBA 0 is a real address.
 * @note Not reentrant; the adapter is single-threaded by contract.
 * @warning Never modify directly.
 * @since 0.1.0
 */
static uint32_t s_fat_cache_lba = k_fs_cache_empty;

/**
 * @var s_fat_cache_owner
 * @brief Mount whose volume ::s_fat_cache holds a sector of.
 * @details Two volumes can be mounted at once and their LBAs mean different
 *          things, so the owner is part of the cache key.
 * @note Not reentrant; the adapter is single-threaded by contract.
 * @warning Never modify directly.
 * @since 0.1.0
 */
static const ra8_fs_mount_t* s_fat_cache_owner = nullptr;

/* =============================================================================
 * Slot lookup
 * =============================================================================
 */

/**
 * @brief Find the allocator state slot bound to @p m.
 *
 * @details Linear search over two entries. Returns NULL rather than binding
 *          lazily: a slot must be RESET when a volume is mounted, and a lazy
 *          bind cannot tell a first use from a reuse of a recycled mount
 *          address.
 *
 * @param[in] m Mount to look up.
 *
 * @return The bound slot, or NULL.
 * @retval non-NULL The slot describing @p m.
 * @retval NULL     @p m has no bound slot; callers fall back to old behaviour.
 *
 * @pre @p m is non-NULL.
 * @pre Mount and unmount are not running concurrently.
 * @post No state modified.
 * @post The returned pointer stays valid until @p m is unmounted.
 *
 * @note Not thread-safe; callers serialise filesystem operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static fat_alloc_state_t* priv_state_for(const ra8_fs_mount_t* m)
{
  for (uint32_t i = 0U; i < (uint32_t)k_fs_alloc_slots; i++) {
    if (s_alloc[i].owner == m) {
      return &s_alloc[i];
    }
  }
  return nullptr;
}

/* `priv_alloc_state_bind()`: see header for the documented contract. */
void priv_alloc_state_bind(const ra8_fs_mount_t* m)
{
  fat_alloc_state_t* st = priv_state_for(m);
  if (st == nullptr) {
    st = priv_state_for(nullptr);
  }
  if (st == nullptr) {
    return;
  }
  st->owner      = m;
  st->next_free  = (uint32_t)k_cluster_first_data;
  st->free_count = (uint32_t)k_fs_free_unknown;
  st->fsinfo_lba = (uint32_t)k_fs_fsinfo_absent;
  st->dirty      = 0U;
}

/* `priv_alloc_state_release()`: see header for the documented contract. */
void priv_alloc_state_release(const ra8_fs_mount_t* m)
{
  fat_alloc_state_t* st = priv_state_for(m);
  if (st != nullptr) {
    *st = (fat_alloc_state_t){};
  }
  if (s_fat_cache_owner == m) {
    s_fat_cache_lba   = (uint32_t)k_fs_cache_empty;
    s_fat_cache_owner = nullptr;
  }
}

/* =============================================================================
 * FAT sector cache
 * =============================================================================
 */

/**
 * @brief True when @p lba lies inside the first FAT copy of @p m.
 *
 * @details Only the first copy is cached. Reads never look anywhere else, and
 *          caching a mirror-copy write would evict the sector an allocation
 *          scan is walking -- disabling the cache during the one storm it
 *          exists for.
 *
 * @param[in] m   Mount providing the FAT geometry.
 * @param[in] lba Volume-relative sector.
 *
 * @return Whether the sector is cacheable.
 * @retval true  @p lba is in `[first_fat_lba, first_fat_lba + fat_size_sectors)`.
 * @retval false Anywhere else, including the mirror FAT copies.
 *
 * @pre @p m is non-NULL with its geometry populated.
 * @pre @p lba is volume-relative, not backend-absolute.
 * @post No state modified.
 * @post Result depends only on the inputs.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @par MC/DC:
 * Decision: `(lba - first_fat_lba) < fat_size_sectors` (1 condition).
 * - a sector of FAT copy 0 -> true  -> cacheable.
 * - a sector of a mirror   -> false -> passed straight to the backend.
 * The subtraction is unsigned, so an LBA BELOW the first FAT copy wraps to a
 * value far above any FAT length and the same test rejects it. Spelling that
 * out as a second range term would add a condition nothing can make false --
 * every caller derives its LBA from `first_fat_lba` -- and an untestable
 * condition is a permanent MC/DC hole.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_lba_is_cacheable(const ra8_fs_mount_t* m, uint32_t lba)
{
  return (lba - m->first_fat_lba) < m->fat_size_sectors;
}

/* `priv_fat_sector_read()`: see header for the documented contract. */
ra8_err_t priv_fat_sector_read(const ra8_fs_mount_t* m, uint32_t lba, uint8_t* buf)
{
  if (!priv_lba_is_cacheable(m, lba)) {
    return priv_read_sector(m, lba, buf);
  }
  if ((s_fat_cache_owner == m) && (s_fat_cache_lba == lba)) {
    priv_byte_copy(buf, s_fat_cache, (uint32_t)k_ra8_fs_bytes_per_sector);
    return k_ra8_ok;
  }
  const ra8_err_t err = priv_read_sector(m, lba, buf);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_byte_copy(s_fat_cache, buf, (uint32_t)k_ra8_fs_bytes_per_sector);
  s_fat_cache_lba   = lba;
  s_fat_cache_owner = m;
  return k_ra8_ok;
}

/* `priv_fat_sector_wrote()`: see header for the documented contract. */
void priv_fat_sector_wrote(const ra8_fs_mount_t* m, const uint8_t* buf, uint32_t lba)
{
  if (!priv_lba_is_cacheable(m, lba)) {
    return;
  }
  priv_byte_copy(s_fat_cache, buf, (uint32_t)k_ra8_fs_bytes_per_sector);
  s_fat_cache_lba   = lba;
  s_fat_cache_owner = m;
}

/* =============================================================================
 * Next-free hint and free-cluster count
 * =============================================================================
 */

/* `priv_alloc_hint_get()`: see header for the documented contract. */
uint32_t priv_alloc_hint_get(const ra8_fs_mount_t* m)
{
  const fat_alloc_state_t* st = priv_state_for(m);
  if (st == nullptr) {
    return (uint32_t)k_cluster_first_data;
  }
  return st->next_free;
}

/* `priv_alloc_hint_set()`: see header for the documented contract. */
void priv_alloc_hint_set(const ra8_fs_mount_t* m, uint32_t cluster)
{
  fat_alloc_state_t* st = priv_state_for(m);
  if (st == nullptr) {
    return;
  }
  st->next_free = cluster;
}

/* `priv_alloc_hint_lower()`: see header for the documented contract. */
void priv_alloc_hint_lower(const ra8_fs_mount_t* m, uint32_t cluster)
{
  fat_alloc_state_t* st = priv_state_for(m);
  if (st == nullptr) {
    return;
  }
  if (cluster < st->next_free) {
    st->next_free = cluster;
  }
}

/* `priv_free_count_took()`: see header for the documented contract. */
void priv_free_count_took(const ra8_fs_mount_t* m, uint32_t n)
{
  fat_alloc_state_t* st = priv_state_for(m);
  if (st == nullptr) {
    return;
  }
  st->dirty = 1U;
  if (st->free_count == (uint32_t)k_fs_free_unknown) {
    return;
  }
  st->free_count = (st->free_count >= n) ? (st->free_count - n) : 0U;
}

/* `priv_free_count_gave()`: see header for the documented contract. */
void priv_free_count_gave(const ra8_fs_mount_t* m, uint32_t n)
{
  fat_alloc_state_t* st = priv_state_for(m);
  if (st == nullptr) {
    return;
  }
  st->dirty = 1U;
  if (st->free_count == (uint32_t)k_fs_free_unknown) {
    return;
  }
  const uint32_t raised = st->free_count + n;
  st->free_count        = (raised > m->count_of_clusters) ? m->count_of_clusters : raised;
}

/* =============================================================================
 * FAT32 FSInfo (MS FAT spec sec 5)
 * =============================================================================
 */

/**
 * @brief Report the FSInfo sector number this volume declares, or absent.
 *
 * @details Re-reads the boot sector rather than caching `BPB_FSInfo` in the
 *          mount: it costs one sector read once per mount and keeps the whole
 *          FSInfo mechanism inside this file. The declared sector must lie
 *          inside the volume's own reserved region -- a BPB pointing anywhere
 *          else is describing something that is not an FSInfo sector.
 *
 * @param[in]  m       Mount to interrogate.
 * @param[out] out_lba Receives the sector number, or ::k_fs_fsinfo_absent.
 *
 * @return Error code.
 * @retval k_ra8_ok    @p out_lba is populated (possibly with "absent").
 * @retval k_ra8_err_* The boot sector could not be re-read.
 *
 * @pre @p m and @p out_lba are non-NULL; the mount's geometry is populated.
 * @pre The mount is FAT32 (callers check; other types have no FSInfo).
 * @post On success @p out_lba is either 0 or a sector inside the reserved region.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise mount operations.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_fsinfo_locate(const ra8_fs_mount_t* m, uint32_t* out_lba)
{
  uint8_t         boot[k_ra8_fs_bytes_per_sector] = {};
  const ra8_err_t err                             = priv_read_sector(m, 0U, boot);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t lba = (uint32_t)priv_rd16(&boot[k_fmt_off_f32_fsinfo]);
  *out_lba           = (uint32_t)k_fs_fsinfo_absent;
  if (lba == (uint32_t)k_fs_fsinfo_absent) {
    return k_ra8_ok;
  }
  if (lba >= m->reserved_sectors) {
    return k_ra8_ok;
  }
  *out_lba = lba;
  return k_ra8_ok;
}

/**
 * @brief Check the three FSInfo signatures in a candidate sector.
 *
 * @details MS FAT spec sec 5. All three must match before any field is
 *          believed; two of three is a sector that happens to start with
 *          "RRaA" and is not an FSInfo sector.
 *
 * @param[in] sec The candidate sector's 512 bytes.
 *
 * @return Whether the sector is a valid FSInfo sector.
 * @retval true  Lead, struct and trailing signatures all match.
 * @retval false Any one of them does not.
 *
 * @pre @p sec is non-NULL and addresses a whole sector.
 * @pre @p sec was read from the sector `BPB_FSInfo` names.
 * @post No state modified.
 * @post Result depends only on @p sec.
 *
 * @note Pure function; trivially thread-safe.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static bool priv_fsinfo_signatures_ok(const uint8_t* sec)
{
  if (priv_rd32(&sec[k_fmt_fsi_off_lead]) != (uint32_t)k_fmt_fsi_lead_sig) {
    return false;
  }
  if (priv_rd32(&sec[k_fmt_fsi_off_struct]) != (uint32_t)k_fmt_fsi_struct_sig) {
    return false;
  }
  return priv_rd32(&sec[k_fmt_fsi_off_trail]) == (uint32_t)k_fmt_fsi_trail_sig;
}

/* `priv_fsinfo_seed()`: see header for the documented contract. */
ra8_err_t priv_fsinfo_seed(const ra8_fs_mount_t* m)
{
  fat_alloc_state_t* st = priv_state_for(m);
  if (st == nullptr) {
    return k_ra8_ok;
  }
  if (m->type != k_ra8_fs_type_fat32) {
    return k_ra8_ok;
  }
  uint32_t  lba = (uint32_t)k_fs_fsinfo_absent;
  ra8_err_t err = priv_fsinfo_locate(m, &lba);
  if (err != k_ra8_ok) {
    return err;
  }
  if (lba == (uint32_t)k_fs_fsinfo_absent) {
    return k_ra8_ok;
  }
  uint8_t sec[k_ra8_fs_bytes_per_sector] = {};
  err                                    = priv_read_sector(m, lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  if (!priv_fsinfo_signatures_ok(sec)) {
    return k_ra8_ok;
  }
  st->fsinfo_lba      = lba;
  const uint32_t nxt  = priv_rd32(&sec[k_fmt_fsi_off_nxtfree]);
  const uint32_t free = priv_rd32(&sec[k_fmt_fsi_off_free]);
  if (nxt >= (uint32_t)k_cluster_first_data) {
    if ((nxt - (uint32_t)k_cluster_first_data) < m->count_of_clusters) {
      st->next_free = nxt;
    }
  }
  if (free <= m->count_of_clusters) {
    st->free_count = free;
  }
  return k_ra8_ok;
}

/* `priv_fsinfo_flush()`: see header for the documented contract. */
ra8_err_t priv_fsinfo_flush(const ra8_fs_mount_t* m)
{
  fat_alloc_state_t* st = priv_state_for(m);
  if (st == nullptr) {
    return k_ra8_ok;
  }
  if (st->fsinfo_lba == (uint32_t)k_fs_fsinfo_absent) {
    return k_ra8_ok;
  }
  if (st->dirty == 0U) {
    return k_ra8_ok;
  }
  uint8_t   sec[k_ra8_fs_bytes_per_sector] = {};
  ra8_err_t err                            = priv_read_sector(m, st->fsinfo_lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  priv_wr32(&sec[k_fmt_fsi_off_free], st->free_count);
  priv_wr32(&sec[k_fmt_fsi_off_nxtfree], st->next_free);
  err = priv_write_sector(m, st->fsinfo_lba, sec);
  if (err != k_ra8_ok) {
    return err;
  }
  st->dirty = 0U;
  return k_ra8_ok;
}
