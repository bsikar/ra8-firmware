/**
 * @file test_ra8_fs_font_reflow.c
 * @brief End-to-end: font stored on a FAT volume -> read via ra8_fs ->
 *        rendered through the heap-free ra8_reflow pipeline.
 *
 * @details
 * Proves the e-reader's SD-card storage path in software, with no board
 * and no board_sim: a real FAT16 volume is built in an in-memory block
 * device, the bundled Literata font is *written into it through ra8_fs*
 * (simulating "drop the font on the SD card"), read back out, and handed
 * to ra8_reflow, which lays out and rasterises a paragraph into an
 * `ra8_gfx` framebuffer. Glyph scratch goes through the no-heap stb arena
 * (`ra8_stbtt_alloc`), so the whole read-and-render path is heap-free
 * exactly as it runs on target -- only the font *storage* moves from the
 * (switch-gated, see #44) OSPI flash to an SD/FAT volume.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_fs.h"
#include "ra8_gfx.h"
#include "ra8_reflow.h"
#include "unity_minimal.h"

/**
 * @enum fs_font_reflow_uint8_const_t
 * @brief Named uint8_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint8_t {
  k_bpb_off_bytes_per_sec = 11U, /**< BPB_BytsPerSec: bytes per sector.             */
  k_bpb_off_rsvd_sec_cnt  = 14U, /**< BPB_RsvdSecCnt: sectors before the first FAT. */
  k_bpb_off_root_ent_cnt  = 17U, /**< BPB_RootEntCnt: root-directory entries.       */
  k_bpb_off_tot_sec16     = 19U, /**< BPB_TotSec16: total sectors.                  */
  k_byte_mask = 0xFFU, /**< Truncates each shifted CRC and length byte for the gzip trailer. */
  k_bpb_off_sec_per_clus = 13, /**< BPB_SecPerClus: sectors per cluster. */
} fs_font_reflow_uint8_const_t;

/* ---------------------------------------------------------------------------
 * In-memory FAT16 block device (mirrors tests/test_ra8_fs_fat.c).
 * ---------------------------------------------------------------------------
 */
typedef enum : uint32_t {
  k_disk_block_size   = 512U,        /**< Disk block size.                        */
  k_disk_blocks_fat16 = 8U * 1024U,  /**< ~4 MiB -> FAT16, easily holds the font. */
  k_bpb_sig_off_a     = 510U,        /**< Boot-sector signature byte 0 offset.    */
  k_bpb_sig_off_b     = 511U,        /**< Boot-sector signature byte 1 offset.    */
  k_bpb_sig_a         = 0x55U,       /**< Boot-sector signature byte 0 (0xAA55).  */
  k_bpb_sig_b         = 0xAAU,       /**< Boot-sector signature byte 1 (0xAA55).  */
  k_bg_argb           = 0xFF000000U, /**< Opaque-black framebuffer background.    */
  k_bpb_off_secperfat = 22U,         /**< BPB offset of "sectors per FAT".        */
} ra8_fs_font_disk_t;

typedef struct {
  uint8_t* bytes;       /**< Bytes.       */
  uint32_t block_count; /**< Block count. */
  uint32_t byte_count;  /**< Byte count.  */
} mem_disk_t;

static mem_disk_t s_disk = {};

