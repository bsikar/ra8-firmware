/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/epub_open/src/main.c
 * @brief On-silicon HIL: open + parse a real .epub from SD via ra8_fs (#114).
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * `epub_parse` proved the epub parse stack runs on the M85 from a baked
 * in-memory blob (#139). This app closes the storage gap: it reads the `.epub`
 * off a real microSD card through `ra8_fs` -- the exact path the e-reader uses --
 * so the byte-twiddling parse layer is exercised against real SD timing + the
 * FAT read path, not just a `.rodata` array.
 *
 * Flow: bring up the Pmod2 microSD over the `ra8_sdmmc_spi` SPI-mode driver, mount
 * an `ra8_fs` volume (formatting FAT32 first if the card is blank), self-provision
 * a known 2-chapter `.epub` onto it if absent (the same `seed_two_chapters` seed
 * as `epub_parse`), then `epub_open_streamed_fs()` it -- the production
 * open path (#230): no whole-file buffer, every ZIP read seeks the card on
 * demand -- and assert:
 *   - chapter (spine) count == 2,
 *   - chapter 0's decompressed XHTML CRC-32 == 0xCF23AEEE (byte-exact for the
 *     seed; identical on ra8_emulator and silicon),
 *   - Dublin Core metadata parses with a non-empty title.
 * The SCI8 console banner on success is:
 *
 *   `epub-hil: chapters=2 ch0_crc=CF23AEEE PASS`
 *
 * The HIL gate is memprobe (J-Link / ra8_emulator `--dump-sym`), not the console:
 * an SD app drives the SCI0 Simple-SPI bus, and ra8_emulator folds every SCI
 * channel into one console line, so the SCI8 banner is interleaved with SPI
 * traffic there (the same reason the sibling SD HIL apps -- `sd_font_render`,
 * `fs_format_mount` -- gate on SWD globals). The success path advances
 * ::g_eoh_heartbeat once per frame and only after every assertion passes
 * (chapter count, byte-exact CRC, metadata); any failure stamps ::g_eoh_err and
 * parks without bumping the heartbeat. So a steadily-advancing heartbeat with a
 * zero ::g_eoh_err proves the whole SD -> ra8_fs -> epub pipeline ran. The
 * console banner remains for a real-bench scope and human triage.
 *
 * Required external hardware (on-bench): Digilent PMOD MicroSD (410-380) in Pmod2
 * (J25) with a microSD inserted. THIS APP MAY FORMAT THE CARD. Under ra8_emulator
 * attach a blank card with `--sd-new 64:fat32`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "epub.h"
#include "epub_fixture.h"
#include "epub_fs.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci_spi.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_spi.h"
#include "ra8_time.h"

/** @enum eoh_consts_t @brief Console / SPI / parse knobs (no magic numbers). */
typedef enum : uint32_t {
  k_eoh_uart_baud   = 115200U,     /**< Console baud.                   */
  k_eoh_spi_chan    = 0U,          /**< Pmod2 / J25 SCI0 Simple-SPI.    */
  k_eoh_expect_chap = 2U,          /**< Spine length of the baked seed. */
  k_eoh_expect_crc  = 0xCF23AEEEU, /**< Chapter-0 XHTML CRC-32 (seed).  */
  k_eoh_chap_cap    = 4096U,       /**< Chapter XHTML scratch capacity. */
  k_eoh_crc_init    = 0xFFFFFFFFU, /**< CRC-32 initial value.           */
  k_eoh_crc_poly    = 0xEDB88320U, /**< CRC-32 reflected polynomial.    */
  k_eoh_crc_bits    = 8U,          /**< Bits folded per byte.           */
  k_eoh_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value.   */
  k_eoh_nibble_bits = 4U,          /**< Bits per hex nibble.            */
  k_eoh_nibble_mask = 0x0FU,       /**< Low-nibble mask.                */
  k_eoh_dec_ten     = 10U,         /**< Hex digit / decimal split.      */
} eoh_consts_t;

/** @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI; CS held by GPIO. */
static const ra8_port_pin_t k_eoh_pin_sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;
static const ra8_port_pin_t k_eoh_pin_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;
static const ra8_port_pin_t k_eoh_pin_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;
static const ra8_port_pin_t k_eoh_pin_cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/** @brief Book path on the SD volume (8.3 short name; ra8_fs is root-only). */
static const char k_eoh_book_path[] = "BOOK.EPB";

/**
 * @enum eoh_err_t
 * @brief Failure-stage codes stamped to ::g_eoh_err for SWD / `--dump-sym`.
 *
 * @details Zero means "no failure yet". Each value identifies the first step
 * that failed, so a memprobe gate can triage without the console.
 */
typedef enum : uint32_t {
  k_eoh_err_none  = 0U, /**< No failure (success path).           */
  k_eoh_err_init  = 1U, /**< CGC / time / console / SPI bring-up. */
  k_eoh_err_card  = 2U, /**< SD card SPI init.                    */
  k_eoh_err_mount = 3U, /**< Mount (and format-if-blank).         */
  k_eoh_err_prov  = 4U, /**< Provision the .epub onto the card.   */
  k_eoh_err_open  = 5U, /**< epub_open_streamed_fs.               */
  k_eoh_err_chap  = 6U, /**< Chapter-count mismatch.              */
  k_eoh_err_load  = 7U, /**< Chapter-0 DEFLATE load.              */
  k_eoh_err_crc   = 8U, /**< Chapter-0 CRC mismatch.              */
  k_eoh_err_meta  = 9U, /**< Metadata parse / empty title.        */
} eoh_err_t;

/** @enum eoh_pace_t @brief Idle-loop pacing for the success heartbeat. */
typedef enum : uint32_t {
  k_eoh_frame_ms = 100U, /**< Heartbeat period in milliseconds. */
} eoh_pace_t;

/**
 * @var g_eoh_err
 * @brief First failing stage (::eoh_err_t), 0 on success. SWD / `--dump-sym`.
 * @note Exported (non-static) so a memprobe gate can read it by symbol.
 */
volatile uint32_t g_eoh_err = (uint32_t)k_eoh_err_none;
/** @brief Parsed chapter (spine) count on success; 0 otherwise. */
volatile uint32_t g_eoh_chapters = 0U;
/** @brief Chapter-0 decompressed-XHTML CRC-32 on success; 0 otherwise. */
volatile uint32_t g_eoh_crc = 0U;
/** @brief Idle heartbeat; advances ONLY after a clean parse (the HIL gate). */
volatile uint32_t g_eoh_heartbeat = 0U;

/** @brief Opened book (large -- file-scope, not on the stack). */
static epub_book_t s_book;
/** @brief Streamed-open source-file context; must outlive @ref s_book (#230). */
static epub_stream_fs_ctx_t s_epub_io;
/** @brief Chapter-0 XHTML scratch (file-scope to keep the stack small). */
static uint8_t s_chapter[k_eoh_chap_cap];
/** @brief SD backend; file-scope so the mount handle may reference it. */
static ra8_fs_backend_t s_backend;

static const uint8_t k_msg_boot[]   = "epub-open-hil: boot\r\n";
static const uint8_t k_msg_cardok[] = "epub-open-hil: card ready\r\n";
static const uint8_t k_msg_finit[]  = "epub-hil: FAIL init\r\n";
static const uint8_t k_msg_fcard[]  = "epub-hil: FAIL card\r\n";
static const uint8_t k_msg_fmount[] = "epub-hil: FAIL mount\r\n";
static const uint8_t k_msg_fprov[]  = "epub-hil: FAIL provision\r\n";
static const uint8_t k_msg_fopen[]  = "epub-hil: FAIL open\r\n";
static const uint8_t k_msg_fchap[]  = "epub-hil: FAIL chapters\r\n";
static const uint8_t k_msg_fload[]  = "epub-hil: FAIL load\r\n";
static const uint8_t k_msg_fcrc[]   = "epub-hil: FAIL crc\r\n";
static const uint8_t k_msg_fmeta[]  = "epub-hil: FAIL meta\r\n";
static const uint8_t k_msg_pre[]    = "epub-hil: chapters=";
static const uint8_t k_msg_crc[]    = " ch0_crc=";
static const uint8_t k_msg_ok[]     = " PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void eoh_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/**
 * @brief Stamp the failure stage, print the FAIL banner, and park the CPU.
 *
 * @details Records @p err in ::g_eoh_err (for the `--dump-sym` / J-Link memprobe
 * gate), emits @p msg on the console (for a real-bench scope), then spins in WFI.
 * The success heartbeat (::g_eoh_heartbeat) is never bumped from here, so a
 * memprobe gate sees a frozen heartbeat plus a non-zero ::g_eoh_err.
 *
 * @param[in] err Failure-stage code (::eoh_err_t).
 * @param[in] msg Console diagnostic bytes.
 * @param[in] len Length of @p msg.
 */
static void eoh_fail(uint32_t err, const uint8_t* msg, uint32_t len)
{
  g_eoh_err = err;
  eoh_print(msg, len);
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief CRC-32 (reflected, poly 0xEDB88320) over @p data. */
static uint32_t eoh_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = (uint32_t)k_eoh_crc_init;
  for (size_t i = 0U; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint32_t bit = 0U; bit < (uint32_t)k_eoh_crc_bits; bit++) {
      const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc                 = (crc >> 1U) ^ ((uint32_t)k_eoh_crc_poly & mask);
    }
  }
  return ~crc;
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void eoh_print_hex(uint32_t value)
{
  uint8_t buf[k_eoh_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_eoh_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_eoh_hex_nibbles - 1U - i) * (uint32_t)k_eoh_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_eoh_nibble_mask;
    buf[i] =
      (uint8_t)((nib < (uint32_t)k_eoh_dec_ten) ? ('0' + nib) : ('A' + (nib - k_eoh_dec_ten)));
  }
  eoh_print(buf, (uint32_t)k_eoh_hex_nibbles);
}

/** @brief Print a small unsigned integer in decimal. */
static void eoh_print_uint(uint32_t value)
{
  uint8_t  buf[k_eoh_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_eoh_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_eoh_dec_ten));
    n++;
    value /= (uint32_t)k_eoh_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    eoh_print(&buf[n - 1U - i], 1U);
  }
}

