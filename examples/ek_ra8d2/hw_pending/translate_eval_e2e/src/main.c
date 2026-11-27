/**
 * @file examples/ek_ra8d2/hw_pending/translate_eval_e2e/src/main.c
 * @brief Target Cortex-M85 E2E entry for `ra8_translate` chapter jobs and `TPAK` runs.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_err.h"
#include "ra8_sdramc.h"
#include "ra8_time.h"
#include "translate_e2e.h"

typedef enum : uint32_t {
  k_main_uart_baud   = 115200U,
  k_main_settle_ms   = 10U,
  k_main_tpak_cap    = 2U * 1024U * 1024U,
  k_main_uart_chunk  = 256U,
  k_main_dec_buf_cap = 24U,
} te2e_main_const_t;

[[gnu::section(".sdram_data")]] static translate_e2e_arena_t s_arena;
[[gnu::section(".sdram_data")]] static uint8_t               s_tpak_buf[k_main_tpak_cap];

static void main_emit_bytes(void* ctx, const char* bytes, size_t len)
{
  (void)ctx;
  size_t off = 0U;
  while (off < len) {
    const size_t rem   = len - off;
    const size_t chunk = (rem > (size_t)k_main_uart_chunk) ? (size_t)k_main_uart_chunk : rem;
    (void)ra8_board_uart_console_write((const uint8_t*)&bytes[off], chunk);
    off += chunk;
  }
}

static void main_print_cstr(const char* s)
{
  main_emit_bytes(nullptr, s, strlen(s));
}

static void main_print_u64(uint64_t val)
{
  char   buf[k_main_dec_buf_cap];
  size_t pos = sizeof(buf);
  if (val == 0U) {
    main_emit_bytes(nullptr, "0", 1U);
    return;
  }
  while (val > 0U) {
    buf[--pos] = (char)('0' + (val % 10U));
    val /= 10U;
  }
  main_emit_bytes(nullptr, &buf[pos], sizeof(buf) - pos);
}

static void main_emit_noop(void* ctx, const char* bytes, size_t len)
{
  (void)ctx;
  (void)bytes;
  (void)len;
}

static void main_panic_halt(const char* stage, ra8_err_t err)
{
  main_print_cstr("translate_eval_e2e: FAIL ");
  main_print_cstr(stage);
  main_print_cstr(" err=");
  main_print_u64((uint64_t)err);
  main_print_cstr("\r\ntranslate_eval_e2e: END\r\n");
  (void)ra8_board_led_on(k_ra8_board_led_red);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

static void main_bringup(void)
{
  const ra8_board_bringup_cfg_t cfg = {
    .console_baud      = (uint32_t)k_main_uart_baud,
    .leds_mask         = (uint32_t)k_ra8_board_bringup_led1 | (uint32_t)k_ra8_board_bringup_led3,
    .enable_interrupts = true,
  };
  ra8_board_bringup_out_t out = {};
  if (ra8_board_bringup(&cfg, &out) != k_ra8_ok) {
    main_panic_halt("bringup", k_ra8_err_hw_init_failed);
  }
  ra8_delay_ms((uint32_t)k_main_settle_ms);
  const ra8_err_t sdram_err = ra8_sdramc_init();
  if (sdram_err != k_ra8_ok) {
    main_panic_halt("sdram", sdram_err);
  }
  memset(&s_arena, 0, sizeof(s_arena));
}

static ra8_err_t main_run_sd_or_mini(translate_e2e_tpak_stats_t* out_stats, bool* out_from_sd)
{
  size_t    tpak_len = 0U;
  ra8_err_t sd_err   = translate_e2e_sd_load_tpak(s_tpak_buf, sizeof(s_tpak_buf), &tpak_len);
  if (sd_err == k_ra8_ok) {
    *out_from_sd = true;
    return translate_e2e_run_tpak(s_tpak_buf,
                                  tpak_len,
                                  s_arena.model_work,
                                  sizeof(s_arena.model_work),
                                  main_emit_bytes,
                                  nullptr,
                                  out_stats);
  }
  if (sd_err != k_ra8_err_not_found) {
    return sd_err;
  }
  *out_from_sd  = false;
  ra8_err_t err = translate_e2e_build_mini_tpak(&s_arena.pages[0],
                                                &s_arena.bubbles[0],
                                                s_tpak_buf,
                                                sizeof(s_tpak_buf),
                                                &tpak_len);
  if (err != k_ra8_ok) {
    return err;
  }
  return translate_e2e_run_tpak(s_tpak_buf,
                                tpak_len,
                                s_arena.model_work,
                                sizeof(s_arena.model_work),
                                main_emit_noop,
                                nullptr,
                                out_stats);
}

/**
 * @brief Target entry: chapter E2E + TRJC parity -> optional SD TPAK corpus trace -> PASS.
 * @since 0.1.0
 */
void main(void)
{
  main_bringup();
  main_print_cstr("translate_eval_e2e: boot\r\n");

  translate_e2e_chapter_report_t ch_report = {};
  ra8_err_t                      err       = translate_e2e_run_chapter_suite(&s_arena, &ch_report);
  if (err != k_ra8_ok) {
    main_panic_halt("chapter", err);
  }
  translate_e2e_tpak_stats_t tpak_stats = {};
  bool                       from_sd    = false;
  err                                   = main_run_sd_or_mini(&tpak_stats, &from_sd);
  if (err != k_ra8_ok) {
    main_panic_halt("tpak", err);
  }
  main_print_cstr("translate_eval_e2e: PASS ckpt_match=");
  main_print_u64(ch_report.checkpoint_bit_exact);
  main_print_cstr(" ch_bubbles=");
  main_print_u64(ch_report.bubble_count);
  main_print_cstr(" sd=");
  main_print_cstr(from_sd ? "tpak" : "none");
  main_print_cstr(" pages=");
  main_print_u64(tpak_stats.page_count);
  main_print_cstr(" bubbles=");
  main_print_u64(tpak_stats.bubble_count);
  main_print_cstr(" furi=");
  main_print_u64((uint64_t)ch_report.furigana_dropped + tpak_stats.furigana_dropped);
  main_print_cstr(" weights=");
  main_print_u64(ch_report.weight_bytes_read + tpak_stats.weight_bytes_read);
  main_print_cstr("\r\ntranslate_eval_e2e: END\r\n");

  (void)ra8_board_led_on(k_ra8_board_led_green);
  while (1) {
    __asm__ volatile("wfi");
  }
}
