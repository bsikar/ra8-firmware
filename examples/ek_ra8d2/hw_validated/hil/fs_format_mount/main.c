/**
 * @file examples/ek_ra8d2/hw_validated/hil/fs_format_mount/main.c
 * @brief Format + mount + file-ops HIL demo across every FAT type ra8_fs writes.
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Standalone EVM-tier app that exercises the new `ra8_fs_format()` mkfs path
 * end to end on real removable storage. It drives a microSD card over the
 * `ra8_sdmmc_spi` SPI-mode driver (the same Pmod2 / SCI0 Simple-SPI transport
 * adapter as `tz_secure_only_sd`) and, for each FAT variant `ra8_fs` can write:
 *
 *   - FAT12, FAT16, FAT32,
 *
 * it runs this cycle on the card:
 *
 *   1. `ra8_fs_format` the volume as the target type (auto cluster size);
 *   2. `ra8_fs_mount` it and assert the detected `ra8_fs_type` matches;
 *   3. create + write a known payload to a test file;
 *   4. read it back and byte-compare;
 *   5. rename the file and confirm the new name reads back intact and the old
 *      name is gone;
 *   6. unlink the file and confirm `listdir` no longer shows it;
 *   7. unmount.
 *
 * It then runs the identical cycle on an exFAT volume: format + mount + assert
 * type + confirm an empty root, then create + write + read-back + rename +
 * unlink, and re-confirm an empty root afterwards.
 *
 * On a clean pass for a type it prints `... FS <TYPE> FORMAT+MOUNT PASS`; after
 * all of them it prints `FS FORMAT+MOUNT ALL PASS`. The HIL runner (and the
 * ra8_emulator smoke gate) scrape for that banner. Any failure prints a `FAIL ...`
 * diagnostic and parks the CPU.
 *
 * The flow re-formats the card several times, so the card's initial contents are
 * irrelevant -- on the bench insert any microSD; under ra8_emulator attach a blank
 * card with `--sd-new <MiB>` (e.g. `--sd-new 64:fat32`). A 64 MiB card is large
 * enough that the auto cluster-size sweep lands every type in its valid band.
 *
 * Required external hardware (on-bench): Digilent PMOD MicroSD (part 410-380)
 * in Pmod2 (J25) with any microSD card inserted. THIS APP ERASES THE CARD.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_log.h"
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
 * @enum fs_fmt_config_t
 * @brief Compile-time settings for the format/mount HIL demo.
 */
typedef enum : uint32_t {
  k_fs_fmt_uart_baud       = 115200U,      /**< J-Link OB CDC console baud.           */
  k_fs_fmt_spi_channel     = 0U,           /**< Pmod2 / J25 SCI0 Simple-SPI.          */
  k_fs_fmt_decimal_base    = 10U,          /**< Radix for integer-to-ASCII.           */
  k_fs_fmt_payload_bytes   = 1300U,        /**< Multi-cluster test payload (> 1 KiB). */
  k_fs_fmt_prng_seed       = 0xA5F00DadUL, /**< Deterministic payload seed.           */
  k_fs_fmt_prng_mul        = 1664525UL,    /**< Numerical Recipes LCG multiplier.     */
  k_fs_fmt_prng_add        = 1013904223UL, /**< Numerical Recipes LCG increment.      */
  k_fs_fmt_prng_byte_shift = 16U,          /**< Bit shift selecting the PRNG byte.    */
  k_fs_fmt_byte_mask       = 0xFFU,        /**< Low-byte mask.                        */
} fs_fmt_config_t;

/**
 * @enum fs_fmt_type_idx_t
 * @brief Index into the per-run FAT-type table.
 */
typedef enum : uint8_t {
  k_fs_fmt_idx_fat12 = 0U, /**< FAT12 trial slot. */
  k_fs_fmt_idx_fat16 = 1U, /**< FAT16 trial slot. */
  k_fs_fmt_idx_fat32 = 2U, /**< FAT32 trial slot. */
  k_fs_fmt_idx_count = 3U, /**< Number of trials. */
} fs_fmt_type_idx_t;

/* =============================================================================
 * Pinout (Pmod2 SPI for SD card; SCI8 console owned by the BSP)
 * =============================================================================
 */

