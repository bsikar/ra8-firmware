/**
 * @file examples/ek_ra8d2/hw_validated/hil/tz_secure_only_sd/main.c
 * @brief SPI-mode SD card round-trip HIL demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the ``ra_sdmmc_spi`` driver
 * against a Digilent PMOD MicroSD (part 410-380, Digikey 1286-1200-ND)
 * plugged into Pmod2 (J25) on the EK-RA8D2. The flow:
 *
 *   1. ``ra_cgc_init`` -- CPUCLK0 = 1 GHz, PCLKA = 125 MHz, SCICLK =
 *      100 MHz.
 *   2. Route SCI8 (TXD8 = PD02, RXD8 = PD03) for the J-Link OB CDC
 *      console at 115200 8N1 -- this is the same console as
 *      ``uart_hello`` and is what the HIL runner scrapes.
 *   3. Route Pmod2 SPI pins (P601 RSPCKB, P602 MISOB, P603 MOSIB,
 *      P604 SSLB0) per EK-RA8D2 v1 User's Manual Table 19 p 27.
 *      The chip-select line is claimed as a plain GPIO so the SD
 *      driver can hold it asserted across multi-byte command frames
 *      (RSPI hardware CS pulses per byte, which the SD SPI mode
 *      protocol cannot tolerate -- SD spec PHY v9 section 7.2.4).
 *   4. ``ra_spi_init`` channel 1 (RSPI bus B) at 400 kHz, mode 0,
 *      MSB-first -- the SD spec mandates the bus opens at <=400 kHz
 *      for the identification phase.
 *   5. ``ra_sdmmc_spi_init`` runs the standard SPI-mode SD bring-up
 *      (CMD0 / CMD8 / ACMD41 / CMD58 / CMD9 / CMD16) and escalates
 *      the bus to 25 MHz default-speed on success.
 *   6. Mount a FAT volume via ``ra_fs_mount`` using the backend
 *      adapter shipped with ``ra_sdmmc_spi``.
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

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_log.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_sdmmc_spi.h"
#include "ra_spi.h"
#include "ra_time.h"

/* =============================================================================
 * Tunables
 * =============================================================================
 */

/**
 * @enum sd_demo_config_t
 * @brief Compile-time settings for the SD round-trip demo.
 */
typedef enum : uint32_t {
  k_sd_demo_uart_baud     = 115200U,
  k_sd_demo_uart_channel  = 8U,
  k_sd_demo_spi_channel   = 1U, /**< Pmod2 / J25 is wired to RSPI bus B = SPI ch 1. */
  k_sd_demo_payload_bytes = 4096U,
  k_sd_demo_prng_seed     = 0x5EEDC0DEUL,
  k_sd_demo_prng_mul      = 1664525UL, /**< Numerical Recipes LCG.    */
  k_sd_demo_prng_add      = 1013904223UL,
} sd_demo_config_t;

/**
 * @enum sd_demo_mb_t
 * @brief Conversion factor 512-byte blocks -> MiB.
 */
typedef enum : uint32_t {
  k_sd_demo_blocks_per_mib = 2048U, /**< 1 MiB / 512 B = 2048 blocks. */
} sd_demo_mb_t;

/* =============================================================================
 * Pinout (SCI8 console + Pmod2 SPI for SD card)
 * =============================================================================
 */

/** @brief SCI8 console TXD = PD02 (UM Table 26 console pinmap). */
static const ra_port_pin_t k_sd_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
/** @brief SCI8 console RXD = PD03 (UM Table 26 console pinmap). */
static const ra_port_pin_t k_sd_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/**
 * @brief Pmod2 SPI pins (J25) -- routed to RSPI bus B per UM Table 19 p 27.
 *
 * @details
 * P604 (SSLB0) is left as a *GPIO* output because SD SPI-mode framing
 * requires CS to stay asserted across the 6-byte command + payload +
 * CRC trailer (SD spec PHY v9 section 7.2.4). The RSPI hardware CS
 * controller pulses CS between every word, which the SD protocol does
 * not tolerate. Driving CS by hand is the standard workaround.
 */
static const ra_port_pin_t k_sd_demo_pin_sck  = (ra_port_pin_t)k_ra_board_pmod2_spi_sck;
static const ra_port_pin_t k_sd_demo_pin_cipo = (ra_port_pin_t)k_ra_board_pmod2_spi_cipo;
static const ra_port_pin_t k_sd_demo_pin_copi = (ra_port_pin_t)k_ra_board_pmod2_spi_copi;
static const ra_port_pin_t k_sd_demo_pin_cs   = (ra_port_pin_t)k_ra_board_pmod2_spi_cs;

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
 * @brief Write a NUL-terminated ASCII byte run on SCI8.
 */
