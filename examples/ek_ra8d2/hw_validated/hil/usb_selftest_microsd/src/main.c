/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_selftest_microsd/src/main.c
 * @brief USB self-loop: HS host reads the microSD card as a read-only MSC
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exposes the onboard Pmod2 microSD card as a USB drive and verifies it
 * against itself on-chip -- no PC in the loop. The two USB ports are
 * cabled to EACH OTHER and one firmware image runs both USB stacks plus
 * the SCI0 Simple-SPI SD driver:
 *
 *  - AT BOOT the device snapshots SD LBA 0..63 (the FAT/exFAT boot
 *    region) into a RAM buffer with ``ra8_sdmmc_spi_read_block`` and
 *    confirms the ``0x55AA`` boot signature. The card is NEVER written
 *    (strictly read-only) so the user's filesystem is untouched.
 *  - USBFS (J11) = DEVICE: a ThreadX + USBX Mass-Storage class exposes
 *    that 64-sector window as a single READ-ONLY logical unit
 *    (``GET_MAX_LUN`` = 0); media-read serves the boot snapshot (no live
 *    card access on the class thread). IRQ-driven through the
 *    `port/usbx/ux_dcd_ra8_usb` bridge.
 *  - USBHS (J7) = HOST: the first-party polled host MSC stack
 *    (`ra8_usb_hmsc`) enumerates the device, READ_CAPACITY + a full
 *    raw READ(10) sweep, and byte-checks every sector against the SD
 *    snapshot -- proving the SD read path AND the USB transport deliver
 *    the card's real content intact, end to end on chip.
 *
 * No filesystem parsing is involved (raw SCSI READ(10)); this is a pure
 * read-path test that needs no host WRITE support and no SD writes.
 *
 * The link runs at 12 Mbps (FS device ceiling; HS host serves an FS
 * downstream device).
 *
 * Verdicts stream over SCI8 (J-Link OB CDC console, 115200) and are
 * mirrored in J-Link-readable probes (``s_dbg_*``).
 *
 * ## Pinout
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role),
 * P8_14 D+, P8_15 D- (PSEL usb_fs). HS host: SW4-8 to Host via the U15
 * expander, PD07 HIGH (U18 supplies J7 VBUS), P4_08 USBHS_VBUS
 * (PSEL usb_hs). Console: PD_02/PD_03 SCI8. microSD: Pmod2 SCI0
 * Simple-SPI (SCK/CIPO/COPI PSEL sci_async, CS GPIO idle high).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sci_spi.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_spi.h"
#include "ra8_time.h"
#include "usb_selftest_microsd_steps.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_dcd_ra8_usb.h"

/* Strong SysTick override: route the tick into BOTH the ra8_time millisecond
 * counter (for ra8_delay_ms and the polled host stack's timeouts) AND
 * ThreadX's timer; the 1 ms pulse also recovers the DCD's storm-guard mask. */

extern void _tx_timer_interrupt(void);

/**
 * @var s_tx_kernel_up
 * @brief Set in ::tx_application_define; gates ThreadX tick delivery.
 * @details main() starts SysTick before tx_kernel_enter and the setup
 *          window is long (U15 expander I2C blocks for ms), so the tick
 *          fires pre-kernel; feeding _tx_timer_interrupt into ThreadX's
 *          zeroed timer state bus-faults. Gate it until the kernel runs.
 * @since 0.1.0
 */
static volatile bool s_tx_kernel_up = false;

void SysTick_Handler(void);
void SysTick_Handler(void)
{
  ra8_time_on_tick();
  if (s_tx_kernel_up) {
    _tx_timer_interrupt();
    ux_dcd_ra8_usb_irq_reenable();
  }
}
#endif

/* -------------------------------------------------------------------------- */
/* Pinout (FSP-aligned, EK-RA8D2 v1 User's Manual) */
/* -------------------------------------------------------------------------- */

