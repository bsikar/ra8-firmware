/**
 * @file test_ra8_fs_fat_file_open_cov.c
 * @brief Coverage booster for libs/ra8_fs/src/ra8_fs_fat_file.c -- open + resolve.
 *
 * @details
 * Dedicated companion test executable that drives the open-path branches in
 * ra8_fs_fat_file.c: priv_truncate_existing via a write-mode re-open, the
 * priv_open_existing mode arms (append offset, read offset, file-table
 * exhaustion), the priv_enter_subdir guards (component too long, non-8.3
 * component, zero-cluster directory entry), the priv_resolve_parent depth
 * bound, and the priv_resolve_dir error and trailing-slash paths.
 *
 * The creation / error-injection half of the suite lives in the split sibling
 * test_ra8_fs_fat_file_err_cov.c. The shared block-device backends and volume
 * builders live in tests/support/fs_fat_file_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_fat_file_test_util.h"
#include "unity_minimal.h"

/**
 * @enum file_open_cov_fixture_t
 * @brief Named constants for the open/resolve coverage vectors.
 *
 * @details k_root_dir_lba mirrors the hand-built FAT16 geometry (1 reserved
 *          sector + 2 FATs of 32 sectors -> root directory at LBA 65); the
 *          k_de_off_* values are classic 32-byte directory-entry field
 *          offsets.
 *
 * @invariant k_de_off_clus_hi / k_de_off_clus_lo lie inside one 32-byte entry.
 * @see test_enter_subdir_zero_cluster()
 */
typedef enum : uint32_t {
  k_stale_sentinel = 99U,  /**< Poison preload proving out-params get set.  */
  k_root_dir_lba   = 65U,  /**< Root-dir LBA of the hand-built FAT16 image. */
  k_de_off_clus_hi = 20U,  /**< DIR_FstClusHI byte offset in a dir entry.   */
  k_de_off_clus_lo = 26U,  /**< DIR_FstClusLO byte offset in a dir entry.   */
  k_deep_path_cap  = 200U, /**< Path buffer size for the 32-deep nest.      */
  k_dec_base       = 10U,  /**< Decimal base for the Dnn digit split.       */
} file_open_cov_fixture_t;

/* ===========================================================================
 * Tests targeting priv_truncate_existing (lines 68-89)
 * ===========================================================================
 */

/**
 * @test test_reopen_write_truncates
 * @brief Re-opening an existing file in write mode calls priv_truncate_existing.
 *
 * @details
 * Creates "DATA.TXT", writes one byte (allocating a cluster), closes it,
 * then re-opens in write mode.  priv_open_existing is invoked with
 * mode==write, which calls priv_truncate_existing to free the old chain and
 * zero the directory-entry size.  The truncated file is verified to be empty.
 *
 * Lines targeted: 68,70,71,72,75,76,77,78,79,80,81,82,83,84,87,88,89
 *                 (priv_truncate_existing body) and 142,143 (write-mode
 *                 branch in priv_open_existing, non-error path).
 *
 * @par MC/DC:
 * Decision: `if (f->first_cluster >= k_cluster_first_data)` (line 70, 1 condition).
 * V1: file has data (first_cluster=2 >= 2) -> TRUE (free_chain called).
 * V2: not exercised here (empty-file truncate is the else-arm; covered by the
 * existing priv_create_new path leaving cluster=0).
 * N=1 condition, 1 independent vector sufficient per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post DATA.TXT contains zero bytes after the second open.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_reopen_write_truncates(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: reopen in write mode calls truncate");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f   = nullptr;
  uint8_t        one = (uint8_t)'A';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "DATA.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Re-open in write mode: exercises priv_open_existing -> priv_truncate_existing. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "DATA.TXT", k_ra8_fs_mode_write, &f));
  uint32_t sz = (uint32_t)k_stale_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(f, &sz));
  TEST_ASSERT_EQ(0U, sz); /* truncated to empty */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: reopen in write mode calls truncate");
}