static void sd_demo_print(const uint8_t* msg, uint32_t len)
{
  (void)ra_sci_write_polling((uint8_t)k_sd_demo_uart_channel, msg, len);
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
      buf[--pos] = (uint8_t)('0' + (uint8_t)(value % 10U));
      value      = value / 10U;
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
 * SPI -> ra_sdmmc_spi transport adapter
 * =============================================================================
 */

/**
 * @brief ``ra_sdmmc_spi_transport_t::set_clock`` shim over ``ra_spi``.
 *
 * @details
 * The mock-friendly transport signature carries PCLKA in the ``ctx``
 * pointer so we can compute the SPBR divisor without reaching into a
 * file-scope global. ``ctx`` is a ``uint32_t*`` to the cached PCLKA Hz.
 */
/* cppcheck-suppress constParameterCallback
 * Reason: this function is bound to ra_sdmmc_spi_transport_t::set_clock,
 * whose signature is `ra_err_t (*)(void*, uint32_t)` -- changing `ctx`
 * to `const void*` would also require changing the function-pointer
 * type, which would break every other transport binding. */
static ra_err_t sd_demo_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra_spi_set_clock((uint8_t)k_sd_demo_spi_channel, hz, pclka_hz);
}

/**
 * @brief ``ra_sdmmc_spi_transport_t::cs`` shim over ``ra_gpio``.
 */
static ra_err_t sd_demo_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra_gpio_write(k_sd_demo_pin_cs, asserted ? k_ra_level_low : k_ra_level_high);
}

/**
 * @brief ``ra_sdmmc_spi_transport_t::xfer`` shim over ``ra_spi_write_read``.
 *
 * @details
 * Falls back to per-byte exchange when either buffer is nullptr, since
 * the underlying ``ra_spi_write_read`` requires both. The driver
 * itself already gracefully handles a fall-back path, but doing the
 * fallback inside the transport keeps the driver-side code simpler.
 */
static ra_err_t sd_demo_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  if ((tx != nullptr) && (rx != nullptr)) {
    return ra_spi_write_read((uint8_t)k_sd_demo_spi_channel, tx, rx, len, k_ra_spi_width_8);
  }
  /* Per-byte fallback so callers can shift out idle 0xFF without
   * allocating an rx buffer they don't need. */
  for (uint32_t i = 0U; i < len; i++) {
    const uint8_t tx_byte = (tx != nullptr) ? tx[i] : 0xFFU;
    uint8_t       rx_byte = 0U;
    ra_err_t      err     = ra_spi_xfer8((uint8_t)k_sd_demo_spi_channel, tx_byte, &rx_byte);
    if (err != k_ra_ok) {
      return err;
    }
    if (rx != nullptr) {
      rx[i] = rx_byte;
    }
  }
  return k_ra_ok;
}

/* =============================================================================
 * Hardware bring-up
 * =============================================================================
 */

/**
 * @brief Route SCI8 console pins to PD02 / PD03.
 */
[[nodiscard]] static ra_err_t sd_demo_console_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_sd_demo_pin_txd, k_ra_psel_sci_async, "tz_sd.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_sd_demo_pin_rxd, k_ra_psel_sci_async, "tz_sd.rxd8");
}

/**
 * @brief Route Pmod2 SPI pins to RSPI bus B and claim CS as a GPIO output.
 *
 * @pre IOPORT module is reachable.
 * @post P601/P602/P603 are in RSPI peripheral mode; P604 is a GPIO output.
 */
[[nodiscard]] static ra_err_t sd_demo_spi_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_sd_demo_pin_sck, k_ra_psel_spi, "tz_sd.sck");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_sd_demo_pin_cipo, k_ra_psel_spi, "tz_sd.cipo");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral(k_sd_demo_pin_copi, k_ra_psel_spi, "tz_sd.copi");
  if (err != k_ra_ok) {
    return err;
  }
  /* CS as GPIO output, idle high (deasserted). */
  return ra_gpio_output_init(k_sd_demo_pin_cs, k_ra_level_high);
}

/**
 * @brief Bring up CGC + SysTick + console SCI + SPI + CS GPIO. Panic on fail.
 *
 * @param[out] out_pclka_hz Cached PCLKA rate (Hz) for the SPI clock shim.
 *
 * @post On success, the console is ready to print and SPI channel 1 is
 *       configured at 400 kHz mode-0 MSB-first, CS deasserted.
 */
static void sd_demo_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    sd_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    sd_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    sd_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    sd_demo_panic_halt();
  }
  if (sd_demo_console_pins_init() != k_ra_ok) {
    sd_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = (uint32_t)k_sd_demo_uart_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_sd_demo_uart_channel, &sci_cfg) != k_ra_ok) {
    sd_demo_panic_halt();
  }
  if (sd_demo_spi_pins_init() != k_ra_ok) {
    sd_demo_panic_halt();
  }
  const ra_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ra_sdmmc_spi_clock_init_hz,
    .pclka_hz  = pclka_hz,
    .mode      = k_ra_spi_mode_0,
    .lsb_first = false,
  };
  if (ra_spi_init((uint8_t)k_sd_demo_spi_channel, &spi_cfg) != k_ra_ok) {
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
    buf[i] = (uint8_t)((state >> 16U) & 0xFFU);
  }
}

