/**
 * @file ra8_fs_fat_exfat_dir.c
 * @brief exFAT directories: path resolution, `mkdir`, `rmdir`.
 *
 * @details
 * Until this file existed, exFAT was a flat namespace here: every scan started
 * at `m->root_cluster`, so `"/logs/a.txt"` was not resolved but matched, after
 * slash-stripping, as one root-level name -- and `mkdir` / `rmdir` declined the
 * volume outright, because a driver with no directory-creation path has no
 * directory to remove either (#605).
 *
 * Three pieces, in the order a nested path uses them:
 *
 * 1. **Descent.** ::priv_exfat_enter looks one component up in the directory it
 *    is standing in and requires the Directory attribute; ::priv_exfat_resolve_parent
 *    drives it component by component, in the shape of the FAT side's
 *    ::priv_resolve_parent, and hands back the parent plus a pointer to the
 *    leaf. Iterative, never recursive (NASA P10 Rule 1).
 * 2. **Creation.** exFAT directories are structurally simpler than FAT's: the
 *    same File + Stream + Name entry set with bit 4 of `FileAttributes` set,
 *    the contents living in the run the Stream entry names, and **no "." /
 *    ".." entries at all** -- the parent is implicit, so there is no dot-entry
 *    packing and no back-link to maintain. An all-zero cluster is already a
 *    valid empty directory, because entry type 0x00 is end-of-directory.
 * 3. **Removal.** ::priv_exfat_rmdir proves the directory holds nothing before
 *    it frees anything, so a refusal costs the volume nothing.
 *
 * The run this file allocates is contiguous and carries NoFatChain, which is
 * why ::exfat_cursor_t has a run bound: the exFAT specification declares the
 * FAT entries of a NoFatChain run invalid, and the ones left behind here read
 * as 0 -- not end-of-chain -- so a FAT-following walk would step into cluster 0.
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
 * Path resolution
 * =============================================================================
 */

/**
 * @brief Descend into the named directory component within @p cur.
 *
 * @details Copies the (non-NUL-terminated) component into a local buffer, looks
 *          it up in @p cur, and requires the matched entry set to carry
 *          ::k_exfat_attr_directory and to name a real heap cluster. The exFAT
 *          counterpart of ::priv_enter_subdir.
 *
 * @param[in]  m    Mounted exFAT volume.
 * @param[in]  cur  Directory the component is looked up in.
 * @param[in]  comp Pointer to the component characters (not NUL-terminated).
 * @param[in]  len  Number of component characters.
 * @param[out] out  Receives the subdirectory location on success.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Component resolved to a subdirectory.
 * @retval k_ra8_err_invalid_arg    Component is over-long, or names a FILE.
 * @retval k_ra8_err_not_found      No such entry in @p cur.
 * @retval k_ra8_err_protocol_error The entry claims no data cluster.
 * @retval k_ra8_err_*              Backend read failure.
 *
 * @pre @p m, @p cur, @p comp and @p out are non-NULL.
 * @pre @p len is the exact component length (no trailing slash).
 * @post On success @p out locates the subdirectory.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_enter(const ra8_fs_mount_t* m,
                                  const exfat_dir_t*    cur,
                                  const char*           comp,
                                  uint32_t              len,
                                  exfat_dir_t*          out)
{
  if (len > (uint32_t)k_exfat_name_cap) {
    return k_ra8_err_invalid_arg;
  }
  char namebuf[(uint32_t)k_exfat_name_cap + 1U] = {};
  for (uint32_t i = 0U; i < len; i++) {
    namebuf[i] = comp[i];
  }
  namebuf[len]                              = '\0';
  uint8_t         strm[k_exfat_entry_bytes] = {};
  uint8_t         attr                      = 0U;
  const ra8_err_t e                         = priv_exfat_find(m, cur, namebuf, strm, &attr);
  if (e != k_ra8_ok) {
    return e;
  }
  if ((attr & (uint8_t)k_exfat_attr_directory) == 0U) {
    return k_ra8_err_invalid_arg; /* a file cannot be an intermediate component */
  }
  if (priv_rd32(&strm[k_exfat_strm_off_clus]) < (uint32_t)k_cluster_first_data) {
    return k_ra8_err_protocol_error; /* a directory always owns a cluster */
  }
  priv_exfat_dir_from_set(m, strm, out);
  return k_ra8_ok;
}

