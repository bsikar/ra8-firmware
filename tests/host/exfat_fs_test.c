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
 * It then exercises the exFAT write path (#104) on the same mounted volume:
 *   6. write_file + read-back + rename + unlink of a single-cluster file,
 *   7. the same cycle for a multi-cluster file (a payload spanning four 4 KiB
 *      clusters), proving the bitmap allocation, the read cluster-walk, and the
 *      multi-cluster free loop that the single-cluster case cannot reach,
 *   8. negative lookups (same-length wrong name; wrong-length name) that drive
 *      the matcher's mismatch branches both ways.
 *
 * @par MC/DC:
 * Every decision in the exFAT section of ra8_fs_fat.c (priv_exfat_create /
 * _alloc_write / _build_set / _take_set / _find_set / _free_clusters and the
 * name-hash / set-checksum helpers) is single-condition -- there is not one
 * ``&&`` / ``||`` compound decision in the whole exFAT body -- so MC/DC for it
 * collapses to branch coverage: each decision must be taken both ways. The
 * cases above drive the match / mismatch and single- / multi-cluster branches
 * accordingly.
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

#include "ra8_fs.h"

#ifndef RA8_EXFAT_FIXTURE
/** @brief RA8 EXFAT FIXTURE. */
#define RA8_EXFAT_FIXTURE "exfat_small.img"
#endif

static const char k_expect[] = "Hello exFAT from the ra_fs standalone test 1234567890\n";

/* #104: a payload that spans several 4 KiB exFAT clusters (with an odd tail) so
 * the write/read/free paths exercise their multi-cluster branches. */
enum : uint32_t { k_mc_payload_bytes = 12425U /**< Mc payload bytes. */ };

static uint8_t* g_img;
static uint32_t g_blocks;
static int      g_found_hello;
static int      g_fail;

static ra8_err_t be_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  memcpy(buf, g_img + ((size_t)lba * 512U), (size_t)count * 512U);
  return k_ra8_ok;
}
static ra8_err_t be_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  memcpy(g_img + ((size_t)lba * 512U), buf, (size_t)count * 512U);
  return k_ra8_ok;
}
static ra8_err_t be_cap(void* ctx, uint32_t* bc, uint32_t* bs)
{
  (void)ctx;
  *bc = g_blocks;
  *bs = 512U;
  return k_ra8_ok;
}

static char g_names[1024];
static void on_entry(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)attr;
  (void)size;
  (void)ctx;
  if (strcmp(name, "HELLO.TXT") == 0) {
    g_found_hello = 1;
  }
  if ((strlen(g_names) + strlen(name) + 2U) < sizeof(g_names)) {
    strcat(g_names, name);
    strcat(g_names, "|");
  }
}

/* Re-list the root and report whether `name` is currently an entry. */
static int name_present(ra8_fs_mount_t* mnt, const char* name)
{
  char needle[64];
  g_names[0] = '\0';
  (void)ra8_fs_listdir(mnt, "/", on_entry, nullptr);
  (void)snprintf(needle, sizeof(needle), "%s|", name);
  return strstr(g_names, needle) != nullptr;
}

