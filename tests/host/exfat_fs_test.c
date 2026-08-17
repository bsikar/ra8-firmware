/**
 * @file tests/host/exfat_fs_test.c
 * @brief Standalone host test for ra8_fs exFAT read (#85), the leading-slash
 *        open regression (#93), and the exFAT write path (#104: create /
 *        multi-cluster write + read-back / rename / unlink).
 *
 * @details
 * Links ONLY ra8_fs_fat.c (no ra8_core_hal -> no ra8_time weak-extern), so it
 * builds and runs on macOS and Linux alike, unlike the unity suite. It opens
 * a tiny real exFAT image (decompressed from a checked-in .gz at build time,
 * path injected via ``RA8_EXFAT_FIXTURE``) over an in-memory block backend and
 * asserts:
 *   1. mount succeeds and detects exFAT,
 *   2. listdir enumerates the known files,
 *   3. open("/HELLO.TXT") -- WITH a leading slash -- succeeds (the #93 fix;
 *      it returned k_ra8_err_not_found before because the exFAT name matcher
 *      did not strip leading slashes like the FAT path does),
 *   4. open("HELLO.TXT") -- without a slash -- also succeeds,
 *   5. the file content reads back byte-for-byte.
 *
 * It then exercises the exFAT write path (#104, now the streaming writer behind
 * `ra8_fs_write_file()`) on the same mounted volume:
 *   6. write_file + read-back + rename + unlink of a single-cluster file,
 *   7. the same cycle for a multi-cluster file (a payload spanning four 4 KiB
 *      clusters), proving the bitmap allocation, the read cluster-walk, and the
 *      multi-cluster free loop that the single-cluster case cannot reach,
 *   8. negative lookups (same-length wrong name; wrong-length name) that drive
 *      the matcher's mismatch branches both ways.
 *
 * @par MC/DC:
 * Every decision this harness reaches -- the entry-set builders, matchers and
 * the cluster-free walk -- is single-condition, so MC/DC for them collapses to
 * branch coverage: each must be taken both ways, and the cases above drive the
 * match / mismatch and single- / multi-cluster branches accordingly. The
 * compound decisions the streaming writer added in #602 (contiguity probe,
 * `NoFatChain` transition, chain walk, allocation survey) carry their vectors
 * in the Unity suite, cited by `path@function`; this harness deliberately
 * exercises the whole-file round trip, not those.
 *
 * The fixture holds HELLO.TXT (the expected string below) + NOTES.TXT, made
 * with a real exFAT formatter so the on-disk up-case table / name hashes /
 * allocation bitmap are genuine. It uses 4 KiB clusters (512 B sectors, 8
 * sectors/cluster) with ~480 free clusters, so the multi-cluster file fits.
 *
 * @author Brighton Sikarskie
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../support/ra8_test_file.h"
#include "../support/ra8_test_file_posix.h"
#include "../support/ra8_test_output.h"
#include "ra8_fs.h"

/** @brief Logical block size of the backing disk image. */
typedef enum : uint16_t {
  k_exfat_test_block_bytes = 512U, /**< Bytes per LBA in the RAM image. */
} exfat_test_geom_t;

/**
 * @enum exfat_test_buf_t
 * @brief Host-side buffer capacities and the payload generator this fixture uses.
 *
 * @details
 * The generator is `buf[i] = (i * k_exfat_pattern_stride + k_exfat_pattern_bias)
 * & k_byte_mask`. A prime stride coprime with 256 keeps the byte pattern from
 * repeating inside one block, so a read that returned the wrong offset -- or a
 * block-aligned duplicate -- cannot compare equal by accident.
 */
