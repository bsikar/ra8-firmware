/**
 * @file examples/ek_ra8d2/hw_validated/hil/epub_stress/main.c
 * @brief On-silicon HIL: large-structure EPUB opens on the static arena (#144).
 *
 * @details
 * Regression gate for #144 bug 1 ("large EPUBs fail to open with no_mem"). On
 * the firmware target, miniz's ZIP central directory AND tinyxml2's OPF DOM
 * both allocate from the **same** 96 KiB static arena
 * (`ra_epub_miniz_alloc` + the arena-backed `operator new` in
 * `ra_epub_cpp_alloc.cpp`). A big real book (many image files + a large OPF
 * manifest) stresses that shared pool. The original no_mem turned out to be the
 * 16 KiB OPF/NCX scratch buffer (fixed in the NCX commit, now 48 KiB), not the
 * arena -- this gate proves the arena itself holds a large-structure book.
 *
 * It opens a baked **synthetic** large-structure EPUB in memory -- 60 chapters
 * + 60 manifest resources + an NCX with 60 navPoints + a cover, **125 archive
 * entries** / a ~10 KB OPF (more files than the 108-file, 41-chapter real Boox
 * book that triggered the report) -- and asserts:
 *
 *   - `ra_epub_open` returns `k_ra_ok` (the shared arena was sufficient),
 *   - chapter count == 60 (full spine parsed),
 *   - NCX TOC count == 60 (every navPoint extracted, #144 bug 2),
 *   - the cover-image manifest item resolved (`cover_path` non-empty).
 *
 * The fixture is synthetic (not the copyrighted novel), tens of KB, so it bakes
 * into MRAM and opens in memory like `epub_parse`. Deterministic, so the
 * board_sim CRC-free banner is the regression net:
 *
 *   `epub-stress-hil: files=125 chapters=60 toc=60 cover=ok PASS`
 *
 * Any failure prints a FAIL banner and halts on a BKPT before PASS.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / App] {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "epub_stress_fixture.h"
#include "ra_cgc.h"
#include "ra_epub.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @enum est_consts_t @brief Console / assertion knobs (no magic numbers). */
typedef enum : uint32_t {
  k_est_uart_chan    = 8U,      /**< SCI8 J-Link OB console.               */
  k_est_uart_baud    = 115200U, /**< Console baud.                         */
  k_est_expect_chap  = 60U,     /**< Spine length of the synthetic book.   */
  k_est_expect_toc   = 60U,     /**< navPoint count in the synthetic NCX.  */
  k_est_expect_files = 125U,    /**< Archive entries (reported, not gated). */
  k_est_dec_ten      = 10U,     /**< Decimal base for the small printer.   */
} est_consts_t;

/** @brief SCI8 console TXD = PD02. */
static const ra_port_pin_t k_est_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03. */
static const ra_port_pin_t k_est_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Opened book (large -- file-scope, not on the stack). */
static ra_epub_book_t s_book;

static const uint8_t k_msg_boot[] = "epub-stress-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "epub-stress-hil: FAIL init\r\n";
static const uint8_t k_msg_open[] = "epub-stress-hil: FAIL open (arena no_mem?)\r\n";
static const uint8_t k_msg_chap[] = "epub-stress-hil: FAIL chapters\r\n";
static const uint8_t k_msg_toc[]  = "epub-stress-hil: FAIL toc\r\n";
static const uint8_t k_msg_cov[]  = "epub-stress-hil: FAIL cover\r\n";
static const uint8_t k_msg_pre[]  = "epub-stress-hil: files=125 chapters=";
static const uint8_t k_msg_tocp[] = " toc=";
static const uint8_t k_msg_covp[] = " cover=ok";
static const uint8_t k_msg_ok[]   = " PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void est_print(const uint8_t* msg, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_est_uart_chan, msg, len);
}

/** @brief Print the fail banner and trap (board_sim halts on the BKPT). */
static void est_panic_halt(const uint8_t* msg, uint32_t len)
{
  est_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a small unsigned integer in decimal. */
static void est_print_uint(uint32_t value)
{
  uint8_t  buf[k_est_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_est_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_est_dec_ten));
    n++;
    value /= (uint32_t)k_est_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    est_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief Route the SCI8 console pins to async mode. */
[[nodiscard]] static ra_err_t est_console_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_est_pin_txd, k_ra_psel_sci_async, "stress.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_est_pin_rxd, k_ra_psel_sci_async, "stress.rxd8");
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void est_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra_cgc_init() != k_ra_ok) || (ra_mstp_init() != k_ra_ok)) {
    est_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if ((ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) ||
      (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok)) {
    est_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    est_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (est_console_pins_init() != k_ra_ok) {
    est_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  const ra_sci_cfg_t cfg = {.baud      = (uint32_t)k_est_uart_baud,
                            .data_bits = k_ra_sci_data_8,
                            .parity    = k_ra_sci_parity_none,
                            .stop_bits = k_ra_sci_stop_1,
                            .pclk_hz   = pclka_hz};
  if (ra_sci_init((uint8_t)k_est_uart_chan, &cfg) != k_ra_ok) {
    est_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/**
 * @brief Open the stress EPUB through the shared arena; assert the structure.
 *
 * @details Halts on any failure stage before returning. Proves the 96 KiB
 * shared arena (miniz central directory + tinyxml2 OPF DOM) holds a
 * 125-entry / 60-chapter book and that the NCX + cover both parse.
 *
 * @param[out] out_chap Receives the parsed chapter count (== 60 on success).
 * @param[out] out_toc  Receives the parsed NCX TOC count (== 60 on success).
 *
 * @pre ::est_setup_or_halt ran.
 * @post On return the assertions held; @p out_chap / @p out_toc are set.
 * @since 0.1.0
 */
static void est_open_or_halt(uint16_t* out_chap, uint16_t* out_toc)
{
  const ra_epub_mem_media_t media = {.data = k_epub_stress_fixture,
                                     .size = (size_t)k_epub_stress_fixture_len};
  if (ra_epub_open(&media, "stress.epub", &s_book) != k_ra_ok) {
    est_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }
  uint16_t chapters = 0U;
  if ((ra_epub_get_chapter_count(&s_book, &chapters) != k_ra_ok) ||
      (chapters != (uint16_t)k_est_expect_chap)) {
    est_panic_halt(k_msg_chap, (uint32_t)sizeof(k_msg_chap) - 1U);
  }
  uint16_t toc = 0U;
  if ((ra_epub_get_toc_count(&s_book, &toc) != k_ra_ok) || (toc != (uint16_t)k_est_expect_toc)) {
    est_panic_halt(k_msg_toc, (uint32_t)sizeof(k_msg_toc) - 1U);
  }
  if (s_book.cover_path[0] == '\0') {
    est_panic_halt(k_msg_cov, (uint32_t)sizeof(k_msg_cov) - 1U);
  }
  *out_chap = chapters;
  *out_toc  = toc;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: open the large-structure EPUB on silicon, print the banner.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The chapters/toc/cover banner is emitted; CPU loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  est_setup_or_halt();
  ra_isr_globals_enable();
  est_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  uint16_t chapters = 0U;
  uint16_t toc      = 0U;
  est_open_or_halt(&chapters, &toc);

  est_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  est_print_uint((uint32_t)chapters);
  est_print(k_msg_tocp, (uint32_t)sizeof(k_msg_tocp) - 1U);
  est_print_uint((uint32_t)toc);
  est_print(k_msg_covp, (uint32_t)sizeof(k_msg_covp) - 1U);
  est_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