/* `priv_exfat_resolve_parent()`: see header for the documented contract. */
ra8_err_t priv_exfat_resolve_parent(const ra8_fs_mount_t* m,
                                    const char*           path,
                                    exfat_dir_t*          out_parent,
                                    const char**          out_leaf)
{
  const char* p = path;
  while (*p == '/') {
    p++;
  }
  exfat_dir_t cur = {};
  priv_exfat_dir_root(m, &cur);
  for (uint32_t depth = 0U; depth < (uint32_t)k_exfat_path_depth; depth++) {
    const char* end = p;
    while (*end != '\0') {
      if (*end == '/') {
        break;
      }
      end++;
    }
    if (*end == '\0') {
      *out_parent = cur;
      *out_leaf   = p;
      return k_ra8_ok;
    }
    exfat_dir_t     next = {};
    const ra8_err_t e    = priv_exfat_enter(m, &cur, p, (uint32_t)(end - p), &next);
    if (e != k_ra8_ok) {
      return e;
    }
    cur = next;
    p   = end;
    while (*p == '/') {
      p++;
    }
  }
  return k_ra8_err_invalid_arg; /* deeper than k_exfat_path_depth */
}

/* `priv_exfat_resolve_dir()`: see header for the documented contract. */
ra8_err_t priv_exfat_resolve_dir(const ra8_fs_mount_t* m, const char* path, exfat_dir_t* out)
{
  const char* p = path;
  while (*p == '/') {
    p++;
  }
  if (*p == '\0') {
    priv_exfat_dir_root(m, out);
    return k_ra8_ok;
  }
  exfat_dir_t     parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t e      = priv_exfat_resolve_parent(m, path, &parent, &leaf);
  if (e != k_ra8_ok) {
    return e;
  }
  const uint32_t len = priv_strlen(leaf);
  if (len == 0U) {
    *out = parent; /* trailing slash: the parent IS the directory */
    return k_ra8_ok;
  }
  return priv_exfat_enter(m, &parent, leaf, len, out);
}

/* `priv_exfat_lookup()`: see header for the documented contract. */
ra8_err_t priv_exfat_lookup(const ra8_fs_mount_t* m,
                            const char*           path,
                            uint8_t*              out_strm,
                            uint8_t*              out_attr)
{
  exfat_dir_t     parent = {};
  const char*     leaf   = nullptr;
  const ra8_err_t e      = priv_exfat_resolve_parent(m, path, &parent, &leaf);
  if (e != k_ra8_ok) {
    return e;
  }
  if (priv_strlen(leaf) == 0U) {
    return k_ra8_err_invalid_arg; /* the volume root is not an entry set */
  }
  return priv_exfat_find(m, &parent, leaf, out_strm, out_attr);
}

/* =============================================================================
 * mkdir
 * =============================================================================
 */

