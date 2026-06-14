/**
 * @file tests/host/exfat_fs_test.c
 * @brief Standalone host test for ra_fs exFAT read (#85) + the leading-slash
 *        open regression (#93).
 *
 * @details
 * Links ONLY ra_fs_fat.c (no ra_core_hal -> no ra_time weak-extern), so it
 * builds and runs on macOS and Linux alike, unlike the unity suite. It opens
 * a tiny real exFAT image (decompressed from a checked-in .gz at build time,
 * path injected via ``RA_EXFAT_FIXTURE``) over an in-memory block backend and
 * asserts:
 *   1. mount succeeds and detects exFAT,
 *   2. listdir enumerates the known files,
 *   3. open("/HELLO.TXT") -- WITH a leading slash -- succeeds (the #93 fix;
 *      it returned k_ra_err_not_found before because the exFAT name matcher
 *      did not strip leading slashes like the FAT path does),
 *   4. open("HELLO.TXT") -- without a slash -- also succeeds,
 *   5. the file content reads back byte-for-byte.
 *
 * The fixture holds HELLO.TXT (the expected string below) + NOTES.TXT, made
 * with a real exFAT formatter so the on-disk up-case table / name hashes /
 * allocation bitmap are genuine.
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

#include "ra_fs.h"

#ifndef RA_EXFAT_FIXTURE
#define RA_EXFAT_FIXTURE "exfat_small.img"
#endif

static const char k_expect[] = "Hello exFAT from the ra_fs standalone test 1234567890\n";

static uint8_t* g_img;
static uint32_t g_blocks;
static int      g_found_hello;
static int      g_fail;

static ra_err_t be_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  (void)ctx;
  memcpy(buf, g_img + (size_t)lba * 512U, (size_t)count * 512U);
  return k_ra_ok;
}
static ra_err_t be_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  (void)ctx;
  memcpy(g_img + (size_t)lba * 512U, buf, (size_t)count * 512U);
  return k_ra_ok;
}
static ra_err_t be_cap(void* ctx, uint32_t* bc, uint32_t* bs)
{
  (void)ctx;
  *bc = g_blocks;
  *bs = 512U;
  return k_ra_ok;
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
static int name_present(ra_fs_mount_t* mnt, const char* name)
{
  char needle[64];
  g_names[0] = '\0';
  (void)ra_fs_listdir(mnt, "/", on_entry, nullptr);
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

/* Open the path, read it, and confirm it is HELLO.TXT's content. */
static void check_open_reads_hello(ra_fs_mount_t* mnt, const char* path)
{
  ra_fs_file_t* fp = nullptr;
  ra_err_t      e  = ra_fs_open(mnt, path, k_ra_fs_mode_read, &fp);
  if (e != k_ra_ok) {
    printf("  [FAIL] open(\"%s\") -> %d\n", path, (int)e);
    g_fail = 1;
    return;
  }
  uint8_t  buf[128] = {};
  uint32_t got      = 0U;
  e                 = ra_fs_read(fp, buf, sizeof(buf) - 1U, &got);
  (void)ra_fs_close(fp);
  const int ok = (e == k_ra_ok) && (got == (uint32_t)strlen(k_expect)) &&
                 (memcmp(buf, k_expect, strlen(k_expect)) == 0);
  printf("  [%s] open(\"%s\") read %u bytes back\n", ok ? "PASS" : "FAIL", path, got);
  if (ok == 0) {
    g_fail = 1;
  }
}

/* exFAT write/create/rename/unlink round-trip, all with leading slashes
 * (#93 covered read; create + rename also have to strip the slash). */
static void check_write_path(ra_fs_mount_t* mnt)
{
  const char*    data = "exFAT write-path payload 0123456789ABCDEF";
  const uint32_t len  = (uint32_t)strlen(data);

  check(ra_fs_write_file(mnt, "/W83.TXT", (const uint8_t*)data, len) == k_ra_ok,
        "write_file(\"/W83.TXT\") with leading slash");
  check(name_present(mnt, "W83.TXT"), "created file stored without the slash");

  ra_fs_file_t* fp = nullptr;
  if (ra_fs_open(mnt, "/W83.TXT", k_ra_fs_mode_read, &fp) == k_ra_ok) {
    uint8_t  buf[64] = {};
    uint32_t got     = 0U;
    ra_err_t e       = ra_fs_read(fp, buf, sizeof(buf) - 1U, &got);
    (void)ra_fs_close(fp);
    check((e == k_ra_ok) && (got == len) && (memcmp(buf, data, len) == 0),
          "written file reads back byte-identical");
  } else {
    check(0, "reopen written file");
  }

  check(ra_fs_rename(mnt, "/W83.TXT", "/W83R.TXT") == k_ra_ok, "rename with leading slashes");
  check(name_present(mnt, "W83R.TXT") && !name_present(mnt, "W83.TXT"), "rename moved the entry");
  check(ra_fs_unlink(mnt, "/W83R.TXT") == k_ra_ok, "unlink with leading slash");
  check(!name_present(mnt, "W83R.TXT"), "unlink removed the entry");
}

int main(int argc, char** argv)
{
  const char* path = (argc > 1) ? argv[1] : RA_EXFAT_FIXTURE;
  FILE*       f    = fopen(path, "rb");
  if (f == nullptr) {
    printf("FAIL: cannot open fixture %s\n", path);
    return 2;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  g_img     = malloc((size_t)sz);
  size_t rd = fread(g_img, 1U, (size_t)sz, f);
  fclose(f);
  if (rd != (size_t)sz) {
    printf("FAIL: short read of fixture\n");
    return 2;
  }
  g_blocks = (uint32_t)((size_t)sz / 512U);

  ra_fs_backend_t be  = {be_read, be_write, be_cap, nullptr};
  ra_fs_mount_t*  mnt = nullptr;
  ra_err_t        e   = ra_fs_mount(&be, &mnt);
  check(e == k_ra_ok, "mount succeeds");
  check((mnt != nullptr) && (mnt->type == k_ra_fs_type_exfat), "volume detected as exFAT");
  if ((e != k_ra_ok) || (mnt == nullptr)) {
    return 1;
  }

  check(ra_fs_listdir(mnt, "/", on_entry, nullptr) == k_ra_ok, "listdir root succeeds");
  check(g_found_hello == 1, "listdir finds HELLO.TXT");

  /* #93: a leading slash must resolve on exFAT just like it does on FAT. */
  check_open_reads_hello(mnt, "/HELLO.TXT");
  check_open_reads_hello(mnt, "HELLO.TXT");

  /* Negative: a missing file still reports not-found. */
  ra_fs_file_t* nf = nullptr;
  check(ra_fs_open(mnt, "/NOPE.TXT", k_ra_fs_mode_read, &nf) == k_ra_err_not_found,
        "missing file -> not_found");

  check_write_path(mnt);

  printf("\n%s\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS");
  return g_fail;
}
