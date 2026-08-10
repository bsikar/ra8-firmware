/**
 * @file test_ra8_fs_unicode_exfat.c
 * @brief A non-ASCII name on exFAT, and the NameHash a host will recompute (#606).
 *
 * @details
 * exFAT mangled a name in two directions at once. The reader copied the LOW
 * BYTE of each UTF-16 unit, so U+00E9 came back as the single byte 0xE9 -- not
 * valid UTF-8 -- and U+4F60 came back as a backtick. The writer did the mirror
 * image: one UTF-16 unit per INPUT BYTE, so a three-byte character became three
 * garbage characters and `NameLength` counted bytes where the format counts
 * units. And `priv_exfat_name_hash()` up-cased with an ASCII-only rule, so the
 * `NameHash` stored on disk was not the one a compliant implementation
 * computes from the volume's own up-case table.
 *
 * That last one is the serious one, and it is why this suite goes further than
 * a round trip. `NameHash` is the index a host probes with: a host can list a
 * file it then cannot open. So the hash is checked against a value this test
 * computes ITSELF, from:
 *
 *   - a UTF-16 unit array written out by hand, not produced by the codec under
 *     test;
 *   - the up-case table read back off the FORMATTED VOLUME and expanded here,
 *     not the one the driver embeds;
 *   - a rotate-add loop written from the specification, not called into.
 *
 * All three could agree with the driver only by both being right.
 *
 * @par Pure-ASCII sources:
 * Every non-ASCII name is a `static const char[]` of byte escapes, and every
 * expected unit array a `uint16_t[]`. No non-ASCII literal appears here.
 *
 * @par Out-of-band evidence:
 * Every case that leaves a non-ASCII name on the volume ends by calling
 * `unicode_dump_image()` from the shared fixture, so `RA8_EXFAT_DUMP_DIR=<dir>`
 * turns this suite into a set of mountable exFAT volumes -- one per UTF-8 form
 * -- and a real operating system can be asked the question no assertion in this
 * file can: whether it DISPLAYS the name we stored. Unset, nothing is written.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_fs_fat_internal.h"
#include "support/fs_fat_exfat_mutate_test_util.h"
#include "support/fs_unicode_exfat_test_util.h"
#include "unity_minimal.h"

/* ===========================================================================
 * Reading the volume's own up-case table
 * ===========================================================================
 */

/**
 * @var s_volume_upcase
 * @brief The mounted volume's up-case table, expanded to one entry per unit.
 * @details Filled by ::load_volume_upcase() from the bytes the FORMATTER wrote,
 *          so every fold this suite checks against comes off the disk rather
 *          than out of the driver. 128 KiB of test-process memory, which is
 *          nothing on a host and never exists on the target.
 * @note Not thread-safe; one suite, one process.
 * @since 0.1.0
 */
static uint16_t s_volume_upcase[k_ux_bmp_units];

