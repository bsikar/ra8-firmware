// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
// C stand-ins for the externs the chapter/glyph TU (src/epub_chapter_abi.zig)
// links: miniz's reader trio, the zip-guard entry hook, the stb_truetype
// family and the sfnt guard. Everything is stub-driven so the tests never
// touch a real archive or a real font: state knobs select the failure path
// and the happy path, and captures record what the TU passed through.

#include <stdint.h>
#include <stddef.h>

static void ts_memset(void* d, int c, size_t n) {
  unsigned char* p = (unsigned char*)d;
  while (n--) { *p++ = (unsigned char)c; }
}

static void ts_memcpy(void* d, const void* s, size_t n) {
  unsigned char* pd = (unsigned char*)d;
  const unsigned char* ps = (const unsigned char*)s;
  while (n--) { *pd++ = *ps++; }
}

typedef uint16_t ra8_err_t;
#define K_RA8_OK ((ra8_err_t)0)

typedef struct {
  uint32_t m_file_index;
  uint64_t m_central_dir_ofs;
  uint16_t m_version_made_by;
  uint16_t m_version_needed;
  uint16_t m_bit_flag;
  uint16_t m_method;
  uint32_t m_crc32;
  uint64_t m_comp_size;
  uint64_t m_uncomp_size;
} mz_zip_archive_file_stat;

int teststub_locate_miss_prefixed = 0;
int teststub_fail_stat            = 0;
int teststub_fail_extract         = 0;
ra8_err_t teststub_guard_result   = K_RA8_OK;
uint64_t teststub_entry_uncomp    = 16;
int teststub_font_offset          = 0;
int teststub_fail_sfnt_dir        = 0;
int teststub_fail_font_init       = 0;
int teststub_box_x0               = 0;
int teststub_box_y0               = 0;
int teststub_box_x1               = 8;
int teststub_box_y1               = 16;

static char capture_located_prefixed[256];
static char capture_located_bare[256];
static char capture_extract_buf[512];
static size_t capture_extract_len;
static int capture_locate_calls;
static float capture_scale;
static int capture_make_stride;
static int capture_make_codepoint;

void teststub_reset(void) {
  capture_located_prefixed[0] = '\0';
  capture_located_bare[0] = '\0';
  capture_extract_buf[0] = '\0';
  capture_extract_len = 0;
  capture_locate_calls = 0;
  capture_scale = 0.0f;
  capture_make_stride = 0;
  capture_make_codepoint = 0;
}

const char* teststub_located_prefixed(void) { return capture_located_prefixed; }
const char* teststub_located_bare(void) { return capture_located_bare; }
const char* teststub_extract_buf(void) { return capture_extract_buf; }
size_t teststub_extract_len(void) { return capture_extract_len; }
int teststub_locate_calls(void) { return capture_locate_calls; }
float teststub_scale(void) { return capture_scale; }
int teststub_make_stride(void) { return capture_make_stride; }
int teststub_make_codepoint(void) { return capture_make_codepoint; }

int mz_zip_reader_locate_file(void* zip, const char* name, const char* comment, unsigned flags) {
  (void)zip; (void)comment; (void)flags;
  ++capture_locate_calls;
  char* slot = (capture_locate_calls == 1) ? capture_located_prefixed : capture_located_bare;
  // bounded copy of the name into the slot
  size_t i = 0;
  while ((i < sizeof(capture_located_prefixed) - 1) && name[i] != '\0') { slot[i] = name[i]; ++i; }
  slot[i] = '\0';
  if ((capture_locate_calls == 1) && teststub_locate_miss_prefixed) {
    return -1;
  }
  return name[0] == '\0' ? -1 : (capture_locate_calls == 1 ? 2 : 3);
}

int mz_zip_reader_file_stat(void* zip, unsigned index, mz_zip_archive_file_stat* stat) {
  (void)zip;
  if (teststub_fail_stat) return 0;
  ts_memset(stat, 0, sizeof(*stat));
  stat->m_file_index = index;
  stat->m_uncomp_size = teststub_entry_uncomp;
  return 1;
}

ra8_err_t priv_epub_zip_guard_entry(const mz_zip_archive_file_stat* stat) {
  (void)stat;
  return teststub_guard_result;
}

int mz_zip_reader_extract_to_mem(void* zip, unsigned index, void* buf, size_t len, unsigned flags) {
  (void)zip; (void)index; (void)flags;
  if (teststub_fail_extract) return 0;
  capture_extract_len = teststub_entry_uncomp < len ? (size_t)teststub_entry_uncomp : len;
  ts_memset(buf, 0xAB, capture_extract_len);
  ts_memcpy(capture_extract_buf, buf, capture_extract_len < sizeof(capture_extract_buf) ? capture_extract_len : sizeof(capture_extract_buf));
  return 1;
}

int stbtt_GetFontOffsetForIndex(const unsigned char* data, int index) {
  (void)data; (void)index;
  return teststub_font_offset;
}

int stbtt_InitFont(void* info, const unsigned char* data, int data_size, int offset) {
  (void)info; (void)data; (void)data_size; (void)offset;
  return teststub_fail_font_init ? 0 : 1;
}

int ra8_stbtt_sfnt_dir_in_bounds(const unsigned char* data, size_t len, unsigned fontstart) {
  (void)data; (void)len; (void)fontstart;
  return !teststub_fail_sfnt_dir;
}

float stbtt_ScaleForPixelHeight(const void* info, float height) {
  (void)info;
  capture_scale = height;
  return height / 32.0f;
}

void stbtt_GetCodepointBitmapBox(const void* info, int codepoint, float xs, float ys, int* ix0, int* iy0, int* ix1, int* iy1) {
  (void)info; (void)codepoint; (void)xs; (void)ys;
  *ix0 = teststub_box_x0;
  *iy0 = teststub_box_y0;
  *ix1 = teststub_box_x1;
  *iy1 = teststub_box_y1;
}

void stbtt_MakeCodepointBitmap(const void* info, unsigned char* output, int w, int h, int stride, float xs, float ys, int codepoint) {
  (void)info; (void)xs; (void)ys;
  capture_make_stride = stride;
  capture_make_codepoint = codepoint;
  if (output != (void*)0 && w > 0 && h > 0) ts_memset(output, 0x77, (size_t)w * (size_t)h);
}