typedef enum : uint16_t {
  k_exfat_path_cap       = 512U,  /**< Host path scratch capacity.           */
  k_exfat_names_cap      = 1024U, /**< Flattened directory-listing capacity. */
  k_exfat_read_chunk     = 128U,  /**< Read-back chunk; over the smallest fixture
                                       file, so a short read shows up.            */
  k_exfat_name_cap       = 64U,   /**< Single-name scratch capacity.             */
  k_exfat_pattern_stride = 31U,   /**< Stride of the payload generator.          */
  k_exfat_pattern_bias   = 7U,    /**< Its bias, so index 0 is not byte 0.       */
  k_byte_mask            = 0xFFU, /**< Truncates the generator back into a byte. */
} exfat_test_buf_t;

/**
 * @enum exfat_fs_test_exit_t
 * @brief Process exit codes this harness reports to the runner.
 *
 * @details
 * The runner distinguishes "the fixture could not be loaded" from "the
 * filesystem under test misbehaved", because the first is an environment
 * problem and the second is a genuine test failure. Keeping them on separate
 * codes means a missing image never reads as an exFAT defect.
 *
 * @invariant ::k_exfat_fs_test_exit_pass is zero so the shell reads it as success.
 *
 * @par Example:
 * @code
 * return k_exfat_fs_test_exit_fixture;  // could not read the disk image
 * @endcode
 *
 * @see main()
 */
typedef enum : uint8_t {
  k_exfat_fs_test_exit_pass    = 0, /**< Every internal_check passed.        */
  k_exfat_fs_test_exit_failed  = 1, /**< At least one internal_check failed. */
  k_exfat_fs_test_exit_fixture = 2, /**< Fixture image could not be loaded.  */
} exfat_fs_test_exit_t;

#ifndef RA8_EXFAT_FIXTURE
/** @brief RA8 EXFAT FIXTURE. */
#define RA8_EXFAT_FIXTURE "exfat_small.img"
#endif

static const char s_expect[] = "Hello exFAT from the ra_fs standalone test 1234567890\n";

/* #104: a payload that spans several 4 KiB exFAT clusters (with an odd tail) so
 * the write/read/free paths exercise their multi-cluster branches. */
enum : uint32_t { k_mc_payload_bytes = 12425U /**< Mc payload bytes. */ };

static uint8_t* s_img;
static uint32_t s_blocks;
static int      s_found_hello;
static int      s_fail;

/**
 * @brief Be read.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] ctx Argument for the bounded test operation.
 * @param[in] lba Argument for the bounded test operation.
 * @param[in] count Argument for the bounded test operation.
 * @param[in,out] buf Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_be_read(void* ctx, uint64_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  memcpy(buf,
         s_img + ((size_t)lba * (size_t)k_exfat_test_block_bytes),
         (size_t)count * (size_t)k_exfat_test_block_bytes);
  return k_ra8_ok;
}
/**
 * @brief Be write.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] ctx Argument for the bounded test operation.
 * @param[in] lba Argument for the bounded test operation.
 * @param[in] count Argument for the bounded test operation.
 * @param[in] buf Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_be_write(void* ctx, uint64_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  memcpy(s_img + ((size_t)lba * (size_t)k_exfat_test_block_bytes),
         buf,
         (size_t)count * (size_t)k_exfat_test_block_bytes);
  return k_ra8_ok;
}
/**
 * @brief Be cap.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] ctx Argument for the bounded test operation.
 * @param[in,out] bc Argument for the bounded test operation.
 * @param[in,out] bs Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_be_cap(void* ctx, uint64_t* bc, uint32_t* bs)
{
  (void)ctx;
  *bc = s_blocks;
  *bs = k_exfat_test_block_bytes;
  return k_ra8_ok;
}

static char s_names[k_exfat_names_cap];
/**
 * @brief On entry.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] name Argument for the bounded test operation.
 * @param[in] attr Argument for the bounded test operation.
 * @param[in] size Argument for the bounded test operation.
 * @param[in,out] ctx Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_on_entry(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)attr;
  (void)size;
  (void)ctx;
  if (strcmp(name, "HELLO.TXT") == 0) {
    s_found_hello = 1;
  }
  /* Append "<name>|" to the running roster. snprintf into the tail bounds the
   * write by construction: the unbounded strcat pair it replaces was correct
   * only because of the length test above, which is easy to break silently. */
  const size_t used = strlen(s_names);
  const size_t room = sizeof(s_names) - used;
  (void)snprintf(&s_names[used], room, "%s|", name);
}