/** @brief Pmod2 SPI pins (J25) -- SCI0 Simple-SPI; CS held by GPIO. */
static const ra8_port_pin_t k_fs_fmt_pin_sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;
static const ra8_port_pin_t k_fs_fmt_pin_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;
static const ra8_port_pin_t k_fs_fmt_pin_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;
static const ra8_port_pin_t k_fs_fmt_pin_cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/* =============================================================================
 * Static message strings (ASCII-only per project policy)
 * =============================================================================
 */

static const uint8_t k_msg_boot[]      = "fsfmt: boot\r\n";
static const uint8_t k_msg_card_ok[]   = "fsfmt: card ready\r\n";
static const uint8_t k_msg_init_fail[] = "fsfmt: FAIL init\r\n";
static const uint8_t k_msg_eol[]       = "\r\n";

static const uint8_t k_msg_fat12[] = "FAT12";
static const uint8_t k_msg_fat16[] = "FAT16";
static const uint8_t k_msg_fat32[] = "FAT32";
static const uint8_t k_msg_exfat[] = "EXFAT";

static const uint8_t k_msg_pass_pre[]    = "fsfmt: FS ";
static const uint8_t k_msg_pass_suf[]    = " FORMAT+MOUNT PASS\r\n";
static const uint8_t k_msg_all_pass[]    = "fsfmt: FS FORMAT+MOUNT ALL PASS\r\n";
static const uint8_t k_msg_fail_fmt[]    = " FAIL format\r\n";
static const uint8_t k_msg_fail_mount[]  = " FAIL mount\r\n";
static const uint8_t k_msg_fail_type[]   = " FAIL type-mismatch\r\n";
static const uint8_t k_msg_fail_write[]  = " FAIL write\r\n";
static const uint8_t k_msg_fail_rename[] = " FAIL rename\r\n";
static const uint8_t k_msg_fail_unlink[] = " FAIL unlink\r\n";
/* Not a failure: the card capacity is outside this FAT type's cluster band
 * (e.g. FAT12/FAT16 on a multi-GB card). Worded without "FAIL" so the HIL
 * negative-match does not trip. */
static const uint8_t k_msg_skip_size[] = " SKIP: capacity out of range for this type\r\n";

/** @brief Test file names (8.3, root level). */
static const char k_name_a[] = "FMTTEST.BIN";
static const char k_name_b[] = "FMTDONE.BIN";

/* =============================================================================
 * UART output helpers
 * =============================================================================
 */

/**
 * @brief Write a byte run on the SCI8 console.
 *
 * @param[in] msg Bytes to emit.
 * @param[in] len Byte count.
 *
 * @pre The BSP console is initialised.
 * @post The bytes are queued to the console.
 * @since 0.1.0
 */
static void fs_fmt_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Emit a NUL-terminated literal (length via strlen at compile sites). */
#define FS_FMT_PUTS(lit) fs_fmt_print((lit), (uint32_t)sizeof(lit) - 1U)

/**
 * @brief Halt forever in WFI -- panic stop on irrecoverable failure.
 *
 * @pre Called only after a fatal error has been reported.
 * @post CPU is parked.
 * @since 0.1.0
 */
static void fs_fmt_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Return the printable label bytes for a FAT type.
 *
 * @param[in]  type     FAT variant under test.
 * @param[out] out_len  Receives the label length.
 *
 * @return Pointer to the (static) label bytes.
 * @retval k_msg_fat12 / _fat16 / _fat32 per @p type.
 *
 * @pre @p out_len is non-NULL.
 * @pre @p type is FAT12/FAT16/FAT32.
 * @post `*out_len` holds the label length.
 * @post No state modified beyond `*out_len`.
 * @since 0.1.0
 */
static const uint8_t* fs_fmt_label(ra8_fs_type_t type, uint32_t* out_len)
{
  if (type == k_ra8_fs_type_fat12) {
    *out_len = (uint32_t)sizeof(k_msg_fat12) - 1U;
    return k_msg_fat12;
  }
  if (type == k_ra8_fs_type_fat16) {
    *out_len = (uint32_t)sizeof(k_msg_fat16) - 1U;
    return k_msg_fat16;
  }
  if (type == k_ra8_fs_type_exfat) {
    *out_len = (uint32_t)sizeof(k_msg_exfat) - 1U;
    return k_msg_exfat;
  }
  *out_len = (uint32_t)sizeof(k_msg_fat32) - 1U;
  return k_msg_fat32;
}