/* ---- SPI -> ra8_sdmmc_spi transport adapter (mirror of fs_format_mount) ---- */

/* cppcheck-suppress constParameterCallback -- bound to ra8_sdmmc_spi_transport_t::set_clock, `ra8_err_t (*)(void*, uint32_t)`; constifying ctx would break the binding. */
static ra8_err_t eoh_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra8_sci_spi_set_clock((uint8_t)k_eoh_spi_chan, hz, pclka_hz);
}

/** @brief ra8_sdmmc_spi_transport_t::cs over ra8_gpio (CS active-low). */
static ra8_err_t eoh_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra8_gpio_write(k_eoh_pin_cs, asserted ? k_ra8_level_low : k_ra8_level_high);
}

/** @brief ra8_sdmmc_spi_transport_t::xfer over ra8_sci_spi_xfer. */
static ra8_err_t eoh_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  return ra8_sci_spi_xfer((uint8_t)k_eoh_spi_chan, tx, rx, len);
}

/** @brief Route Pmod2 SPI pins and claim CS as a GPIO output (idle high). */
[[nodiscard]] static ra8_err_t eoh_spi_pins_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(k_eoh_pin_sck, k_ra8_psel_sci_async, "eoh.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_eoh_pin_cipo, k_ra8_psel_sci_async, "eoh.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_eoh_pin_copi, k_ra8_psel_sci_async, "eoh.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_output_init(k_eoh_pin_cs, k_ra8_level_high);
}

