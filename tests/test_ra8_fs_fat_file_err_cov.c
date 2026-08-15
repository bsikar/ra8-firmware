/**
 * @file test_ra8_fs_fat_file_err_cov.c
 * @brief Coverage booster for libs/ra8_fs/src/ra8_fs_fat_file.c -- create + I/O errors.
 *
 * @details
 * Dedicated companion test executable that drives the creation and
 * error-injection branches in ra8_fs_fat_file.c: the priv_create_new error
 * paths (full root directory, exhausted file-slot table), the ra8_fs_open
 * guards (unmounted handle, exFAT dispatch), and the deterministic
 * backend-failure paths through priv_free_chain, priv_truncate_existing,
 * priv_write_new_dir_entry, and priv_dir_find.
 *
 * The open / resolve half of the suite lives in the split sibling
 * test_ra8_fs_fat_file_open_cov.c. The shared block-device backends and
 * volume builders live in tests/support/fs_fat_file_test_util.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "support/fs_fat_file_test_util.h"
#include "unity_minimal.h"

/* ===========================================================================
 * Tests targeting priv_create_new error paths (lines 401, 405)
 * ===========================================================================
 */

/**
 * @test test_root_dir_full
 * @brief Attempting to create a file in a full root directory returns no_mem.
 *
 * @details
 * The FAT16 volume has 16 root-directory entries.  After creating 16 files
 * a seventeenth creation attempt causes priv_dir_find_free to return
 * k_ra8_err_no_mem, which priv_create_new propagates at line 401.
 *
 * Line targeted: 401.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` after priv_dir_find_free in
 * priv_create_new (1 condition).
 * V1: root full -> k_ra8_err_no_mem -> TRUE -> propagated.
 * V2: free slot -> FALSE (the sixteen creations above).
 * N=1 condition, both outcomes pinned inside this test.
 *
 * @pre Volume is formatted and accessible.
 * @post The seventeenth ra8_fs_open returns k_ra8_err_no_mem.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_root_dir_full(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: root directory full returns no_mem");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Create 16 files to fill all root directory slots. */
  ra8_fs_file_t* tmp       = nullptr;
  const char*    names[16] = {
    "F00.TXT",
    "F01.TXT",
    "F02.TXT",
    "F03.TXT",
    "F04.TXT",
    "F05.TXT",
    "F06.TXT",
    "F07.TXT",
    "F08.TXT",
    "F09.TXT",
    "F10.TXT",
    "F11.TXT",
    "F12.TXT",
    "F13.TXT",
    "F14.TXT",
    "F15.TXT",
  };
  for (uint32_t i = 0U; i < 16U; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, names[i], k_ra8_fs_mode_write, &tmp));
    TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  }

  /* Seventeenth file: root directory is full, priv_dir_find_free fails. */
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "F16.TXT", k_ra8_fs_mode_write, &tmp));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs_fat_file cov: root directory full returns no_mem");
}

/**
 * @test test_create_no_file_slot
 * @brief File-table exhaustion during file creation returns no_mem.
 *
 * @details
 * Pre-creates four files and opens all four to fill the static file-slot
 * table.  A fifth open for a new (non-existing) file goes through
 * priv_create_new; priv_dir_find_free succeeds but priv_alloc_file_slot
 * returns nullptr and priv_create_new returns k_ra8_err_no_mem (line 405).
 *
 * Line targeted: 405.
 *
 * @par MC/DC:
 * Decision: `if (f == nullptr)` after priv_alloc_file_slot in
 * priv_create_new (1 condition).
 * V1: all four slots taken -> nullptr -> TRUE -> k_ra8_err_no_mem.
 * V2: slot available -> FALSE (the four preceding creations).
 * N=1 condition, both outcomes pinned inside this test.
 *
 * @pre Volume is formatted and accessible.
 * @post The fifth open returns k_ra8_err_no_mem.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_create_no_file_slot(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: create new file with full slot table");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Pre-create four files, then open them all to consume all slots. */
  ra8_fs_file_t* tmp = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PA.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PB.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PC.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PD.TXT", k_ra8_fs_mode_write, &tmp));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(tmp));

  ra8_fs_file_t* fa = nullptr;
  ra8_fs_file_t* fb = nullptr;
  ra8_fs_file_t* fc = nullptr;
  ra8_fs_file_t* fd = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PA.TXT", k_ra8_fs_mode_read, &fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PB.TXT", k_ra8_fs_mode_read, &fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PC.TXT", k_ra8_fs_mode_read, &fc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "PD.TXT", k_ra8_fs_mode_read, &fd));

  /* NEW.TXT does not exist; create path -> dir slot free but no file slot. */
  ra8_fs_file_t* fn = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_no_mem, ra8_fs_open(h, "NEW.TXT", k_ra8_fs_mode_write, &fn));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fa));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fb));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fc));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(fd));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs_fat_file cov: create new file with full slot table");
}