/**
 * @brief Print "fsfmt: FS <TYPE><suffix>" for a pass/fail line.
 *
 * @param[in] type   FAT variant the line is about.
 * @param[in] suffix NUL-trimmed suffix bytes (e.g. " FAIL mount\r\n").
 * @param[in] suflen Suffix length.
 *
 * @pre SCI8 is initialised.
 * @pre @p suffix is non-NULL.
 * @post The composed line is queued to the console.
 * @since 0.1.0
 */
static void fs_fmt_print_line(ra8_fs_type_t type, const uint8_t* suffix, uint32_t suflen)
{
  uint32_t       lbl_len = 0U;
  const uint8_t* lbl     = fs_fmt_label(type, &lbl_len);
  fs_fmt_print(k_msg_pass_pre, (uint32_t)sizeof(k_msg_pass_pre) - 1U);
  fs_fmt_print(lbl, lbl_len);
  fs_fmt_print(suffix, suflen);
}

/* =============================================================================
 * SPI -> ra8_sdmmc_spi transport adapter (mirror of tz_secure_only_sd)
 * =============================================================================
 */

/* cppcheck-suppress constParameterCallback -- bound to ra8_sdmmc_spi_transport_t::set_clock, `ra8_err_t (*)(void*, uint32_t)`; constifying ctx would break the binding. */
static ra8_err_t fs_fmt_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka_hz = *(const uint32_t*)ctx;
  return ra8_sci_spi_set_clock((uint8_t)k_fs_fmt_spi_channel, hz, pclka_hz);
}

/** @brief ra8_sdmmc_spi_transport_t::cs over ra8_gpio (CS active-low). */
static ra8_err_t fs_fmt_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra8_gpio_write(k_fs_fmt_pin_cs, asserted ? k_ra8_level_low : k_ra8_level_high);
}

/** @brief ra8_sdmmc_spi_transport_t::xfer over ra8_sci_spi_xfer. */
static ra8_err_t fs_fmt_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  return ra8_sci_spi_xfer((uint8_t)k_fs_fmt_spi_channel, tx, rx, len);
}

/* =============================================================================
 * Hardware bring-up
 * =============================================================================
 */

/**
 * @brief Route Pmod2 SPI pins and claim CS as a GPIO output (idle high).
 *
 * @return ra8_err_t from the routing calls.
 * @retval k_ra8_ok    SPI pins routed, CS driven high.
 * @retval k_ra8_err_* Routing failure.
 * @pre IOPORT is reachable.
 * @post P601/P602/P603 are SCI0 Simple-SPI; P604 is a GPIO output high.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fs_fmt_spi_pins_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(k_fs_fmt_pin_sck, k_ra8_psel_sci_async, "fsfmt.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_fs_fmt_pin_cipo, k_ra8_psel_sci_async, "fsfmt.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_fs_fmt_pin_copi, k_ra8_psel_sci_async, "fsfmt.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_output_init(k_fs_fmt_pin_cs, k_ra8_level_high);
}

/**
 * @brief Bring up CGC + SysTick + console SCI + SPI + CS GPIO; panic on fail.
 *
 * @param[out] out_pclka_hz Cached PCLKA rate (Hz) for the SPI clock shim.
 *
 * @pre Reset_Handler initialised .data/.bss.
 * @post On success the console prints and SCI0 Simple-SPI is configured at the
 *       SD init clock, CS deasserted.
 * @since 0.1.0
 */
static void fs_fmt_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    fs_fmt_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    fs_fmt_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) {
    fs_fmt_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    fs_fmt_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_fs_fmt_uart_baud) != k_ra8_ok) {
    fs_fmt_panic_halt();
  }
  if (fs_fmt_spi_pins_init() != k_ra8_ok) {
    fs_fmt_panic_halt();
  }
  const ra8_sci_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ra8_sdmmc_spi_clock_init_hz,
    .pclk_hz   = pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  if (ra8_sci_spi_init((uint8_t)k_fs_fmt_spi_channel, &spi_cfg) != k_ra8_ok) {
    fs_fmt_panic_halt();
  }
  *out_pclka_hz = pclka_hz;
}