/**
 * @brief Write the payload to ``/test.txt`` on the mounted FAT volume.
 */
[[nodiscard]] static ra_err_t
sd_demo_write_payload(ra_fs_mount_t* mount, const uint8_t* payload, uint32_t len)
{
  ra_fs_file_t* f   = nullptr;
  ra_err_t      err = ra_fs_open(mount, "TEST.TXT", k_ra_fs_mode_write, &f);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_fs_write(f, payload, len);
  if (err != k_ra_ok) {
    (void)ra_fs_close(f);
    return err;
  }
  return ra_fs_close(f);
}

/**
 * @brief Read the payload back into ``buf`` and report bytes copied.
 */
[[nodiscard]] static ra_err_t
sd_demo_read_payload(ra_fs_mount_t* mount, uint8_t* buf, uint32_t cap, uint32_t* out_len)
{
  ra_fs_file_t* f   = nullptr;
  ra_err_t      err = ra_fs_open(mount, "TEST.TXT", k_ra_fs_mode_read, &f);
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_fs_read(f, buf, cap, out_len);
  if (err != k_ra_ok) {
    (void)ra_fs_close(f);
    return err;
  }
  return ra_fs_close(f);
}

/**
 * @brief Print the detected card capacity in MiB on SCI8.
 */
static void sd_demo_print_capacity(void)
{
  uint32_t cap_blocks = 0U;
  (void)ra_sdmmc_spi_get_capacity(&cap_blocks);
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
  const ra_sdmmc_spi_transport_t transport = {
    .set_clock = sd_demo_spi_set_clock,
    .cs        = sd_demo_spi_cs,
    .xfer      = sd_demo_spi_xfer,
    .ctx       = pclka_hz,
  };
  if (ra_sdmmc_spi_init(&transport) != k_ra_ok) {
    sd_demo_print(k_sd_demo_msg_init_fail, (uint32_t)sizeof(k_sd_demo_msg_init_fail) - 1U);
    sd_demo_panic_halt();
  }
  sd_demo_print(k_sd_demo_msg_init_ok, (uint32_t)sizeof(k_sd_demo_msg_init_ok) - 1U);
}

/**
 * @brief Bind the SD backend, mount the FAT volume, panic-halt on failure.
 */
static ra_fs_mount_t* sd_demo_mount_or_halt(void)
{
  ra_fs_backend_t backend = {};
  if (ra_sdmmc_spi_bind_fs_backend(&backend) != k_ra_ok) {
    sd_demo_print(k_sd_demo_msg_mount_fail, (uint32_t)sizeof(k_sd_demo_msg_mount_fail) - 1U);
    sd_demo_panic_halt();
  }
  ra_fs_mount_t* mount = nullptr;
  if (ra_fs_mount(&backend, &mount) != k_ra_ok) {
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring up the bus, probe the SD card,
 *        write a payload, read it back, compare, print result.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On clean entry the CPU loops forever after printing the result.
 * @post On any HAL init failure the function halts in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  sd_demo_setup_or_halt(&pclka_hz);
  ra_isr_globals_enable();
  ra_log_init();
  sd_demo_print(k_sd_demo_msg_boot, (uint32_t)sizeof(k_sd_demo_msg_boot) - 1U);

  sd_demo_init_card_or_halt(&pclka_hz);
  sd_demo_print_capacity();
  ra_fs_mount_t* mount = sd_demo_mount_or_halt();

  static uint8_t s_payload[k_sd_demo_payload_bytes];
  static uint8_t s_readback[k_sd_demo_payload_bytes];
  sd_demo_fill_payload(s_payload, (uint32_t)k_sd_demo_payload_bytes);
  memset(s_readback, 0, sizeof(s_readback));

  if (sd_demo_write_payload(mount, s_payload, (uint32_t)k_sd_demo_payload_bytes) != k_ra_ok) {
    sd_demo_print(k_sd_demo_msg_write_fail, (uint32_t)sizeof(k_sd_demo_msg_write_fail) - 1U);
    sd_demo_panic_halt();
  }
  uint32_t got = 0U;
  if (sd_demo_read_payload(mount, s_readback, (uint32_t)k_sd_demo_payload_bytes, &got) != k_ra_ok) {
    sd_demo_print(k_sd_demo_msg_read_fail, (uint32_t)sizeof(k_sd_demo_msg_read_fail) - 1U);
    sd_demo_panic_halt();
  }
  sd_demo_compare_and_report(s_payload, s_readback, got);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