/** @brief Bring up CGC + SysTick + console SCI + SPI + CS GPIO; halt on fail. */
static void eoh_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) ||
      (ra8_time_init(cpuclk0_hz) != k_ra8_ok) ||
      (ra8_board_uart_console_init((uint32_t)k_eoh_uart_baud) != k_ra8_ok)) {
    eoh_fail((uint32_t)k_eoh_err_init, k_msg_finit, (uint32_t)sizeof(k_msg_finit) - 1U);
  }
  if (eoh_spi_pins_init() != k_ra8_ok) {
    eoh_fail((uint32_t)k_eoh_err_init, k_msg_finit, (uint32_t)sizeof(k_msg_finit) - 1U);
  }
  const ra8_sci_spi_cfg_t spi_cfg = {.baud_hz   = (uint32_t)k_ra8_sdmmc_spi_clock_init_hz,
                                     .pclk_hz   = pclka_hz,
                                     .mode      = k_ra8_spi_mode_0,
                                     .lsb_first = false};
  if (ra8_sci_spi_init((uint8_t)k_eoh_spi_chan, &spi_cfg) != k_ra8_ok) {
    eoh_fail((uint32_t)k_eoh_err_init, k_msg_finit, (uint32_t)sizeof(k_msg_finit) - 1U);
  }
  *out_pclka_hz = pclka_hz;
}