/** @brief USBFS VBUS sense pin (P4_07, PSEL = 0x13). */
static const ra8_port_pin_t k_microsd_pin_fs_vbus = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra8_port_pin_t k_microsd_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14). */
static const ra8_port_pin_t k_microsd_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_microsd_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_microsd_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra8_port_pin_t k_microsd_pin_hs_pwr = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

/** @brief Pmod2 microSD SPI clock (SCI0, Simple-SPI). */
static const ra8_port_pin_t k_microsd_pin_sd_sck = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck;
/** @brief Pmod2 microSD SPI CIPO (card -> MCU). */
static const ra8_port_pin_t k_microsd_pin_sd_cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo;
/** @brief Pmod2 microSD SPI COPI (MCU -> card). */
static const ra8_port_pin_t k_microsd_pin_sd_copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi;
/** @brief Pmod2 microSD chip-select (GPIO, idle high). */
static const ra8_port_pin_t k_microsd_pin_sd_cs = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/*
 * The shared compile-time tunables, hex/mask formatter sizing, LUN geometry,
 * host/device progress phases, SCSI sense triples, and USB LANGID bytes all
 * live as typed enums in "usb_selftest_microsd_steps.h" so main.c and both
 * helper siblings observe one definition.
 */

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* ThreadX workers + USBX pool storage */
/* -------------------------------------------------------------------------- */

/**
 * @var s_device_thread
 * @brief ThreadX TCB for the USBX device-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_device_thread;

/**
 * @var s_device_stack
 * @brief Stack backing storage for ::s_device_thread.
 * @since 0.1.0
 */
static UCHAR s_device_stack[k_microsd_thread_stack];

/**
 * @var s_host_thread
 * @brief ThreadX TCB for the host-side worker thread.
 * @note Single-writer (worker only).
 * @since 0.1.0
 */
static TX_THREAD s_host_thread;

/**
 * @var s_host_stack
 * @brief Stack backing storage for ::s_host_thread.
 * @since 0.1.0
 */
static UCHAR s_host_stack[k_microsd_host_stack];

/* The USBX pool, SCSI INQUIRY strings, USB descriptor frameworks, and the
 * device/host-owned J-Link progress probes live in
 * "usb_selftest_microsd_steps.c"; only the SD bring-up probes and the two
 * cross-TU snapshot symbols are owned here. */

/* -------------------------------------------------------------------------- */
/* J-Link probes (SD bring-up owned) */
/* -------------------------------------------------------------------------- */

/** @brief SD bring-up result: 0 = card read into snapshot OK, else error. */
static volatile uint32_t s_dbg_sd_err = (uint32_t)k_microsd_no_mismatch;
/** @brief SD card capacity in 512-B blocks (sanity probe). */
static volatile uint32_t s_dbg_sd_blocks;

/**
 * @var s_usb_selftest_microsd_dbg_bootsig
 * @brief 1 = LBA0 carries the 0x55AA FAT/exFAT boot signature.
 * @details Stamped here at SD bring-up and read by the host-side verify
 *          ladder in "usb_selftest_microsd_steps.c".
 * @note Single-writer at boot; read-only afterwards.
 * @since 0.1.0
 */
volatile uint32_t s_usb_selftest_microsd_dbg_bootsig;

/** @brief PCLKA in Hz, captured at setup for the SD SPI clock adapter. */
static uint32_t s_pclka_hz = 0U;

/**
 * @var s_usb_selftest_microsd_sd_snapshot
 * @brief Read-only RAM snapshot of SD LBA 0..63, taken once at boot.
 * @details Populated here before the kernel starts; consumed by both the
 *          device-side media-read callback and the host-side verify ladder
 *          in "usb_selftest_microsd_steps.c".
 * @note Single-writer at boot; read-only afterwards.
 * @since 0.1.0
 */
UCHAR s_usb_selftest_microsd_sd_snapshot[k_microsd_snap_bytes];

