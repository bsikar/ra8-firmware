/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_pending/manual/usb_msc_sdcard/main.c
 * @brief USB MSC device serving the LIVE full-capacity Pmod2 SD card (R/W)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exposes the Pmod2 SD-over-SPI card as a real, WRITABLE USB drive: plug
 * the board's USB-FS receptacle (J11) into a computer and the card mounts
 * with its actual filesystem and full CSD-derived capacity -- copy a book
 * on, eject, done. This is the ingestion transport for the e-reader
 * (drag-a-file UX): no card pulling, no snapshot volume, no synthesized
 * FAT image anywhere.
 *
 *  - AT BOOT the SCI0 Simple-SPI transport comes up via
 *    ``ra8_sdmmc_spi_transport_sci``, ``ra8_sdmmc_spi_init`` enumerates the
 *    card, and the CSD capacity is latched into
 *    ::s_usb_msc_sdcard_blocks for the MSC LUN geometry. No card -> the
 *    FAIL banner prints and the device never attaches (an empty drive is
 *    worse than none).
 *  - USBFS (J11) = DEVICE: a ThreadX + USBX Mass-Storage class exposes
 *    ONE writable logical unit spanning the whole card. media-read runs
 *    each SCSI READ(10) as one CMD18 multi-block streak
 *    (``ra8_sdmmc_spi_read_blocks``); media-write runs each WRITE(10)
 *    chunk as one CMD25 multi-block streak
 *    (``ra8_sdmmc_spi_write_blocks``). IRQ-driven through the
 *    `port/usbx/ux_dcd_ra8_usb` bridge.
 *
 * SINGLE OWNER WARNING: while a host has this drive mounted, the HOST
 * owns the card. The firmware must not mount or read the card itself
 * concurrently (no shelf scan, no ra8_fs mount) -- the only card user
 * after boot is the USBX storage-class thread. Concurrent access would
 * interleave SD commands mid-transaction and corrupt the filesystem.
 *
 * Verdicts stream over SCI8 (J-Link OB CDC console, 115200) and are
 * mirrored in J-Link-readable probes (``s_dbg_*``).
 *
 * ## Pinout
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role),
 * P8_14 D+, P8_15 D- (PSEL usb_fs). Console: PD_02/PD_03 SCI8. microSD:
 * Pmod2 SCI0 Simple-SPI (SCK/CIPO/COPI PSEL sci_async, CS GPIO idle
 * high).
 *
 * @author Brighton Sikarskie
 * @date 2026-07-08
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_sdmmc_spi.h"
#include "ra8_time.h"
#include "usb_msc_sdcard_steps.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_dcd_ra8_usb.h"

/* Strong SysTick override: route the tick into BOTH the ra8_time millisecond
 * counter (for ra8_delay_ms and the SD driver's timeouts) AND ThreadX's
 * timer; the 1 ms pulse also recovers the DCD's storm-guard mask. */

extern void _tx_timer_interrupt(void);

/**
 * @var s_tx_kernel_up
 * @brief Set in ::tx_application_define; gates ThreadX tick delivery.
 * @details main() starts SysTick before tx_kernel_enter and the SD card
 *          bring-up window is long (the ACMD41 ready-poll takes ms), so
 *          the tick fires pre-kernel; feeding _tx_timer_interrupt into
 *          ThreadX's zeroed timer state bus-faults. Gate it until the
 *          kernel runs.
 * @note Single-writer (tx_application_define); read by the ISR.
 * @warning Do not set manually -- the kernel must own its timer state.
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

/** @brief USBFS VBUS sense pin (P4_07, PSEL = usb_fs). */
static const ra8_port_pin_t k_sdmsc_pin_fs_vbus = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra8_port_pin_t k_sdmsc_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14). */
static const ra8_port_pin_t k_sdmsc_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_sdmsc_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/* -------------------------------------------------------------------------- */
/* J-Link probes + the cross-TU capacity latch */
/* -------------------------------------------------------------------------- */

/** @brief SD bring-up result: 0 = card enumerated OK, else the error code. */
static volatile uint32_t s_dbg_sd_err;

