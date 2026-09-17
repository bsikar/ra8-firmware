/**
 * @file examples/ek_ra8d2/hw_validated/manual/usb_host_file_ops/src/main.c
 * @brief USB host MSC + ra8_fs file-operations exerciser for EK-RA8D2 (USB-HS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Manual HIL test that proves real file operations work end to end on a USB
 * thumb drive plugged into the J7 USB-HS jack: the board enumerates the stick
 * via ::ra8_usb_hmsc_enumerate, mounts its FAT/exFAT volume through `ra8_fs`
 * (block I/O bridged onto ::ra8_usb_hmsc_read10 / ::ra8_usb_hmsc_write10),
 * then runs a 9-step suite over the J-Link OB CDC console:
 *
 *  1. cleanup: best-effort unlink of leftover test files
 *  2. listdir of the root directory (every entry printed)
 *  3. write a new file `USBTEST.TXT` with a known payload
 *  4. open + read back + byte-compare the payload
 *  5. listdir must show `USBTEST.TXT`
 *  6. rename `USBTEST.TXT` -> `USBDONE.TXT`
 *  7. old name must be gone, new name must read back intact
 *  8. listdir must show `USBDONE.TXT` and not `USBTEST.TXT`
 *  9. unlink `USBDONE.TXT`; listdir must no longer show it
 *
 * Every step prints a pass/fail verdict; the run ends with
 * `ALL FILE OPS PASSED` and a solid LED1. The ladder retries every 5 s so a
 * freshly inserted drive is picked up automatically.
 *
 * Board specifics (EK-RA8D2 v1 UM):
 *  - PD07 HIGH = U18 supplies VBUS to J7 (Sec 6.2 p 34, 2 A budget).
 *  - SW4-8 must sit in the Host position; set in software through the
 *    U15 expander (::ra8_board_io_expander_set_usbhs_host_mode).
 *  - P4_08 is the only PFS-muxed USBHS pin (VBUS sense, PSEL 0x14);
 *    D+/D- are dedicated PHY package balls.
 *
 * @author Brighton Sikarskie
 * @date 2026-06-12
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_hmsc.h"
#include "usb_host_file_ops_steps.h"

/* =============================================================================
 * Pin assignments
 * =============================================================================
 */

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_fileops_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra8_port_pin_t k_fileops_pin_hs_pwr = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

/** @brief USBFS VBUS sense pin (P4_07, PSEL = 0x13). */
static const ra8_port_pin_t k_fileops_pin_fs_vbus = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;

/** @brief USBFS VBUSEN (P5_00): the USB function drives host VBUS. */
static const ra8_port_pin_t k_fileops_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14) -- PFS-muxed, unlike the dedicated HS balls. */
static const ra8_port_pin_t k_fileops_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_fileops_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/* =============================================================================
 * Panic stop
 * =============================================================================
 */

/**
 * @brief Halt forever in WFI -- panic stop.
 *
 * @pre Called only after a fatal boot error.
 * @pre Interrupts may be in any state.
 * @post CPU is parked; only a debugger or external reset wakes it.
 * @post No further code runs.
 * @note Never returns.
 * @since 0.1.0
 */
static void fileops_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/* =============================================================================
 * Bring-up + ladder
 * =============================================================================
 */

/**
 * @brief Route both USB ports' board pins for the host role. Panic-halts.
 *
 * @details HS (J7): SW4-8 to Host via the U15 expander, PD07 HIGH (U18
 * supplies the jack), P4_08 to USBHS_VBUS. FS: P4_07 (VBUS sense),
 * P5_00 (VBUSEN -- the controller drives host VBUS through it), and the
 * PFS-muxed data lines P8_14/P8_15.
 *
 * @pre IOPORT and the U15 expander are reachable.
 * @pre Called once from ::fileops_setup_or_halt.
 * @post Both ports' pins carry their USB peripheral functions.
 * @post PD07 is HIGH (J7 powered).
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void fileops_route_usb_or_halt(void)
{
  if (ra8_board_io_expander_set_usbhs_host_mode() != k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_gpio_output_init(k_fileops_pin_hs_pwr, k_ra8_level_high) != k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_fileops_pin_hs_vbus, k_ra8_psel_usb_hs, "fileops.hs_vbus") !=
      k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_fileops_pin_fs_vbus, k_ra8_psel_usb_fs, "fileops.fs_vbus") !=
      k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_fileops_pin_fs_vbusen, k_ra8_psel_usb_fs, "fileops.fs_vbusen") !=
      k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_fileops_pin_fs_dp, k_ra8_psel_usb_fs, "fileops.fs_dp") !=
      k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_fileops_pin_fs_dm, k_ra8_psel_usb_fs, "fileops.fs_dm") !=
      k_ra8_ok) {
    fileops_panic_halt();
  }
}

/**
 * @brief Bring CGC + both USB clocks + SysTick + SCI8 + LEDs + pins up.
 *        Panic-halts on any failure.
 *
 * @details Enables the USBHS 60 MHz PLL and the USBFS 48 MHz PLL2 clock
 * (the latter must run before MSTPB11 is released or UACT never sticks),
 * then routes both ports' pins. The controller itself is brought up per
 * retry cycle by the ladder (`ra8_usb_hmsc_init`), since the app
 * alternates between the HS and FS ports.
 *
 * @pre Reset_Handler has finished C runtime init.
 * @pre SystemInit has run.
 * @post Console prints work; both USB ports' pins and clocks are live.
 * @post LED1/LED2 are initialized.
 * @note Called exactly once from main.
 * @since 0.1.0
 */
