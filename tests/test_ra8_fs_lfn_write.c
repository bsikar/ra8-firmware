/**
 * @file test_ra8_fs_lfn_write.c
 * @brief Creating a VFAT long name on FAT12/16/32 (#600).
 *
 * @details
 * Creating a name that is not 8.3-representable used to return
 * `k_ra8_err_invalid_arg`; three in-tree consumers had already bent around
 * that. These cases prove the name the caller asked for is what lands on the
 * card, survives a close/reopen, and comes back out of `listdir`. Taking one
 * away again -- where the orphan defect lives -- is `test_ra8_fs_lfn_erase.c`.
 *
 * Two things are checked at every step, not one. The public API's answer is
 * necessary but not sufficient: a create that returns `k_ra8_ok` while writing
 * a malformed chain passes a return-code-only test. So every mutation is
 * followed by a RAW SCAN of the directory, written from the FAT specification
 * rather than from the driver's own helpers (see the shared fixture), so that
 * a driver-side mistake cannot agree with itself.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_lfn_write_test_util.h"
#include "unity_minimal.h"

/**
 * @enum lww_fill_t
 * @brief How full a directory is made before the case under test runs.
 *
 * @details Both counts are one short of what the geometry allows, which is the
 *          whole point: the long name that follows needs three slots and must
 *          therefore either grow the directory or be refused.
 *
 * @invariant Each is less than the 16 slots a one-sector directory holds.
 * @see test_chain_grows_the_directory()
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_lww_sub_fill  = 13U, /**< "." + ".." + 13 = 15 of a subdirectory's 16. */
  k_lww_root_fill = 14U, /**< 14 of the fixed root's 16, leaving two.      */
} lww_fill_t;

/* ===========================================================================
 * Tests: the name survives the round trip
 * ===========================================================================
 */

/**
 * @test test_long_name_round_trip
 * @brief A 26-character name is created, read back, and listed as itself.
 *
 * @details Twenty-six characters is two full long-name groups plus its 8.3
 *          entry, so the chain is three slots. The raw scan afterwards proves
 *          the shape as well as the behaviour: two attr-0x0F slots, both bound
 *          by checksum to the single live entry behind them, and no orphans.
 *
 * @par MC/DC:
 * Decision: `if (kind == k_name_kind_long)` in `priv_dir_reserve()`
 * (1 condition).
 * - V1: `"My Long Document Name.txt"` -> true  -> an alias and a chain.
 * - V2: `"SHORT.TXT"`                 -> false (test_case_flags_replace_a_chain).
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post The file resolves by its long name and the root holds no orphans.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_long_name_round_trip(void)
{
  TEST_BEGIN("lfn write: a 26-character name round-trips");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_write_and_verify(h, "/My Long Document Name.txt");
  TEST_ASSERT_EQ(1U, internal_listdir_is_exactly(h, "/", "My Long Document Name.txt"));

  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(1U, r.live);
  TEST_ASSERT_EQ(2U, r.lfn_slots);
  TEST_ASSERT_EQ(2U, r.chained);
  TEST_ASSERT_EQ(0U, r.orphans);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn write: a 26-character name round-trips");
}

/**
 * @test test_group_boundary_lengths
 * @brief Names of exactly 13 and exactly 26 characters carry no terminator.
 *
 * @details The specification's sharpest corner. A group holds thirteen
 *          characters; when the name ends exactly on a boundary there is no
 *          room left for the NUL that normally follows it, and the reassembler
 *          has to stop because it runs out of groups instead. Getting this
 *          wrong writes a fourteenth character into the next group -- or drops
 *          the thirteenth -- and neither shows up on any other length.
 *
 * @par MC/DC:
 * Decision: the `pos < nlen` / `pos == nlen` ladder in `priv_lfn_fill_slot()`.
 * - V1: 13 characters -> every `pos < nlen`, the `== nlen` arm never runs.
 * - V2: 14 characters -> group 2 takes `pos < nlen` once, then `== nlen`, then
 *   the 0xFFFF padding arm.
 * - V3: 26 characters -> two full groups, neither reaching `== nlen`.
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post All three names resolve and list as themselves.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_group_boundary_lengths(void)
{
  TEST_BEGIN("lfn write: 13 / 14 / 26-character group boundaries");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* 13, 14 and 26 characters, none of them 8.3-representable. */
  const char* n13 = "/Thirteen1.chr";
  const char* n14 = "/Fourteen12.chr";
  const char* n26 = "/TwentySixCharsHere.picture";
  TEST_ASSERT_EQ(13U, strlen(&n13[1]));
  TEST_ASSERT_EQ(14U, strlen(&n14[1]));
  TEST_ASSERT_EQ(26U, strlen(&n26[1]));

  internal_write_and_verify(h, n13);
  internal_write_and_verify(h, n14);
  internal_write_and_verify(h, n26);

  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_collect_cb, &l));
  TEST_ASSERT_EQ(3U, l.count);
  TEST_ASSERT_EQ(0, strcmp(l.name[0], "Thirteen1.chr"));
  TEST_ASSERT_EQ(0, strcmp(l.name[1], "Fourteen12.chr"));
  TEST_ASSERT_EQ(0, strcmp(l.name[2], "TwentySixCharsHere.picture"));

  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(3U, r.live);
  TEST_ASSERT_EQ(0U, r.orphans);
  /* 13 -> 1 group, 14 -> 2, 26 -> 2. */
  TEST_ASSERT_EQ(5U, r.lfn_slots);
  TEST_ASSERT_EQ(5U, r.chained);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn write: 13 / 14 / 26-character group boundaries");
}