/* -------------------------------------------------------------------------- */
/* microSD over SCI0 Simple-SPI -- READ-ONLY backing snapshot */
/* -------------------------------------------------------------------------- */

/* cppcheck-suppress-begin [constParameterCallback] -- these three hooks
 * are bound to the ra8_sdmmc_spi_transport_t vtable, whose signatures take
 * a non-const void* ctx; const-qualifying would break the type. */

/**
 * @brief SD transport hook: program the Simple-SPI bit clock.
 *
 * @param[in] ctx PCLKA-Hz pointer (::s_pclka_hz).
 * @param[in] hz  Requested SCLK frequency in Hz.
 *
 * @return ra8_err_t from ::ra8_sci_spi_set_clock.
 * @retval k_ra8_ok Clock applied.
 *
 * @pre ::ra8_sci_spi_init has run for the SD channel.
 * @pre @p ctx is non-null.
 * @post SCI0 Simple-SPI baud reflects @p hz.
 * @post No other state changes.
 *
 * @note Boot context; not ISR-safe.
 * @since 0.1.0
 */
static ra8_err_t microsd_spi_set_clock(void* ctx, uint32_t hz)
{
  const uint32_t pclka = *(const uint32_t*)ctx;
  return ra8_sci_spi_set_clock((uint8_t)k_microsd_sd_spi_channel, hz, pclka);
}

/**
 * @brief SD transport hook: drive the chip-select line.
 *
 * @param[in] ctx Unused.
 * @param[in] asserted true = select (CS low), false = deselect (CS high).
 *
 * @return ra8_err_t from ::ra8_gpio_write.
 * @retval k_ra8_ok CS level updated.
 *
 * @pre CS was configured as a GPIO output.
 * @pre The pin maps to the Pmod2 microSD CS.
 * @post CS reflects @p asserted (active-low).
 * @post No other state changes.
 *
 * @note SD SPI framing needs CS held across the whole command.
 * @since 0.1.0
 */
static ra8_err_t microsd_spi_cs(void* ctx, bool asserted)
{
  (void)ctx;
  return ra8_gpio_write(k_microsd_pin_sd_cs, asserted ? k_ra8_level_low : k_ra8_level_high);
}

/**
 * @brief SD transport hook: full-duplex SPI byte exchange.
 *
 * @param[in]  ctx Unused.
 * @param[in]  tx  Bytes to clock out (may be null for read-only).
 * @param[out] rx  Bytes clocked in (may be null for write-only).
 * @param[in]  len Transfer length in bytes.
 *
 * @return ra8_err_t from ::ra8_sci_spi_xfer.
 * @retval k_ra8_ok Transfer complete.
 *
 * @pre ::ra8_sci_spi_init has run for the SD channel.
 * @pre CS is asserted by the caller around the framed command.
 * @post @p rx holds the clocked-in bytes when non-null.
 * @post No other state changes.
 *
 * @note Boot context; not ISR-safe.
 * @since 0.1.0
 */
static ra8_err_t microsd_spi_xfer(void* ctx, const uint8_t* tx, uint8_t* rx, uint32_t len)
{
  (void)ctx;
  return ra8_sci_spi_xfer((uint8_t)k_microsd_sd_spi_channel, tx, rx, len);
}
/* cppcheck-suppress-end [constParameterCallback] */