/* ===========================================================================
 * Tests targeting priv_open_existing modes (lines 128, 148, 149)
 * ===========================================================================
 */

/**
 * @test test_open_append_mode
 * @brief Opening an existing file in append mode sets offset to size.
 *
 * @details
 * Writes a 4-byte file, closes, then re-opens in append mode.  On append the
 * offset is advanced to size (line 148).  A subsequent ra8_fs_tell confirms
 * the position.  A read-mode re-open is also done to cover the else branch
 * (line 149) in priv_open_existing.
 *
 * Lines targeted: 148 (append body), 149 (else clause for read mode).
 *
 * @par MC/DC:
 * Decision: `if (mode == k_ra8_fs_mode_append)` in priv_open_existing
 * (1 condition).
 * V1: mode=append -> TRUE  -> offset advanced to size_bytes (pos == 4).
 * V2: mode=read   -> FALSE -> offset stays 0 (pos == 0).
 * Both vectors run inside this test; N=1 condition, both outcomes pinned.
 *
 * @pre Volume is formatted and accessible.
 * @post APND.TXT has 4 bytes; append handle position equals 4.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_open_append_mode(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: open existing in append mode");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f          = nullptr;
  uint8_t        payload[4] = {1U, 2U, 3U, 4U};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "APND.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, payload, sizeof(payload)));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Append mode: covers line 148 (f->offset = f->size_bytes). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "APND.TXT", k_ra8_fs_mode_append, &f));
  uint32_t pos = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &pos));
  TEST_ASSERT_EQ(4U, pos);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Read mode: covers line 149 (else branch in priv_open_existing). */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "APND.TXT", k_ra8_fs_mode_read, &f));
  pos = (uint32_t)k_stale_sentinel;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_tell(f, &pos));
  TEST_ASSERT_EQ(0U, pos);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: open existing in append mode");
}

/**
 * @test test_open_existing_no_mem
 * @brief File-table exhaustion returns k_ra8_err_no_mem from priv_open_existing.
 *
 * @details
 * Creates five files, opens four to fill all four file slots (k_ra8_fs_max_files
 * == 4), then tries to open the fifth existing file.  priv_alloc_file_slot
 * returns nullptr and priv_open_existing returns k_ra8_err_no_mem (line 128).
 *
 * Line targeted: 128.
 *
 * @par MC/DC:
 * Decision: `if (f == nullptr)` after priv_alloc_file_slot in
 * priv_open_existing (1 condition).
 * V1: all four slots taken -> nullptr -> TRUE -> k_ra8_err_no_mem.
 * V2: slot available -> FALSE -> normal open (the four preceding opens).
 * N=1 condition, both outcomes pinned inside this test.
 *
 * @pre Volume is formatted and accessible.
 * @post All five files were created; the fifth open attempt fails.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_open_existing_no_mem(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: open existing file with full file table");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Create five files on disk so a fifth one exists to open. */
  ra8_fs_file_t* tmp = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "B.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "C.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "D.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "E.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));

  /* Fill all four file slots. */
  ra8_fs_file_t* fa = nullptr;
  ra8_fs_file_t* fb = nullptr;
  ra8_fs_file_t* fc = nullptr;
  ra8_fs_file_t* fd = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "A.TXT", k_ra8_fs_mode_read, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "B.TXT", k_ra8_fs_mode_read, &fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "C.TXT", k_ra8_fs_mode_read, &fc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "D.TXT", k_ra8_fs_mode_read, &fd));

  /* Fifth open: existing file but no slot available -- line 128. */
  ra8_fs_file_t* fe = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "E.TXT", k_ra8_fs_mode_read, &fe));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: open existing file with full file table");
}

/* ===========================================================================
 * Tests targeting priv_enter_subdir guards (lines 264, 273, 287)
 * ===========================================================================
 */

