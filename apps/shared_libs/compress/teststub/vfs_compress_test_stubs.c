/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Brighton Sikarskie
 *
 * Test stand-ins for the seams ra8_vfs_compress_abi.zig links: a RAM-backed
 * VFS/fs pair and a reversible codec stub. The real codec has its own
 * package tests (RA8FW-1155); the real ra8_io fabric is exercised by the
 * stub-driver link and the host apps. No libc headers (freestanding-safe).
 */

#include <stddef.h>
#include <stdint.h>

typedef uint16_t ra8_err_t;
#define K_RA8_OK 0u
#define K_RA8_ERR_INVALID_SIZE 0x105u
#define K_RA8_ERR_NOT_FOUND 0x106u
#define K_RA8_ERR_NULL_PTR 0x504u

/* ra8_fs_types.h: k_ra8_fs_mode_read = 0, k_ra8_fs_mode_write = 1. */
#define K_RA8_FS_MODE_READ 0
#define K_RA8_FS_MODE_WRITE 1

static uint8_t  ram[256];
static uint32_t ram_len = 0;
static uint32_t write_calls = 0;
static uint32_t open_write_calls = 0;

int ra8_io_vfs_open(const char* path, int mode, void*** out_file)
{
  if (path == (void*)0 || out_file == (void*)0) {
    return (int)K_RA8_ERR_NULL_PTR;
  }
  /* Refuse "absent:/..." like a missing mount (k_ra8_err_not_found). */
  if (path[0] == 'a' && path[1] == 'b' && path[2] == 's') {
    return (int)K_RA8_ERR_NOT_FOUND;
  }
  if (mode == K_RA8_FS_MODE_WRITE) {
    open_write_calls++;
  }
  *out_file = (void**)(uintptr_t)0x1000;
  return (int)K_RA8_OK;
}

int ra8_fs_write(void** file, const uint8_t* buf, uint32_t len)
{
  (void)file;
  if (buf == (void*)0) {
    return (int)K_RA8_ERR_NULL_PTR;
  }
  for (uint32_t i = 0; i < len; i++) {
    ram[i] = buf[i];
  }
  ram_len = len;
  write_calls++;
  return (int)K_RA8_OK;
}

int ra8_fs_read(void** file, uint8_t* buf, uint32_t max_len, uint32_t* got_len)
{
  (void)file;
  if (buf == (void*)0 || got_len == (void*)0) {
    return (int)K_RA8_ERR_NULL_PTR;
  }
  uint32_t n = (ram_len < max_len) ? ram_len : max_len;
  for (uint32_t i = 0; i < n; i++) {
    buf[i] = ram[i];
  }
  *got_len = n;
  return (int)K_RA8_OK;
}

int ra8_fs_close(void** file)
{
  (void)file;
  return (int)K_RA8_OK;
}

/* Reversible codec stub: "RAW^" + XOR 0x5A of the payload. */
int ra8_compress(const uint8_t* src, uint32_t src_len, uint8_t* out, uint32_t out_cap,
                 void* scratch, uint32_t scratch_len, uint32_t* out_len)
{
  (void)scratch;
  (void)scratch_len;
  if (src == (void*)0 || out == (void*)0 || out_len == (void*)0) {
    return (int)K_RA8_ERR_NULL_PTR;
  }
  if (src_len + 4u > out_cap) {
    return (int)K_RA8_ERR_INVALID_SIZE;
  }
  out[0] = 'R';
  out[1] = 'A';
  out[2] = 'W';
  out[3] = '^';
  for (uint32_t i = 0; i < src_len; i++) {
    out[4 + i] = (uint8_t)(src[i] ^ 0x5A);
  }
  *out_len = src_len + 4u;
  return (int)K_RA8_OK;
}

int ra8_decompress(const uint8_t* src, uint32_t src_len, uint8_t* out, uint32_t out_cap,
                   uint32_t* out_len)
{
  if (src == (void*)0 || out == (void*)0 || out_len == (void*)0) {
    return (int)K_RA8_ERR_NULL_PTR;
  }
  if (src_len < 4u) {
    return (int)0x103; /* k_ra8_err_invalid_arg */
  }
  uint32_t n = src_len - 4u;
  if (n > out_cap) {
    return (int)K_RA8_ERR_INVALID_SIZE;
  }
  for (uint32_t i = 0; i < n; i++) {
    out[i] = (uint8_t)(src[4 + i] ^ 0x5A);
  }
  *out_len = n;
  return (int)K_RA8_OK;
}