/**
 * @brief Route Pmod2 SCI0 pins and open the Simple-SPI controller for the card.
 *
 * @details SCK/CIPO/COPI go to the SCI async PSEL; CS is a plain GPIO held
 * across each multi-byte SD command (the SCI hardware CS pulses per word
 * and cannot frame an SD transaction), idle high. Boots SPI at the slow
 * init clock; ::ra8_sdmmc_spi_init negotiates up afterwards.
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok SCI0 Simple-SPI controller is up.
 * @retval (other) A pin route, GPIO, or SPI-init step failed.
 *
 * @pre ::s_pclka_hz holds the live PCLKA frequency.
 * @pre The Pmod2 microSD module is seated.
 * @post SCI0 Simple-SPI is initialized; CS idles high.
 * @post Pmod2 SPI pins carry the SCI peripheral function.
 *
 * @note Boot context; single-threaded.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t microsd_sd_spi_open(void)
{
  ra8_err_t err =
    ra8_pfs_route_peripheral(k_microsd_pin_sd_sck, k_ra8_psel_sci_async, "microsd.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_microsd_pin_sd_cipo, k_ra8_psel_sci_async, "microsd.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_microsd_pin_sd_copi, k_ra8_psel_sci_async, "microsd.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gpio_output_init(k_microsd_pin_sd_cs, k_ra8_level_high);
  if (err != k_ra8_ok) {
    return err;
  }
  const ra8_sci_spi_cfg_t spi_cfg = {
    .baud_hz   = (uint32_t)k_ra8_sdmmc_spi_clock_init_hz,
    .pclk_hz   = s_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  return ra8_sci_spi_init((uint8_t)k_microsd_sd_spi_channel, &spi_cfg);
}

/**
 * @brief Bring the card up and snapshot LBA 0..63 into RAM (read-only).
 *
 * @details Opens the SPI controller, inits ::ra8_sdmmc_spi (card enumeration),
 * records capacity, then reads the exposed window into
 * ::s_usb_selftest_microsd_sd_snapshot with ::ra8_sdmmc_spi_read_block. NEVER
 * writes the card. Stamps the SD probes and flags the FAT/exFAT 0x55AA boot
 * signature on LBA 0.
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok Snapshot captured; the snapshot buffer is valid.
 * @retval (other) First failing bring-up or block-read step.
 *
 * @pre ::s_pclka_hz is set; the console is up for diagnostics.
 * @pre A card is seated in the Pmod2 slot.
 * @post ::s_usb_selftest_microsd_sd_snapshot holds LBA 0..63; SD probes reflect
 *       the outcome.
 * @post The card is untouched (no writes).
 *
 * @note Boot context; runs once before the kernel starts.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t microsd_sd_snapshot(void)
{
  ra8_err_t err = microsd_sd_spi_open();
  if (err != k_ra8_ok) {
    s_dbg_sd_err = (uint32_t)err;
    return err;
  }
  const ra8_sdmmc_spi_transport_t transport = {
    .set_clock = microsd_spi_set_clock,
    .cs        = microsd_spi_cs,
    .xfer      = microsd_spi_xfer,
    .ctx       = &s_pclka_hz,
  };
  err = ra8_sdmmc_spi_init(&transport);
  if (err != k_ra8_ok) {
    s_dbg_sd_err = (uint32_t)err;
    return err;
  }
  uint32_t blocks = 0U;
  (void)ra8_sdmmc_spi_get_capacity(&blocks);
  s_dbg_sd_blocks = blocks;
  for (uint32_t lba = 0U; lba < (uint32_t)k_microsd_sectors; lba++) {
    err = ra8_sdmmc_spi_read_block(
      lba,
      &s_usb_selftest_microsd_sd_snapshot[lba * (uint32_t)k_microsd_block_size]);
    if (err != k_ra8_ok) {
      s_dbg_sd_err = (uint32_t)err;
      return err;
    }
  }
  const bool sig =
    (s_usb_selftest_microsd_sd_snapshot[k_microsd_bootsig_off] == (UCHAR)k_microsd_bootsig_lo) &&
    (s_usb_selftest_microsd_sd_snapshot[k_microsd_bootsig_off + 1U] == (UCHAR)k_microsd_bootsig_hi);
  s_usb_selftest_microsd_dbg_bootsig = sig ? 1U : 0U;
  s_dbg_sd_err                       = 0U;
  return k_ra8_ok;
}

/**
 * @brief ThreadX application-define hook. Spawns both workers.
 *
 * @details Device worker at priority 8, host worker at 24 (below the
 * USBX class threads). Sets ::s_tx_kernel_up so SysTick may feed
 * ThreadX from here on.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @pre Static stacks are reserved at file scope.
 * @post Two auto-start worker threads are queued.
 * @post ``s_tx_kernel_up`` is true.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  static CHAR s_device_thread_name[] = "microsd_device";
  static CHAR s_host_thread_name[]   = "microsd_host";

  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_thread_create(&s_device_thread,
                         s_device_thread_name,
                         microsd_device_worker,
                         0UL,
                         s_device_stack,
                         k_microsd_thread_stack,
                         (UINT)k_microsd_dev_priority,
                         (UINT)k_microsd_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
  (void)tx_thread_create(&s_host_thread,
                         s_host_thread_name,
                         microsd_host_worker,
                         0UL,
                         s_host_stack,
                         k_microsd_host_stack,
                         (UINT)k_microsd_host_priority,
                         (UINT)k_microsd_host_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_OFF_TARGET */