/* Re-list the root and report whether `name` is currently an entry. */
/**
 * @brief Name present.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] mnt Argument for the bounded test operation.
 * @param[in] name Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static int internal_name_present(ra8_fs_mount_t* mnt, const char* name)
{
  char needle[k_exfat_name_cap];
  s_names[0] = '\0';
  (void)ra8_fs_listdir(mnt, "/", internal_on_entry, nullptr);
  (void)snprintf(needle, sizeof(needle), "%s|", name);
  return strstr(s_names, needle) != nullptr;
}

/** @brief Write one prefix/path/suffix diagnostic to the host result descriptor.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] prefix Argument for the bounded test operation.
 * @param[in] path Argument for the bounded test operation.
 * @param[in] suffix Argument for the bounded test operation.
 * @return Function-specific result consumed by the calling test.
 * @retval 0 Zero or false result; nonzero values describe the alternate result.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
*/
RA8_INTERNAL static bool
internal_output_path(const char* prefix, const char* path, const char* suffix)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  if (!internal_test_output_fd_init(&output, &state, STDOUT_FILENO)) {
    return false;
  }
  (void)internal_test_output_text(&output, prefix);
  (void)internal_test_output_text(&output, path);
  (void)internal_test_output_text(&output, suffix);
  return output.status == k_ra8_test_output_ok;
}

/**
 * @brief Check.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] cond Argument for the bounded test operation.
 * @param[in] what Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check(int cond, const char* what)
{
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  bool                 wrote  = internal_test_output_fd_init(&output, &state, STDOUT_FILENO);
  if (wrote) {
    (void)internal_test_output_text(&output, "  [");
    (void)internal_test_output_text(&output, (cond != 0) ? "PASS" : "FAIL");
    (void)internal_test_output_text(&output, "] ");
    (void)internal_test_output_text(&output, what);
    (void)internal_test_output_text(&output, "\n");
    wrote = output.status == k_ra8_test_output_ok;
  }
  if ((cond == 0) || !wrote) {
    s_fail = 1;
  }
}

/*
 * Dump the current in-memory image to "$RA8_EXFAT_DUMP.<tag>" when that env var
 * is set; a no-op otherwise. Lets the mutated volume be checked out-of-band:
 *   RA8_EXFAT_DUMP=/tmp/x ./test_ra8_fs_exfat
 *   # macOS (fsck_exfat needs a block device, not a plain file):
 *   DEV=$(hdiutil attach -nomount -readonly \
 *         -imagekey diskimage-class=CRawDiskImage /tmp/x.bigfile | awk 'NR==1{print $1}')
 *   fsck_exfat -n "${DEV}s1" ; hdiutil detach "$DEV"
 *   # Linux (fsck.exfat validates the raw partition slice directly):
 *   dd if=/tmp/x.bigfile bs=512 skip=2048 count=4096 of=/tmp/vol.img ; fsck.exfat -n /tmp/vol.img
 * #104 confirmed both the BIG.BIN-present and post-unlink images fsck-clean
 * ("The volume RAFS appears to be OK").
 */