/**
 * @test test_alias_collisions
 * @brief Three names sharing a basis are filed under ~1, ~2 and ~3.
 *
 * @details `Long Document One.txt`, `...Two.txt` and `...Three.txt` all reduce
 *          to the basis `LONGDO`, so the tail is the only thing that keeps
 *          their 8.3 entries apart. The aliases are read straight out of the
 *          root sector: an implementation that reused `LONGDO~1` would still
 *          pass a lookup test, because the long name would match first.
 *
 * @par MC/DC:
 * Decision: `if (err == k_ra8_err_not_found)` in `priv_alias_unique()`
 * (1 condition).
 * - V1: `~1` free      -> true  -> taken on the first probe (the first file).
 * - V2: `~1` taken     -> false -> the loop advances (the second and third).
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post The three files resolve by their long names and hold distinct aliases.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_alias_collisions(void)
{
  TEST_BEGIN("lfn write: colliding basis names get ~1 ~2 ~3");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_write_and_verify(h, "/Long Document One.txt");
  internal_write_and_verify(h, "/Long Document Two.txt");
  internal_write_and_verify(h, "/Long Document Three.txt");

  /* Slots: [lfn][lfn][8.3] x 3 -- each of these names is 21..23 chars. */
  const char* want[3] = {"LONGDO~1TXT", "LONGDO~2TXT", "LONGDO~3TXT"};
  uint32_t    found   = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_lw_per_sec; i++) {
    const uint8_t* ent = internal_root_slot(h, i);
    if (ent[k_lw_off_name] == (uint8_t)k_lw_free_perm) {
      break;
    }
    if (ent[k_lw_off_attr] == (uint8_t)k_lw_attr_lfn) {
      continue;
    }
    TEST_ASSERT(found < 3U);
    TEST_ASSERT_EQ(0, memcmp(ent, want[found], (size_t)k_lw_name_len));
    found++;
  }
  TEST_ASSERT_EQ(3U, found);

  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_collect_cb, &l));
  TEST_ASSERT_EQ(3U, l.count);
  TEST_ASSERT_EQ(0, strcmp(l.name[2], "Long Document Three.txt"));
  TEST_ASSERT_EQ(0U, internal_count_orphan_slots(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn write: colliding basis names get ~1 ~2 ~3");
}

/**
 * @test test_case_flags_replace_a_chain
 * @brief A name that differs from 8.3 only in case costs no chain at all.
 *
 * @details `data.log` is stored as `DATA    LOG` with both `DIR_NTRes` case
 *          bits set, which is what Windows, Linux and macOS all write. The
 *          proof is structural: one slot in the root, zero long-name slots, and
 *          the listing still says `data.log`. `Mix.Log` mixes cases inside the
 *          base, which no flag can express, so that one does get a chain. It
 *          has to be a different name, not `Data.Log`: FAT lookup is
 *          case-insensitive, so `Data.Log` IS `data.log` and opening it would
 *          truncate the file rather than make a second one.
 *
 * @par MC/DC:
 * Decision: `if (base_seen == k_case_seen_mixed)` in `priv_name_case_kind()`
 * (1 condition).
 * - V1: `data.log` -> false -> short, flags 0x18.
 * - V2: `Mix.Log`  -> true  -> long, a chain.
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post Both names list as written; only the mixed-case one has a chain.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_case_flags_replace_a_chain(void)
{
  TEST_BEGIN("lfn write: NTRes case flags instead of a chain");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_write_and_verify(h, "/data.log");
  TEST_ASSERT_EQ(1U, internal_listdir_is_exactly(h, "/", "data.log"));

  const uint8_t* ent = internal_root_slot(h, 0U);
  TEST_ASSERT_EQ(0, memcmp(ent, "DATA    LOG", (size_t)k_lw_name_len));
  TEST_ASSERT_EQ(k_lw_ntres_base | k_lw_ntres_ext, ent[k_lw_off_ntres]);

  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(1U, r.live);
  TEST_ASSERT_EQ(0U, r.lfn_slots);

  /* Mixed case inside the base needs the chain the flags cannot give. */
  internal_write_and_verify(h, "/Mix.Log");
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(2U, r.live);
  TEST_ASSERT_EQ(1U, r.lfn_slots);
  TEST_ASSERT_EQ(1U, r.chained);
  TEST_ASSERT_EQ(0U, r.orphans);

  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_collect_cb, &l));
  TEST_ASSERT_EQ(2U, l.count);
  TEST_ASSERT_EQ(0, strcmp(l.name[0], "data.log"));
  TEST_ASSERT_EQ(0, strcmp(l.name[1], "Mix.Log"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn write: NTRes case flags instead of a chain");
}

/**
 * @test test_case_flags_each_half
 * @brief The base and extension flags are set independently of one another.
 *
 * @details Four shapes that a single "is it lower case" flag could not tell
 *          apart: lower base with upper extension, upper base with lower
 *          extension, a base with no case at all (digits) whose extension still
 *          carries its own, and a lower-flagged base that mixes letters with a
 *          digit and a `{` -- the flag lower-cases the letters and must leave
 *          the others alone.
 *
 * The `{` is the load-bearing character: every case test on this path is
 * `(c >= 'A') && (c <= 'Z')` or its lower-case twin, and `{` is the only
 * routine 8.3 character that takes the TRUE arm of the first condition and the
 * FALSE arm of the second. Without it those decisions never see that pair.
 *
 * @par MC/DC:
 * Decision: `if (ext_seen == k_case_seen_lower)` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@priv_name_case_kind` (1 condition).
 * - V1: `readme.TXT` -> false -> only the base flag.
 * - V2: `NOTICE.txt` -> true  -> only the extension flag.
 * .
 * Decision: `if ((c >= 'A') && (c <= 'Z'))` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@internal_case_observe` (2 conditions).
 * - V3: `'N'` in `NOTICE`   -> C1=T, C2=T -> counted as upper.
 * - V4: `'0'` in `0123`     -> C1=F       -> neither class.
 * - V5: `'{'` in `a1b2{x`   -> C1=T, C2=F -> neither class, and it is the only
 *   ordinary 8.3 character that takes this pair.
 * V3+V4 isolate C1; V3+V5 isolate C2.
 * .
 * Decision: `if ((c >= 'a') && (c <= 'z'))` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@internal_case_observe` (2 conditions).
 * - V6: `'a'` in `a1b2{x`   -> C1=T, C2=T -> counted as lower.
 * - V7: `'0'` in `0123`     -> C1=F       -> neither class.
 * - V8: `'{'` in `a1b2{x`   -> C1=T, C2=F -> neither class.
 * V6+V7 isolate C1; V6+V8 isolate C2.
 * .
 * Decision: `if ((c >= 'A') && (c <= 'Z'))` in
 * `libs/ra8_fs/src/ra8_fs_fat_name.c@internal_case_apply` (2 conditions), reached
 * while rendering a flagged name back out through `listdir`.
 * - V9:  `'A'` of `A1B2{X`  -> C1=T, C2=T -> lower-cased.
 * - V10: `'1'` of `A1B2{X`  -> C1=F       -> passed through.
 * - V11: `'{'` of `A1B2{X`  -> C1=T, C2=F -> passed through.
 * V9+V10 isolate C1; V9+V11 isolate C2.
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post All four names list exactly as they were written.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_case_flags_each_half(void)
{
  TEST_BEGIN("lfn write: base and extension case flags are independent");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Three DISTINCT 8.3 names: `README.txt` would pack to the same eleven bytes
   * as `readme.TXT` and open the same file, proving nothing. */
  internal_write_and_verify(h, "/readme.TXT");
  internal_write_and_verify(h, "/NOTICE.txt");
  internal_write_and_verify(h, "/0123.log");
  /* A lower-flagged half holding a digit and a `{`: the flag lower-cases the
   * letters and must leave both alone. The `{` matters on its own -- it is
   * ABOVE 'Z' in ASCII, so it is the vector that separates "is a letter" from
   * "is at least as big as 'A'" in every case test on the path. */
  internal_write_and_verify(h, "/a1b2{x.log");

  TEST_ASSERT_EQ(k_lw_ntres_base, internal_root_slot(h, 0U)[k_lw_off_ntres]);
  TEST_ASSERT_EQ(k_lw_ntres_ext, internal_root_slot(h, 1U)[k_lw_off_ntres]);
  TEST_ASSERT_EQ(k_lw_ntres_ext, internal_root_slot(h, 2U)[k_lw_off_ntres]);

  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/", internal_collect_cb, &l));
  TEST_ASSERT_EQ(4U, l.count);
  TEST_ASSERT_EQ(0, strcmp(l.name[0], "readme.TXT"));
  TEST_ASSERT_EQ(0, strcmp(l.name[1], "NOTICE.txt"));
  TEST_ASSERT_EQ(0, strcmp(l.name[2], "0123.log"));
  TEST_ASSERT_EQ(0, strcmp(l.name[3], "a1b2{x.log"));

  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(0U, r.lfn_slots); /* none of the four needed a chain */

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn write: base and extension case flags are independent");
}

