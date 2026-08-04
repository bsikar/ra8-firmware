/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_pending/manual/usb_printer_vendor/main.c
 * @brief Native (bare-metal) USB printer + vendor composite device for EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Enumerates the EK-RA8D2 USB-FS port as a **composite** device that exposes
 * two interfaces in a single configuration so one enumeration exercises both
 * native class layers (issue #265):
 *
 *  - IF0: USB **Printer** class 0x07 / subclass 0x01 / protocol 0x02
 *    (bi-directional), driven by ``ra8_usb_pprn``. Bulk OUT EP 0x01 carries
 *    the host's print job (echoed to the UART console); bulk IN EP 0x82
 *    carries printer status. GET_PORT_STATUS / GET_DEVICE_ID / SOFT_RESET
 *    are answered from the class layer's port-status shadow.
 *  - IF1: **Vendor** specific class 0xFF, driven by ``ra8_usb_pvnd``. Bulk
 *    OUT EP 0x02 -> bulk IN EP 0x81 form a loopback a host libusb / WinUSB
 *    tool can round-trip.
 *
 * Unlike the ThreadX + USBX device examples, this app is fully bare-metal:
 * the native ``ra8_usb`` driver has no chapter-9 responder, so ``main.c``
 * runs a small polled chapter-9 loop (see ``usb_printer_vendor_ch9.c`` for the
 * pure SETUP router + descriptor tables) that answers the standard
 * GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION requests on EP0 and hands
 * the printer / vendor SETUPs to the class layers.
 *
 * ## Pinout (USB-FS, EK-RA8D2 board layer)
 *
 * | Net           | Pin    | Source                          |
 * |---------------|--------|---------------------------------|
 * | USB_FS_VBUS   | P4_07  | ``k_ra8_board_usbfs_pin_vbus``   |
 * | USB_FS_VBUSEN | P5_00  | ``k_ra8_board_usbfs_pin_vbusen`` |
 * | USB_FS_DP     | P8_14  | ``k_ra8_board_usbfs_pin_dp``     |
 * | USB_FS_DM     | P8_15  | ``k_ra8_board_usbfs_pin_dm``     |
 *
 * ## Verification
 *
 * - **ra8_emulator (EIL, headless):** the emulated chapter-9 host walks the
 *   enumeration script against the polled responder; the run reaches
 *   ``device CONFIGURED``. Gated by
 *   ``scripts/emu/smoke.sh usb_printer_vendor``.
 * - **Hardware (TODO):** the print-job -> UART echo needs a real host print
 *   subsystem (CUPS / usblp) driving the printer interface, and the vendor
 *   bulk loopback needs a host-side libusb script; both are marked
 *   ``TODO(host-side print job)`` / ``TODO(host-side vendor loopback)`` below
 *   because they cannot be driven without an attached PC.
 *
 * @author Brighton Sikarskie
 * @date 2026-07-16
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "ra8_usb_pprn.h"
#include "ra8_usb_pvnd.h"
#include "ra8_usb_regs.h"
#include "usb_printer_vendor_ch9.h"

/* -------------------------------------------------------------------------- */
/* Tunables + wire constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum demo_cfg_t
 * @brief Compile-time settings for the polled device loop.
 */
typedef enum : uint32_t {
  k_demo_console_baud = 115200U, /**< J-Link OB VCOM console baud.       */
  k_demo_poll_budget  = 200000U, /**< Bounded chapter-9 poll iterations. */
} demo_cfg_t;

/**
 * @enum demo_bulk_t
 * @brief Bulk staging sizes for the printer / vendor data path.
 */
typedef enum : uint16_t {
  k_demo_bulk_cap = 64U, /**< Bulk-FS max packet; one drain per BRDY. */
} demo_bulk_t;

/**
 * @enum demo_intsts0_t
 * @brief The INTSTS0 event bits the polled loop reacts to (HUM Ch 36.2.9).
 */
typedef enum : uint16_t {
  k_demo_int_brdy = (uint16_t)(1U << (uint16_t)k_ra8_int0_bit_brdy), /**< Buffer ready.  */
  k_demo_int_ctrt = (uint16_t)(1U << (uint16_t)k_ra8_int0_bit_ctrt), /**< Control stage. */
  k_demo_int_dvst = (uint16_t)(1U << (uint16_t)k_ra8_int0_bit_dvst), /**< Device state.  */
} demo_intsts0_t;

/**
 * @var s_device_id
 * @brief IEEE 1284 device-ID payload returned by Printer GET_DEVICE_ID.
 * @details Big-endian 2-byte length prefix then the ID string, per USB
 *          Printer Class 1.1 sec 4.2.1. Length prefix = 0x0031 = 49 bytes
 *          (the 2 length bytes plus the 47-char body).
 * @note Read-only wire-format constant.
 * @since 0.1.0
 */
static const uint8_t s_device_id[] = {
  0x00U, 0x31U, 'M', 'F', 'G', ':', 'R', 'e', 'n', 'e', 's', 'a', 's', ';', 'M', 'D', 'L', ':', 'E',
  'K',   '-',   'R', 'A', '8', 'D', '2', ';', 'C', 'M', 'D', ':', 'R', 'A', 'W', ';', 'C', 'L', 'S',
  ':',   'P',   'R', 'I', 'N', 'T', 'E', 'R', ';', 'D', 'E', 'S', 'C', ':', 'R', 'A', '8',
};

/**
 * @var s_configured
 * @brief Latched true once the host drives the device to CONFIGURED.
 * @note Written only by the poll loop; read for the banner + counters.
 * @warning Do not mutate outside ::demo_handle_dvst.
 * @since 0.1.0
 */
static volatile bool s_configured = false;

/**
 * @var g_usb_print_jobs
 * @brief HIL liveness counter -- print-job packets drained to the UART.
 * @details Advances on every bulk-OUT packet the host sends to the printer
 *          interface. Read externally over SWD by the HIL mem-probe.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_usb_print_jobs = 0U;

/**
 * @var g_usb_vendor_loops
 * @brief HIL liveness counter -- vendor bulk packets looped back.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_usb_vendor_loops = 0U;

/* -------------------------------------------------------------------------- */
/* USB-FS pin identifiers (from the board layer) */
/* -------------------------------------------------------------------------- */

/** @brief USB_FS VBUS pin, packed ``ra8_port_pin_t``. @since 0.1.0 */
static const ra8_port_pin_t k_demo_pin_vbus = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;
/** @brief USB_FS VBUSEN pin, packed ``ra8_port_pin_t``. @since 0.1.0 */
static const ra8_port_pin_t k_demo_pin_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;
/** @brief USB_FS D+ pin, packed ``ra8_port_pin_t``. @since 0.1.0 */
static const ra8_port_pin_t k_demo_pin_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;
/** @brief USB_FS D- pin, packed ``ra8_port_pin_t``. @since 0.1.0 */
static const ra8_port_pin_t k_demo_pin_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/* -------------------------------------------------------------------------- */
/* Console helper */
/* -------------------------------------------------------------------------- */

/**
 * @brief Write a NUL-terminated string to the UART console.
 *
 * @param[in] s String to emit (non-NULL, NUL-terminated).
 *
 * @pre ``ra8_board_uart_console_init`` succeeded.
 * @pre @p s is non-NULL.
 * @post The bytes have been queued to the console SCI.
 *
 * @note Not thread-safe; single-writer boot / poll context.
 * @since 0.1.0
 */
static void demo_puts(const char* s)
{
  if (s == nullptr) {
    return;
  }
  (void)ra8_board_uart_console_write((const uint8_t*)s, strlen(s));
}

/* -------------------------------------------------------------------------- */
/* Class-setup callbacks */
/* -------------------------------------------------------------------------- */

/**
 * @brief Printer class-setup handler: answer GET_PORT_STATUS / GET_DEVICE_ID.
 *
 * @details Invoked by ``ra8_usb_pprn_handle_setup`` after it validates the
 * envelope. GET_PORT_STATUS stages the 1-byte port-status shadow; GET_DEVICE_ID
 * stages the IEEE 1284 identity string; SOFT_RESET carries no data.
 *
 * @param[in] ctx   Unused context.
 * @param[in] setup Decoded printer class SETUP (non-NULL, class-validated).
 *
 * @return ``k_ra8_ok`` to ACK the status stage, else EP0 stalls.
 * @retval k_ra8_ok Data staged (or none required).
 *
 * @pre ``ra8_usb_pprn_init`` succeeded.
 * @pre @p setup is non-NULL (class-layer guarantee).
 * @post For a control-IN request the DCP holds the response bytes.
 *
 * @note Runs in the poll loop's dispatch context.
 * @since 0.1.0
 */
static ra8_err_t demo_printer_setup(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  if (setup == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (setup->b_request == (uint8_t)k_pv_pprn_get_port_status) {
    uint8_t         status = 0U;
    const ra8_err_t st_err = ra8_usb_pprn_get_port_status(&status);
    if (st_err != k_ra8_ok) {
      return st_err;
    }
    return ra8_usb_dcp_in_data(k_ra8_usb_speed_fs, &status, 1U);
  }
  if (setup->b_request == (uint8_t)k_pv_pprn_get_device_id) {
    const uint16_t want = pv_clamp_len((uint16_t)sizeof(s_device_id), setup->w_length);
    return ra8_usb_dcp_in_data(k_ra8_usb_speed_fs, s_device_id, want);
  }
  return k_ra8_ok; /* SOFT_RESET: no data stage. */
}

/**
 * @brief Vendor class-setup handler: ACK the vendor control request.
 *
 * @details The demo defines no vendor control protocol; every vendor-recipient
 * request is ACKed so a host libusb tool can probe the interface. Real
 * firmware would decode @p setup->b_request here.
 *
 * @param[in] ctx   Unused context.
 * @param[in] setup Decoded vendor SETUP (non-NULL, envelope-validated).
 *
 * @return ``k_ra8_ok`` to ACK.
 * @retval k_ra8_ok Always.
 *
 * @pre ``ra8_usb_pvnd_init`` succeeded.
 * @pre @p setup is non-NULL (class-layer guarantee).
 * @post No device state mutated.
 *
 * @note Runs in the poll loop's dispatch context.
 * @since 0.1.0
 */
static ra8_err_t demo_vendor_setup(void* ctx, const ra8_usb_setup_t* setup)
{
  (void)ctx;
  if (setup == nullptr) {
    return k_ra8_err_null_ptr;
  }
  return k_ra8_ok;
}

/* -------------------------------------------------------------------------- */
/* Polled chapter-9 SETUP handling */
/* -------------------------------------------------------------------------- */

/**
 * @brief Stage a standard GET_DESCRIPTOR response on EP0.
 *
 * @param[in] action ::k_pv_action_get_* descriptor action.
 * @param[in] setup  The originating SETUP (for its ``wLength`` ceiling).
 *
 * @return ``ra8_err_t`` from the DCP data stage.
 * @retval k_ra8_ok Descriptor staged (PID = BUF).
 * @retval k_ra8_err_not_supported Action names no descriptor.
 *
 * @pre ``ra8_usb_pprn_init`` / ``_pvnd_init`` succeeded.
 * @pre @p setup is non-NULL.
 * @post On success the DCP holds up to ``wLength`` descriptor bytes.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t demo_stage_descriptor(pv_action_t action, const ra8_usb_setup_t* setup)
{
  const uint8_t* desc = nullptr;
  uint16_t       full = 0U;
  if (!pv_descriptor(action, &desc, &full)) {
    return k_ra8_err_not_supported;
  }
  const uint16_t want = pv_clamp_len(full, setup->w_length);
  return ra8_usb_dcp_in_data(k_ra8_usb_speed_fs, desc, want);
}

/**
 * @brief Dispatch one decoded SETUP packet from the DCP.
 *
 * @details Routes via ::pv_route_setup: descriptor actions stage bytes,
 * SET_ADDRESS latches the address, SET_CONFIGURATION ACKs and the class
 * actions hand off to ``ra8_usb_pprn_handle_setup`` / ``_pvnd_handle_setup``.
 * Unsupported requests STALL EP0.
 *
 * @param[in] setup Decoded SETUP packet (non-NULL).
 *
 * @pre @p setup is non-NULL.
 * @pre The USB device layers are initialized.
 * @post EP0 has been advanced (data staged, ACKed, or stalled).
 *
 * @note Not thread-safe; single poll-loop caller.
 * @since 0.1.0
 */
static void demo_dispatch_setup(const ra8_usb_setup_t* setup)
{
  const pv_action_t action = pv_route_setup(setup);
  ra8_err_t         err    = k_ra8_ok;
  switch (action) {
    case k_pv_action_get_device_desc:
    case k_pv_action_get_config_desc:
    case k_pv_action_get_string_desc:
      err = demo_stage_descriptor(action, setup);
      break;
    case k_pv_action_set_address:
      err = ra8_usb_set_address(k_ra8_usb_speed_fs, (uint8_t)setup->w_value);
      if (err == k_ra8_ok) {
        err = ra8_usb_control_response(k_ra8_usb_speed_fs, true);
      }
      break;
    case k_pv_action_set_configuration:
      err = ra8_usb_control_response(k_ra8_usb_speed_fs, true);
      break;
    case k_pv_action_printer_class:
      err = ra8_usb_pprn_handle_setup(setup);
      break;
    case k_pv_action_vendor_class:
      err = ra8_usb_pvnd_handle_setup(setup);
      break;
    case k_pv_action_stall:
    default:
      err = k_ra8_err_not_supported;
      break;
  }
  if (err != k_ra8_ok) {
    (void)ra8_usb_control_response(k_ra8_usb_speed_fs, false); /* STALL EP0. */
  }
}

/**
 * @brief React to a device-state transition (INTSTS0.DVST).
 *
 * @details On the host's bus reset (Default state) the DCP is re-armed; on
 * CONFIGURED the banner is emitted once. USB 2.0 sec 9.1 "Device States".
 *
 * @pre The USB device layers are initialized.
 * @post ``s_configured`` reflects the current state.
 * @post On Default the DCP was re-armed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void demo_handle_dvst(void)
{
  ra8_usb_dev_state_t state   = k_ra8_usb_dev_state_powered;
  const ra8_err_t     dev_err = ra8_usb_get_device_state(k_ra8_usb_speed_fs, &state);
  if (dev_err != k_ra8_ok) {
    return;
  }
  if (state == k_ra8_usb_dev_state_default) {
    (void)ra8_usb_device_busreset_rearm(k_ra8_usb_speed_fs);
  } else if ((state == k_ra8_usb_dev_state_configured) && !s_configured) {
    s_configured = true;
    demo_puts("USB PRINTER+VENDOR CONFIGURED\r\n");
  } else {
    /* Address / Powered / Suspended: nothing to do here. */
  }
}