/**
 * @brief Absolute byte offset of @p cluster's first byte in the RAM disk.
 *
 * @param[in] h       Mounted exFAT volume.
 * @param[in] cluster Cluster number (2-based, as the format counts).
 *
 * @return Offset within `s_disk.bytes`.
 * @retval 0..byte_count The cluster's first byte.
 *
 * @pre @p h is mounted and backed by the fixture's RAM disk.
 * @pre @p cluster is at least 2 and inside the heap.
 * @post No state is modified.
 * @post The result is partition-adjusted, like ::root_byte().
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint32_t cluster_byte(const ra8_fs_mount_t* h, uint32_t cluster)
{
  const uint32_t lba =
    h->partition_base_lba + h->first_data_lba + ((uint64_t)(cluster - 2U) * h->sectors_per_cluster);
  return lba * (uint32_t)k_mut_block_size;
}

/**
 * @brief Read a little-endian 32-bit value out of the RAM disk.
 *
 * @param[in] off Byte offset within `s_disk.bytes`.
 *
 * @return The value.
 * @retval 0..0xFFFFFFFF Whatever the four bytes hold.
 *
 * @pre `s_disk.bytes` is allocated and @p off + 3 is inside it.
 * @pre The caller wants little-endian order.
 * @post No state is modified.
 * @post Independent of the driver's own accessors, deliberately.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint32_t disk_rd32(uint32_t off)
{
  return disk_rd16(off) | (disk_rd16(off + 2U) << (uint32_t)k_mut_shift_byte16);
}

/**
 * @brief Decode the compressed up-case table at @p base into ::s_volume_upcase.
 *
 * @details The specification's run-length form, decoded from the document
 *          rather than by calling the driver. Split out of ::load_volume_upcase
 *          so neither half crosses the statement gate.
 *
 * @param[in] base  Byte offset of the table's first word in the RAM disk.
 * @param[in] words Number of 16-bit words the table occupies.
 *
 * @return Nothing.
 *
 * @pre ::s_volume_upcase has been seeded with the identity.
 * @pre @p base addresses @p words readable words in `s_disk.bytes`.
 * @post ::s_volume_upcase holds the volume's mapping for every BMP unit.
 * @post No volume state is modified.
 *
 * @note Not thread-safe (writes the suite singleton).
 * @since 0.1.0
 */
static void expand_volume_upcase(uint32_t base, uint32_t words)
{
  uint32_t idx = 0U;
  uint32_t w   = 0U;
  while ((w < words) && (idx < (uint32_t)k_ux_bmp_units)) {
    const uint32_t v = disk_rd16(base + (w * 2U));
    w++;
    if (v != (uint32_t)k_ux_run_tag) {
      s_volume_upcase[idx] = (uint16_t)v;
      idx++;
      continue;
    }
    if (w >= words) {
      break; /* the trailing tag with no length: U+FFFF maps to itself */
    }
    idx += disk_rd16(base + (w * 2U));
    w++;
  }
}

/**
 * @brief Expand the mounted volume's compressed up-case table into ::s_volume_upcase.
 *
 * @details The specification's compressed encoding, decoded here from the
 *          document rather than by calling the driver: a unit that is not
 *          0xFFFF is the mapping for the next code point, and 0xFFFF introduces
 *          a run length of code points that map to themselves.
 *
 * @param[in]  h        Mounted exFAT volume.
 * @param[out] out_csum Receives the up-case entry's stored TableChecksum.
 *
 * @return Nothing; a missing table is reported by the harness.
 *
 * @pre @p h is mounted, exFAT, and backed by the fixture's RAM disk.
 * @pre @p out_csum is non-NULL.
 * @post ::s_volume_upcase holds one entry per BMP code unit.
 * @post No volume state is modified.
 *
 * @note Not thread-safe (writes the suite singleton).
 * @since 0.1.0
 */
static void load_volume_upcase(const ra8_fs_mount_t* h, uint32_t* out_csum)
{
  for (uint32_t u = 0U; u < (uint32_t)k_ux_bmp_units; u++) {
    s_volume_upcase[u] = (uint16_t)u;
  }
  uint32_t entry_off = 0U;
  uint8_t  found     = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_ux_scan_slots; i++) {
    const uint32_t off = root_byte(h, i);
    if (s_disk.bytes[off] == (uint8_t)k_ux_type_upcase) {
      entry_off = off;
      found     = 1U;
      break;
    }
  }
  if (found == 0U) {
    TEST_FAIL_FMT("%s", "no up-case directory entry on the formatted volume");
    return;
  }
  *out_csum              = disk_rd32(entry_off + (uint32_t)k_ux_upc_off_csum);
  const uint32_t cluster = disk_rd32(entry_off + (uint32_t)k_mut_strm_off_clus);
  const uint32_t bytes   = disk_rd32(entry_off + (uint32_t)k_mut_strm_off_dlen);
  expand_volume_upcase(cluster_byte(h, cluster), bytes / 2U);
}

