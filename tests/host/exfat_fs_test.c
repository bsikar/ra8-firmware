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

static void on_entry(const char* name, uint8_t attr, uint32_t size, void* ctx)
{
  (void)attr;
  (void)size;
  (void)ctx;
  if (strcmp(name, "HELLO.TXT") == 0) {
    g_found_hello = 1;
  }
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

  printf("\n%s\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS");
  return g_fail;
}
