/**
 * @file examples/ek_ra8d2/hw_validated/hil/tz_secure_only_sd/main.c
 * @brief SPI-mode SD card round-trip HIL demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the ``ra8_sdmmc_spi`` driver
 * against a Digilent PMOD MicroSD (part 410-380, Digikey 1286-1200-ND)
 * plugged into Pmod2 (J25) on the EK-RA8D2. The flow:
 *
 *   1. ``ra8_cgc_init`` -- CPUCLK0 = 1 GHz, PCLKA = 125 MHz, SCICLK =
 *      100 MHz.
 *   2. Route SCI8 (TXD8 = PD02, RXD8 = PD03) for the J-Link OB CDC
 *      console at 115200 8N1 -- this is the same console as
 *      ``uart_hello`` and is what the HIL runner scrapes.
 *   3. Route Pmod2 SPI pins (P601 RSPCKB, P602 MISOB, P603 MOSIB,
 *      P604 SSLB0) per EK-RA8D2 v1 User's Manual Table 19 p 27.
 *      The chip-select line is claimed as a plain GPIO so the SD
 *      driver can hold it asserted across multi-byte command frames
 *      (the SCI hardware CS pulses per byte, which the SD SPI mode
 *      protocol cannot tolerate -- SD spec PHY v9 section 7.2.4).
 *   4. ``ra8_sci_spi_init`` channel 0 (SCI0 Simple-SPI) at 400 kHz,
 *      mode 0, MSB-first -- the SD spec mandates the bus opens at
 *      <=400 kHz for the identification phase.
 *   5. ``ra8_sdmmc_spi_init`` runs the standard SPI-mode SD bring-up
 *      (CMD0 / CMD8 / ACMD41 / CMD58 / CMD9 / CMD16) and escalates
 *      the bus to 25 MHz default-speed on success.
 *   6. Mount a FAT volume via ``ra8_fs_mount`` using the backend
 *      adapter shipped with ``ra8_sdmmc_spi``.
 *   7. Write a fixed-seed pseudo-random payload to ``/test.txt``,
 *      seek back to 0, read it into a second buffer, byte-compare.
 *   8. Print the round-trip result over SCI8. The HIL runner scrapes
 *      for the literal ``sd: roundtrip ok`` line and treats it as
 *      proof the SD path works end-to-end.
 *
 * Required external hardware: Digilent PMOD MicroSD plugged into J25,
 * with a FAT-formatted (FAT16 or FAT32) microSD card inserted. The
 * demo does NOT format the card.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_log.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci_spi.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_spi.h"
#include "ra8_time.h"

/* =============================================================================
 * Tunables
 * =============================================================================
 */

/**
 * @enum sd_demo_config_t
 * @brief Compile-time settings for the SD round-trip demo.
 */
typedef enum : uint32_t {
  k_sd_decimal_base       = 10U,          /**< Radix for integer-to-ASCII.                       */
  k_sd_spi_idle_byte      = 0xFFU,        /**< Byte clocked out on read-only SPI xfers.          */
  k_sd_prng_byte_shift    = 16U,          /**< Bit shift selecting the PRNG output byte.         */
  k_sd_byte_mask          = 0xFFU,        /**< Low-byte mask.                                    */
  k_sd_demo_uart_baud     = 115200U,      /**< SD demo UART baud.                                */
  k_sd_demo_spi_channel   = 0U,           /**< Pmod2 / J25 is SCI0 Simple-SPI (HUM Table 20.13). */
  k_sd_demo_payload_bytes = 4096U,        /**< SD demo payload bytes.                            */
  k_sd_demo_prng_seed     = 0x5EEDC0DEUL, /**< SD demo prng seed.                                */
  k_sd_demo_prng_mul      = 1664525UL,    /**< Numerical Recipes LCG.                            */
  k_sd_demo_prng_add      = 1013904223UL, /**< SD demo prng add.                                 */
} sd_demo_config_t;

/**
 * @enum sd_demo_mb_t
 * @brief Conversion factor 512-byte blocks -> MiB.
 */
typedef enum : uint32_t {
  k_sd_demo_blocks_per_mib = 2048U, /**< 1 MiB / 512 B = 2048 blocks. */
} sd_demo_mb_t;

/* =============================================================================
 * Pinout (Pmod2 SPI for SD card; SCI8 console owned by the board BSP)
 * =============================================================================
 */

/**
 * @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI per HUM Table 20.13.
 *
 * @details
 * P604 is left as a *GPIO* output because SD SPI-mode framing
 * requires CS to stay asserted across the 6-byte command + payload +
 * CRC trailer (SD spec PHY v9 section 7.2.4). The SCI hardware CS
 * controller pulses CS between every word, which the SD protocol does
 * not tolerate. Driving CS by hand is the standard workaround.
 */