/**
 * @var s_usb_msc_sdcard_blocks
 * @brief Live SD card capacity in 512-byte blocks (0 = no card).
 * @details Stamped here once during the pre-kernel SD bring-up from
 *          ``ra8_sdmmc_spi_get_capacity`` (CSD-derived) and consumed by the
 *          worker + media callbacks in ``usb_msc_sdcard_steps.c``.
 * @note Single-writer at boot; read-only afterwards.
 * @warning Do not modify after the USBX class registers -- the LUN
 *          geometry is latched from this value.
 * @since 0.1.0
 */
volatile uint32_t s_usb_msc_sdcard_blocks;

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
static void sdmsc_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Print "usb_msc_sdcard: card <N> blocks (<M> MiB)" at boot.
 *
 * @details Records what the LUN is about to expose; the MiB figure is the
 * block count divided by ::k_sdmsc_blocks_per_mib.
 *
 * @param[in] blocks CSD-derived capacity in 512-byte blocks.
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The banner is queued.
 *
 * @pre SCI8 init already ran.
 * @pre @p blocks is the live card capacity (non-zero).
 * @post One ASCII banner line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t sdmsc_print_card_banner(uint32_t blocks)
{
  ra8_err_t err = sdmsc_print("usb_msc_sdcard: card ");
  if (err != k_ra8_ok) {
    return err;
  }
  err = sdmsc_print_dec(blocks);
  if (err != k_ra8_ok) {
    return err;
  }
  err = sdmsc_print(" blocks (");
  if (err != k_ra8_ok) {
    return err;
  }
  err = sdmsc_print_dec(blocks / (uint32_t)k_sdmsc_blocks_per_mib);
  if (err != k_ra8_ok) {
    return err;
  }
  return sdmsc_print(" MiB)\r\n");
}

/**
 * @brief Bring the live SD card up and latch its full CSD capacity.
 *
 * @details Routes the Pmod2 pins + opens SCI0 Simple-SPI through the
 * ``ra8_sdmmc_spi_transport_sci`` factory, runs the SD identification
 * sequence (``ra8_sdmmc_spi_init``: CMD0/CMD8/ACMD41/CMD58/CMD9, then the
 * clock escalates to data speed), and stores the 512-byte block count in
 * ::s_usb_msc_sdcard_blocks. Prints the card banner on success and the
 * FAIL diagnostic on any failing step. Never writes the card here.
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok  Card enumerated; the capacity latch is non-zero.
 * @retval (other)  First failing transport / identification step.
 *
 * @pre ``ra8_cgc_init`` ran and @p pclka_hz is the live PCLKA rate.
 * @pre A card is seated in the Pmod2 slot (else this reports the error).
 * @post On success ::s_usb_msc_sdcard_blocks holds the card capacity.
 * @post On failure ::s_dbg_sd_err records the error and the latch stays 0.
 *
 * @param[in] pclka_hz Live PCLKA rate (Hz) feeding the SCI baud divider.
 *
 * @note Boot context; runs once before the kernel starts.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t sdmsc_card_up(uint32_t pclka_hz)
{
  const ra8_sdmmc_spi_sci_pins_t pins = {
    .sck  = (ra8_port_pin_t)k_ra8_board_pmod2_spi_sck,
    .cipo = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo,
    .copi = (ra8_port_pin_t)k_ra8_board_pmod2_spi_copi,
    .cs   = (ra8_port_pin_t)k_ra8_board_pmod2_spi_cs,
  };
  ra8_sdmmc_spi_transport_t transport = {};

  ra8_err_t err =
    ra8_sdmmc_spi_transport_sci((uint8_t)k_sdmsc_sd_spi_channel, pclka_hz, &pins, &transport);
  if (err != k_ra8_ok) {
    s_dbg_sd_err = (uint32_t)err;
    (void)sdmsc_print_fail("sd transport", err);
    return err;
  }
  err = ra8_sdmmc_spi_init(&transport);
  if (err != k_ra8_ok) {
    s_dbg_sd_err = (uint32_t)err;
    (void)sdmsc_print_fail("sd init (card seated?)", err);
    return err;
  }
  uint32_t blocks = 0U;
  err             = ra8_sdmmc_spi_get_capacity(&blocks);
  if (err != k_ra8_ok) {
    s_dbg_sd_err = (uint32_t)err;
    (void)sdmsc_print_fail("sd capacity", err);
    return err;
  }
  s_usb_msc_sdcard_blocks = blocks;
  s_dbg_sd_err            = 0U;
  return sdmsc_print_card_banner(blocks);
}

/**
 * @brief Route the four USB-FS pins for the device role.
 *
 * @details P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (else peripheral
 * routing forces host VBUSEN and blocks device enum), P8_14/P8_15 data.
 *
 * @pre IOPORT module is reachable.
 * @pre Called once from ::sdmsc_setup_or_halt.
 * @post FS pins carry the device role.
 * @post VBUSEN idles LOW.
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void sdmsc_route_usb_or_halt(void)
{
  if (ra8_pfs_route_peripheral(k_sdmsc_pin_fs_vbus, k_ra8_psel_usb_fs, "sdmsc.fs_vbus") !=
      k_ra8_ok) {
    sdmsc_panic_halt();
  }
  if (ra8_gpio_output_init(k_sdmsc_pin_fs_vbusen, k_ra8_level_low) != k_ra8_ok) {
    sdmsc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_sdmsc_pin_fs_dp, k_ra8_psel_usb_fs, "sdmsc.fs_dp") != k_ra8_ok) {
    sdmsc_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_sdmsc_pin_fs_dm, k_ra8_psel_usb_fs, "sdmsc.fs_dm") != k_ra8_ok) {
    sdmsc_panic_halt();
  }
}

/**
 * @brief Bring CGC + USBFS clock + SysTick + SCI8 + LEDs + SD + pins up.
 *
 * @details USBFS needs the 48 MHz PLL2 reference. SCI8 is the J-Link OB
 * CDC console at 115200. The SD card is enumerated pre-kernel (its
 * capacity sizes the MSC LUN); a missing card prints FAIL and leaves the
 * capacity latch 0 -- the worker then refuses to attach, but the board
 * stays alive for diagnostics.
 *
 * @pre Reset_Handler finished C runtime init.
 * @pre SystemInit has run.
 * @post Console works; USB-FS pins and clock are live.
 * @post ::s_usb_msc_sdcard_blocks reflects the card probe.
 *
 * @note Panic-halts on infrastructure failure; called once from main.
 * @since 0.1.0
 */