/**
 * @brief Zero every sector of one cluster.
 *
 * @details An all-zero exFAT directory cluster is a valid EMPTY directory: the
 *          first entry's type byte is 0x00, which is the end-of-directory
 *          marker. There are no "." / ".." entries to stamp -- exFAT has none.
 *
 * @param[in] m    Mounted exFAT volume.
 * @param[in] clus Cluster to clear.
 *
 * @return Error code.
 * @retval k_ra8_ok    Every sector of the cluster reads as zero.
 * @retval k_ra8_err_* Backend write failure.
 *
 * @pre @p m is non-NULL; @p clus is a heap cluster.
 * @pre The cluster is not referenced by any entry set yet.
 * @post On success the cluster is an empty exFAT directory.
 * @post No bitmap or directory state is modified here.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_zero_cluster(const ra8_fs_mount_t* m, uint32_t clus)
{
  const uint8_t  zero[k_ra8_fs_bytes_per_sector] = {};
  const uint32_t base                            = priv_cluster_to_lba(m, clus);
  for (uint32_t s = 0U; s < m->sectors_per_cluster; s++) {
    const ra8_err_t e = priv_write_sector(m, base + s, zero);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Build the File + Stream + Name entry set a DIRECTORY needs.
 *
 * @details The file-side builder in `ra8_fs_fat_exfat_write.c` makes a
 *          zero-length, cluster-less set, because a streaming write starts with
 *          nothing and grows (#602). A directory is the opposite: it is born
 *          owning exactly one cluster and never grows here, so its set carries
 *          the Directory attribute, the cluster, NoFatChain, and a DataLength
 *          equal to the ALLOCATION -- an exFAT directory's length is its
 *          allocation, not a byte count, and the specification requires a
 *          directory's ValidDataLength to equal DataLength. Both are written.
 *
 * @param[in]  m     Mounted exFAT volume.
 * @param[out] set   Buffer of at least ::k_exfat_max_set_bytes bytes.
 * @param[in]  name  Leaf name (ASCII).
 * @param[in]  nlen  Name length in characters.
 * @param[in]  first The directory's own first cluster.
 *
 * @return The total byte length of the built set.
 * @retval >0 Number of bytes written into @p set.
 *
 * @pre @p m, @p set and @p name are non-NULL; @p set is large enough.
 * @pre `priv_strlen(name) == nlen` and @p nlen is within ::k_exfat_name_cap.
 * @post @p set holds a complete entry set with a valid SetChecksum.
 * @post No volume state is modified.
 *
 * @note Not thread-safe against ::ra8_fs_set_clock; install the clock first.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static uint32_t priv_exfat_build_dir_set(const ra8_fs_mount_t* m,
                                         uint8_t*              set,
                                         const char*           name,
                                         uint32_t              nlen,
                                         uint32_t              first)
{
  const uint32_t name_entries =
    (nlen + (uint32_t)k_exfat_name_per_entry - 1U) / (uint32_t)k_exfat_name_per_entry;
  const uint32_t sec_count = 1U + name_entries;
  const uint32_t total     = (1U + sec_count) * (uint32_t)k_exfat_entry_bytes;
  for (uint32_t i = 0U; i < total; i++) {
    set[i] = 0U;
  }
  set[0]                      = (uint8_t)k_exfat_entry_file;
  set[k_exfat_off_file_secnt] = (uint8_t)sec_count;
  priv_wr16(&set[k_exfat_off_file_attr], (uint16_t)k_exfat_attr_directory);
  /* Before the SetChecksum below, which covers these bytes (#601). */
  priv_exfat_file_stamp_create(set);
  const uint32_t cbytes        = m->sectors_per_cluster * k_ra8_fs_bytes_per_sector;
  uint8_t*       strm          = &set[k_exfat_entry_bytes];
  strm[0]                      = (uint8_t)k_exfat_entry_stream;
  strm[k_exfat_strm_off_flags] = (uint8_t)k_exfat_secflag_alloc;
  strm[k_exfat_strm_off_nlen]  = (uint8_t)nlen;
  priv_wr16(&strm[k_exfat_off_strm_hash], priv_exfat_name_hash(name, nlen));
  priv_wr32(&strm[k_exfat_off_strm_valid], cbytes);
  priv_wr32(&strm[k_exfat_strm_off_clus], first);
  priv_wr32(&strm[k_exfat_strm_off_dlen], cbytes);
  for (uint32_t n = 0U; n < name_entries; n++) {
    uint8_t* ne = &set[(size_t)(2U + n) * (size_t)k_exfat_entry_bytes];
    ne[0]       = (uint8_t)k_exfat_entry_name;
    for (uint32_t c = 0U; c < (uint32_t)k_exfat_name_per_entry; c++) {
      const uint32_t pos = (n * (uint32_t)k_exfat_name_per_entry) + c;
      if (pos < nlen) {
        ne[k_exfat_name_off + (c * 2U)] = (uint8_t)name[pos];
      }
    }
  }
  priv_wr16(&set[k_exfat_off_file_csum], priv_exfat_set_checksum(set, total));
  return total;
}