static const ra8_port_pin_t k_sd_demo_pin_sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;
static const ra8_port_pin_t k_sd_demo_pin_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;
static const ra8_port_pin_t k_sd_demo_pin_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;
static const ra8_port_pin_t k_sd_demo_pin_cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/* =============================================================================
 * UART output helpers
 * =============================================================================
 */

/** @brief Static message strings -- ASCII-only per project policy. */
static const uint8_t k_sd_demo_msg_boot[]       = "sd: boot\r\n";
static const uint8_t k_sd_demo_msg_init_ok[]    = "sd: card ready\r\n";
static const uint8_t k_sd_demo_msg_mount_ok[]   = "sd: fs mounted\r\n";
static const uint8_t k_sd_demo_msg_ok[]         = "sd: roundtrip ok\r\n";
static const uint8_t k_sd_demo_msg_card_pre[]   = "sd: card=";
static const uint8_t k_sd_demo_msg_mb_suf[]     = " MB\r\n";
static const uint8_t k_sd_demo_msg_init_fail[]  = "sd: FAIL init\r\n";
static const uint8_t k_sd_demo_msg_mount_fail[] = "sd: FAIL mount\r\n";
static const uint8_t k_sd_demo_msg_write_fail[] = "sd: FAIL write\r\n";
static const uint8_t k_sd_demo_msg_read_fail[]  = "sd: FAIL read\r\n";
static const uint8_t k_sd_demo_msg_cmp_pre[]    = "sd: FAIL @ offset ";
static const uint8_t k_sd_demo_msg_eol[]        = "\r\n";

/**
 * @brief Write a NUL-terminated ASCII byte run on the BSP console (SCI8).
 */
static void sd_demo_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/**
 * @brief Write the decimal expansion of ``value`` on SCI8.
 */
static void sd_demo_print_u32(uint32_t value)
{
  uint8_t  buf[11];
  uint32_t pos = (uint32_t)sizeof(buf);
  if (value == 0U) {
    buf[--pos] = (uint8_t)'0';
  } else {
    while ((value > 0U) && (pos > 0U)) {
      buf[--pos] = (uint8_t)('0' + (uint8_t)(value % k_sd_decimal_base));
      value      = value / k_sd_decimal_base;
    }
  }
  sd_demo_print(&buf[pos], (uint32_t)sizeof(buf) - pos);
}

/**
 * @brief Halt forever in WFI -- panic stop on irrecoverable boot failure.
 *
 * @pre Called only after a fatal error in boot.
 * @post CPU is parked.
 * @since 0.1.0
 */
static void sd_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/* =============================================================================
 * SPI -> ra8_sdmmc_spi transport adapter
 * =============================================================================
 */

/**
 * @brief ``ra8_sdmmc_spi_transport_t::set_clock`` shim over ``ra8_spi``.
 *
 * @details
 * The mock-friendly transport signature carries PCLKA in the ``ctx``
 * pointer so we can compute the SPBR divisor without reaching into a
 * file-scope global. ``ctx`` is a ``uint32_t*`` to the cached PCLKA Hz.
 */
/* cppcheck-suppress constParameterCallback -- bound to ra8_sdmmc_spi_transport_t::set_clock, `ra8_err_t (*)(void*, uint32_t)`; constifying ctx would break the binding. */
static ra8_err_t sd_demo_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra8_sci_spi_set_clock((uint8_t)k_sd_demo_spi_channel, hz, pclka_hz);
}

/**
 * @brief ``ra8_sdmmc_spi_transport_t::cs`` shim over ``ra8_gpio``.
 */
static ra8_err_t sd_demo_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra8_gpio_write(k_sd_demo_pin_cs, asserted ? k_ra8_level_low : k_ra8_level_high);
}

/**
 * @brief ``ra8_sdmmc_spi_transport_t::xfer`` shim over ``ra8_sci_spi_xfer``.
 *
 * @details
 * ``ra8_sci_spi_xfer`` is full-duplex and already handles a NULL ``tx``
 * (shifts idle 0xFF) or NULL ``rx`` (discards), so the transport is a
 * thin pass-through.
 */
static ra8_err_t sd_demo_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  return ra8_sci_spi_xfer((uint8_t)k_sd_demo_spi_channel, tx, rx, len);
}

/* =============================================================================
 * Hardware bring-up
 * =============================================================================
 */

/**
 * @brief Route Pmod2 pins to SCI0 Simple-SPI and claim CS as a GPIO output.
 *
 * @pre IOPORT module is reachable.
 * @post P601/P602/P603 are in SCI0 Simple-SPI mode; P604 is a GPIO output.
 */