static void sdmsc_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    sdmsc_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    sdmsc_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    sdmsc_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, &pclka_hz) != k_ra8_ok) {
    sdmsc_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    sdmsc_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_sdmsc_baud) != k_ra8_ok) {
    sdmsc_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    sdmsc_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    sdmsc_panic_halt();
  }
  /* Enumerate the card and latch its capacity before the kernel starts.
   * On failure s_dbg_sd_err records it, the FAIL banner is on the wire,
   * and the worker refuses to attach -- the board does not hang. */
  (void)sdmsc_card_up(pclka_hz);
  sdmsc_route_usb_or_halt();
}

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* ThreadX worker storage + kernel entry */
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
static UCHAR s_device_stack[k_sdmsc_thread_stack];

/**
 * @brief ThreadX application-define hook. Spawns the device worker.
 *
 * @details One worker at priority 8 (above the USBX class threads it
 * creates). Sets ::s_tx_kernel_up so SysTick may feed ThreadX from here
 * on.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @pre Static stacks are reserved at file scope.
 * @post One auto-start worker thread is queued.
 * @post ``s_tx_kernel_up`` is true.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_thread_create(&s_device_thread,
                         "sdmsc_device",
                         sdmsc_device_worker,
                         0UL,
                         s_device_stack,
                         k_sdmsc_thread_stack,
                         (UINT)k_sdmsc_dev_priority,
                         (UINT)k_sdmsc_dev_priority,
                         TX_NO_TIME_SLICE,
                         TX_AUTO_START);
}
#endif /* !RA8_OFF_TARGET */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details The USB clock, pins, console, and the SD card all come up
 * before the kernel so the worker only deals with USB stack bring-up.
 *
 * @return Never returns (``tx_kernel_enter`` is __noreturn).
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR, FPU, priority grouping.
 * @post On clean entry the CPU stays in tx_kernel_enter forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
int32_t main(void)
{
  sdmsc_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  sdmsc_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