/* ===========================================================================
 * Tests targeting ra8_fs_open guards (lines 453, 462)
 * ===========================================================================
 */

/**
 * @test test_open_not_in_use
 * @brief ra8_fs_open on an unmounted handle returns k_ra8_err_invalid_state.
 *
 * @details
 * Mounts, stores the handle, unmounts (setting in_use=0), then calls
 * ra8_fs_open on the stale handle.  Line 453 returns k_ra8_err_invalid_state.
 *
 * Line targeted: 453.
 *
 * @par MC/DC:
 * Decision: `if (handle->in_use == 0U)` (line 452, 1 condition).
 * V1: in_use=0 (unmounted) -> TRUE -> k_ra8_err_invalid_state (line 453).
 * V2: in_use=1 (mounted)   -> FALSE -> continue (all other tests).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_open returns k_ra8_err_invalid_state.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_open_not_in_use(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: ra8_fs_open on unmounted handle");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));

  /* h is a pointer into the static pool; unmount sets in_use=0. */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, ra8_fs_open(h, "DEAD.TXT", k_ra8_fs_mode_read, &f));

  internal_free_volume();
  TEST_END("ra8_fs_fat_file cov: ra8_fs_open on unmounted handle");
}

/**
 * @test test_open_exfat_dispatch
 * @brief ra8_fs_open on an exFAT volume dispatches to priv_exfat_open.
 *
 * @details
 * Formats and mounts an exFAT volume, then calls ra8_fs_open in read mode for
 * a non-existent file.  Line 462 (`return priv_exfat_open(...)`) is executed;
 * priv_exfat_open returns k_ra8_err_not_found since the file does not exist.
 *
 * Line targeted: 462.
 *
 * @par MC/DC:
 * Decision: `if (handle->type == k_ra8_fs_type_exfat)` in ra8_fs_open
 * (1 condition).
 * V1: exFAT volume -> TRUE -> priv_exfat_open dispatch (this test).
 * V2: FAT16 volume -> FALSE (every FAT16 test in this suite).
 * N=1 condition, 1 independent vector per DO-178C 6.4.4.3.
 *
 * @pre s_disk has space for 65536 sectors (32 MiB).
 * @post ra8_fs_open returns k_ra8_err_not_found.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_open_exfat_dispatch(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: ra8_fs_open dispatches to exFAT");
  internal_build_exfat_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_fs_open(h, "EFILE.TXT", k_ra8_fs_mode_read, &f));

  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs_fat_file cov: ra8_fs_open dispatches to exFAT");
}

/* ===========================================================================
 * Tests requiring I/O error injection (lines 73, 85, 143-145, 167, 409, 484)
 * ===========================================================================
 */