[[nodiscard]] static ra8_err_t sd_demo_spi_pins_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(k_sd_demo_pin_sck, k_ra8_psel_sci_async, "tz_sd.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_sd_demo_pin_cipo, k_ra8_psel_sci_async, "tz_sd.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_sd_demo_pin_copi, k_ra8_psel_sci_async, "tz_sd.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  /* CS as GPIO output, idle high (deasserted). */
  return ra8_gpio_output_init(k_sd_demo_pin_cs, k_ra8_level_high);
}

/**
 * @brief Bring up CGC + SysTick + console SCI + SPI + CS GPIO. Panic on fail.
 *
 * @param[out] out_pclka_hz Cached PCLKA rate (Hz) for the SPI clock shim.
 *
 * @post On success, the console is ready to print and SCI0 Simple-SPI is
 *       configured at 400 kHz mode-0 MSB-first, CS deasserted.
 */
static void sd_demo_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_sd_demo_uart_baud) != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  if (sd_demo_spi_pins_init() != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  const ra8_sci_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ra8_sdmmc_spi_clock_init_hz,
    .pclk_hz   = pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  if (ra8_sci_spi_init((uint8_t)k_sd_demo_spi_channel, &spi_cfg) != k_ra8_ok) {
    sd_demo_panic_halt();
  }
  *out_pclka_hz = pclka_hz;
}

/* =============================================================================
 * Demo payload + round-trip
 * =============================================================================
 */

/**
 * @brief Fill ``buf`` with a fixed-seed LCG pseudo-random sequence.
 *
 * @details
 * A 32-bit linear-congruential generator (Numerical Recipes) gives a
 * deterministic byte sequence so the test can reseed and compare.
 */
static void sd_demo_fill_payload(uint8_t* buf, uint32_t len)
{
  uint32_t state = (uint32_t)k_sd_demo_prng_seed;
  for (uint32_t i = 0U; i < len; i++) {
    state  = state * (uint32_t)k_sd_demo_prng_mul + (uint32_t)k_sd_demo_prng_add;
    buf[i] = (uint8_t)((state >> k_sd_prng_byte_shift) & k_sd_byte_mask);
  }
}

/**
 * @brief Write the payload to ``TEST.TXT`` on the mounted volume.
 *
 * @details Uses the one-shot ::ra8_fs_write_file so the same path works on
 * FAT and exFAT (exFAT only supports whole-file creation). It does not
 * overwrite: if ``TEST.TXT`` already exists from a previous run the existing
 * file is kept, and the read-back below still validates persistence.
 */
[[nodiscard]] static ra8_err_t
sd_demo_write_payload(ra8_fs_mount_t* mount, const uint8_t* payload, uint32_t len)
{
  ra8_fs_file_t* existing = nullptr;
  if (ra8_fs_open(mount, "TEST.TXT", k_ra8_fs_mode_read, &existing) == k_ra8_ok) {
    return ra8_fs_close(existing);
  }
  return ra8_fs_write_file(mount, "TEST.TXT", payload, len);
}

/**
 * @brief Read the payload back into ``buf`` and report bytes copied.
 */
[[nodiscard]] static ra8_err_t
sd_demo_read_payload(ra8_fs_mount_t* mount, uint8_t* buf, uint32_t cap, uint32_t* out_len)
{
  ra8_fs_file_t* f   = nullptr;
  ra8_err_t      err = ra8_fs_open(mount, "TEST.TXT", k_ra8_fs_mode_read, &f);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_fs_read(f, buf, cap, out_len);
  if (err != k_ra8_ok) {
    (void)ra8_fs_close(f);
    return err;
  }
  return ra8_fs_close(f);
}

/**
 * @brief Print the detected card capacity in MiB on SCI8.
 */
static void sd_demo_print_capacity(void)
{
  uint32_t cap_blocks = 0U;
  (void)ra8_sdmmc_spi_get_capacity(&cap_blocks);
  const uint32_t cap_mib = cap_blocks / (uint32_t)k_sd_demo_blocks_per_mib;
  sd_demo_print(k_sd_demo_msg_card_pre, (uint32_t)sizeof(k_sd_demo_msg_card_pre) - 1U);
  sd_demo_print_u32(cap_mib);
  sd_demo_print(k_sd_demo_msg_mb_suf, (uint32_t)sizeof(k_sd_demo_msg_mb_suf) - 1U);
}

/**
 * @brief Compare payload buffers and print "ok" or first-mismatch offset.
 */