static void fileops_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_fileops_baud) != k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    fileops_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    fileops_panic_halt();
  }
  fileops_route_usb_or_halt();
}

/**
 * @brief Map a USB speed to the printable port name.
 *
 * @param[in] speed Controller selector.
 * @return Static NUL-terminated name string.
 * @retval "USB-HS" For ::k_ra8_usb_speed_hs.
 * @pre None -- total over the two host speeds.
 * @pre @p speed is one of the two host-capable controllers.
 * @post No state changes.
 * @post Returned pointer references static storage.
 * @note Pure function.
 * @since 0.1.0
 */
static const char* fileops_port_name(ra8_usb_speed_t speed)
{
  if (speed == k_ra8_usb_speed_hs) {
    return "USB-HS";
  }
  return "USB-FS";
}

/**
 * @brief Print "device attached vid=0x.. pid=0x.. port=USB-xS".
 *
 * @param[in] device Enumerated device snapshot.
 * @param[in] speed  Controller the device answered on.
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok All chunks queued.
 * @pre SCI8 init already ran; @p device is non-NULL.
 * @pre The snapshot was filled by ::ra8_usb_hmsc_enumerate.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fileops_print_attach(const ra8_usb_hmsc_device_t* device,
                                                    ra8_usb_speed_t              speed)
{
  ra8_err_t err = fileops_print("ra8d2 fileops: device attached vid=0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print_hex((uint32_t)device->vendor_id, (uint8_t)k_fileops_hex_chars_u16);
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print(" pid=0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print_hex((uint32_t)device->product_id, (uint8_t)k_fileops_hex_chars_u16);
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print(" port=");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print(fileops_port_name(speed));
  if (err != k_ra8_ok) {
    return err;
  }
  return fileops_print("\r\n");
}

/**
 * @brief One full pass on one port: host up, enumerate, mount, run the
 *        9-step file-op suite.
 *
 * @details Brings the selected controller up (`ra8_usb_hmsc_init`), so the
 * caller can alternate between the HS and FS ports per retry cycle. On
 * any failure the controller is closed again before returning; the mount
 * handle is always released so the static mount-slot pool cannot leak.
 *
 * @param[in] speed Controller to drive this cycle (HS or FS port).
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The suite printed `ALL FILE OPS PASSED`.
 * @pre ::fileops_setup_or_halt completed.
 * @pre The USB drive is inserted in either host jack.
 * @post On success LED1 is on and all verdict lines are printed.
 * @post The volume's test files are removed again (suite is self-cleaning).
 * @note Blocking; bounded by the class-layer timeouts.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t fileops_run_ladder(ra8_usb_speed_t speed)
{
  ra8_err_t err = fileops_print("ra8d2 fileops: probing ");
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print(fileops_port_name(speed));
  if (err != k_ra8_ok) {
    return err;
  }
  err = fileops_print("\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_hmsc_init(speed);
  if (err != k_ra8_ok) {
    (void)fileops_print_fail("host init", err);
    return err;
  }

  ra8_usb_hmsc_device_t device = {};
  err                          = ra8_usb_hmsc_enumerate(&device);
  if (err != k_ra8_ok) {
    (void)ra8_usb_hmsc_close();
    return err;
  }
  err = fileops_print_attach(&device, speed);
  if (err != k_ra8_ok) {
    (void)ra8_usb_hmsc_close();
    return err;
  }

  ra8_fs_mount_t* mount = nullptr;
  err                   = fileops_mount_volume(&mount);
  if (err != k_ra8_ok) {
    fileops_probe_layout();
    (void)ra8_usb_hmsc_close();
    return err;
  }

  err = fileops_suite_create(mount);
  if (err == k_ra8_ok) {
    err = fileops_suite_mutate(mount);
  }
  (void)ra8_fs_unmount(mount);
  if (err != k_ra8_ok) {
    (void)ra8_usb_hmsc_close();
    return err;
  }

  err = fileops_print("ra8d2 fileops: ALL FILE OPS PASSED\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led1);
  return k_ra8_ok;
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

/**
 * @brief Application entry. Alternates between the HS and FS host ports
 *        and runs the file-op suite until it fully passes.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 * @post On a clean run every suite verdict line is printed, LED1 is lit,
 *       and the CPU parks in WFI.
 * @post On any HAL init failure the function halts in WFI.
 * @since 0.1.0
 */
void main(void)
{
  fileops_setup_or_halt();

  ra8_isr_globals_enable();

  if (fileops_print("ra8d2 fileops: ready, plug a USB drive into either USB jack\r\n") !=
      k_ra8_ok) {
    fileops_panic_halt();
  }

  /* Alternate ports until the suite fully passes: a drive inserted in
   * either jack (or reseated) is picked up by a following cycle. */
  ra8_usb_speed_t speed = k_ra8_usb_speed_hs;
  ra8_err_t       err   = fileops_run_ladder(speed);
  while (err != k_ra8_ok) {
    (void)fileops_print_fail("ladder", err);
    (void)fileops_print("ra8d2 fileops: trying the other port in 5 s\r\n");
    ra8_delay_ms(k_fileops_retry_ms);
    if (speed == k_ra8_usb_speed_hs) {
      speed = k_ra8_usb_speed_fs;
    } else {
      speed = k_ra8_usb_speed_hs;
    }
    err = fileops_run_ladder(speed);
  }

  while (1) {
    ra8_delay_ms(k_fileops_idle_ms);
    __asm__ volatile("wfi");
  }
}