/**
 * @test test_free_chain_write_error
 * @brief A backend write failure in priv_free_chain surfaces through truncate.
 *
 * @details
 * Creates "WFAIL.TXT" with one byte of data (one cluster allocated), closes
 * it, swaps the mount's backend to the inject backend with writes_fail=1, then
 * re-opens in write mode.  priv_truncate_existing calls priv_free_chain which
 * attempts to write the freed FAT entry, fails, and returns k_ra8_err_hw_error.
 * priv_truncate_existing propagates it (lines 72-73) and priv_open_existing
 * sets in_use=0 and returns (lines 143-145).
 *
 * Lines targeted: 73, 143, 144, 145.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 72 (1 condition, truncate free chain).
 * V1: priv_free_chain fails (write error) -> TRUE  -> line 73 (return err).
 * V2: priv_free_chain succeeds            -> FALSE -> continue (test_reopen_write_truncates).
 *
 * Decision: `if (err != k_ra8_ok)` at line 143 (1 condition, truncate result in open_existing).
 * V1: truncate fails  -> TRUE  -> lines 144-145.
 * V2: truncate OK     -> FALSE -> continue (test_reopen_write_truncates).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_open returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_free_chain_write_error(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: free_chain write error propagates through truncate");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f   = nullptr;
  uint8_t        one = (uint8_t)'X';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "WFAIL.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Save backend, install write-failing inject backend. */
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, (uint32_t)k_cov_reads_inf, 1U);

  /* Re-open in write mode: free_chain write fails -> lines 72-73 -> 143-145. */
  ra8_fs_file_t* f2 = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "WFAIL.TXT", k_ra8_fs_mode_write, &f2));

  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs_fat_file cov: free_chain write error propagates through truncate");
}

/**
 * @test test_truncate_dir_read_error
 * @brief A backend read error during directory re-read in priv_truncate_existing.
 *
 * @details
 * Creates "RFAIL.TXT" with one byte, closes, swaps to the inject backend with
 * reads_left=2.  The re-open sequence consumes:
 *   Read 1 -- priv_dir_find reads the root-directory sector (LBA 65).
 *   Read 2 -- priv_free_chain reads the FAT sector (LBA 1).
 *   Read 3 -- priv_truncate_existing: priv_read_sector(dir LBA) -> FAIL.
 * Lines 84-85 in priv_truncate_existing are hit, then propagated through
 * priv_open_existing (lines 143-145).
 *
 * Lines targeted: 85, 143, 144, 145.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 84 (1 condition, read-sector in truncate).
 * V1: read fails  -> TRUE  -> line 85 (return err).
 * V2: read OK     -> FALSE -> continue (test_reopen_write_truncates).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_open returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_truncate_dir_read_error(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: directory read error inside priv_truncate_existing");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  ra8_fs_file_t* f   = nullptr;
  uint8_t        one = (uint8_t)'Y';
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(h, "RFAIL.TXT", k_ra8_fs_mode_write, &f));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(f, &one, 1U));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(f));

  /* Save backend, install 2-reads-then-fail inject backend (writes OK). */
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, 2U, 0U);

  /*
   * Read 1: priv_dir_find reads root dir sector (LBA 65) -> OK.
   * Read 2: priv_free_chain reads FAT sector (LBA 1)     -> OK.
   * Read 3: priv_truncate_existing reads LBA 65          -> FAIL.
   */
  ra8_fs_file_t* f2 = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "RFAIL.TXT", k_ra8_fs_mode_write, &f2));

  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs_fat_file cov: directory read error inside priv_truncate_existing");
}