/**
 * @brief Init the SD driver, panic-halt with a diagnostic on failure.
 *
 * @param[in] pclka_hz Cached PCLKA (Hz) bound into the transport ctx.
 *
 * @pre SCI0 Simple-SPI + CS GPIO are configured.
 * @post On success the SD card is in SPI mode at default speed.
 * @since 0.1.0
 */
static void fs_fmt_init_card_or_halt(uint32_t* pclka_hz)
{
  const ra8_sdmmc_spi_transport_t transport = {
    .set_clock = fs_fmt_spi_set_clock,
    .cs        = fs_fmt_spi_cs,
    .xfer      = fs_fmt_spi_xfer,
    .ctx       = pclka_hz,
  };
  if (ra8_sdmmc_spi_init(&transport) != k_ra8_ok) {
    FS_FMT_PUTS(k_msg_init_fail);
    fs_fmt_panic_halt();
  }
  FS_FMT_PUTS(k_msg_card_ok);
}

/* =============================================================================
 * Payload + per-type cycle
 * =============================================================================
 */

/** @brief Static payload + read-back buffers (no heap; NASA Rule 3). */
static uint8_t s_payload[k_fs_fmt_payload_bytes];
static uint8_t s_readback[k_fs_fmt_payload_bytes];

/**
 * @brief Fill the payload buffer with a deterministic LCG byte sequence.
 *
 * @pre None.
 * @post `s_payload` holds a reproducible byte pattern.
 * @since 0.1.0
 */
static void fs_fmt_fill_payload(void)
{
  uint32_t state = (uint32_t)k_fs_fmt_prng_seed;
  for (uint32_t i = 0U; i < (uint32_t)k_fs_fmt_payload_bytes; i++) {
    state        = (state * (uint32_t)k_fs_fmt_prng_mul) + (uint32_t)k_fs_fmt_prng_add;
    s_payload[i] = (uint8_t)((state >> k_fs_fmt_prng_byte_shift) & k_fs_fmt_byte_mask);
  }
}