static void check(int cond, const char* what)
{
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
  if (cond == 0) {
    g_fail = 1;
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
static void maybe_dump_image(const char* tag)
{
  const char* base = getenv("RA8_EXFAT_DUMP");
  if (base == nullptr) {
    return;
  }
  char path[512];
  (void)snprintf(path, sizeof(path), "%s.%s", base, tag);
  FILE* o = fopen(path, "wb");
  if (o == nullptr) {
    return;
  }
  (void)fwrite(g_img, 1U, (size_t)g_blocks * 512U, o);
  (void)fclose(o);
  printf("  [dump] %s\n", path);
}

/* Open the path, read it, and confirm it is HELLO.TXT's content. */
static void check_open_reads_hello(ra8_fs_mount_t* mnt, const char* path)
{
  ra8_fs_file_t* fp = nullptr;
  ra8_err_t      e  = ra8_fs_open(mnt, path, k_ra8_fs_mode_read, &fp);
  if (e != k_ra8_ok) {
    printf("  [FAIL] open(\"%s\") -> %d\n", path, (int)e);
    g_fail = 1;
    return;
  }
  uint8_t  buf[128] = {};
  uint32_t got      = 0U;
  e                 = ra8_fs_read(fp, buf, sizeof(buf) - 1U, &got);
  (void)ra8_fs_close(fp);
  const int ok = (e == k_ra8_ok) && (got == (uint32_t)strlen(k_expect)) &&
                 (memcmp(buf, k_expect, strlen(k_expect)) == 0);
  printf("  [%s] open(\"%s\") read %u bytes back\n", ok ? "PASS" : "FAIL", path, got);
  if (ok == 0) {
    g_fail = 1;
  }
}

/* exFAT write/create/rename/unlink round-trip, all with leading slashes
 * (#93 covered read; create + rename also have to strip the slash). */
static void check_write_path(ra8_fs_mount_t* mnt)
{
  const char*    data = "exFAT write-path payload 0123456789ABCDEF";
  const uint32_t len  = (uint32_t)strlen(data);

  check(ra8_fs_write_file(mnt, "/W83.TXT", (const uint8_t*)data, len) == k_ra8_ok,
        "write_file(\"/W83.TXT\") with leading slash");
  check(name_present(mnt, "W83.TXT"), "created file stored without the slash");

  ra8_fs_file_t* fp = nullptr;
  if (ra8_fs_open(mnt, "/W83.TXT", k_ra8_fs_mode_read, &fp) == k_ra8_ok) {
    uint8_t   buf[64] = {};
    uint32_t  got     = 0U;
    ra8_err_t e       = ra8_fs_read(fp, buf, sizeof(buf) - 1U, &got);
    (void)ra8_fs_close(fp);
    check((e == k_ra8_ok) && (got == len) && (memcmp(buf, data, len) == 0),
          "written file reads back byte-identical");
  } else {
    check(0, "reopen written file");
  }

  check(ra8_fs_rename(mnt, "/W83.TXT", "/W83R.TXT") == k_ra8_ok, "rename with leading slashes");
  check(name_present(mnt, "W83R.TXT") && !name_present(mnt, "W83.TXT"), "rename moved the entry");
  check(ra8_fs_unlink(mnt, "/W83R.TXT") == k_ra8_ok, "unlink with leading slash");
  check(!name_present(mnt, "W83R.TXT"), "unlink removed the entry");
}

/* #104: multi-cluster exFAT write. A payload larger than one 4 KiB cluster must
 * allocate a contiguous run in the bitmap, read back byte-identical across the
 * cluster boundaries, and free every cluster on unlink -- branches the
 * single-cluster W83.TXT case never reaches. */
static void check_multicluster_path(ra8_fs_mount_t* mnt)
{
  static uint8_t s_big[k_mc_payload_bytes];
  static uint8_t s_back[k_mc_payload_bytes];
  for (uint32_t i = 0U; i < k_mc_payload_bytes; i++) {
    s_big[i] = (uint8_t)(((i * 31U) + 7U) & 0xFFU);
  }

  check(ra8_fs_write_file(mnt, "/BIG.BIN", s_big, k_mc_payload_bytes) == k_ra8_ok,
        "write_file multi-cluster (> 3 clusters)");
  check(name_present(mnt, "BIG.BIN"), "multi-cluster file listed");

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
    check((e == k_ra8_ok) && (total == k_mc_payload_bytes) &&
            (memcmp(s_back, s_big, k_mc_payload_bytes) == 0),
          "multi-cluster file reads back byte-identical");
  } else {
    check(0, "reopen multi-cluster file");
  }

  /* Image now holds a live multi-cluster file -- snapshot it for the
   * out-of-band fsck_exfat check (acceptance: stays fsck-clean after writes). */
  maybe_dump_image("bigfile");

  check(ra8_fs_rename(mnt, "/BIG.BIN", "/BIG2.BIN") == k_ra8_ok, "multi-cluster rename");
  check(name_present(mnt, "BIG2.BIN") && !name_present(mnt, "BIG.BIN"),
        "multi-cluster rename moved the entry");
  check(ra8_fs_unlink(mnt, "/BIG2.BIN") == k_ra8_ok, "multi-cluster unlink frees the chain");
  check(!name_present(mnt, "BIG2.BIN"), "multi-cluster file gone after unlink");
}

/* #104: drive the name-matcher's mismatch branches in priv_exfat_take_set --
 * one wrong name of the SAME length (the byte-compare fails) and one of a
 * DIFFERENT length (the length pre-filter fails). Both must report not_found. */
static void check_lookup_mismatch_branches(ra8_fs_mount_t* mnt)
{
  ra8_fs_file_t* nf = nullptr;
  check(ra8_fs_open(mnt, "/WORLD.TXT", k_ra8_fs_mode_read, &nf) == k_ra8_err_not_found,
        "same-length wrong name -> not_found (byte-compare branch)");
  check(ra8_fs_open(mnt, "/AB.TX", k_ra8_fs_mode_read, &nf) == k_ra8_err_not_found,
        "wrong-length name -> not_found (length-prefilter branch)");
}

int main(int argc, char** argv)
{
  const char* path = (argc > 1) ? argv[1] : RA8_EXFAT_FIXTURE;
  FILE*       f    = fopen(path, "rb");
  if (f == nullptr) {
    printf("FAIL: cannot open fixture %s\n", path);
    return 2;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    printf("FAIL: cannot seek fixture %s\n", path);
    (void)fclose(f);
    return 2;
  }
  const long sz = ftell(f);
  if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
    printf("FAIL: cannot size fixture %s\n", path);
    (void)fclose(f);
    return 2;
  }
  g_img = malloc((size_t)sz);
  if (g_img == nullptr) {
    printf("FAIL: out of memory for fixture\n");
    (void)fclose(f);
    return 2;
  }
  const size_t rd = fread(g_img, 1U, (size_t)sz, f);
  (void)fclose(f);
  if (rd != (size_t)sz) {
    printf("FAIL: short read of fixture\n");
    return 2;
  }
  g_blocks = (uint32_t)((size_t)sz / 512U);

  ra8_fs_backend_t be  = {.read_block   = be_read,
                          .write_block  = be_write,
                          .get_capacity = be_cap,
                          .erase_blocks = nullptr,
                          .ctx          = nullptr};
  ra8_fs_mount_t*  mnt = nullptr;
  ra8_err_t        e   = ra8_fs_mount(&be, &mnt);
  check(e == k_ra8_ok, "mount succeeds");
  check((mnt != nullptr) && (mnt->type == k_ra8_fs_type_exfat), "volume detected as exFAT");
  if ((e != k_ra8_ok) || (mnt == nullptr)) {
    return 1;
  }

  check(ra8_fs_listdir(mnt, "/", on_entry, nullptr) == k_ra8_ok, "listdir root succeeds");
  check(g_found_hello == 1, "listdir finds HELLO.TXT");

  /* #93: a leading slash must resolve on exFAT just like it does on FAT. */
  check_open_reads_hello(mnt, "/HELLO.TXT");
  check_open_reads_hello(mnt, "HELLO.TXT");

  /* Negative: a missing file still reports not-found. */
  ra8_fs_file_t* nf = nullptr;
  check(ra8_fs_open(mnt, "/NOPE.TXT", k_ra8_fs_mode_read, &nf) == k_ra8_err_not_found,
        "missing file -> not_found");

  check_write_path(mnt);
  check_multicluster_path(mnt);
  check_lookup_mismatch_branches(mnt);
  maybe_dump_image("empty"); /* all files unlinked -> back to a clean root. */

  printf("\n%s\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS");
  return g_fail;
}