/* -------------------------------------------------------------------------- */
/* Startup */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @details Last-resort stop; only a debugger or reset recovers.
 *
 * @pre Called only after a fatal boot error.
 * @pre Interrupts may be in any state.
 * @post CPU is parked.
 * @post No further code runs.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void microsd_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route both ports' pins: FS as device, HS as host.
 *
 * @details FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (else
 * peripheral routing forces host VBUSEN and blocks device enum),
 * P8_14/P8_15 data. HS host: SW4-8 to Host via the U15 expander, PD07
 * HIGH (U18 supplies J7), P4_08 VBUS sense.
 *
 * @pre IOPORT and the U15 expander are reachable.
 * @pre Called once from ::microsd_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role.
 * @post PD07 is HIGH (J7 powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void microsd_route_usb_or_halt(void)
{
  if (ra8_pfs_route_peripheral(k_microsd_pin_fs_vbus, k_ra8_psel_usb_fs, "microsd.fs_vbus") !=
      k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_gpio_output_init(k_microsd_pin_fs_vbusen, k_ra8_level_low) != k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_microsd_pin_fs_dp, k_ra8_psel_usb_fs, "microsd.fs_dp") !=
      k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_microsd_pin_fs_dm, k_ra8_psel_usb_fs, "microsd.fs_dm") !=
      k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_board_io_expander_set_usbhs_host_mode() != k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_gpio_output_init(k_microsd_pin_hs_pwr, k_ra8_level_high) != k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_microsd_pin_hs_vbus, k_ra8_psel_usb_hs, "microsd.hs_vbus") !=
      k_ra8_ok) {
    microsd_panic_halt();
  }
}

/**
 * @brief Bring CGC + both USB clocks + SysTick + SCI8 + LEDs + pins up.
 *
 * @details USBFS needs the 48 MHz PLL2 reference; USBHS needs its UTMI
 * PLL. SCI8 is the J-Link OB CDC console at 115200.
 *
 * @pre Reset_Handler finished C runtime init.
 * @pre SystemInit has run.
 * @post Console works; both USB ports' pins and clocks are live.
 * @post LED1/LED2 are initialized.
 *
 * @note Panic-halts on any failure; called once from main.
 * @since 0.1.0
 */
static void microsd_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) {
    microsd_panic_halt();
  }
  s_pclka_hz = pclka_hz;
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_microsd_baud) != k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    microsd_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    microsd_panic_halt();
  }
  /* Snapshot SD LBA 0..63 into RAM (read-only) before the kernel starts.
   * On failure s_dbg_sd_err records it and the host verifier reports the
   * missing boot signature -- the board does not hang. */
  (void)microsd_sd_snapshot();
  microsd_route_usb_or_halt();
}

/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details Both USB controllers' clocks and pins come up before the
 * kernel so the workers only deal with stack bring-up.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU, priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
void main(void)
{
  microsd_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  microsd_panic_halt();
}