/**
 * @brief Compute an exFAT NameHash from first principles.
 *
 * @details The specification's definition, written out here: fold the UP-CASED
 *          name one byte at a time, low byte then high byte, rotating the
 *          accumulator right and adding. The fold comes from ::s_volume_upcase,
 *          which came off the disk, so nothing in this function shares an
 *          implementation with what it checks.
 *
 * @param[in] units Name as UTF-16 code units.
 * @param[in] n     Number of units.
 *
 * @return The hash.
 * @retval 0..0xFFFF The folded value.
 *
 * @pre @p units holds at least @p n units.
 * @pre ::load_volume_upcase() has run for the volume in question.
 * @post No state is modified.
 * @post The result depends only on the inputs and the volume's table.
 *
 * @note Not thread-safe (reads the suite singleton).
 * @since 0.1.0
 */
static uint32_t expected_name_hash(const uint16_t* units, uint32_t n)
{
  uint32_t h = 0U;
  for (uint32_t i = 0U; i < n; i++) {
    const uint32_t u  = (uint32_t)s_volume_upcase[units[i]];
    const uint32_t lo = u & (uint32_t)k_mut_mask_byte;
    const uint32_t hi = (u >> (uint32_t)k_mut_shift_byte8) & (uint32_t)k_mut_mask_byte;
    h                 = ((((h & 1U) != 0U) ? (uint32_t)k_ux_csum_hi_bit : 0U) + (h >> 1U) + lo) &
                        (uint32_t)k_ux_csum_mask;
    h                 = ((((h & 1U) != 0U) ? (uint32_t)k_ux_csum_hi_bit : 0U) + (h >> 1U) + hi) &
                        (uint32_t)k_ux_csum_mask;
  }
  return h;
}

/**
 * @brief `ra8_fs_listdir()` callback recording whether @p want was reported.
 *
 * @param[in] name Entry name as the driver chose to report it.
 * @param[in] attr Unused.
 * @param[in] size Unused.
 * @param[in] ctx  Pointer to a `const char*` holding the name looked for; the
 *                 pointer is set to nullptr once it has been seen.
 *
 * @return Nothing.
 *
 * @pre @p name and @p ctx are non-NULL.
 * @pre `*ctx` is the name being looked for, or already nullptr.
 * @post `*ctx` is nullptr once a byte-exact match has been reported.
 * @post No other state is modified.
 *
 * @note Not thread-safe against the same @p ctx.
 * @since 0.1.0
 */
static void seen_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)attr;
  (void)size;
  const char** want = (const char**)ctx;
  if ((*want != nullptr) && (strcmp(name, *want) == 0)) {
    *want = nullptr;
  }
}

/**
 * @brief Does a root listing report @p want, byte for byte?
 *
 * @param[in,out] h    Mounted exFAT volume.
 * @param[in]     want The name expected, as the caller spelled it.
 *
 * @return Presence flag.
 * @retval 1 The name was reported exactly.
 * @retval 0 It was absent or reported under another spelling.
 *
 * @pre @p h is mounted and @p want is non-NULL.
 * @pre The root directory is the only namespace exFAT has here.
 * @post No state is modified.
 * @post The comparison was byte-exact, so a substitution would fail it.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint8_t listing_holds(ra8_fs_mount_t* h, const char* want)
{
  const char* probe = want;
  if (ra8_fs_listdir(h, "/", seen_cb, (void*)&probe) != k_ra8_ok) {
    return 0U;
  }
  return (probe == nullptr) ? 1U : 0U;
}

/* ===========================================================================
 * Tests
 * ===========================================================================
 */

/**
 * @test test_exfat_non_ascii_round_trip
 * @brief Create, list, re-open, rename and unlink, for 2-, 3- and 4-byte names.
 *
 * @details exFAT's whole-file write path, its scan, its in-place rename and its
 *          unlink each reach the name through a different helper, and all four
 *          had to stop counting bytes. The four-byte name is the one that
 *          proves surrogate pairs travel: six units from eight bytes, so a
 *          `NameLength` derived from `strlen` would be two too many and the
 *          scan would never match the set it just wrote.
 *
 * @par MC/DC:
 * No new compound decision; the composition of what
 * `tests/test_ra8_fs_utf.c` isolates, wired to the exFAT paths.
 *
 * @pre A formatted exFAT volume; nothing open.
 * @post Each name was created, listed, re-opened, renamed and unlinked.
 *
 * @since 0.1.0
 */