static ra8_err_t mem_read(void* ctx, uint32_t lba, uint32_t count, uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(buf,
         &d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_write(void* ctx, uint32_t lba, uint32_t count, const uint8_t* buf)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  if (lba + count > d->block_count) {
    return k_ra8_err_out_of_range;
  }
  memcpy(&d->bytes[(size_t)lba * (uint32_t)k_disk_block_size],
         buf,
         (size_t)count * (uint32_t)k_disk_block_size);
  return k_ra8_ok;
}

static ra8_err_t mem_capacity(void* ctx, uint32_t* block_count, uint32_t* block_size)
{
  mem_disk_t* d = (mem_disk_t*)ctx;
  *block_count  = d->block_count;
  *block_size   = (uint32_t)k_disk_block_size;
  return k_ra8_ok;
}

static const ra8_fs_backend_t s_backend = {
  .read_block   = mem_read,
  .write_block  = mem_write,
  .get_capacity = mem_capacity,
  .ctx          = &s_disk,
};

static void put16(uint8_t* p, uint32_t off, uint16_t v)
{
  p[off]     = (uint8_t)(v & k_byte_mask);
  p[off + 1] = (uint8_t)((v >> 8) & k_byte_mask);
}

/** @brief Format the in-memory block device as an empty FAT16 volume. */
static void build_fat16_volume(void)
{
  free(s_disk.bytes);
  s_disk.byte_count  = (uint32_t)k_disk_blocks_fat16 * (uint32_t)k_disk_block_size;
  s_disk.bytes       = (uint8_t*)calloc(1, s_disk.byte_count);
  s_disk.block_count = (uint32_t)k_disk_blocks_fat16;
  if (s_disk.bytes == nullptr) {
    TEST_FAIL_FMT("%s", "calloc failed");
  }
  uint8_t* bpb = &s_disk.bytes[0];
  put16(bpb, k_bpb_off_bytes_per_sec, (uint16_t)k_disk_block_size); /* bytes/sector     */
  bpb[k_bpb_off_sec_per_clus] = 1U;                                 /* sectors/cluster  */
  put16(bpb, k_bpb_off_rsvd_sec_cnt, 1U);                           /* reserved sectors */
  bpb[16] = 2U;                                                     /* number of FATs   */
  put16(bpb, k_bpb_off_root_ent_cnt, 16U);                          /* root entries     */
  put16(bpb, k_bpb_off_tot_sec16, (uint16_t)k_disk_blocks_fat16);   /* total sectors    */
  put16(bpb, (uint32_t)k_bpb_off_secperfat, 32U);                   /* sectors/FAT      */
  bpb[k_bpb_sig_off_a] = (uint8_t)k_bpb_sig_a;
  bpb[k_bpb_sig_off_b] = (uint8_t)k_bpb_sig_b;
}

/* ---------------------------------------------------------------------------
 * Font loading (bundled Literata, located relative to __FILE__).
 * ---------------------------------------------------------------------------
 */
enum {
  k_font_cap = 2U * 1024U * 1024U, /**< Literata is < 2 MiB. */
  k_path_cap = 1024,               /**< Path cap.            */
  k_fb_w     = 384,                /**< Fb w.                */
  k_fb_h     = 256,                /**< Fb h.                */
  k_font_px  = 18,                 /**< Font px.             */
};

static uint8_t*  s_src_font;  /**< Font as read from disk.        */
static uint8_t*  s_card_font; /**< Font as read back from the FS. */
static uint32_t  s_font_len;
static uint32_t* s_fb; /**< ARGB8888 framebuffer. */

static bool load_src_font(void)
{
  char path[k_path_cap];
  (void)snprintf(path, sizeof(path), "%s", __FILE__);
  /* strip "test_ra8_fs_font_reflow.c" and "tests/" -> repo root. */
  for (int drop = 0; drop < 2; ++drop) {
    size_t n = strlen(path);
    while (n > 0U && path[n - 1U] != '/') {
      --n;
    }
    if (n > 0U) {
      path[n - 1U] = '\0';
    }
  }
  const size_t base = strlen(path);
  (void)snprintf(&path[base], sizeof(path) - base, "/libs/fonts/Literata-Regular.ttf");
  FILE* fp = fopen(path, "rb");
  if (fp == nullptr) {
    return false;
  }
  size_t n = fread(s_src_font, 1U, k_font_cap, fp);
  (void)fclose(fp);
  if (n < 16U) {
    return false;
  }
  s_font_len = (uint32_t)n;
  return true;
}

/**
 * @brief Make a FAT16 volume, write the font, and read it back byte-identically.
 * @pre s_src_font holds the loaded font of length s_font_len.
 * @post s_card_font holds the font read back through the real FAT path.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void fat_reflow_write_read_font(void)
{
  /* 1. Make a fresh FAT16 "card" and mount it. */
  build_fat16_volume();
  ra8_fs_mount_t* mnt = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_mount(&s_backend, &mnt));

  /* 2. Write the font onto the card through the real FAT write path. */
  ra8_fs_file_t* wf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mnt, "FONT.OTF", k_ra8_fs_mode_write, &wf));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_write(wf, s_src_font, s_font_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(wf));

  /* 3. Read it back off the card into the font buffer ra8_reflow will use. */
  ra8_fs_file_t* rf = nullptr;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_open(mnt, "FONT.OTF", k_ra8_fs_mode_read, &rf));
  uint32_t sz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_size(rf, &sz));
  TEST_ASSERT_EQ(s_font_len, sz);
  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_read(rf, s_card_font, k_font_cap, &got));
  TEST_ASSERT_EQ(s_font_len, got);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_close(rf));
  /* Bytes round-tripped intact through the FAT layer. */
  TEST_ASSERT_EQ(0, memcmp(s_src_font, s_card_font, s_font_len));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_fs_unmount(mnt));
}