/**
 * @brief Drain a bulk-OUT print job and echo it to the UART console.
 *
 * @details TODO(host-side print job): a physical host print subsystem
 * (CUPS / usblp) must send the job; ra8_emulator reaches CONFIGURED but does not
 * drive bulk traffic, so this path runs only on hardware.
 *
 * @pre ``ra8_usb_pprn_init`` succeeded.
 * @post On data, the bytes were echoed and ::g_usb_print_jobs advanced.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void demo_drain_print_job(void)
{
  uint8_t         job[k_demo_bulk_cap];
  uint16_t        got = 0U;
  const ra8_err_t err = ra8_usb_pprn_recv(job, (uint16_t)sizeof(job), &got);
  if ((err == k_ra8_ok) && (got > 0U)) {
    (void)ra8_board_uart_console_write(job, (size_t)got);
    g_usb_print_jobs += 1U;
  }
}

/**
 * @brief Drain a bulk-OUT vendor packet and loop it back on the vendor IN pipe.
 *
 * @details TODO(host-side vendor loopback): a host libusb / WinUSB tool must
 * drive the bulk transfer; this path runs only on hardware.
 *
 * @pre ``ra8_usb_pvnd_init`` succeeded.
 * @post On data, the packet was echoed back and ::g_usb_vendor_loops advanced.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void demo_drain_vendor(void)
{
  uint8_t         pkt[k_demo_bulk_cap];
  uint16_t        got = 0U;
  const ra8_err_t err = ra8_usb_pvnd_recv(pkt, (uint16_t)sizeof(pkt), &got);
  if ((err == k_ra8_ok) && (got > 0U)) {
    (void)ra8_usb_pvnd_send(pkt, got);
    g_usb_vendor_loops += 1U;
  }
}

/**
 * @brief Run one poll-loop iteration: service EP0 + bulk events.
 *
 * @pre The USB device layers are initialized and attached.
 * @post Any pending CTRT / DVST / BRDY event was serviced.
 *
 * @note Not thread-safe; single poll-loop caller.
 * @since 0.1.0
 */