static void test_exfat_non_ascii_round_trip(void)
{
  TEST_BEGIN("exfat unicode: 2-, 3- and 4-byte names survive every verb");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const char* names[] = {s_acc_u8, s_cjk_u8, s_emoji_u8};
  const char* tags[]  = {"unicode_name_2byte", "unicode_name_3byte", "unicode_name_4byte"};
  uint8_t     payload[k_ux_payload] = {};
  for (uint32_t i = 0U; i < (uint32_t)k_ux_payload; i++) {
    payload[i] = (uint8_t)(i + 1U);
  }

  for (uint32_t i = 0U; i < (uint32_t)(sizeof(names) / sizeof(names[0])); i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, names[i], payload, (uint32_t)k_ux_payload));
    TEST_ASSERT_EQ(1U, listing_holds(h, names[i]));
    /* One image per UTF-8 form, dumped while the name is still on the volume:
     * the rename and unlink below take it away again, and an image of an empty
     * root proves nothing about how a host renders anything. Separate tags so a
     * host that mis-renders exactly one form names which one. */
    unicode_dump_image(tags[i], h->partition_base_lba);

    /* Re-open by the same name and read the bytes back. */
    ra8_fs_file_t* f                  = nullptr;
    uint8_t        back[k_ux_payload] = {};
    uint32_t       got                = 0U;
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, names[i], k_ra8_fs_mode_read, &f));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(f, back, (uint32_t)k_ux_payload, &got));
    TEST_ASSERT_EQ(k_ux_payload, got);
    TEST_ASSERT_EQ(0, memcmp(payload, back, (size_t)k_ux_payload));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

    /* Rename to another non-ASCII name, then take it away. */
    char renamed[k_ux_path_cap] = {};
    (void)snprintf(renamed, sizeof(renamed), "x%s", names[i]);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_rename(h, names[i], renamed));
    TEST_ASSERT_EQ(0U, listing_holds(h, names[i]));
    TEST_ASSERT_EQ(1U, listing_holds(h, renamed));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unlink(h, renamed));
    TEST_ASSERT_EQ(0U, listing_holds(h, renamed));
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat unicode: 2-, 3- and 4-byte names survive every verb");
}

/**
 * @test test_exfat_name_entries_hold_real_units
 * @brief The Name entries carry whole UTF-16 units, and NameLength counts them.
 *
 * @details Read straight off the disk and compared against a unit array written
 *          out by hand. Before the fix this test would have found nine units
 *          (one per UTF-8 byte) where the specification wants eight, with the
 *          two bytes of U+00E9 sitting in two separate Latin-1-ish units.
 *
 * @par MC/DC:
 * Decision: `if (pos < nlen)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_write.c@priv_exfat_build_set` (1 cond).
 * - V1: inside the name -> false arm -> a unit is written (asserted here).
 * - V2: past it         -> true arm  -> the slot stays zero (asserted here).
 *
 * @pre A formatted exFAT volume; nothing open.
 * @post NameLength and every stored unit matched the hand-written expectation.
 *
 * @since 0.1.0
 */
static void test_exfat_name_entries_hold_real_units(void)
{
  TEST_BEGIN("exfat unicode: Name entries hold units, NameLength counts units");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t payload = (uint8_t)'x';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, s_acc_u8, &payload, 1U));

  const uint32_t strm = root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  const uint32_t name = root_byte(h, (uint32_t)k_mut_root_name0_idx);
  const uint32_t want = (uint32_t)(sizeof(s_acc_units) / sizeof(s_acc_units[0]));

  TEST_ASSERT_EQ(want, s_disk.bytes[strm + (uint32_t)k_ux_strm_off_len]);
  for (uint32_t i = 0U; i < want; i++) {
    TEST_ASSERT_EQ(s_acc_units[i], disk_rd16(name + (uint32_t)k_ux_name_off + (i * 2U))); /* V1 */
  }
  /* The slots past the name stay zero, which is what a reader uses to stop. */
  TEST_ASSERT_EQ(0U, disk_rd16(name + (uint32_t)k_ux_name_off + (want * 2U))); /* V2 */

  unicode_dump_image("unicode_name_units", h->partition_base_lba);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat unicode: Name entries hold units, NameLength counts units");
}