/**
 * @test test_enter_subdir_name_too_long
 * @brief A path component longer than k_ra8_fs_short_name_len is rejected.
 *
 * @details
 * ra8_fs_open with a path whose first component has 13 characters triggers the
 * `len > k_ra8_fs_short_name_len` check in priv_enter_subdir (line 264).
 *
 * Line targeted: 264.
 *
 * @par MC/DC:
 * Decision: `if (len > k_ra8_fs_short_name_len)` in priv_enter_subdir
 * (1 condition).
 * V1: 13-char component -> TRUE -> k_ra8_err_invalid_arg.
 * V2: short component -> FALSE (every multi-component sibling test).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post Open returns k_ra8_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_enter_subdir_name_too_long(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: subdir component name too long");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Component "TOOLONGNAME13" is 13 chars > k_ra8_fs_short_name_len (12). */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_open(h, "/TOOLONGNAME13/FILE.TXT", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: subdir component name too long");
}

/**
 * @test test_enter_subdir_invalid_83
 * @brief A component that is not an 8.3-representable name is rejected.
 *
 * @details
 * A component that begins with a dot (e.g. ".HIDDEN") cannot be packed into
 * an 8.3 name by priv_path_to_83 (returns 0), so priv_enter_subdir returns
 * k_ra8_err_invalid_arg (line 273).
 *
 * Line targeted: 273.
 *
 * @par MC/DC:
 * Decision: `if (priv_path_to_83(comp, name83) == 0U)` in priv_enter_subdir
 * (1 condition).
 * V1: dot-leading component ".HIDDEN" -> pack fails (0) -> TRUE -> invalid_arg.
 * V2: valid 8.3 component -> FALSE (test_resolve_dir_trailing_slash).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post Open returns k_ra8_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_enter_subdir_invalid_83(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: subdir component fails 8.3 pack");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* ".HIDDEN" starts with a dot so priv_path_to_83 returns 0. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_fs_open(h, "/.HIDDEN/FILE.TXT", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: subdir component fails 8.3 pack");
}

/**
 * @test test_enter_subdir_zero_cluster
 * @brief A directory entry whose first cluster is < 2 triggers a protocol error.
 *
 * @details
 * Creates directory "CORR" (which lands at root LBA 65, offset 0), then
 * manually zeroes the cluster fields (DIR_FstClusHI at offset 20 and
 * DIR_FstClusLO at offset 26) in the backing byte array.  When a subsequent
 * ra8_fs_open tries to descend into "/CORR/FILE.TXT", priv_enter_subdir finds
 * first_cluster < k_cluster_first_data and returns k_ra8_err_protocol_error
 * (line 287).
 *
 * Line targeted: 287.
 *
 * @par MC/DC:
 * Decision: `if (cl < k_cluster_first_data)` (line 286, 1 condition).
 * V1: cl=0 (zeroed entry) -> TRUE -> k_ra8_err_protocol_error.
 * V2: normal directory -> FALSE -> success (exercised by test_resolve_dir_trailing_slash).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post Open returns k_ra8_err_protocol_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_enter_subdir_zero_cluster(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: subdir entry has cluster 0");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Create the directory -- its dir entry goes at root LBA 65, offset 0. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/CORR"));

  /* Corrupt first-cluster fields in the raw backing store.
   * Root dir starts at sector 65 (reserved=1 + 2*fat_sz=64, hence LBA 65).
   * Entry 0 is at offset 0.  DIR_FstClusHI is at byte 20, DIR_FstClusLO at 26.
   * Zeroing them sets cluster to 0 which is < k_cluster_first_data (2). */
  uint32_t off = (uint32_t)k_root_dir_lba * (uint32_t)k_cov_block_size;
  s_disk.bytes[off + (uint32_t)k_de_off_clus_hi]      = 0U;
  s_disk.bytes[off + (uint32_t)k_de_off_clus_hi + 1U] = 0U;
  s_disk.bytes[off + (uint32_t)k_de_off_clus_lo]      = 0U;
  s_disk.bytes[off + (uint32_t)k_de_off_clus_lo + 1U] = 0U;

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_protocol_error,
                 ra8_fs_open(h, "/CORR/FILE.TXT", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: subdir entry has cluster 0");
}

