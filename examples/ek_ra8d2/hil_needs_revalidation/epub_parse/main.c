/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/epub_parse/main.c
 * @brief On-silicon HIL: ra8_epub parse layer runs on the target (#139).
 *
 * @details
 * First firmware app to exercise `ra8_epub` (+ vendored miniz ZIP + bounded XML reader)
 * on real silicon. The EPUB parse path is byte-twiddling C/C++ that has only
 * ever run on the x86 host; this gate runs it on the EK-RA8D2.
 *
 * It opens a baked known-good `.epub` (the `seed_two_chapters` fuzzer seed) in
 * memory, exercising **both** miniz allocation paths through the zero-heap
 * `ra8_epub_miniz_alloc` static arena -- `ra8_epub_open` (ZIP central directory)
 * and `ra8_epub_load_chapter` (DEFLATE decompressor) -- then CRC-32s chapter 0's
 * decompressed XHTML and reads the Dublin Core metadata. The console banner is:
 *
 *   `epub: chapters=2 ch0_crc=<8hex> PASS`
 *
 * Deterministic (a fixed blob through a deterministic parser), so the banner is
 * identical on host, ra8_emulator, and silicon. Any failure on any path prints a
 * FAIL banner and halts on a BKPT before the PASS line, so the gate is exact.
 *
 *
 * [Ring 7 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "epub_fixture.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @enum ep_consts_t @brief Console / parse knobs (no magic numbers). */
typedef enum : uint32_t {
  k_ep_uart_baud   = 115200U,     /**< Console baud.                   */
  k_ep_expect_chap = 2U,          /**< Spine length of the baked seed. */
  k_ep_chap_cap    = 4096U,       /**< Chapter XHTML scratch capacity. */
  k_ep_crc_init    = 0xFFFFFFFFU, /**< CRC-32 initial value.           */
  k_ep_crc_poly    = 0xEDB88320U, /**< CRC-32 reflected polynomial.    */
  k_ep_crc_bits    = 8U,          /**< Bits folded per byte.           */
  k_ep_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value.   */
  k_ep_nibble_bits = 4U,          /**< Bits per hex nibble.            */
  k_ep_nibble_mask = 0x0FU,       /**< Low-nibble mask.                */
  k_ep_dec_ten     = 10U,         /**< Hex digit / decimal split.      */
} ep_consts_t;

/** @brief Opened book (large -- file-scope, not on the stack). */
static ra8_epub_book_t s_book;
/** @brief Chapter-0 XHTML scratch (file-scope to keep the stack small). */
static uint8_t s_chapter[k_ep_chap_cap];

static const uint8_t k_msg_boot[] = "epub-parse-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "epub-parse-hil: FAIL init\r\n";
static const uint8_t k_msg_open[] = "epub: FAIL open\r\n";
static const uint8_t k_msg_chap[] = "epub: FAIL chapters\r\n";
static const uint8_t k_msg_load[] = "epub: FAIL load\r\n";
static const uint8_t k_msg_meta[] = "epub: FAIL meta\r\n";
static const uint8_t k_msg_pre[]  = "epub: chapters=";
static const uint8_t k_msg_crc[]  = " ch0_crc=";
static const uint8_t k_msg_ok[]   = " PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void ep_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void ep_panic_halt(const uint8_t* msg, uint32_t len)
{
  ep_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief CRC-32 (reflected, poly 0xEDB88320) over @p data. */
static uint32_t ep_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = (uint32_t)k_ep_crc_init;
  for (size_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint32_t bit = 0U; bit < (uint32_t)k_ep_crc_bits; bit++) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_ep_crc_poly & mask);
    }
  }
  return ~crc;
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void ep_print_hex(uint32_t value)
{
  uint8_t buf[k_ep_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_ep_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_ep_hex_nibbles - 1U - i) * (uint32_t)k_ep_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_ep_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_ep_dec_ten) ? ('0' + nib) : ('A' + (nib - k_ep_dec_ten)));
  }
  ep_print(buf, (uint32_t)k_ep_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void ep_print_uint(uint32_t value)
{
  uint8_t  buf[k_ep_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_ep_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_ep_dec_ten));
    n++;
    value /= (uint32_t)k_ep_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    ep_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void ep_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    ep_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    ep_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    ep_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_ep_uart_baud) != k_ra8_ok) {
    ep_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/**
 * @brief Open the baked EPUB, verify the spine, load + CRC chapter 0.
 *
 * @details
 * Exercises both miniz allocation paths through the static arena: the open
 * (central directory) and the chapter load (DEFLATE decompressor). Halts on
 * any failure before returning.
 *
 * @param[out] out_crc Receives the CRC-32 of chapter 0's decompressed XHTML.
 * @return The chapter (spine) count.
 */
static uint16_t ep_parse_or_halt(uint32_t* out_crc)
{
  const ra8_epub_mem_media_t media = {.data = k_epub_fixture, .size = (size_t)k_epub_fixture_len};
  if (ra8_epub_open(&media, "book.epub", &s_book) != k_ra8_ok) {
    ep_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }
  uint16_t chapters = 0U;
  if ((ra8_epub_get_chapter_count(&s_book, &chapters) != k_ra8_ok) ||
      (chapters != (uint16_t)k_ep_expect_chap)) {
    ep_panic_halt(k_msg_chap, (uint32_t)sizeof(k_msg_chap) - 1U);
  }
  size_t got = 0U;
  if ((ra8_epub_load_chapter(&s_book, 0U, s_chapter, (size_t)k_ep_chap_cap, &got) != k_ra8_ok) ||
      (got == 0U)) {
    ep_panic_halt(k_msg_load, (uint32_t)sizeof(k_msg_load) - 1U);
  }
  ra8_epub_metadata_t meta = {};
  if (ra8_epub_get_metadata(&s_book, &meta) != k_ra8_ok) {
    ep_panic_halt(k_msg_meta, (uint32_t)sizeof(k_msg_meta) - 1U);
  }
  *out_crc = ep_crc32(s_chapter, got);
  return chapters;
}

/**
 * @brief App entry: parse the baked EPUB on silicon, print the banner.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The chapters/ch0-CRC banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
void main(void)
{
  ep_setup_or_halt();
  ra8_isr_globals_enable();
  ep_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  uint32_t       ch0_crc  = 0U;
  const uint16_t chapters = ep_parse_or_halt(&ch0_crc);

  ep_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  ep_print_uint((uint32_t)chapters);
  ep_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  ep_print_hex(ch0_crc);
  ep_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