/**
 * @test test_exfat_name_hash_matches_an_independent_computation
 * @brief The stored NameHash is the one a compliant reader recomputes.
 *
 * @details The case the whole exFAT half turns on. The expected value is built
 *          from a hand-written unit array, the up-case table read back off the
 *          volume, and a rotate-add loop written from the specification -- so
 *          agreeing with the driver is not something either side can arrange.
 *          Three names, one per UTF-8 length, because the surrogate pair is the
 *          one case where a unit falls outside the table entirely and must fold
 *          to itself.
 *
 * @par MC/DC:
 * Decision: the `priv_exfat_csum_add` fold in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_write.c@priv_exfat_name_hash`, driven
 * through its wrap condition `((cs & 1U) != 0U)` (1 condition). Three names of
 * different lengths and contents drive both arms many times over; the assertion
 * is exact equality with an independently computed value, which is a stronger
 * statement than either arm on its own.
 *
 * @pre A formatted exFAT volume; nothing open.
 * @post Every stored NameHash equalled the independently computed one, and the
 *       ASCII control equalled it too.
 *
 * @since 0.1.0
 */
static void test_exfat_name_hash_matches_an_independent_computation(void)
{
  TEST_BEGIN("exfat unicode: the stored NameHash is the one a host recomputes");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint32_t stored_csum = 0U;
  load_volume_upcase(h, &stored_csum);

  const char*     names[]  = {s_acc_u8, s_cjk_u8, s_emoji_u8};
  const uint16_t* units[]  = {s_acc_units, s_cjk_units, s_emoji_units};
  const uint32_t  counts[] = {(uint32_t)(sizeof(s_acc_units) / sizeof(s_acc_units[0])),
                              (uint32_t)(sizeof(s_cjk_units) / sizeof(s_cjk_units[0])),
                              (uint32_t)(sizeof(s_emoji_units) / sizeof(s_emoji_units[0]))};

  const uint8_t payload = (uint8_t)'x';
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(names) / sizeof(names[0])); i++) {
    build_exfat_volume();
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
    load_volume_upcase(h, &stored_csum);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, names[i], &payload, 1U));

    const uint32_t strm = root_byte(h, (uint32_t)k_mut_root_strm0_idx);
    TEST_ASSERT_EQ(counts[i], s_disk.bytes[strm + (uint32_t)k_ux_strm_off_len]);
    TEST_ASSERT_EQ(expected_name_hash(units[i], counts[i]),
                   disk_rd16(strm + (uint32_t)k_ux_strm_off_hsh));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  }

  /* The ASCII control: the same machinery on a name that has no non-ASCII unit
   * at all, so a hash that happened to be right only for ASCII would look no
   * different here -- which is why it is the control and not the evidence. */
  build_exfat_volume();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  load_volume_upcase(h, &stored_csum);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "PLAIN.TXT", &payload, 1U));
  static const uint16_t s_plain_units[] = {'P', 'L', 'A', 'I', 'N', '.', 'T', 'X', 'T'};
  const uint32_t        strm            = root_byte(h, (uint32_t)k_mut_root_strm0_idx);
  TEST_ASSERT_EQ(expected_name_hash(s_plain_units, 9U),
                 disk_rd16(strm + (uint32_t)k_ux_strm_off_hsh));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat unicode: the stored NameHash is the one a host recomputes");
}