/**
 * @brief Maybe dump image.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in] tag Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_maybe_dump_image(const char* tag)
{
  const char* base = getenv("RA8_EXFAT_DUMP");
  if (base == nullptr) {
    return;
  }
  char path[k_exfat_path_cap];
  (void)snprintf(path, sizeof(path), "%s.%s", base, tag);
  const ra8_test_file_result_t result =
    internal_test_file_replace(path, s_img, (size_t)s_blocks * (size_t)k_exfat_test_block_bytes);
  if (result.status != k_ra8_test_file_ok) {
    return;
  }
  if (!internal_output_path("  [dump] ", path, "\n")) {
    s_fail = 1;
  }
}

/* Open the path, read it, and confirm it is HELLO.TXT's content. */
/**
 * @brief Check open reads hello.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] mnt Argument for the bounded test operation.
 * @param[in] path Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_open_reads_hello(ra8_fs_mount_t* mnt, const char* path)
{
  ra8_fs_file_t* fp = nullptr;
  ra8_err_t      e  = ra8_fs_open(mnt, path, k_ra8_fs_mode_read, &fp);
  if (e != k_ra8_ok) {
    ra8_test_output_t    output = {};
    ra8_test_output_fd_t state  = {};
    (void)internal_test_output_fd_init(&output, &state, STDOUT_FILENO);
    (void)internal_test_output_text(&output, "  [FAIL] open(\"");
    (void)internal_test_output_text(&output, path);
    (void)internal_test_output_text(&output, "\") -> ");
    (void)internal_test_output_i64(&output, e);
    (void)internal_test_output_text(&output, "\n");
    s_fail = 1;
    return;
  }
  uint8_t  buf[k_exfat_read_chunk] = {};
  uint32_t got                     = 0U;
  e                                = ra8_fs_read(fp, buf, sizeof(buf) - 1U, &got);
  (void)ra8_fs_close(fp);
  const int            ok     = (e == k_ra8_ok) && (got == (uint32_t)strlen(s_expect)) &&
                                (memcmp(buf, s_expect, strlen(s_expect)) == 0);
  ra8_test_output_t    output = {};
  ra8_test_output_fd_t state  = {};
  (void)internal_test_output_fd_init(&output, &state, STDOUT_FILENO);
  (void)internal_test_output_text(&output, "  [");
  (void)internal_test_output_text(&output, (ok != 0) ? "PASS" : "FAIL");
  (void)internal_test_output_text(&output, "] open(\"");
  (void)internal_test_output_text(&output, path);
  (void)internal_test_output_text(&output, "\") read ");
  (void)internal_test_output_u64(&output, got);
  (void)internal_test_output_text(&output, " bytes back\n");
  if ((ok == 0) || (output.status != k_ra8_test_output_ok)) {
    s_fail = 1;
  }
}

/* exFAT write/create/rename/unlink round-trip, all with leading slashes
 * (#93 covered read; create + rename also have to strip the slash). */
/**
 * @brief Check write path.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] mnt Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_write_path(ra8_fs_mount_t* mnt)
{
  const char*    data = "exFAT write-path payload 0123456789ABCDEF";
  const uint32_t len  = (uint32_t)strlen(data);

  internal_check(ra8_fs_write_file(mnt, "/W83.TXT", (const uint8_t*)data, len) == k_ra8_ok,
                 "write_file(\"/W83.TXT\") with leading slash");
  internal_check(internal_name_present(mnt, "W83.TXT"), "created file stored without the slash");

  ra8_fs_file_t* fp = nullptr;
  if (ra8_fs_open(mnt, "/W83.TXT", k_ra8_fs_mode_read, &fp) == k_ra8_ok) {
    uint8_t   buf[k_exfat_name_cap] = {};
    uint32_t  got                   = 0U;
    ra8_err_t e                     = ra8_fs_read(fp, buf, sizeof(buf) - 1U, &got);
    (void)ra8_fs_close(fp);
    internal_check((e == k_ra8_ok) && (got == len) && (memcmp(buf, data, len) == 0),
                   "written file reads back byte-identical");
  } else {
    internal_check(0, "reopen written file");
  }

  internal_check(ra8_fs_rename(mnt, "/W83.TXT", "/W83R.TXT") == k_ra8_ok,
                 "rename with leading slashes");
  internal_check(internal_name_present(mnt, "W83R.TXT") && !internal_name_present(mnt, "W83.TXT"),
                 "rename moved the entry");
  internal_check(ra8_fs_unlink(mnt, "/W83R.TXT") == k_ra8_ok, "unlink with leading slash");
  internal_check(!internal_name_present(mnt, "W83R.TXT"), "unlink removed the entry");
}

/* #104: multi-cluster exFAT write. A payload larger than one 4 KiB cluster must
 * allocate a contiguous run in the bitmap, read back byte-identical across the
 * cluster boundaries, and free every cluster on unlink -- branches the
 * single-cluster W83.TXT case never reaches. */