/* ===========================================================================
 * Tests: directories
 * ===========================================================================
 */

/**
 * @test test_chain_grows_the_directory
 * @brief A run that does not fit before end-of-directory grows a subdirectory.
 *
 * @details With one sector per cluster a subdirectory holds sixteen slots, two
 *          of which are "." and "..". Filling it to a single free slot and then
 *          asking for a name that needs three forces the growth path: allocate
 *          a cluster, zero it, link it, and let the run straddle the boundary.
 *          The file is then read back through the seam.
 *
 * @par MC/DC:
 * Decision: `if (ferr != k_ra8_err_no_mem)` in `priv_dir_reserve()`
 * (1 condition).
 * - V1: a run was found -> true  -> returned at once (every other case here).
 * - V2: no run          -> false -> the directory grows and the search repeats.
 *
 * @pre A hand-built FAT16 volume (SPC=1) is mounted.
 * @post The long name resolves and reads back through the new cluster.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_chain_grows_the_directory(void)
{
  TEST_BEGIN("lfn write: a run that does not fit grows the directory");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));
  /* "." + ".." + 13 files = 15 of the cluster's 16 slots. */
  internal_create_empty_files(h, "/SUB", (uint32_t)k_lww_sub_fill);

  /* Needs two long-name slots plus its 8.3 entry: one free slot is not enough. */
  internal_write_and_verify(h, "/SUB/Spilling Over The Edge.txt");

  name_list_t l = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/SUB", internal_collect_cb, &l));
  TEST_ASSERT_EQ(k_lww_sub_fill + 1U, l.count);
  TEST_ASSERT_EQ(0, strcmp(l.name[(uint32_t)k_lww_sub_fill], "Spilling Over The Edge.txt"));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn write: a run that does not fit grows the directory");
}