/* ===========================================================================
 * Tests targeting priv_resolve_parent depth (line 329)
 * ===========================================================================
 */

/**
 * @brief Append @p seg to @p path at @p base_len, failing the test on overflow.
 *
 * @param[in,out] path     Path buffer to extend.
 * @param[in]     cap      Capacity of @p path in bytes, including the NUL.
 * @param[in]     base_len Current length of @p path.
 * @param[in]     seg      NUL-terminated segment to append.
 * @return New length of @p path, excluding the NUL.
 *
 * @pre @p path and @p seg are non-null.
 * @pre @p base_len is the current length of @p path.
 * @post @p path is NUL-terminated at the returned length.
 * @post The test has failed outright if the append would overflow @p cap.
 *
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static uint32_t path_append(char* path, uint32_t cap, uint32_t base_len, const char* seg)
{
  const uint32_t seg_len = (uint32_t)strlen(seg);
  if ((base_len + seg_len) >= cap) {
    TEST_FAIL_FMT("%s", "path buffer overflow");
  }
  for (uint32_t k = 0U; k < seg_len; k++) {
    path[base_len + k] = seg[k];
  }
  const uint32_t len = base_len + seg_len;
  path[len]          = '\0';
  return len;
}

/**
 * @brief Create 32 nested directories, building their path as it goes.
 *
 * @details
 * Each component is spelled `/Dnn` -- the leading letter keeps the name a legal
 * 8.3 short name, which a purely numeric component would not be. Every level is
 * created for real, so the resulting path resolves component by component and
 * only the depth bound can reject it.
 *
 * @param[in]  h    Mounted volume to create the directories on.
 * @param[out] path Receives the 32-component directory path.
 * @param[in]  cap  Capacity of @p path in bytes, including the NUL.
 * @return Length of @p path, excluding the NUL.
 *
 * @pre @p h is a mounted volume and @p path is non-null.
 * @pre @p cap is large enough for 32 four-character components.
 * @post All 32 directories exist on the volume.
 * @post @p path names the deepest of them.
 *
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static uint32_t mkdir_nested_chain(ra8_fs_mount_t* h, char* path, uint32_t cap)
{
  uint32_t base_len = 0U;
  for (uint32_t i = 0U; i < 32U; i++) {
    /* Format component as /Dnn -- avoids leading digits in 8.3 names. */
    char seg[8] = {};
    seg[0]      = '/';
    seg[1]      = 'D';
    seg[2]      = (char)('0' + (i / (uint32_t)k_dec_base));
    seg[3]      = (char)('0' + (i % (uint32_t)k_dec_base));
    seg[4]      = '\0';
    base_len    = path_append(path, cap, base_len, seg);
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, path));
  }
  return base_len;
}

/**
 * @test test_resolve_parent_too_deep
 * @brief A path with 32 real nested directories hits the depth guard.
 *
 * @details
 * Creates 32 nested directories (/D00, /D00/D01, ...) so that every component
 * resolves successfully.  A final ra8_fs_open with the 33-component path causes
 * the depth loop in priv_resolve_parent to exhaust its bound (k_path_max_depth
 * == 32) and return k_ra8_err_invalid_arg (line 329).
 *
 * Line targeted: 329.
 *
 * @par MC/DC:
 * Decision: the depth-guard exit `if (depth >= k_path_max_depth)` in
 * priv_resolve_parent (1 condition).
 * V1: 33-component path -> loop bound exhausted -> TRUE -> invalid_arg.
 * V2: shallow path -> FALSE (every other test in this suite).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post Open returns k_ra8_err_invalid_arg.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_resolve_parent_too_deep(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: path depth exceeds k_path_max_depth");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Build 32 nested directories.  Each component is "Dnn" (3 chars). */
  char           mkdir_path[k_deep_path_cap] = {};
  const uint32_t base_len = mkdir_nested_chain(h, mkdir_path, (uint32_t)sizeof(mkdir_path));
  /* Append leaf "/LEAF.TXT" to form a 33-component path. */
  (void)path_append(mkdir_path, (uint32_t)sizeof(mkdir_path), base_len, "/LEAF.TXT");

  /* 32 intermediate components all exist, so the depth loop runs out. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_fs_open(h, mkdir_path, k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: path depth exceeds k_path_max_depth");
}

/* ===========================================================================
 * Tests targeting priv_resolve_dir paths (lines 348, 355-356)
 * ===========================================================================
 */