static void demo_poll_once(void)
{
  uint16_t        sts     = 0U;
  const ra8_err_t sts_err = ra8_usb_get_status(k_ra8_usb_speed_fs, &sts);
  if (sts_err != k_ra8_ok) {
    return;
  }
  if ((sts & (uint16_t)k_demo_int_ctrt) != 0U) {
    (void)ra8_usb_clear_status(k_ra8_usb_speed_fs, (uint16_t)k_demo_int_ctrt);
    ra8_usb_setup_t setup  = {};
    const ra8_err_t rd_err = ra8_usb_read_setup_if_valid(k_ra8_usb_speed_fs, &setup);
    if (rd_err == k_ra8_ok) {
      demo_dispatch_setup(&setup); /* fresh SETUP. */
    } else {
      (void)ra8_usb_control_response(k_ra8_usb_speed_fs, true); /* status-stage ACK. */
    }
  }
  if ((sts & (uint16_t)k_demo_int_dvst) != 0U) {
    (void)ra8_usb_clear_status(k_ra8_usb_speed_fs, (uint16_t)k_demo_int_dvst);
    demo_handle_dvst();
  }
  if ((sts & (uint16_t)k_demo_int_brdy) != 0U) {
    (void)ra8_usb_clear_status(k_ra8_usb_speed_fs, (uint16_t)k_demo_int_brdy);
    demo_drain_print_job();
    demo_drain_vendor();
  }
}

