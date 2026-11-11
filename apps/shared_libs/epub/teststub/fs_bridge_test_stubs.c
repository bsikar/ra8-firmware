/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 *
 * Test-only stand-ins for the ra8_fs seam and the epub core entry points
 * (epub_open_streamed, epub_close) behind the epub_fs bridge. Mirrors
 * zip_guard_test_stubs.c. Raw C ABI: the bridge calls these through the
 * unchanged C headers, and the knobs below drive every error path.
 */
#include <stdint.h>

int fs_open_err, fs_seek_err, fs_read_err;
uint64_t fs_size_value = 100;
uint32_t fs_read_got;
int fs_open_calls, fs_close_calls;
int fs_streamed_err, fs_close_err, epub_close_calls;
uint64_t fs_streamed_size_seen;
void* fs_streamed_ctx_seen;
void* fs_streamed_read_seen;
void* fs_stub_file_ptr;
void* fs_stub_book_ptr;

static int fs_the_file;
static int fs_the_book;

uint16_t ra8_fs_open(void* mount, const char* path, int mode, void** out_file)
{
  (void)mount;
  (void)path;
  (void)mode;
  fs_open_calls++;
  if (fs_open_err != 0) {
    return (uint16_t)fs_open_err;
  }
  *out_file = &fs_the_file;
  return 0;
}

uint16_t ra8_fs_close(void* file)
{
  (void)file;
  fs_close_calls++;
  return 0;
}

uint16_t ra8_fs_seek(void* file, uint64_t offset_bytes)
{
  (void)file;
  (void)offset_bytes;
  return (uint16_t)fs_seek_err;
}

uint16_t ra8_fs_read(void* file, uint8_t* buf, uint32_t max_len, uint32_t* got_len)
{
  (void)file;
  (void)buf;
  (void)max_len;
  if (fs_read_err != 0) {
    return (uint16_t)fs_read_err;
  }
  *got_len = fs_read_got;
  return 0;
}

uint16_t ra8_fs_size(const void* file, uint64_t* out_bytes)
{
  (void)file;
  *out_bytes = fs_size_value;
  return 0;
}

/* epub core stand-ins. The media layout is read by raw offset (fn ptr, ctx,
 * u64 size) to keep this TU free of the epub.h include web. */
uint16_t epub_open_streamed(void* media, const char* path, void* out_book)
{
  (void)path;
  (void)out_book;
  fs_streamed_size_seen = *(uint64_t*)((char*)media + 16);
  fs_streamed_ctx_seen = *(void**)((char*)media + 8);
  fs_streamed_read_seen = *(void**)media;
  return (uint16_t)fs_streamed_err;
}

uint16_t epub_close(void* book)
{
  (void)book;
  epub_close_calls++;
  return (uint16_t)fs_close_err;
}

/* Addresses the tests compare handles against. */
const void* fs_stub_file_addr = &fs_the_file;
const void* fs_stub_book_addr = &fs_the_book;

/* Keep the mutable alias pointers the Zig tests read. */
static void* fs_stub_file_ptr_init = &fs_the_file;
static void* fs_stub_book_ptr_init = &fs_the_book;

/* The linker wants definitions for the extern vars the Zig side declares;
 * alias them to the real addresses at load time via constructors is overkill,
 * so assign once in a constructor. */
__attribute__((constructor)) static void fs_bridge_stubs_init(void)
{
  fs_stub_file_ptr = fs_stub_file_ptr_init;
  fs_stub_book_ptr = fs_stub_book_ptr_init;
}