/**
 * @brief Render a paragraph with the card-loaded font and assert real ink.
 * @pre s_card_font holds a valid font of length s_font_len.
 * @post At least one non-background pixel was inked into s_fb.
 * @note Not thread-safe; single-threaded host-test helper.
 * @since 0.1.0
 */
static void fat_reflow_render_and_count(void)
{
  /* 4. Render a paragraph with the card-loaded font (heap-free glyphs). */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_gfx_init(s_fb, (uint16_t)k_fb_w, (uint16_t)k_fb_h, k_ra8_gfx_format_argb8888));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_gfx_clear((uint32_t)k_bg_argb));

  ra8_reflow_t engine = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_init((uint16_t)k_fb_w,
                                 (uint16_t)k_fb_h,
                                 s_card_font,
                                 s_font_len,
                                 (uint16_t)k_font_px,
                                 0xFFFFFFFFU,
                                 0xFF4080FFU,
                                 &engine));
  const char* xhtml = "<html><body><p>The font for this paragraph was loaded from a FAT "
                      "filesystem, not embedded flash.</p></body></html>";
  uint32_t    pages = 0U;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_reflow_layout_chapter(&engine, (const uint8_t*)xhtml, strlen(xhtml), &pages));
  TEST_ASSERT(pages >= 1U);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_reflow_render_page(&engine, 0U, nullptr));

  /* 5. Confirm real ink landed (some pixel differs from the cleared bg). */
  uint32_t ink = 0U;
  for (size_t i = 0U; i < (size_t)k_fb_w * (size_t)k_fb_h; ++i) {
    if (s_fb[i] != (uint32_t)k_bg_argb) {
      ++ink;
    }
  }
  (void)fprintf(stderr,
                "[info] font %u bytes via FAT; %u inked pixels over %u pages\n",
                s_font_len,
                ink,
                pages);
  TEST_ASSERT(ink > 0U);
}

/**
 * @test test_font_on_fat_renders_via_reflow
 *
 * @par MC/DC:
 * Integration test -- no compound decision of its own. It drives the
 * already-MC/DC-covered ra8_fs / ra8_reflow APIs through one round trip:
 * write font -> read font -> layout -> render. The single boolean checks
 * here (font present, size match, ink present) are each exercised true on
 * the success path and would fail the assert otherwise.
 */
static void test_font_on_fat_renders_via_reflow(void)
{
  TEST_BEGIN("font on FAT volume -> ra8_fs -> ra8_reflow render");

  s_src_font  = (uint8_t*)malloc(k_font_cap);
  s_card_font = (uint8_t*)malloc(k_font_cap);
  s_fb        = (uint32_t*)malloc((size_t)k_fb_w * (size_t)k_fb_h * sizeof(uint32_t));
  TEST_ASSERT_NOT_NULL(s_src_font);
  TEST_ASSERT_NOT_NULL(s_card_font);
  TEST_ASSERT_NOT_NULL(s_fb);

  if (!load_src_font()) {
    (void)fprintf(stderr, "[SKIP] Literata font not found; skipping\n");
    TEST_END("font on FAT volume -> ra8_fs -> ra8_reflow render");
    return;
  }

  fat_reflow_write_read_font();
  fat_reflow_render_and_count();

  free(s_disk.bytes);
  s_disk.bytes = nullptr;
  free(s_src_font);
  free(s_card_font);
  free(s_fb);
  TEST_END("font on FAT volume -> ra8_fs -> ra8_reflow render");
}

int32_t main(void)
{
  test_font_on_fat_renders_via_reflow();
  (void)fprintf(stderr, "[OK ] test_ra8_fs_font_reflow.c\n");
  return 0;
}