/* -------------------------------------------------------------------------- */
/* Bring-up helpers */
/* -------------------------------------------------------------------------- */

/**
 * @brief Halt forever in WFI after a fatal bring-up error.
 *
 * @pre Reached only on an unrecoverable init failure.
 * @post CPU parked until reset.
 *
 * @note Not reachable post-boot.
 * @since 0.1.0
 */
static void demo_panic_halt(void)
{
  demo_puts("usb_printer_vendor: FAIL bring-up\r\n");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route the four USB-FS pins to the USBFS controller.
 *
 * @return ``ra8_err_t`` from the first failing route, or ``k_ra8_ok``.
 * @retval k_ra8_ok All four pins routed.
 *
 * @pre IOPORT reachable; single-threaded init context.
 * @post On success the four USB-FS pins are in USB peripheral mode.
 *
 * @note VBUSEN is driven low (device mode); peripheral routing would force it
 *       high (host mode) and block enumeration.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t demo_pins_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(k_demo_pin_vbus, k_ra8_psel_usb_fs, "usbpv.vbus");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gpio_output_init(k_demo_pin_vbusen, k_ra8_level_low);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(k_demo_pin_dp, k_ra8_psel_usb_fs, "usbpv.dp");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_pfs_route_peripheral(k_demo_pin_dm, k_ra8_psel_usb_fs, "usbpv.dm");
}