/**
 * @brief Allocate and initialise one cluster as an empty exFAT directory.
 *
 * @details Scans the allocation bitmap for a single free cluster, zeroes it,
 *          and only then marks the bit. That order is deliberate: a failure
 *          while zeroing leaves a cluster that is still free, so the volume
 *          loses nothing, whereas marking first would leak it.
 *
 * @param[in,out] m        Mounted exFAT volume.
 * @param[out]    out_clus Receives the new directory's cluster.
 * @param[out]    out_bmp  Receives the bitmap's first LBA (for rollback).
 *
 * @return Error code.
 * @retval k_ra8_ok         Cluster allocated, zeroed and marked used.
 * @retval k_ra8_err_no_mem The volume has no free cluster.
 * @retval k_ra8_err_*      Bitmap or backend failure.
 *
 * @pre @p m, @p out_clus and @p out_bmp are non-NULL; the mount is exFAT.
 * @pre The bitmap region is contiguous on disk.
 * @post On success the cluster is zeroed and its bitmap bit is set.
 * @post On failure no bitmap bit is set.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_dir_alloc(const ra8_fs_mount_t* m, uint32_t* out_clus, uint32_t* out_bmp)
{
  uint32_t  bclus = 0U;
  uint32_t  blen  = 0U;
  ra8_err_t e     = priv_exfat_find_bitmap(m, &bclus, &blen);
  if (e != k_ra8_ok) {
    return e;
  }
  *out_bmp = priv_cluster_to_lba(m, bclus);
  e        = priv_exfat_bitmap_scan(m, *out_bmp, 1U, out_clus);
  if (e != k_ra8_ok) {
    return e;
  }
  e = priv_exfat_zero_cluster(m, *out_clus);
  if (e != k_ra8_ok) {
    return e;
  }
  e = priv_exfat_bitmap_mark(m, *out_bmp, *out_clus, 1U);
  if (e != k_ra8_ok) {
    return e;
  }
  /* Only now, with the bit actually set, is it true that the next free cluster
   * starts past this one (#607). */
  priv_alloc_hint_set(m, *out_clus + 1U);
  return k_ra8_ok;
}

/**
 * @brief Decide whether @p leaf may be created in @p parent.
 *
 * @details Length first, then the name lookup. Split out of ::priv_exfat_mkdir
 *          so that function stays inside the function-size gate.
 *
 * @param[in] m      Mounted exFAT volume.
 * @param[in] parent Directory the name would be created in.
 * @param[in] leaf   Leaf name (ASCII).
 * @param[in] nlen   Length of @p leaf.
 *
 * @return Error code.
 * @retval k_ra8_ok              The name is free and storable.
 * @retval k_ra8_err_invalid_arg @p leaf is empty (the root) or over-long.
 * @retval k_ra8_err_exists      A file or directory already has that name.
 * @retval k_ra8_err_*           Backend read failure.
 *
 * @pre @p m, @p parent and @p leaf are non-NULL; the mount is exFAT.
 * @pre ``priv_strlen(leaf) == nlen``.
 * @post No volume state is modified.
 * @post On k_ra8_ok nothing in @p parent answers to @p leaf.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_mkdir_check(const ra8_fs_mount_t* m,
                                        const exfat_dir_t*    parent,
                                        const char*           leaf,
                                        uint32_t              nlen)
{
  if (nlen == 0U) {
    return k_ra8_err_invalid_arg; /* "/" or a trailing slash: the root itself */
  }
  if (nlen > (uint32_t)k_exfat_name_cap) {
    return k_ra8_err_invalid_arg;
  }
  uint8_t strm[k_exfat_entry_bytes] = {};
  uint8_t attr                      = 0U;
  /* mkdir REFUSES an existing name rather than replacing it, unlike
   * ra8_fs_write_file(), which is a create-or-replace verb (#603). A directory
   * and a file are both an entry set under that name, so both refuse here.
   *
   * The third verdict is the one that is easy to drop: a BACKEND FAILURE is not
   * "the name is absent". Testing only for k_ra8_ok would turn an unreadable
   * directory into a green light to create, and the volume would then hold two
   * entry sets for one name -- the #603 duplicate, reintroduced through the
   * error path instead of the happy one. */
  const ra8_err_t fe = priv_exfat_find(m, parent, leaf, strm, &attr);
  if (fe == k_ra8_ok) {
    return k_ra8_err_exists;
  }
  if (fe != k_ra8_err_not_found) {
    return fe;
  }
  return k_ra8_ok;
}