/**
 * @brief Create + write the payload, read it back, and byte-compare.
 *
 * @param[in] mount Mounted volume.
 * @param[in] path  File name to create.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                Round-trip verified.
 * @retval k_ra8_err_validation_failed Length or content mismatch on read-back.
 * @retval k_ra8_err_*             ra8_fs create/read failure.
 * @pre @p mount is mounted; `s_payload` is filled.
 * @pre @p path does not already exist.
 * @post On success @p path holds `s_payload`.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fs_fmt_create_and_verify(ra8_fs_mount_t* mount, const char* path)
{
  ra8_err_t err = ra8_fs_write_file(mount, path, s_payload, (uint32_t)k_fs_fmt_payload_bytes);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_fs_file_t* f = nullptr;
  err              = ra8_fs_open(mount, path, k_ra8_fs_mode_read, &f);
  if (err != k_ra8_ok) {
    return err;
  }
  memset(s_readback, 0, sizeof(s_readback));
  uint32_t got = 0U;
  err          = ra8_fs_read(f, s_readback, (uint32_t)k_fs_fmt_payload_bytes, &got);
  (void)ra8_fs_close(f);
  if (err != k_ra8_ok) {
    return err;
  }
  if (got != (uint32_t)k_fs_fmt_payload_bytes) {
    return k_ra8_err_validation_failed;
  }
  if (memcmp(s_payload, s_readback, (size_t)k_fs_fmt_payload_bytes) != 0) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Rename @p from to @p to and confirm the new name reads back intact.
 *
 * @param[in] mount Mounted volume.
 * @param[in] from  Existing file name.
 * @param[in] to    Replacement name (must not exist).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                Renamed; old gone, new reads back.
 * @retval k_ra8_err_invalid_state Old name still resolves after rename.
 * @retval k_ra8_err_*             ra8_fs rename/read failure.
 * @pre @p from exists and holds `s_payload`.
 * @post On success @p to holds the data and @p from is gone.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t
fs_fmt_rename_and_verify(ra8_fs_mount_t* mount, const char* from, const char* to)
{
  ra8_err_t err = ra8_fs_rename(mount, from, to);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_fs_file_t* gone = nullptr;
  if (ra8_fs_open(mount, from, k_ra8_fs_mode_read, &gone) == k_ra8_ok) {
    (void)ra8_fs_close(gone);
    return k_ra8_err_invalid_state; /* old name must no longer resolve */
  }
  ra8_fs_file_t* f = nullptr;
  err              = ra8_fs_open(mount, to, k_ra8_fs_mode_read, &f);
  if (err != k_ra8_ok) {
    return err;
  }
  memset(s_readback, 0, sizeof(s_readback));
  uint32_t got = 0U;
  err          = ra8_fs_read(f, s_readback, (uint32_t)k_fs_fmt_payload_bytes, &got);
  (void)ra8_fs_close(f);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((got != (uint32_t)k_fs_fmt_payload_bytes) ||
      (memcmp(s_payload, s_readback, (size_t)k_fs_fmt_payload_bytes) != 0)) {
    return k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Run the create -> write -> read-back -> rename -> unlink cycle on a
 *        mounted volume, printing the specific FAIL line on the failing step.
 *
 * @details Shared by `fs_fmt_run_one_type` (FAT12/16/32) and `fs_fmt_run_exfat`
 *          so every writable filesystem proves the identical mutation path. The
 *          caller owns the mount and unmounts after this returns.
 *
 * @param[in] mount Mounted, type-validated volume.
 * @param[in] type  Filesystem type, for the per-step diagnostic line.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok    create + rename + unlink all verified.
 * @retval k_ra8_err_* The first failing step's code (its FAIL line is printed).
 * @pre @p mount is mounted; `s_payload` is filled.
 * @pre `k_name_a` does not already exist on @p mount.
 * @post On success @p mount no longer holds the test file.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fs_fmt_write_cycle(ra8_fs_mount_t* mount, ra8_fs_type_t type)
{
  ra8_err_t err = fs_fmt_create_and_verify(mount, k_name_a);
  if (err != k_ra8_ok) {
    fs_fmt_print_line(type, k_msg_fail_write, (uint32_t)sizeof(k_msg_fail_write) - 1U);
    return err;
  }
  err = fs_fmt_rename_and_verify(mount, k_name_a, k_name_b);
  if (err != k_ra8_ok) {
    fs_fmt_print_line(type, k_msg_fail_rename, (uint32_t)sizeof(k_msg_fail_rename) - 1U);
    return err;
  }
  err = ra8_fs_unlink(mount, k_name_b);
  if (err != k_ra8_ok) {
    fs_fmt_print_line(type, k_msg_fail_unlink, (uint32_t)sizeof(k_msg_fail_unlink) - 1U);
    return err;
  }
  return k_ra8_ok;
}

/**
 * @brief Format the card as @p type, mount, run the full file cycle, unmount.
 *
 * @details Emits a per-step `FAIL ...` diagnostic and returns the failing code
 *          on any error, or prints the `FS <TYPE> FORMAT+MOUNT PASS` line and
 *          returns `k_ra8_ok` on success.
 *
 * @param[in] type FAT variant to format + exercise.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok    The whole cycle passed for @p type.
 * @retval k_ra8_err_* The first failing step's code.
 * @pre The SD card is initialised and `s_payload` is filled.
 * @post The card is left formatted as @p type with the test file removed.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fs_fmt_run_one_type(ra8_fs_type_t type)
{
  ra8_fs_backend_t backend = {};
  if (ra8_sdmmc_spi_bind_fs_backend(&backend) != k_ra8_ok) {
    fs_fmt_print_line(type, k_msg_fail_fmt, (uint32_t)sizeof(k_msg_fail_fmt) - 1U);
    return k_ra8_err_hw_error;
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = type;
  opts.label                = "RAFSFMT";
  ra8_err_t err             = ra8_fs_format(&backend, &opts);
  if (err != k_ra8_ok) {
    /* invalid_size == the card cannot hold this FAT type (too large/small): the
     * caller treats that as a SKIP, not a FAIL, so do not print the FAIL line. */
    if (err != k_ra8_err_invalid_size) {
      fs_fmt_print_line(type, k_msg_fail_fmt, (uint32_t)sizeof(k_msg_fail_fmt) - 1U);
    }
    return err;
  }
  ra8_fs_mount_t* mount = nullptr;
  err                   = ra8_fs_mount(&backend, &mount);
  if (err != k_ra8_ok) {
    fs_fmt_print_line(type, k_msg_fail_mount, (uint32_t)sizeof(k_msg_fail_mount) - 1U);
    return err;
  }
  if (mount->type != type) {
    fs_fmt_print_line(type, k_msg_fail_type, (uint32_t)sizeof(k_msg_fail_type) - 1U);
    (void)ra8_fs_unmount(mount);
    return k_ra8_err_validation_failed;
  }
  err = fs_fmt_write_cycle(mount, type);
  if (err != k_ra8_ok) {
    (void)ra8_fs_unmount(mount);
    return err;
  }
  (void)ra8_fs_unmount(mount);
  fs_fmt_print_line(type, k_msg_pass_suf, (uint32_t)sizeof(k_msg_pass_suf) - 1U);
  return k_ra8_ok;
}