/**
 * @brief Bring up both native class layers and advertise the device.
 *
 * @details Initialises ``ra8_usb_pprn`` (printer pipes PIPE3/PIPE4) then
 * ``ra8_usb_pvnd`` (vendor pipes PIPE5/PIPE1), caches each layer's descriptor
 * blob + the printer device-ID, installs the class-setup callbacks, and raises
 * the D+ pull-up so the host begins enumeration. Both ``*_init`` calls run the
 * shared ``ra8_usb_device_init``; the second is idempotent for this polled
 * device because the loop reads INTSTS0 directly and does not rely on the
 * per-pipe BRDY enables the re-init clears.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Both class layers up, pull-up raised.
 *
 * @pre CGC + USB clock + pins already configured.
 * @post On success the device advertises on the bus.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t demo_usb_bringup(void)
{
  const uint8_t* config_desc = nullptr;
  uint16_t       config_len  = 0U;
  if (!pv_descriptor(k_pv_action_get_config_desc, &config_desc, &config_len)) {
    return k_ra8_err_not_supported;
  }

  ra8_err_t err = ra8_usb_pprn_init(k_ra8_usb_speed_fs);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_pprn_set_descriptors(config_desc,
                                     config_len,
                                     s_device_id,
                                     (uint16_t)sizeof(s_device_id));
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_pvnd_init(k_ra8_usb_speed_fs);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_pvnd_set_descriptors(config_desc, config_len);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_pprn_attach_setup_handler(demo_printer_setup, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_pvnd_attach_setup_handler(demo_vendor_setup, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_usb_device_attach(k_ra8_usb_speed_fs, true);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring up the clocks + USB + poll chapter-9.
 *
 * @return Never returns in normal operation.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit configured VTOR / FPU.
 * @post On clean bring-up the CPU stays in the poll loop.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @note Single entry point; not re-entrant.
 * @since 0.1.0
 */
int32_t main(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (ra8_board_uart_console_init(k_demo_console_baud) != k_ra8_ok) {
    demo_panic_halt();
  }
  if (demo_pins_init() != k_ra8_ok) {
    demo_panic_halt();
  }

  ra8_isr_globals_enable();

  if (demo_usb_bringup() != k_ra8_ok) {
    demo_panic_halt();
  }
  demo_puts("USB PRINTER+VENDOR READY\r\n");

  /* Bounded outer supervisor: each pass runs the poll budget, then re-checks
   * for progress. The device idles here forever answering the host. */
  for (uint32_t guard = 0U; guard < (uint32_t)k_demo_poll_budget; guard++) {
    demo_poll_once();
  }

  /* Poll budget exhausted (host never enumerated): park. */
  while (1) {
    demo_poll_once();
  }
}
#pragma GCC diagnostic pop