/**
 * @test test_upcase_table_is_the_canonical_one
 * @brief The fold this build uses is the table the volume actually carries.
 *
 * @details Two statements, and the second is the strong one. First, the up-case
 *          entry's `TableChecksum` equals both ::priv_exfat_upcase_checksum()
 *          and the published checksum of the canonical Microsoft table, which
 *          is an external reference rather than one of our own numbers. Second,
 *          ::priv_exfat_upcase_unit() agrees with the expanded on-disk table for
 *          every one of the 65536 BMP code units -- so the compressed-table walk
 *          is not merely right about the handful of characters a spot check
 *          would cover.
 *
 * @par MC/DC:
 * Decision: `if (v != k_exfat_upc_run_tag)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_upcase.c@priv_exfat_upcase_unit` (1 cond).
 * Decision: `if (want < (idx + run))` in the same function (1 condition).
 * Decision: `if (w >= k_exfat_upc_words)` after a tag (1 condition).
 * The sweep below drives every arm of all three, many times: the table holds
 * four identity runs, thousands of explicit mappings, and one trailing tag with
 * no length after it.
 *
 * @pre A formatted exFAT volume; nothing open.
 * @post The checksum matched two independent references, and the walk agreed
 *       with the volume's table across the whole BMP.
 *
 * @since 0.1.0
 */
static void test_upcase_table_is_the_canonical_one(void)
{
  TEST_BEGIN("exfat unicode: the fold agrees with the volume's own table");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint32_t stored_csum = 0U;
  load_volume_upcase(h, &stored_csum);
  TEST_ASSERT_EQ(k_ux_upcase_csum, stored_csum);
  TEST_ASSERT_EQ(k_ux_upcase_csum, priv_exfat_upcase_checksum());

  /* The mount agrees the table is one it can fold with. */
  TEST_ASSERT_EQ(1U, h->exfat_upcase_ok);

  uint32_t mismatches = 0U;
  for (uint32_t u = 0U; u < (uint32_t)k_ux_bmp_units; u++) {
    if (priv_exfat_upcase_unit((uint16_t)u) != s_volume_upcase[u]) {
      mismatches++;
    }
  }
  TEST_ASSERT_EQ(0U, mismatches);

  /* Non-vacuity: the sweep would report zero mismatches just as happily if the
   * table had failed to load and both sides were the identity. The canonical
   * table maps 874 of the 65536 units to something other than themselves
   * (measured 2026-08-04); the floor is set below that and far above zero, so
   * a table that did not load fails here instead of passing silently. */
  uint32_t mapped = 0U;
  for (uint32_t u = 0U; u < (uint32_t)k_ux_bmp_units; u++) {
    if ((uint32_t)s_volume_upcase[u] != u) {
      mapped++;
    }
  }
  TEST_ASSERT(mapped > (uint32_t)k_ux_mapped_floor);
  /* And one mapping by name, so "loaded something" is not enough either. */
  TEST_ASSERT_EQ(0x00C9U, s_volume_upcase[0x00E9U]);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat unicode: the fold agrees with the volume's own table");
}

/**
 * @test test_exfat_mixed_case_lookup
 * @brief An upper-case spelling opens a lower-case name, accents included.
 *
 * @par MC/DC:
 * Decision: `if (upcase(unit) != upcase(name[pos + i]))` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_read.c@priv_exfat_name_chunk_eq` (1 cond).
 * - V1: equal after folding -> false -> the set matches (the open here).
 * - V2: a different name    -> true  -> no match (the control here).
 *
 * @pre A formatted exFAT volume; nothing open.
 * @post The upper-case spelling opened the file; an unrelated one did not.
 *
 * @since 0.1.0
 */
static void test_exfat_mixed_case_lookup(void)
{
  TEST_BEGIN("exfat unicode: an upper-case spelling opens an accented name");
  build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  const uint8_t payload = (uint8_t)'x';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, s_acc_u8, &payload, 1U));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, s_acc_upper_u8, k_ra8_fs_mode_read, &f)); /* V1 */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "CAFE.TXT", k_ra8_fs_mode_read, &f)); /* V2 */

  /* Reported as written, not as matched. */
  TEST_ASSERT_EQ(1U, listing_holds(h, s_acc_u8));

  unicode_dump_image("unicode_mixed_case", h->partition_base_lba);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat unicode: an upper-case spelling opens an accented name");
}