/**
 * @brief Check multicluster path.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] mnt Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_multicluster_path(ra8_fs_mount_t* mnt)
{
  static uint8_t s_big[k_mc_payload_bytes];
  static uint8_t s_back[k_mc_payload_bytes];
  for (uint32_t i = 0U; i < k_mc_payload_bytes; i++) {
    s_big[i] = (uint8_t)(((i * k_exfat_pattern_stride) + k_exfat_pattern_bias) & k_byte_mask);
  }

  internal_check(ra8_fs_write_file(mnt, "/BIG.BIN", s_big, k_mc_payload_bytes) == k_ra8_ok,
                 "write_file multi-cluster (> 3 clusters)");
  internal_check(internal_name_present(mnt, "BIG.BIN"), "multi-cluster file listed");

  ra8_fs_file_t* fp = nullptr;
  if (ra8_fs_open(mnt, "/BIG.BIN", k_ra8_fs_mode_read, &fp) == k_ra8_ok) {
    uint32_t  total = 0U;
    ra8_err_t e     = k_ra8_ok;
    while (total < k_mc_payload_bytes) {
      uint32_t got = 0U;
      e            = ra8_fs_read(fp, s_back + total, k_mc_payload_bytes - total, &got);
      if ((e != k_ra8_ok) || (got == 0U)) {
        break;
      }
      total += got;
    }
    (void)ra8_fs_close(fp);
    internal_check((e == k_ra8_ok) && (total == k_mc_payload_bytes) &&
                     (memcmp(s_back, s_big, k_mc_payload_bytes) == 0),
                   "multi-cluster file reads back byte-identical");
  } else {
    internal_check(0, "reopen multi-cluster file");
  }

  /* Image now holds a live multi-cluster file -- snapshot it for the
   * out-of-band fsck_exfat internal_check (acceptance: stays fsck-clean after writes). */
  internal_maybe_dump_image("bigfile");

  internal_check(ra8_fs_rename(mnt, "/BIG.BIN", "/BIG2.BIN") == k_ra8_ok, "multi-cluster rename");
  internal_check(internal_name_present(mnt, "BIG2.BIN") && !internal_name_present(mnt, "BIG.BIN"),
                 "multi-cluster rename moved the entry");
  internal_check(ra8_fs_unlink(mnt, "/BIG2.BIN") == k_ra8_ok,
                 "multi-cluster unlink frees the chain");
  internal_check(!internal_name_present(mnt, "BIG2.BIN"), "multi-cluster file gone after unlink");
}

/* #104: drive the name-matcher's mismatch branches in priv_exfat_take_set --
 * one wrong name of the SAME length (the byte-compare fails) and one of a
 * DIFFERENT length (the length pre-filter fails). Both must report not_found. */