/**
 * @test test_resolve_dir_error_path
 * @brief priv_resolve_dir propagates a not-found error from priv_resolve_parent.
 *
 * @details
 * ra8_fs_listdir with a multi-component path whose first component does not
 * exist causes priv_resolve_parent -> priv_enter_subdir to return
 * k_ra8_err_not_found, which priv_resolve_dir then propagates at line 348.
 *
 * Line targeted: 348.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after priv_resolve_parent in
 * priv_resolve_dir (1 condition).
 * V1: missing first component -> not_found -> TRUE -> propagated.
 * V2: parent resolves -> FALSE (test_resolve_dir_trailing_slash).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post listdir returns k_ra8_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_resolve_dir_error_path(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: resolve_dir propagates not-found error");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  uint32_t count = 0U;
  /* "/NOMATCH/SUB" -- NOMATCH does not exist so priv_enter_subdir returns
   * not_found, propagated by priv_resolve_dir (line 348). */
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_listdir(h, "/NOMATCH/SUB", count_cb, &count));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: resolve_dir propagates not-found error");
}

/**
 * @test test_resolve_dir_trailing_slash
 * @brief A trailing slash in a listdir path resolves to the parent directory.
 *
 * @details
 * Creates subdirectory "SUB", then calls ra8_fs_listdir with "/SUB/".  In
 * priv_resolve_dir, priv_resolve_parent enters "SUB" and returns with an
 * empty leaf pointer (the path ended in '/').  The `len == 0` branch at
 * line 354 is taken, setting *out = parent (line 355) and returning k_ra8_ok
 * (line 356).
 *
 * Lines targeted: 355, 356.
 *
 * @par MC/DC:
 * Decision: `if (len == 0U)` (line 354, 1 condition).
 * V1: path="/SUB/" -> trailing slash -> leaf="" -> len=0 -> TRUE (lines 355-356).
 * V2: path="/SUB"  -> leaf="SUB"   -> len=3  -> FALSE (priv_enter_subdir called).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post listdir on the trailing-slash path succeeds with an empty directory.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void test_resolve_dir_trailing_slash(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: trailing slash resolves to parent dir");
  build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mkdir(h, "/SUB"));

  uint32_t count = 0U;
  /* "/SUB/" -> trailing slash -> resolve to the SUB directory itself. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_listdir(h, "/SUB/", count_cb, &count));
  TEST_ASSERT_EQ(0U, count);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  free_volume();
  TEST_END("ra8_fs_fat_file cov: trailing slash resolves to parent dir");
}
/* ===========================================================================
 * Main
 * ===========================================================================
 */

/**
 * @brief Test executable entry point.
 *
 * @details Runs the open + resolve coverage tests in sequence. Each test is
 *          self-contained: it builds the volume, mounts, exercises the target
 *          branches, unmounts, and frees the disk.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides calloc/free and stderr.
 * @post The targeted open/resolve branches in ra8_fs_fat_file.c are exercised.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int32_t main(void)
{
  test_reopen_write_truncates();
  test_open_append_mode();
  test_open_existing_no_mem();
  test_enter_subdir_name_too_long();
  test_enter_subdir_invalid_83();
  test_enter_subdir_zero_cluster();
  test_resolve_parent_too_deep();
  test_resolve_dir_error_path();
  test_resolve_dir_trailing_slash();
  (void)fprintf(stderr, "[OK  ] test_ra8_fs_fat_file_open_cov.c\n");
  return 0;
}