/**
 * @test test_fixed_root_cannot_grow
 * @brief A FAT16 root with no long-enough run reports no_mem, not corruption.
 *
 * @details The FAT12/16 volume root is a fixed sector region; there is no chain
 *          to extend. Fourteen files leave two free slots, and a name needing
 *          three has nowhere to go. The directory must be left exactly as it
 *          was -- a half-written chain would be worse than the refusal.
 *
 * @par MC/DC:
 * Decision: `if ((loc->is_root != 0U) && (m->type != k_ra8_fs_type_fat32))` in
 * `libs/ra8_fs/src/ra8_fs_fat_lfn_write.c@internal_dir_grow` (2 conditions).
 * - V1: the FAT16 root       -> C1=T, C2=T -> k_ra8_err_no_mem (this case).
 * - V2: a FAT16 subdirectory -> C1=F       -> the chain is extended
 *   (test_chain_grows_the_directory).
 * - V3: a FAT32 root         -> C1=T, C2=F -> the chain is extended, because a
 *   FAT32 root IS a chain (test_ra8_fs_fat_variants.c covers FAT32 creation;
 *   the condition pair is what makes the two roots behave differently).
 * V1+V2 isolate C1; V1+V3 isolate C2.
 *
 * @pre A hand-built FAT16 volume is mounted.
 * @post The create was refused and the root holds no orphan or partial chain.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_fixed_root_cannot_grow(void)
{
  TEST_BEGIN("lfn write: the fixed root cannot grow -> no_mem");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  internal_create_empty_files(h, "/", (uint32_t)k_lww_root_fill);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_fs_open(h, "/Needs Three Slots Here.txt", k_ra8_fs_mode_write, &f));

  scan_result_t r = {};
  internal_scan_root_of(h, &r);
  TEST_ASSERT_EQ(k_lww_root_fill, r.live);
  TEST_ASSERT_EQ(0U, r.lfn_slots);
  TEST_ASSERT_EQ(0U, r.orphans);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn write: the fixed root cannot grow -> no_mem");
}

/**
 * @test test_unstorable_names_are_refused
 * @brief Names no encoding can hold are refused, and change nothing.
 *
 * @details A `?` is illegal in a long name as well as an 8.3 one; an empty leaf
 *          is not a name; and 248 characters is one past what nineteen
 *          long-name groups can carry. All three are refused by
 *          ::priv_name_classify() before a slot is reserved.
 *
 * @par MC/DC:
 * Decision: `if ((n == 0U) || (n > k_lfn_write_max))` in
 * `priv_name_classify()` (2 conditions).
 * - V1: `"Ordinary.txt"` -> F,F -> accepted (every other case here).
 * - V2: `""`             -> T   -> invalid.
 * - V3: 248 characters   -> F,T -> invalid; 247 is accepted.
 * V1+V2 prove the length test's first condition independently; V1+V3 the second.
 *
 * @pre A hand-built FAT16 volume is mounted and empty.
 * @post No entry was written for any of the refused names.
 *
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity. @note Test-only helpers retain no hidden ownership beyond documented fixture state.
 */