/**
 * @brief exFAT `stat` on the fixture's real directory, file, and a missing name (#609).
 *
 * @details The fixture was written by a real exFAT formatter and carries
 * `.fseventsd` -- a genuine DIRECTORY entry (FileAttributes 0x12: hidden |
 * directory) -- alongside two ordinary files. That is the case the old
 * VFS `stat` got wrong and could not have got right: it opened the path and
 * reported `is_directory = false` unconditionally, so a folder came back as an
 * existing zero-byte file. Nothing else in the suite can assert it, because
 * exFAT directory CREATION is out of scope here (#611), so this image is the
 * only real exFAT directory the tests have.
 *
 * @param[in] mnt The mounted fixture volume.
 *
 * @return Nothing; failures are recorded through ::internal_check.
 *
 * @pre @p mnt is a mounted exFAT volume.
 * @pre The fixture still carries `.fseventsd` and HELLO.TXT.
 * @post No volume state is modified.
 * @post Every assertion has been recorded in ``s_fail``.
 *
 * @note Reads only; the write-path checks run after this one.
 *
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_stat_paths(ra8_fs_mount_t* mnt)
{
  ra8_fs_stat_t dir = {};
  internal_check(ra8_fs_stat(mnt, "/.fseventsd", &dir) == k_ra8_ok,
                 "stat finds the fixture's directory");
  internal_check(dir.is_directory, "a real exFAT directory reports is_directory");
  internal_check(dir.size_bytes == 0U, "a directory reports length 0");
  internal_check((dir.attr & (uint8_t)k_ra8_fs_attr_directory) != 0U,
                 "its attr carries the directory bit");
  internal_check((dir.attr & (uint8_t)k_ra8_fs_attr_hidden) != 0U,
                 "and its hidden bit survives -- the attr is the entry's, not a constant");

  ra8_fs_stat_t file = {};
  internal_check(ra8_fs_stat(mnt, "/HELLO.TXT", &file) == k_ra8_ok, "stat finds an ordinary file");
  internal_check(!file.is_directory, "a file does not report is_directory");
  internal_check(file.size_bytes == (uint32_t)strlen(s_expect), "a file reports its real length");

  ra8_fs_stat_t gone = {};
  internal_check(ra8_fs_stat(mnt, "/NOPE.TXT", &gone) == k_ra8_err_not_found,
                 "a missing name is not-found, not an empty file");

  ra8_fs_stat_t root = {};
  internal_check(ra8_fs_stat(mnt, "/", &root) == k_ra8_ok, "stat resolves the volume root");
  internal_check(root.is_directory, "the root reports as a directory");
}

/**
 * @brief Check lookup mismatch branches.
 * @details Performs one bounded, deterministic operation for this host test.
 * @param[in,out] mnt Argument for the bounded test operation.
 * @pre Pointer arguments, when present, address their documented test storage.
 * @pre Scalar arguments satisfy the bounds asserted by this test helper.
 * @post The helper completes only the bounded test operation described above.
 * @post Failures are returned or reported through the test assertion surface.
 * @note Test-only helper with no production ABI.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_check_lookup_mismatch_branches(ra8_fs_mount_t* mnt)
{
  ra8_fs_file_t* nf = nullptr;
  internal_check(ra8_fs_open(mnt, "/WORLD.TXT", k_ra8_fs_mode_read, &nf) == k_ra8_err_not_found,
                 "same-length wrong name -> not_found (byte-compare branch)");
  internal_check(ra8_fs_open(mnt, "/AB.TX", k_ra8_fs_mode_read, &nf) == k_ra8_err_not_found,
                 "wrong-length name -> not_found (length-prefilter branch)");
}

/**
 * @brief Slurp the exFAT fixture image at @p path into the RAM-backed disk.
 *
 * @details
 * Sizes the file by seeking to its end, allocates the whole image, and reads it
 * in one go, then derives the block count the backend reports. Every failure
 * path names what went wrong on the result descriptor and releases it, so the caller
 * only has to distinguish loaded from not-loaded. The image is deliberately
 * read whole rather than paged: the tests seek all over it, and a fixture small
 * enough to commit is small enough to hold.
 *
 * @param[in] path Filesystem path to the fixture image; must be non-NULL.
 *
 * @return Whether the image is now resident and addressable.
 * @retval true  `s_img` holds the image and `s_blocks` its 512-byte block count.
 * @retval false The image could not be read; the reason was printed.
 *
 * @pre @p path names a readable regular file.
 * @pre `s_img` is unset -- this runs once, before any mount.
 * @post On success `s_img` and `s_blocks` describe the whole image.
 * @post On failure no file handle leaks, whichever step failed.
 *
 * @note Not thread-safe; publishes to file-scope state.
 *
 * @see internal_be_read()  The backend that serves blocks out of `s_img`.

 * @since 0.1.0
*/
RA8_INTERNAL static bool internal_load_fixture_image(const char* path)
{
  ra8_test_file_result_t probe = internal_test_file_read(path, nullptr, 0U, nullptr, 0U);
  if (probe.status != k_ra8_test_file_capacity) {
    (void)internal_output_path("FAIL: cannot open fixture ", path, "\n");
    return false;
  }
  const size_t max_image = (size_t)UINT32_MAX * (size_t)k_exfat_test_block_bytes;
  if ((probe.required == 0U) || (probe.required > max_image)) {
    (void)internal_output_path("FAIL: cannot size fixture ", path, "\n");
    return false;
  }
  s_img            = malloc(probe.required);
  uint8_t* staging = malloc(probe.required);
  if ((s_img == nullptr) || (staging == nullptr)) {
    free(s_img);
    free(staging);
    s_img = nullptr;
    (void)internal_test_output_fd_text(STDOUT_FILENO, "FAIL: out of memory for fixture\n");
    return false;
  }
  const ra8_test_file_result_t result =
    internal_test_file_read(path, s_img, probe.required, staging, probe.required);
  free(staging);
  if (result.status != k_ra8_test_file_ok) {
    free(s_img);
    s_img = nullptr;
    (void)internal_test_output_fd_text(STDOUT_FILENO, "FAIL: short read of fixture\n");
    return false;
  }
  s_blocks = (uint32_t)(result.transferred / (size_t)k_exfat_test_block_bytes);
  return true;
}