/* `priv_exfat_mkdir()`: see header for the documented contract. */
ra8_err_t priv_exfat_mkdir(const ra8_fs_mount_t* m, const char* path)
{
  exfat_dir_t parent = {};
  const char* leaf   = nullptr;
  ra8_err_t   e      = priv_exfat_resolve_parent(m, path, &parent, &leaf);
  if (e != k_ra8_ok) {
    return e;
  }
  const uint32_t nlen = priv_strlen(leaf);
  e                   = priv_exfat_mkdir_check(m, &parent, leaf, nlen);
  if (e != k_ra8_ok) {
    return e;
  }
  /* Reserve the parent's slot run BEFORE allocating, so a full parent directory
   * costs no bitmap churn and needs no rollback. */
  const uint32_t need =
    2U + ((nlen + (uint32_t)k_exfat_name_per_entry - 1U) / (uint32_t)k_exfat_name_per_entry);
  uint32_t dclus = 0U;
  uint32_t didx  = 0U;
  e              = priv_exfat_find_dir_space(m, &parent, need, &dclus, &didx);
  if (e != k_ra8_ok) {
    return e;
  }
  uint32_t newclus = 0U;
  uint32_t bmp_lba = 0U;
  e                = priv_exfat_dir_alloc(m, &newclus, &bmp_lba);
  if (e != k_ra8_ok) {
    return e;
  }
  uint8_t        set[k_exfat_max_set_bytes] = {};
  const uint32_t bytes = priv_exfat_build_dir_set(m, set, leaf, nlen, newclus);
  e                    = priv_exfat_write_dir_set(m, dclus, didx, set, bytes);
  if (e != k_ra8_ok) {
    (void)priv_exfat_bitmap_clear(m, bmp_lba, newclus, 1U); /* never leak the cluster */
    return e;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * rmdir
 * =============================================================================
 */

/**
 * @brief Decide whether an exFAT directory holds any live entry.
 *
 * @details Walks the directory and stops at the first definite answer. Only two
 *          things count as "not an occupant": the end-of-directory marker
 *          (0x00), which ends the walk, and an entry whose in-use bit is clear,
 *          which is a deleted remnant. Everything else -- a File entry, a
 *          Stream entry, a Name entry, or any typed entry another
 *          implementation left -- is an occupant, exactly as the FAT side's
 *          ::priv_dir_is_empty treats a live 8.3 entry (#604).
 *
 *          exFAT has no "." / ".." entries, so unlike FAT there is nothing to
 *          discount before the count starts: an empty exFAT directory really is
 *          an empty cluster.
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[in]  dir       The directory under test.
 * @param[out] out_empty Receives 1 when the directory is removable, else 0.
 *
 * @return Error code.
 * @retval k_ra8_ok    Verdict written to ``*out_empty``.
 * @retval k_ra8_err_* Backend read failure.
 *
 * @pre @p m, @p dir and @p out_empty are non-NULL.
 * @pre @p dir was built from an entry set carrying the directory attribute.
 * @post On success ``*out_empty`` is 0 or 1.
 * @post No volume state is modified.
 *
 * @note The walk is bounded twice over: by the directory's own run (or FAT
 *       chain) and by ::k_exfat_scan_limit (NASA Power of 10 Rule 2).
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
priv_exfat_dir_is_empty(const ra8_fs_mount_t* m, const exfat_dir_t* dir, uint8_t* out_empty)
{
  exfat_cursor_t cur = {};
  priv_exfat_cursor_init(dir, &cur);
  while (cur.scanned < (uint32_t)k_exfat_scan_limit) {
    uint8_t         e[k_exfat_entry_bytes] = {};
    const ra8_err_t r                      = priv_exfat_next_entry(m, &cur, e);
    if (r == k_ra8_err_not_found) {
      *out_empty = 1U; /* the run ended without an occupant and without a 0x00 */
      return k_ra8_ok;
    }
    if (r != k_ra8_ok) {
      return r;
    }
    if (e[0] == (uint8_t)k_exfat_entry_eod) {
      *out_empty = 1U;
      return k_ra8_ok;
    }
    if ((e[0] & (uint8_t)k_exfat_inuse_bit) == 0U) {
      continue; /* a deleted remnant is not an occupant */
    }
    *out_empty = 0U;
    return k_ra8_ok;
  }
  *out_empty = 1U; /* GCOVR_EXCL_LINE -- k_exfat_scan_limit (65536) entries required */
  return k_ra8_ok; /* GCOVR_EXCL_LINE                                                */
}