static void sd_demo_compare_and_report(const uint8_t* a, const uint8_t* b, uint32_t got)
{
  if (got != (uint32_t)k_sd_demo_payload_bytes) {
    sd_demo_print(k_sd_demo_msg_cmp_pre, (uint32_t)sizeof(k_sd_demo_msg_cmp_pre) - 1U);
    sd_demo_print_u32(got);
    sd_demo_print(k_sd_demo_msg_eol, (uint32_t)sizeof(k_sd_demo_msg_eol) - 1U);
    sd_demo_panic_halt();
  }
  for (uint32_t i = 0U; i < (uint32_t)k_sd_demo_payload_bytes; i++) {
    if (a[i] != b[i]) {
      sd_demo_print(k_sd_demo_msg_cmp_pre, (uint32_t)sizeof(k_sd_demo_msg_cmp_pre) - 1U);
      sd_demo_print_u32(i);
      sd_demo_print(k_sd_demo_msg_eol, (uint32_t)sizeof(k_sd_demo_msg_eol) - 1U);
      sd_demo_panic_halt();
    }
  }
  sd_demo_print(k_sd_demo_msg_ok, (uint32_t)sizeof(k_sd_demo_msg_ok) - 1U);
}

/**
 * @brief Init the SD driver and panic-halt with a diagnostic on failure.
 */
static void sd_demo_init_card_or_halt(uint32_t* pclka_hz)
{
  const ra8_sdmmc_spi_transport_t transport = {
    .set_clock = sd_demo_spi_set_clock,
    .cs        = sd_demo_spi_cs,
    .xfer      = sd_demo_spi_xfer,
    .ctx       = pclka_hz,
  };
  if (ra8_sdmmc_spi_init(&transport) != k_ra8_ok) {
    sd_demo_print(k_sd_demo_msg_init_fail, (uint32_t)sizeof(k_sd_demo_msg_init_fail) - 1U);
    sd_demo_panic_halt();
  }
  sd_demo_print(k_sd_demo_msg_init_ok, (uint32_t)sizeof(k_sd_demo_msg_init_ok) - 1U);
}

/**
 * @brief Bind the SD backend, mount the FAT volume, panic-halt on failure.
 */
static ra8_fs_mount_t* sd_demo_mount_or_halt(void)
{
  ra8_fs_backend_t backend = {};
  if (ra8_sdmmc_spi_bind_fs_backend(&backend) != k_ra8_ok) {
    sd_demo_print(k_sd_demo_msg_mount_fail, (uint32_t)sizeof(k_sd_demo_msg_mount_fail) - 1U);
    sd_demo_panic_halt();
  }
  ra8_fs_mount_t* mount = nullptr;
  if (ra8_fs_mount(&backend, &mount) != k_ra8_ok) {
    sd_demo_print(k_sd_demo_msg_mount_fail, (uint32_t)sizeof(k_sd_demo_msg_mount_fail) - 1U);
    sd_demo_panic_halt();
  }
  sd_demo_print(k_sd_demo_msg_mount_ok, (uint32_t)sizeof(k_sd_demo_msg_mount_ok) - 1U);
  return mount;
}

/* =============================================================================
 * Main
 * =============================================================================
 */

/**
 * @brief Application entry: bring up the bus, probe the SD card,
 *        write a payload, read it back, compare, print result.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU loops forever after printing the result.
 * @post On any HAL init failure the function halts in WFI.
 * @since 0.1.0
 */
void main(void)
{
  uint32_t pclka_hz = 0U;
  sd_demo_setup_or_halt(&pclka_hz);
  ra8_isr_globals_enable();
  ra8_log_init();
  sd_demo_print(k_sd_demo_msg_boot, (uint32_t)sizeof(k_sd_demo_msg_boot) - 1U);

  sd_demo_init_card_or_halt(&pclka_hz);
  sd_demo_print_capacity();
  ra8_fs_mount_t* mount = sd_demo_mount_or_halt();

  static uint8_t s_payload[k_sd_demo_payload_bytes];
  static uint8_t s_readback[k_sd_demo_payload_bytes];
  sd_demo_fill_payload(s_payload, (uint32_t)k_sd_demo_payload_bytes);
  memset(s_readback, 0, sizeof(s_readback));

  if (sd_demo_write_payload(mount, s_payload, (uint32_t)k_sd_demo_payload_bytes) != k_ra8_ok) {
    sd_demo_print(k_sd_demo_msg_write_fail, (uint32_t)sizeof(k_sd_demo_msg_write_fail) - 1U);
    sd_demo_panic_halt();
  }
  uint32_t got = 0U;
  if (sd_demo_read_payload(mount, s_readback, (uint32_t)k_sd_demo_payload_bytes, &got) !=
      k_ra8_ok) {
    sd_demo_print(k_sd_demo_msg_read_fail, (uint32_t)sizeof(k_sd_demo_msg_read_fail) - 1U);
    sd_demo_panic_halt();
  }
  sd_demo_compare_and_report(s_payload, s_readback, got);

  while (1) {
    __asm__ volatile("wfi");
  }
}