/** @brief Init the SD card over SPI; halt with a diagnostic on failure. */
static void eoh_init_card_or_halt(uint32_t* pclka_hz)
{
  const ra8_sdmmc_spi_transport_t transport = {.set_clock = eoh_spi_set_clock,
                                               .cs        = eoh_spi_cs,
                                               .xfer      = eoh_spi_xfer,
                                               .ctx       = pclka_hz};
  if (ra8_sdmmc_spi_init(&transport) != k_ra8_ok) {
    eoh_fail((uint32_t)k_eoh_err_card, k_msg_fcard, (uint32_t)sizeof(k_msg_fcard) - 1U);
  }
  eoh_print(k_msg_cardok, (uint32_t)sizeof(k_msg_cardok) - 1U);
}

/**
 * @brief Mount the card, formatting FAT32 first if it is blank/unmountable.
 *
 * @details Binds the SD backend, tries to mount; a blank card (e.g. ra8_emulator's
 * `--sd-new` before any format, or a fresh bench card) fails to mount, so it is
 * formatted FAT32 and mounted. Halts on a hard failure.
 *
 * @return The mounted volume handle (never NULL on return).
 */
static ra8_fs_mount_t* eoh_mount_or_halt(void)
{
  if (ra8_sdmmc_spi_bind_fs_backend(&s_backend) != k_ra8_ok) {
    eoh_fail((uint32_t)k_eoh_err_mount, k_msg_fmount, (uint32_t)sizeof(k_msg_fmount) - 1U);
  }
  ra8_fs_mount_t* mount = nullptr;
  if (ra8_fs_mount(&s_backend, &mount) != k_ra8_ok) {
    ra8_fs_format_opts_t opts = {};
    opts.type                 = k_ra8_fs_type_fat32;
    opts.label                = "RAEPUB";
    if ((ra8_fs_format(&s_backend, &opts) != k_ra8_ok) ||
        (ra8_fs_mount(&s_backend, &mount) != k_ra8_ok)) {
      eoh_fail((uint32_t)k_eoh_err_mount, k_msg_fmount, (uint32_t)sizeof(k_msg_fmount) - 1U);
    }
  }
  return mount;
}

/** @brief Write the baked .epub onto the volume if @ref k_eoh_book_path absent. */
static void eoh_provision_or_halt(ra8_fs_mount_t* mount)
{
  ra8_fs_file_t* file = nullptr;
  if (ra8_fs_open(mount, k_eoh_book_path, k_ra8_fs_mode_read, &file) == k_ra8_ok) {
    (void)ra8_fs_close(file); /* already present -- nothing to provision. */
    return;
  }
  if (ra8_fs_write_file(mount, k_eoh_book_path, k_epub_fixture, (uint32_t)k_epub_fixture_len) !=
      k_ra8_ok) {
    eoh_fail((uint32_t)k_eoh_err_prov, k_msg_fprov, (uint32_t)sizeof(k_msg_fprov) - 1U);
  }
}