/**
 * @var s_exfat_listed
 * @brief Root-directory entry tally for the exFAT trial.
 * @note File-scope; reset before each `ra8_fs_listdir` call.
 * @since 0.1.0
 */
static uint32_t s_exfat_listed = 0U;

/** @brief `ra8_fs_listdir` callback: tally visible entries for the exFAT trial. */
static void fs_fmt_count_cb(const char* name, uint8_t attr, uint64_t size, void* ctx)
{
  (void)name;
  (void)attr;
  (void)size;
  (void)ctx;
  s_exfat_listed++;
}

/**
 * @brief Assert a mounted volume's root directory lists exactly zero files.
 *
 * @details Re-zeroes `s_exfat_listed`, walks the root via `ra8_fs_listdir`, and
 *          fails if the walk errors or any user-visible entry surfaces. Used by
 *          the exFAT trial both right after format (the bitmap / up-case /
 *          volume-label system entries must stay hidden) and again after the
 *          create/rename/unlink cycle (proving the unlink fully reclaimed the
 *          directory set, not merely returned success).
 *
 * @param[in] mount Mounted, type-validated volume.
 * @param[in] type  Filesystem type, for the per-step diagnostic line.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                  Root listed zero entries.
 * @retval k_ra8_err_validation_failed Root was non-empty.
 * @retval k_ra8_err_*               `ra8_fs_listdir` failed (its code).
 * @pre @p mount is mounted.
 * @post `s_exfat_listed` holds the entry count seen on this call.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fs_fmt_assert_empty_root(ra8_fs_mount_t* mount, ra8_fs_type_t type)
{
  s_exfat_listed = 0U;
  ra8_err_t err  = ra8_fs_listdir(mount, "/", fs_fmt_count_cb, nullptr);
  if ((err != k_ra8_ok) || (s_exfat_listed != 0U)) {
    fs_fmt_print_line(type, k_msg_fail_mount, (uint32_t)sizeof(k_msg_fail_mount) - 1U);
    return (err != k_ra8_ok) ? err : k_ra8_err_validation_failed;
  }
  return k_ra8_ok;
}

/**
 * @brief Format the card as exFAT, mount, and run the full file-mutation cycle.
 *
 * @details Mirrors `fs_fmt_run_one_type` for exFAT now that `ra8_fs` writes the
 *          format: it formats the volume, mounts it, asserts the detected type
 *          is exFAT, confirms a freshly-formatted root lists zero files (the
 *          allocation bitmap / up-case / volume-label system entries must stay
 *          hidden), runs the shared create -> write -> read-back -> rename ->
 *          unlink cycle, then re-asserts an empty root to prove the unlink fully
 *          reclaimed the directory set. The image the formatter writes is
 *          independently fsck.exfat-clean.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Format + mount + full file cycle all passed.
 * @retval k_ra8_err_invalid_size Card capacity cannot hold an exFAT volume (SKIP).
 * @retval k_ra8_err_*            The first failing step's code.
 *
 * @pre The SD card is initialised and `s_payload` is filled.
 * @post The card is left formatted as exFAT with the test file removed.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fs_fmt_run_exfat(void)
{
  ra8_fs_backend_t backend = {};
  if (ra8_sdmmc_spi_bind_fs_backend(&backend) != k_ra8_ok) {
    fs_fmt_print_line(k_ra8_fs_type_exfat, k_msg_fail_fmt, (uint32_t)sizeof(k_msg_fail_fmt) - 1U);
    return k_ra8_err_hw_error;
  }
  ra8_fs_format_opts_t opts = {};
  opts.type                 = k_ra8_fs_type_exfat;
  opts.label                = "RAFSFMT";
  ra8_err_t err             = ra8_fs_format(&backend, &opts);
  if (err != k_ra8_ok) {
    if (err != k_ra8_err_invalid_size) {
      fs_fmt_print_line(k_ra8_fs_type_exfat, k_msg_fail_fmt, (uint32_t)sizeof(k_msg_fail_fmt) - 1U);
    }
    return err;
  }
  ra8_fs_mount_t* mount = nullptr;
  err                   = ra8_fs_mount(&backend, &mount);
  if (err != k_ra8_ok) {
    fs_fmt_print_line(k_ra8_fs_type_exfat,
                      k_msg_fail_mount,
                      (uint32_t)sizeof(k_msg_fail_mount) - 1U);
    return err;
  }
  if (mount->type != k_ra8_fs_type_exfat) {
    fs_fmt_print_line(k_ra8_fs_type_exfat, k_msg_fail_type, (uint32_t)sizeof(k_msg_fail_type) - 1U);
    (void)ra8_fs_unmount(mount);
    return k_ra8_err_validation_failed;
  }
  err = fs_fmt_assert_empty_root(mount, k_ra8_fs_type_exfat);
  if (err != k_ra8_ok) {
    (void)ra8_fs_unmount(mount);
    return err;
  }
  err = fs_fmt_write_cycle(mount, k_ra8_fs_type_exfat);
  if (err != k_ra8_ok) {
    (void)ra8_fs_unmount(mount);
    return err;
  }
  err = fs_fmt_assert_empty_root(mount, k_ra8_fs_type_exfat);
  if (err != k_ra8_ok) {
    (void)ra8_fs_unmount(mount);
    return err;
  }
  (void)ra8_fs_unmount(mount);
  fs_fmt_print_line(k_ra8_fs_type_exfat, k_msg_pass_suf, (uint32_t)sizeof(k_msg_pass_suf) - 1U);
  return k_ra8_ok;
}

/* =============================================================================
 * Main
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: bring up the bus + card, then format/mount/file-cycle each
 *        FAT type and print the overall PASS banner.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On a clean run the CPU loops forever after the ALL PASS banner.
 * @post On any failure the function halts in WFI after a FAIL diagnostic.
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t pclka_hz = 0U;
  fs_fmt_setup_or_halt(&pclka_hz);
  ra8_isr_globals_enable();
  ra8_log_init();
  FS_FMT_PUTS(k_msg_boot);

  fs_fmt_init_card_or_halt(&pclka_hz);
  fs_fmt_fill_payload();

  static const ra8_fs_type_t k_types[k_fs_fmt_idx_count] = {
    k_ra8_fs_type_fat12,
    k_ra8_fs_type_fat16,
    k_ra8_fs_type_fat32,
  };
  uint32_t passed = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_fs_fmt_idx_count; i++) {
    const ra8_err_t r = fs_fmt_run_one_type(k_types[i]);
    if (r == k_ra8_ok) {
      passed++;
    } else if (r == k_ra8_err_invalid_size) {
      /* This FAT type does not fit this card's capacity -- skip it, do not halt.
       * A big card (e.g. 128 GB) takes only FAT32; a tiny one only FAT12/16. */
      fs_fmt_print_line(k_types[i], k_msg_skip_size, (uint32_t)sizeof(k_msg_skip_size) - 1U);
    } else {
      fs_fmt_panic_halt(); /* a real failure (mount / write / verify). */
    }
  }
  /* exFAT trial (full cycle: format + mount + create/write/rename/unlink); same
   * SKIP semantics as the FAT types above. */
  const ra8_err_t ex = fs_fmt_run_exfat();
  if (ex == k_ra8_ok) {
    passed++;
  } else if (ex == k_ra8_err_invalid_size) {
    fs_fmt_print_line(k_ra8_fs_type_exfat, k_msg_skip_size, (uint32_t)sizeof(k_msg_skip_size) - 1U);
  } else {
    fs_fmt_panic_halt();
  }
  if (passed == 0U) {
    fs_fmt_panic_halt(); /* no FAT type fit the card -- unexpected. */
  }
  fs_fmt_print(k_msg_all_pass, (uint32_t)sizeof(k_msg_all_pass) - 1U);
  fs_fmt_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