RA8_INTERNAL static void internal_test_unstorable_names_are_refused(void)
{
  TEST_BEGIN("lfn write: unstorable names are refused");
  internal_build_fat16_vol();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "/who?.txt", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, "/", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_mkdir(h, "/bad|name"));

  /* One past 19 groups of 13, and then exactly 19 groups. The longest storable
   * name needs twenty consecutive slots, which the sixteen-slot fixed root
   * cannot supply however empty it is, so it goes in a subdirectory that can
   * grow (the refusal above needs no room at all). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/D"));
  char toolong[k_lw_path_cap] = {};
  (void)snprintf(toolong, sizeof(toolong), "%s", "/D/");
  const uint32_t lead = (uint32_t)strlen(toolong);
  for (uint32_t i = 0U; i < ((uint32_t)k_lw_max_name + 1U); i++) {
    toolong[lead + i] = 'x';
  }
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, toolong, k_ra8_fs_mode_write, &f));

  toolong[lead + (uint32_t)k_lw_max_name] = '\0';
  TEST_ASSERT_EQ(k_lw_max_name, strlen(&toolong[lead]));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, toolong, k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));
  TEST_ASSERT_EQ(1U, internal_listdir_is_exactly(h, "/D", &toolong[lead]));
  TEST_ASSERT_EQ(0U, internal_count_orphan_slots(h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_vol();
  TEST_END("lfn write: unstorable names are refused");
}

/**
 * @brief Run every case in this suite.
 *
 * @return Process exit status.
 * @retval 0 Every case passed; a failure aborts inside the harness instead.
 *
 * @pre The host allocator is available.
 * @pre No other suite shares this process.
 * @post Every volume allocated here has been freed.
 * @post The harness has printed one line per case.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_test_long_name_round_trip();
  internal_test_group_boundary_lengths();
  internal_test_alias_collisions();
  internal_test_case_flags_replace_a_chain();
  internal_test_case_flags_each_half();
  internal_test_chain_grows_the_directory();
  internal_test_fixed_root_cannot_grow();
  internal_test_unstorable_names_are_refused();
  return 0;
}