/**
 * @brief Resolve a path to a removable exFAT directory's entry set.
 *
 * @details Resolves the parent, refuses the volume root, locates the leaf's
 *          entry set with per-entry positions, and requires it to carry
 *          ::k_exfat_attr_directory and to name a real heap cluster. Split out
 *          of ::priv_exfat_rmdir so both stay inside the function-size gate.
 *
 * @param[in]  m         Mounted exFAT volume.
 * @param[in]  path      Directory path to remove.
 * @param[out] pos       Receives the set's per-entry positions.
 * @param[out] out_count Receives the entry count (1 + SecondaryCount).
 * @param[out] strm      Receives the 32-byte Stream-extension entry.
 * @param[out] out_dir   Receives the directory's own location.
 *
 * @return Error code.
 * @retval k_ra8_ok                 Located; outputs populated.
 * @retval k_ra8_err_invalid_arg    @p path is the root, or names a FILE.
 * @retval k_ra8_err_not_found      No such entry, or a component is missing.
 * @retval k_ra8_err_protocol_error The entry claims no data cluster.
 * @retval k_ra8_err_*              Backend read failure.
 *
 * @pre Every pointer argument is non-NULL; the mount is exFAT.
 * @pre @p pos has ::k_exfat_set_max_entries slots.
 * @post On success the outputs describe an on-disk directory entry set.
 * @post No volume state is modified.
 *
 * @note Not thread-safe; callers serialise.
 *
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t priv_exfat_rmdir_locate(const ra8_fs_mount_t* m,
                                         const char*           path,
                                         exfat_setpos_t*       pos,
                                         uint32_t*             out_count,
                                         uint8_t*              strm,
                                         exfat_dir_t*          out_dir)
{
  exfat_dir_t parent = {};
  const char* leaf   = nullptr;
  ra8_err_t   e      = priv_exfat_resolve_parent(m, path, &parent, &leaf);
  if (e != k_ra8_ok) {
    return e;
  }
  if (priv_strlen(leaf) == 0U) {
    return k_ra8_err_invalid_arg; /* "/" or a trailing slash: the root itself */
  }
  uint8_t file_e[k_exfat_entry_bytes] = {};
  e                                   = priv_exfat_find_set(m,
                                                            &parent,
                                                            leaf,
                                                            pos,
                                                            (uint32_t)k_exfat_set_max_entries,
                                                            out_count,
                                                            file_e,
                                                            strm);
  if (e != k_ra8_ok) {
    return e;
  }
  if ((file_e[k_exfat_off_file_attr] & (uint8_t)k_exfat_attr_directory) == 0U) {
    return k_ra8_err_invalid_arg; /* a file: ra8_fs_unlink() is the verb for it */
  }
  if (priv_rd32(&strm[k_exfat_strm_off_clus]) < (uint32_t)k_cluster_first_data) {
    return k_ra8_err_protocol_error; /* a directory always owns a cluster */
  }
  priv_exfat_dir_from_set(m, strm, out_dir);
  return k_ra8_ok;
}

/* `priv_exfat_rmdir()`: see header for the documented contract. */
ra8_err_t priv_exfat_rmdir(const ra8_fs_mount_t* m, const char* path)
{
  exfat_setpos_t pos[k_exfat_set_max_entries]  = {};
  uint32_t       count                     = 0U;
  uint8_t        strm[k_exfat_entry_bytes] = {};
  exfat_dir_t    victim                    = {};
  ra8_err_t      e = priv_exfat_rmdir_locate(m, path, pos, &count, strm, &victim);
  if (e != k_ra8_ok) {
    return e;
  }
  uint8_t empty = 0U;
  e             = priv_exfat_dir_is_empty(m, &victim, &empty);
  if (e != k_ra8_ok) {
    return e;
  }
  if (empty == 0U) {
    return k_ra8_err_not_empty;
  }
  /* Entries first, clusters second -- the same order ::priv_exfat_unlink uses.
   * A fault between the two leaves clusters marked used with nothing pointing
   * at them, which a host `fsck` reports and repairs; the reverse order would
   * leave a live entry set pointing at free space, which it cannot. */
  e = priv_exfat_drop_set(m, pos, count);
  if (e != k_ra8_ok) {
    return e;
  }
  return priv_exfat_free_clusters(m, strm);
}