/**
 * @test test_write_dir_entry_read_error
 * @brief A backend read error in priv_write_new_dir_entry propagates to line 167.
 *
 * @details
 * Uses a fresh (empty) volume and swaps to the inject backend with reads_left=3.
 * The ra8_fs_open for a new file (write mode) consumes:
 *   Read 1 -- priv_dir_find reads root dir sector (LBA 65) -> not found.
 *   Read 2 -- priv_dir_find_long reads root dir sector     -> not found.
 *   Read 3 -- priv_dir_find_free reads root dir sector     -> finds free slot.
 *   Read 4 -- priv_write_new_dir_entry: priv_read_sector(free_lba) -> FAIL.
 * Line 167 (`return err`) in priv_write_new_dir_entry is hit, and the error
 * propagates to priv_create_new line 409 (`return err`).
 *
 * Lines targeted: 167, 409.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_ok)` at line 166 (1 condition, read in write_new_dir_entry).
 * V1: read fails  -> TRUE  -> line 167 (return err).
 * V2: read OK     -> FALSE -> continue (all file-creation tests).
 *
 * Decision: `if (err != k_ra8_ok)` at line 408 (1 condition, write_new_dir_entry result).
 * V1: write_new_dir_entry fails -> TRUE  -> line 409.
 * V2: write_new_dir_entry OK    -> FALSE -> continue (all file-creation tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_open returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_write_dir_entry_read_error(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: read error in priv_write_new_dir_entry");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Swap to 3-reads-then-fail backend on an empty volume. */
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, 3U, 0U);

  /*
   * Read 1: priv_dir_find reads LBA 65         -> not found.
   * Read 2: priv_dir_find_long reads LBA 65    -> not found.
   * Read 3: priv_dir_find_free reads LBA 65    -> free slot found.
   * Read 4: priv_write_new_dir_entry(LBA 65)   -> FAIL -> line 167 -> line 409.
   */
  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "NDIR.TXT", k_ra8_fs_mode_write, &f));

  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs_fat_file cov: read error in priv_write_new_dir_entry");
}

/**
 * @test test_dir_find_io_error
 * @brief A backend read failure during priv_dir_find surfaces at ra8_fs_open line 484.
 *
 * @details
 * Swaps to the inject backend with reads_left=0 (fail immediately).  The
 * first read inside priv_dir_find fails, returning k_ra8_err_hw_error (which is
 * neither k_ra8_ok nor k_ra8_err_not_found).  ra8_fs_open propagates it at
 * line 484 (`return err`).
 *
 * Line targeted: 484.
 *
 * @par MC/DC:
 * Decision: `if (err != k_ra8_err_not_found)` at line 483 (1 condition).
 * V1: err=k_ra8_err_hw_error -> TRUE  -> line 484.
 * V2: err=k_ra8_err_not_found -> FALSE -> continue to creation path (all create tests).
 *
 * @pre Volume is formatted and accessible.
 * @post ra8_fs_open returns k_ra8_err_hw_error.
 *
 * @note Not thread-safe.
 * @since 0.1.0 @pre Pointer arguments address their documented readable or writable extents. @post No access exceeds a caller-advertised capacity.
 */
RA8_INTERNAL static void internal_test_dir_find_io_error(void)
{
  TEST_BEGIN("ra8_fs_fat_file cov: dir_find I/O error propagated at ra8_fs_open line 484");
  internal_build_fat16_volume();
  ra8_fs_mount_t* h = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &h));

  /* Swap to a backend that fails the very first read. */
  ra8_fs_backend_t saved = h->backend;
  internal_swap_to_inject(h, 0U, 0U);

  ra8_fs_file_t* f = nullptr;
  TEST_ASSERT_EQ(k_ra8_err_hw_error, ra8_fs_open(h, "FIND.TXT", k_ra8_fs_mode_read, &f));

  h->backend = saved;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(h));
  internal_free_volume();
  TEST_END("ra8_fs_fat_file cov: dir_find I/O error propagated at ra8_fs_open line 484");
}
/* ===========================================================================
 * Main
 * ===========================================================================
 */

/**
 * @brief Test executable entry point.
 *
 * @details Runs the creation + error-injection coverage tests in sequence.
 *          Each test is self-contained: it builds the volume, mounts,
 *          exercises the target branches, unmounts, and frees the disk.
 *
 * @return 0 on success (all tests passed).
 *
 * @pre Host environment provides calloc/free and stderr.
 * @post The targeted create/error branches in ra8_fs_fat_file.c are exercised.
 *
 * @note Not thread-safe (single-threaded test runner).
 * @since 0.1.0
 */
int32_t main(void)
{
  internal_test_root_dir_full();
  internal_test_create_no_file_slot();
  internal_test_open_not_in_use();
  internal_test_open_exfat_dispatch();
  internal_test_free_chain_write_error();
  internal_test_truncate_dir_read_error();
  internal_test_write_dir_entry_read_error();
  internal_test_dir_find_io_error();
  return 0;
}