int main(int argc, char** argv)
{
  const char* path = (argc > 1) ? argv[1] : RA8_EXFAT_FIXTURE;
  if (!internal_load_fixture_image(path)) {
    return (int)k_exfat_fs_test_exit_fixture;
  }

  ra8_fs_backend_t be  = {.read_block   = internal_be_read,
                          .write_block  = internal_be_write,
                          .get_capacity = internal_be_cap,
                          .erase_blocks = nullptr,
                          .ctx          = nullptr};
  ra8_fs_mount_t*  mnt = nullptr;
  ra8_err_t        e   = ra8_fs_mount(&be, &mnt);
  internal_check(e == k_ra8_ok, "mount succeeds");
  internal_check((mnt != nullptr) && (mnt->type == k_ra8_fs_type_exfat),
                 "volume detected as exFAT");
  if ((e != k_ra8_ok) || (mnt == nullptr)) {
    return (int)k_exfat_fs_test_exit_failed;
  }

  internal_check(ra8_fs_listdir(mnt, "/", internal_on_entry, nullptr) == k_ra8_ok,
                 "listdir root succeeds");
  internal_check(s_found_hello == 1, "listdir finds HELLO.TXT");

  /* #93: a leading slash must resolve on exFAT just like it does on FAT. */
  internal_check_open_reads_hello(mnt, "/HELLO.TXT");
  internal_check_open_reads_hello(mnt, "HELLO.TXT");

  /* Negative: a missing file still reports not-found. */
  ra8_fs_file_t* nf = nullptr;
  internal_check(ra8_fs_open(mnt, "/NOPE.TXT", k_ra8_fs_mode_read, &nf) == k_ra8_err_not_found,
                 "missing file -> not_found");

  internal_check_stat_paths(mnt);
  internal_check_write_path(mnt);
  internal_check_multicluster_path(mnt);
  internal_check_lookup_mismatch_branches(mnt);
  internal_maybe_dump_image("empty"); /* all files unlinked -> back to a clean root. */

  if (internal_test_output_fd_text(STDOUT_FILENO,
                                   (s_fail != 0) ? "\nRESULT: FAIL\n" : "\nRESULT: PASS\n") !=
      k_ra8_test_output_ok) {
    s_fail = 1;
  }
  return s_fail;
}
