/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/ereader_shelf/src/sh_comic.c
 * @brief Full-page image reader for CBZ / CBR comic archives (#236).
 *
 * @details
 * The shelf's fourth reader surface. A comic archive is a container of page
 * images (JPEG / PNG / ...) in reading order; ::ra8_comic opens either a `.cbz`
 * (ZIP of images) or a `.cbr` (RAR of images) behind one interface and streams
 * one page's *encoded* bytes on demand (bounded RAM, no whole-archive residency
 * -- see ra8_comic.h). This module pages through those images:
 *
 *   - Opens the archive over a baked MRAM blob or the held-open SD file
 *     (::sh_sd_book_open + ::sh_sd_comic_read), building ::ra8_comic's sorted
 *     page index into caller-owned arenas (no heap; NASA P10 Rule 3).
 *   - Renders the current page full-screen: extract the page's encoded image,
 *     then aspect-fit decode + blit it into the content box below the header via
 *     the integer ::ra8_img_decode_blit pipeline -- the same decoder the EPUB
 *     cover path uses, so "page-0-as-cover" and page N share one code path.
 *   - Pages by re-decoding on each turn (one page + one decode arena resident at
 *     a time). A future large-manga path can stream through the #231/#232 tile
 *     cache instead of a whole-page decode; the encoded-page buffer bounds the
 *     openable page size for now (TODO(#231): tile huge pages rather than cap).
 *
 * @par Right-to-left (manga) reading
 * Raw CBZ/CBR carry no reading-direction metadata, so direction is an app-level
 * toggle (`g_sh.comic_rtl`, flipped by SW1 on the comic screen). ::sh_comic_edge_dir
 * mirrors the edge-tap zones for RTL: the left edge advances in manga order.
 *
 * @par Deterministic self-check
 * ::sh_comic_selfcheck decodes the baked CBZ + CBR fixtures' page 0 into an
 * off-screen scratch and FNV-hashes them (an integer pipeline -> a
 * toolchain-independent digest identical on host / ra8_emulator / silicon), which
 * the boot banner pins as the comic golden without disturbing the shelf render.
 *
 *
 * [Ring 6 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#include <string.h>

#include "ra8_comic.h"
#include "ra8_gfx.h"
#include "ra8_gfx_font.h"
#include "ra8_reflow_image.h"
#include "sh_app.h"
#include "sh_comic_fixture.h"

/** @enum sh_comic_const_t @brief Comic reader buffer + layout constants. */
typedef enum : uint32_t {
  k_shc_page_cap    = 1024U,              /**< Max pages in the sorted index.     */
  k_shc_names_cap   = 128U * 1024U,       /**< Page-name arena bytes.             */
  k_shc_pagebuf_cap = 4U * 1024U * 1024U, /**< One encoded page image, bytes.     */
  k_shc_arena_cap   = 8U * 1024U * 1024U, /**< stb_image decode bump arena bytes. */
  k_shc_probe_w     = 160U,               /**< Self-check scratch width, pixels.  */
  k_shc_probe_h     = 120U,               /**< Self-check scratch height, pixels. */
  k_shc_probe_bg    = 0x202028U,          /**< Self-check clear colour (matches   */
                                          /**< ereader_comic -> same page digest). */
  k_shc_live_bg    = 0x101010U,           /**< Live comic backdrop (near-black).   */
  k_shc_fnv_offset = 2166136261U,         /**< FNV-1a-32 offset basis.             */
  k_shc_fnv_prime  = 16777619U,           /**< FNV-1a-32 prime.                    */
  k_shc_hex_digits = 8U,                  /**< Hex digits in a 32-bit CRC.         */
  k_shc_nib_bits   = 4U,                  /**< Bits per hex nibble.                */
  k_shc_nib_mask   = 0xFU,                /**< Low-nibble mask.                    */
  k_shc_thirds     = 3U,                  /**< Edge-tap split (left/centre/right). */
} sh_comic_const_t;

/**
 * @struct sh_comic_blob_t
 * @brief Resident-buffer backing for the comic reader's seek+read seam.
 * @details Bound for baked-MRAM comics + the self-check fixtures, where the
 *          archive bytes are already addressable (a bounds-checked memcpy).
 */
typedef struct {
  const uint8_t* d; /**< Archive bytes.  */
  size_t         n; /**< Archive length. */
} sh_comic_blob_t;

/** @brief The open comic (large: page-index storage + backend scratch). */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static ra8_comic_t s_comic;
/** @brief Caller-owned sorted page index. */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static ra8_comic_page_t s_pages[k_shc_page_cap];
/** @brief Caller-owned page-name arena. */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static char s_names[k_shc_names_cap];
/** @brief One page's extracted encoded image (the decoder input). */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_pagebuf[k_shc_pagebuf_cap];
/** @brief stb_image decode bump arena (zero-heap). */
[[gnu::section(".sdram_data"), gnu::aligned(8)]] static uint8_t s_arena[k_shc_arena_cap];
/** @brief Off-screen RGB565 scratch for the deterministic self-check decode. */
[[gnu::section(".sdram_data"),
  gnu::aligned(8)]] static uint16_t s_probe_rgb[(size_t)k_shc_probe_h * (size_t)k_shc_probe_w];

/** @brief Resident-blob backing for baked / self-check comics. */
static sh_comic_blob_t s_blob;
/** @brief true when the open comic's backing is a held SD file (close it too). */
static bool s_from_sd;

/** @brief ::ra8_comic_read_fn over a resident archive buffer (bounds-clamped). */
// cppcheck-suppress constParameterCallback -- ctx is pinned by the ra8_comic_read_fn vtable signature (void* ctx forwarded verbatim into ra8_rar_open); const cannot cascade the shared read seam
static size_t sh_comic_blob_read(void* ctx, uint64_t off, void* buf, size_t len)
{
  const sh_comic_blob_t* s = (const sh_comic_blob_t*)ctx;
  if ((s == nullptr) || (buf == nullptr) || (off >= (uint64_t)s->n)) {
    return 0U;
  }
  const uint64_t avail = (uint64_t)s->n - off;
  const size_t   k     = (len > (size_t)avail) ? (size_t)avail : len;
  (void)memcpy(buf, &s->d[off], k);
  return k;
}

/** @brief FNV-1a-32 over @p n bytes of @p p. */
static uint32_t sh_comic_fnv(const void* p, size_t n)
{
  const uint8_t* b = (const uint8_t*)p;
  uint32_t       h = (uint32_t)k_shc_fnv_offset;
  for (size_t i = 0U; i < n; ++i) {
    h = (h ^ (uint32_t)b[i]) * (uint32_t)k_shc_fnv_prime;
  }
  return h;
}

/** @brief Bind ::ra8_comic over @p rd + set g_sh.comic_count / page; ok on success. */
static bool sh_comic_bind(ra8_comic_read_fn rd, void* ctx, uint64_t size)
{
  const ra8_err_t err = ra8_comic_open(&s_comic,
                                       rd,
                                       ctx,
                                       size,
                                       s_pages,
                                       (uint32_t)k_shc_page_cap,
                                       s_names,
                                       (uint32_t)k_shc_names_cap);
  if (err != k_ra8_ok) {
    return false;
  }
  g_sh.comic_count = ra8_comic_page_count(&s_comic);
  g_sh.comic_page  = 0U;
  return g_sh.comic_count > 0U;
}

/** @brief Extract page @p page's encoded bytes and decode+blit into the box. */
static ra8_err_t sh_comic_blit_page(uint32_t page,
                                    int32_t  x,
                                    int32_t  y,
                                    int32_t  w,
                                    int32_t  h,
                                    int32_t* out_w,
                                    int32_t* out_h)
{
  if ((g_sh.comic_count == 0U) || (page >= g_sh.comic_count)) {
    return k_ra8_err_out_of_range;
  }
  size_t          got  = 0U;
  const ra8_err_t rerr = ra8_comic_page_read(&s_comic, page, s_pagebuf, sizeof s_pagebuf, &got);
  if (rerr != k_ra8_ok) {
    return rerr;
  }
  ra8_img_arena_t arena = {.base = s_arena, .cap = sizeof s_arena, .offset = 0U, .live = 0U};
  int32_t         dw    = 0;
  int32_t         dh    = 0;
  const ra8_err_t derr  = ra8_img_decode_blit(&arena, s_pagebuf, got, x, y, w, h, &dw, &dh);
  if (out_w != nullptr) {
    *out_w = dw;
  }
  if (out_h != nullptr) {
    *out_h = dh;
  }
  return derr;
}

bool sh_comic_open(uint16_t idx)
{
  sh_comic_close();
  if (idx >= g_sh.book_count) {
    return false;
  }
  const sh_entry_t* e = &g_sh.entry[idx];
  if (!sh_fmt_is_comic(e->fmt)) {
    return false;
  }
  if (e->from_sd) {
    uint32_t len = 0U;
    if (!sh_sd_book_open(e->sd_name, &len)) {
      return false;
    }
    if (!sh_comic_bind(sh_sd_comic_read, nullptr, (uint64_t)len)) {
      sh_sd_book_close();
      return false;
    }
    s_from_sd = true;
    return true;
  }
  s_blob.d = e->blob;
  s_blob.n = (size_t)e->blob_len;
  if (!sh_comic_bind(sh_comic_blob_read, &s_blob, (uint64_t)e->blob_len)) {
    return false;
  }
  s_from_sd = false;
  return true;
}

void sh_comic_close(void)
{
  (void)ra8_comic_close(&s_comic); /* idempotent after a failed / no open */
  if (s_from_sd) {
    sh_sd_book_close();
    s_from_sd = false;
  }
  g_sh.comic_count = 0U;
  g_sh.comic_page  = 0U;
}

/** @brief Centre-align @p s horizontally for the 8px bitmap font. */
static int32_t sh_comic_center_x(const char* s)
{
  return ((int32_t)k_sh_fb_w - ((int32_t)strlen(s) * (int32_t)k_sh_glyph_w)) / 2;
}

void sh_comic_render(void)
{
  (void)ra8_gfx_clear((uint32_t)k_shc_live_bg);

  char   right[k_sh_linebuf];
  size_t rp       = 0U;
  rp              = sh_fmt_uint(right, rp, g_sh.comic_page + 1U);
  right[rp++]     = '/';
  rp              = sh_fmt_uint(right, rp, g_sh.comic_count);
  right[rp++]     = ' ';
  right[rp++]     = ' ';
  const char* dir = g_sh.comic_rtl ? "RTL" : "LTR";
  for (const char* s = dir; *s != '\0'; ++s) {
    right[rp++] = *s;
  }
  right[rp] = '\0';
  sh_titlebar("< Library", right);

  const int32_t   box_y = (int32_t)k_sh_bar_h;
  const int32_t   box_h = (int32_t)k_sh_fb_h - (int32_t)k_sh_bar_h;
  const ra8_err_t err =
    sh_comic_blit_page(g_sh.comic_page, 0, box_y, (int32_t)k_sh_fb_w, box_h, nullptr, nullptr);
  if (err != k_ra8_ok) {
    const char* msg = "Unable to render this page";
    (void)ra8_gfx_text_out(sh_comic_center_x(msg),
                           box_y + (box_h / 2),
                           msg,
                           &ra8_gfx_font_8x16,
                           (uint32_t)k_sh_col_sub,
                           (uint32_t)k_shc_live_bg);
  }
}

int32_t sh_comic_edge_dir(int32_t x)
{
  const int32_t third = (int32_t)k_sh_fb_w / (int32_t)k_shc_thirds;
  int32_t       base  = 0;
  if (x < third) {
    base = -1;
  } else if (x >= ((int32_t)k_sh_fb_w - third)) {
    base = 1;
  }
  /* Manga (RTL) mirrors the edges: the left edge advances, the right goes back. */
  return g_sh.comic_rtl ? -base : base;
}

bool sh_comic_turn(int32_t dir)
{
  const uint32_t n = g_sh.comic_count;
  if (n == 0U) {
    return false;
  }
  if ((dir > 0) && ((g_sh.comic_page + 1U) < n)) {
    g_sh.comic_page += 1U;
    return true;
  }
  if ((dir < 0) && (g_sh.comic_page > 0U)) {
    g_sh.comic_page -= 1U;
    return true;
  }
  return false;
}

bool sh_comic_tap(int32_t x, bool header)
{
  if (header) {
    /* The header's right half carries the "p/N  LTR|RTL" indicator; tapping it
     * flips the reading direction. The left half steps back to the shelf. */
    if (x >= ((int32_t)k_sh_fb_w / 2)) {
      sh_comic_toggle_rtl();
    } else {
      g_sh.screen = k_sh_screen_shelf;
    }
    return true;
  }
  const int32_t d = sh_comic_edge_dir(x);
  return (d != 0) && sh_comic_turn(d);
}

void sh_comic_toggle_rtl(void)
{
  g_sh.comic_rtl = !g_sh.comic_rtl;
}

/** @brief Open @p blob, decode its page 0 into the scratch, hash it; then close. */
static sh_comic_probe_t sh_comic_probe_blob(const uint8_t* blob, uint32_t len)
{
  sh_comic_probe_t r = {};
  s_blob.d           = blob;
  s_blob.n           = (size_t)len;
  s_from_sd          = false;
  if (!sh_comic_bind(sh_comic_blob_read, &s_blob, (uint64_t)len)) {
    return r;
  }
  r.pages = g_sh.comic_count;
  (void)ra8_gfx_init(s_probe_rgb,
                     (uint16_t)k_shc_probe_w,
                     (uint16_t)k_shc_probe_h,
                     k_ra8_gfx_format_rgb565);
  (void)ra8_gfx_clear((uint32_t)k_shc_probe_bg);
  int32_t dw = 0;
  int32_t dh = 0;
  if (sh_comic_blit_page(0U, 0, 0, (int32_t)k_shc_probe_w, (int32_t)k_shc_probe_h, &dw, &dh) ==
      k_ra8_ok) {
    r.ok  = true;
    r.w   = dw;
    r.h   = dh;
    r.crc = sh_comic_fnv(s_probe_rgb, sizeof s_probe_rgb);
  }
  sh_comic_close();
  return r;
}

void sh_comic_selfcheck(sh_comic_probe_t* cbz, sh_comic_probe_t* cbr, bool* rtl_ok)
{
  if ((cbz == nullptr) || (cbr == nullptr) || (rtl_ok == nullptr)) {
    return;
  }
  *cbz = sh_comic_probe_blob(k_comic_cbz, k_comic_cbz_len);
  *cbr = sh_comic_probe_blob(k_comic_cbr, k_comic_cbr_len);

  const bool    saved     = g_sh.comic_rtl;
  const int32_t right     = (int32_t)k_sh_fb_w - 1;
  g_sh.comic_rtl          = false;
  const int32_t ltr_right = sh_comic_edge_dir(right);
  const int32_t ltr_left  = sh_comic_edge_dir(0);
  g_sh.comic_rtl          = true;
  const int32_t rtl_right = sh_comic_edge_dir(right);
  const int32_t rtl_left  = sh_comic_edge_dir(0);
  g_sh.comic_rtl          = saved;
  *rtl_ok = (ltr_right == 1) && (ltr_left == -1) && (rtl_right == -1) && (rtl_left == 1);

  /* Restore the live-framebuffer binding the probe rebound to the scratch. */
  (void)
    ra8_gfx_init(sh_fb_pixels(), (uint16_t)k_sh_fb_w, (uint16_t)k_sh_fb_h, k_ra8_gfx_format_rgb565);
}

void sh_comic_append_banner(char*                   b,
                            size_t*                 pos,
                            const sh_comic_probe_t* cbz,
                            const sh_comic_probe_t* cbr,
                            bool                    rtl_ok)
{
  if ((b == nullptr) || (pos == nullptr) || (cbz == nullptr) || (cbr == nullptr)) {
    return;
  }
  const sh_comic_probe_t* const set[] = {cbz, cbr};
  const char* const             tag[] = {" cbz=", " cbr="};
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(set) / sizeof(set[0])); ++i) {
    size_t p = *pos;
    for (const char* s = tag[i]; *s != '\0'; ++s) {
      b[p++] = *s;
    }
    p      = sh_fmt_uint(b, p, set[i]->pages);
    b[p++] = ':';
    p      = sh_fmt_uint(b, p, (uint32_t)set[i]->w);
    b[p++] = 'x';
    p      = sh_fmt_uint(b, p, (uint32_t)set[i]->h);
    b[p++] = ':';
    for (uint32_t d = 0U; d < (uint32_t)k_shc_hex_digits; ++d) {
      const uint32_t nib =
        (set[i]->crc >> (((uint32_t)k_shc_hex_digits - 1U - d) * (uint32_t)k_shc_nib_bits)) &
        (uint32_t)k_shc_nib_mask;
      b[p++] =
        (char)((nib < k_sh_dec_base) ? ('0' + nib) : ('A' + (nib - (uint32_t)k_sh_dec_base)));
    }
    *pos = p;
  }
  const char* rtl = rtl_ok ? " rtl=OK" : " rtl=NO";
  size_t      p   = *pos;
  for (const char* s = rtl; *s != '\0'; ++s) {
    b[p++] = *s;
  }
  *pos = p;
}