/**
 * @test test_foreign_upcase_table_refuses_non_ascii
 * @brief A volume whose fold this build cannot reproduce refuses, loudly.
 *
 * @details The honest half of the design. `NameHash` is defined over the
 *          VOLUME's up-case table; this build embeds the canonical one and
 *          checks the volume's `TableChecksum` against it at mount. When they
 *          disagree there is no way to store a non-ASCII name whose hash the
 *          host will recompute, so the operation is refused with
 *          `k_ra8_err_not_supported` rather than filed under a hash nothing
 *          agrees with.
 *
 *          ASCII is unaffected, and the control below is what makes that claim
 *          testable rather than decorative: on the same corrupted volume an
 *          ASCII name still creates, lists and opens.
 *
 * @par MC/DC:
 * Decision: `if (m->exfat_upcase_ok != 0U)` in
 * `libs/ra8_fs/src/ra8_fs_fat_exfat_read.c@priv_exfat_name_to_units` (1 cond).
 * - V1: the canonical table -> true  -> anything goes (every sibling case).
 * - V2: a foreign one       -> false -> the ASCII test decides (this case).
 *
 * Decision: `if (priv_utf16_all_ascii(...) != 0U)` in the same function (1 cond).
 * - V3: an ASCII name    -> true  -> allowed (the control here).
 * - V4: a non-ASCII name -> false -> `k_ra8_err_not_supported` (this case).
 *
 * @pre A formatted exFAT volume; nothing open.
 * @post Non-ASCII was refused with the documented code; ASCII still worked.
 *
 * @since 0.1.0
 */
static void test_foreign_upcase_table_refuses_non_ascii(void)
{
  TEST_BEGIN("exfat unicode: a foreign up-case table refuses non-ASCII, loudly");
  build_exfat_volume();

  /* Corrupt only the up-case entry's TableChecksum. The table bytes are left
   * alone, so this is precisely the "some other implementation's table" case
   * and nothing else about the volume changes. */
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  const uint32_t upc = root_byte(h, (uint32_t)k_mut_root_upcase_idx);
  TEST_ASSERT_EQ(k_ux_type_upcase, s_disk.bytes[upc]);
  TEST_ASSERT_EQ(1U, h->exfat_upcase_ok);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  s_disk.bytes[upc + (uint32_t)k_ux_upc_off_csum] ^= (uint8_t)1U;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(0U, h->exfat_upcase_ok); /* V2 */

  const uint8_t payload = (uint8_t)'x';
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_write_file(h, s_acc_u8, &payload, 1U)); /* V4 */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, ra8_fs_open(h, s_acc_u8, k_ra8_fs_mode_read, &f));

  /* The control: ASCII folds the same under every conforming table, so it is
   * not affected and must still work end to end. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write_file(h, "PLAIN.TXT", &payload, 1U)); /* V3 */
  TEST_ASSERT_EQ(1U, listing_holds(h, "PLAIN.TXT"));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PLAIN.TXT", k_ra8_fs_mode_read, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("exfat unicode: a foreign up-case table refuses non-ASCII, loudly");
}

/**
 * @brief Run every case in this suite.
 *
 * @return Process exit status.
 * @retval 0 Every case passed; a failure aborts inside the harness instead.
 *
 * @pre No other suite shares this process.
 * @pre The fixture's RAM disk is not held by anything else.
 * @post Every case in the file has run and released its volume.
 * @post Nothing remains allocated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int32_t main(void)
{
  test_exfat_non_ascii_round_trip();
  test_exfat_name_entries_hold_real_units();
  test_exfat_name_hash_matches_an_independent_computation();
  test_upcase_table_is_the_canonical_one();
  test_exfat_mixed_case_lookup();
  test_foreign_upcase_table_refuses_non_ascii();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_unicode_exfat.c\n");
  return 0;
}