/**
 * @brief Open the provisioned .epub off SD, verify the spine, CRC chapter 0.
 *
 * @param[in]  mount   Mounted SD volume holding @ref k_eoh_book_path.
 * @param[out] out_crc Receives the CRC-32 of chapter 0's decompressed XHTML.
 * @return The chapter (spine) count.
 */
static uint16_t eoh_parse_or_halt(ra8_fs_mount_t* mount, uint32_t* out_crc)
{
  if (epub_open_streamed_fs(mount, k_eoh_book_path, &s_epub_io, &s_book) != k_ra8_ok) {
    eoh_fail((uint32_t)k_eoh_err_open, k_msg_fopen, (uint32_t)sizeof(k_msg_fopen) - 1U);
  }
  uint16_t chapters = 0U;
  if ((epub_get_chapter_count(&s_book, &chapters) != k_ra8_ok) ||
      (chapters != (uint16_t)k_eoh_expect_chap)) {
    eoh_fail((uint32_t)k_eoh_err_chap, k_msg_fchap, (uint32_t)sizeof(k_msg_fchap) - 1U);
  }
  size_t got = 0U;
  if ((epub_load_chapter(&s_book, 0U, s_chapter, (size_t)k_eoh_chap_cap, &got) != k_ra8_ok) ||
      (got == 0U)) {
    eoh_fail((uint32_t)k_eoh_err_load, k_msg_fload, (uint32_t)sizeof(k_msg_fload) - 1U);
  }
  const uint32_t crc = eoh_crc32(s_chapter, got);
  if (crc != (uint32_t)k_eoh_expect_crc) {
    eoh_fail((uint32_t)k_eoh_err_crc, k_msg_fcrc, (uint32_t)sizeof(k_msg_fcrc) - 1U);
  }
  epub_metadata_t meta = {};
  if ((epub_get_metadata(&s_book, &meta) != k_ra8_ok) || (meta.title == nullptr) ||
      (meta.title[0] == '\0')) {
    eoh_fail((uint32_t)k_eoh_err_meta, k_msg_fmeta, (uint32_t)sizeof(k_msg_fmeta) - 1U);
  }
  *out_crc = crc;
  return chapters;
}

/**
 * @brief App entry: SD bring-up -> provision -> open/parse -> heartbeat idle.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post On success ::g_eoh_chapters / ::g_eoh_crc hold the parsed results, the
 *       banner is emitted, and ::g_eoh_heartbeat advances once per frame.
 * @post On any failure ::g_eoh_err is non-zero and the CPU parks (no heartbeat).
 * @since 0.1.0
 */
void main(void)
{
  uint32_t pclka_hz = 0U;
  eoh_setup_or_halt(&pclka_hz);
  ra8_isr_globals_enable();
  ra8_log_init();
  eoh_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  eoh_init_card_or_halt(&pclka_hz);
  ra8_fs_mount_t* const mount = eoh_mount_or_halt();
  eoh_provision_or_halt(mount);

  uint32_t       ch0_crc  = 0U;
  const uint16_t chapters = eoh_parse_or_halt(mount, &ch0_crc);

  /* Latch the results for the J-Link / `--dump-sym` memprobe gate. */
  g_eoh_chapters = (uint32_t)chapters;
  g_eoh_crc      = ch0_crc;

  eoh_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  eoh_print_uint((uint32_t)chapters);
  eoh_print(k_msg_crc, (uint32_t)sizeof(k_msg_crc) - 1U);
  eoh_print_hex(ch0_crc);
  eoh_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  /* Idle with a heartbeat that advances ONLY here -- the failure path parks in
   * WFI without bumping it -- so a memprobe gate proves the full SD -> ra8_fs ->
   * epub open/parse pipeline ran and the chapter-0 CRC matched. */
  while (1) {
    ra8_delay_ms(k_eoh_frame_ms);
    g_eoh_heartbeat++;
  }
}
