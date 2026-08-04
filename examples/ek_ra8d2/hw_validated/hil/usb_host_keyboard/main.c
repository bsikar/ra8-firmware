/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/usb_host_keyboard/main.c
 * @brief USB host-mode HID boot-keyboard over the self-loop (no real keyboard)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Validates the USB HID **keyboard** host path with no real keyboard: the board
 * hosts on one jack and simulates a boot-keyboard peripheral on the other over
 * the loop cable. One image runs both USB stacks:
 *
 *  - USBFS (J11) = DEVICE (the fake keyboard): a ThreadX + USBX HID class
 *    advertising the standard boot-keyboard report descriptor (interface
 *    subclass 1 / protocol 1). A worker continuously queues the 8-byte boot
 *    report [modifier][reserved][keycode x6] with the keycodes for "RA8D2".
 *  - USBHS (J7) = HOST: a polled host on the first-party `ra8_usb_host_*`
 *    primitives. It enumerates the keyboard, opens the interrupt-IN endpoint,
 *    polls reports, verifies the body, and DECODES the keycodes (bytes 2..)
 *    back to ASCII -- proving the HID keyboard report path end to end on chip.
 *
 * This is the boot-keyboard counterpart to `usb_selftest_hid` (which uses a
 * generic vendor report); the original version needed a real USB keyboard in
 * J7. Verdicts stream over SCI8 (J-Link OB CDC, 115200); ``s_dbg_*`` mirror
 * progress for J-Link.
 *
 * ## Pinout
 *
 * FS device: P4_07 VBUS sense, P5_00 VBUSEN GPIO LOW (device role),
 * P8_14 D+, P8_15 D- (PSEL usb_fs). HS host: SW4-8 to Host via the U15
 * expander, PD07 HIGH (U18 supplies J7 VBUS), P4_08 USBHS_VBUS
 * (PSEL usb_hs). Console: PD_02/PD_03 SCI8 (PSEL sci_async).
 *
 * @author Brighton Sikarskie
 * @date 2026-06-13
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_isr.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"
#include "ra8_usb.h"
#include "usb_host_keyboard_steps.h"

#ifndef RA8_OFF_TARGET
#include "tx_api.h"
#include "ux_api.h"
#include "ux_dcd_ra8_usb.h"
#include "ux_device_class_hid.h"
#include "ux_device_stack.h"

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
static const ra8_port_pin_t k_hid_pin_fs_vbus = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbus;

/** @brief USBFS VBUSEN (P5_00) -- GPIO LOW for the device role. */
static const ra8_port_pin_t k_hid_pin_fs_vbusen = (ra8_port_pin_t)k_ra8_board_usbfs_pin_vbusen;

/** @brief USBFS D+ (P8_14). */
static const ra8_port_pin_t k_hid_pin_fs_dp = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dp;

/** @brief USBFS D- (P8_15). */
static const ra8_port_pin_t k_hid_pin_fs_dm = (ra8_port_pin_t)k_ra8_board_usbfs_pin_dm;

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra8_port_pin_t k_hid_pin_hs_vbus = (ra8_port_pin_t)k_ra8_board_usbhs_pin_vbus;

/** @brief J7 host-power switch (PD07): HIGH = U18 supplies VBUS (UM 6.2). */
static const ra8_port_pin_t k_hid_pin_hs_pwr = (ra8_port_pin_t)k_ra8_board_usbhs_pin_pwr;

/* -------------------------------------------------------------------------- */
/* Tunables */
/* -------------------------------------------------------------------------- */

/** @brief ASCII the host decoded from the read keycodes (filled by the verify). */
static char s_typed[k_hid_nkeys + 1U] = {};

/**
 * @enum hid_phase_t
 * @brief J-Link probe values marking host-ladder progress.
 */
typedef enum : uint32_t {
  k_hid_phase_boot   = 0U, /**< Host thread not started.    */
  k_hid_phase_init   = 1U, /**< Host controller init.       */
  k_hid_phase_enum   = 2U, /**< Enumerating.                */
  k_hid_phase_verify = 3U, /**< Reading + checking reports. */
  k_hid_phase_pass   = 4U, /**< All reports verified.       */
} hid_phase_t;

#ifndef RA8_OFF_TARGET

/* -------------------------------------------------------------------------- */
/* ThreadX host worker + shared activation semaphore */
/* -------------------------------------------------------------------------- */

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
static UCHAR s_host_stack[k_hid_host_stack];

/**
 * @var s_usb_host_keyboard_hid_active_sem
 * @brief Posted by the activate callback so the send worker blocks on it
 *        instead of polling ``s_hid_class`` with tx_thread_sleep (which has
 *        been observed never returning on this silicon under load).
 * @details Defined here; the device worker (sibling TU) and this main.c both
 *          reference it via the extern in `usb_host_keyboard_steps.h`.
 * @note Single-producer (class thread), single-consumer (send worker).
 * @since 0.1.0
 */
TX_SEMAPHORE s_usb_host_keyboard_hid_active_sem;

/* -------------------------------------------------------------------------- */
/* J-Link probes (host side) */
/* -------------------------------------------------------------------------- */

/** @brief Host-ladder phase marker (::hid_phase_t). */
static volatile uint32_t s_dbg_phase;
/** @brief Reports the host confirmed pattern-equal (expect ::k_hid_rounds). */
static volatile uint32_t s_dbg_rounds_ok;
/** @brief Device-reported product id captured at enumeration. */
static volatile uint32_t s_dbg_pid;
/** @brief First mismatching report round, or ::k_hid_no_mismatch. */
static volatile uint32_t s_dbg_mismatch = (uint32_t)k_hid_no_mismatch;
/** @brief Completed full passes (sticky success counter). */
static volatile uint32_t s_dbg_pass_count;
/** @brief Seq byte of the most recent report the host read back. */
static volatile uint32_t s_dbg_last_seq;

/* -------------------------------------------------------------------------- */
/* Keycode decode (host side) */
/* -------------------------------------------------------------------------- */

/**
 * @brief Decode one HID Usage-Table keycode to its ASCII character.
 *
 * @param[in] kc Keycode byte from a boot-keyboard report (bytes 2..).
 * @return The ASCII letter ('A'..'Z') / digit ('0'..'9'), or '?' if @p kc is
 *         not in the alphanumeric ranges this self-test uses.
 * @since 0.1.0
 */
static char hid_keycode_to_ascii(uint8_t kc)
{
  if ((kc >= (uint8_t)k_hid_kc_a) && (kc <= (uint8_t)k_hid_kc_z)) {
    return (char)('A' + (int)(kc - (uint8_t)k_hid_kc_a));
  }
  if ((kc >= (uint8_t)k_hid_kc_1) && (kc < (uint8_t)k_hid_kc_0)) {
    return (char)('1' + (int)(kc - (uint8_t)k_hid_kc_1));
  }
  if (kc == (uint8_t)k_hid_kc_0) {
    return '0';
  }
  return '?';
}

/* -------------------------------------------------------------------------- */
/* Host side: self-contained polled enumerate + HID interrupt-IN read */
/* -------------------------------------------------------------------------- */

/**
 * @enum hid_usb_req_t
 * @brief Standard chapter-9 request / descriptor constants for the host.
 */
typedef enum : uint16_t {
  k_hid_bm_std_dev_in   = 0x80U, /**< bmRequestType: Std | Device | In.  */
  k_hid_bm_std_dev_out  = 0x00U, /**< bmRequestType: Std | Device | Out. */
  k_hid_breq_get_desc   = 0x06U, /**< GET_DESCRIPTOR.                    */
  k_hid_breq_set_addr   = 0x05U, /**< SET_ADDRESS.                       */
  k_hid_breq_set_config = 0x09U, /**< SET_CONFIGURATION.                 */
  k_hid_desc_device     = 0x01U, /**< DEVICE descriptor type.            */
  k_hid_dev_desc_len    = 18U,   /**< DEVICE descriptor length.          */
  k_hid_off_dev_pid     = 10U,   /**< idProduct LSB byte offset.         */
  k_hid_byte_bits       = 8U,    /**< Bits per byte.                     */
  k_hid_config_value    = 1U,    /**< bConfigurationValue to select.     */
} hid_usb_req_t;

/**
 * @enum hid_enum_tune_t
 * @brief Timing / retry tunables for the polled enumeration ladder.
 */
typedef enum : uint32_t {
  k_hid_vbus_settle_ms = 200U,      /**< VBUS settle before probing.          */
  k_hid_attach_to_ms   = 2000U,     /**< Wait for the D+ pull-up.             */
  k_hid_debounce_ms    = 500U,      /**< Post-attach debounce (>=100 ms).     */
  k_hid_reset_hold_ms  = 50U,       /**< USB bus-reset hold (>=10 ms).        */
  k_hid_recovery_ms    = 20U,       /**< Post-reset recovery (TRSTRCY).       */
  k_hid_addr_settle_ms = 5U,        /**< Post-SET_ADDRESS recovery.           */
  k_hid_enum_tries     = 8U,        /**< Reset+probe attempts.                */
  k_hid_attach_spin    = 50000000U, /**< Attach spin cap (frozen-tick guard). */
} hid_enum_tune_t;

/**
 * @brief GET_DESCRIPTOR(DEVICE) over the polled control engine.
 *
 * @param[out] desc Receives the 18-byte device descriptor.
 *
 * @return Read outcome.
 * @retval k_ra8_ok           All 18 bytes arrived.
 * @retval k_ra8_err_hw_error A short descriptor came back.
 *
 * @pre The bus is reset and the DCP targets the device's current address.
 * @pre @p desc holds at least ::k_hid_dev_desc_len bytes.
 * @post @p desc carries the device descriptor on success.
 * @post No global state changes.
 *
 * @note Blocking (polled control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_ctrl_get_dev_desc(uint8_t* desc)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_hid_bm_std_dev_in,
    .b_request       = (uint8_t)k_hid_breq_get_desc,
    .w_value         = (uint16_t)((uint16_t)k_hid_desc_device << (uint16_t)k_hid_byte_bits),
    .w_index         = 0U,
    .w_length        = (uint16_t)k_hid_dev_desc_len,
  };
  uint16_t        rx = 0U;
  const ra8_err_t err =
    ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, desc, (uint16_t)k_hid_dev_desc_len, &rx);
  if (err != k_ra8_ok) {
    return err;
  }
  if (rx != (uint16_t)k_hid_dev_desc_len) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

/**
 * @brief Wait for the device to attach, then bus-reset + read its descriptor.
 *
 * @details Waits for the D+ pull-up (LNST leaves SE0) plus the spec
 * debounce, then each attempt drives a full bus reset (returns the device
 * to address 0), re-enables SOF, targets address 0, and reads the device
 * descriptor. The first attempt that returns all 18 bytes wins.
 *
 * @param[out] desc Receives the winning 18-byte device descriptor.
 *
 * @return Hunt outcome.
 * @retval k_ra8_ok             The device answered at address 0.
 * @retval k_ra8_err_hw_timeout Nothing attached / nothing answered.
 *
 * @pre ::ra8_usb_host_init ran (host mode up, VBUS supplied).
 * @pre ::ra8_time_init has run (ms delays).
 * @post On success the DCP targets address 0 with UACT on.
 * @post On failure the bus is left in the last attempt's state.
 *
 * @note Blocking; worst case a few seconds.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_enum_hunt(uint8_t* desc)
{
  ra8_delay_ms(k_hid_vbus_settle_ms);
  const uint32_t t0 = ra8_time_ms();
  for (uint32_t spin = 0U; spin < (uint32_t)k_hid_attach_spin; spin++) {
    if (ra8_usb_host_line_state(k_ra8_usb_speed_hs) != 0U) {
      break;
    }
    if ((ra8_time_ms() - t0) > (uint32_t)k_hid_attach_to_ms) {
      break;
    }
  }
  ra8_delay_ms(k_hid_debounce_ms);
  ra8_err_t err = k_ra8_err_hw_timeout;
  for (uint8_t attempt = 0U; attempt < (uint8_t)k_hid_enum_tries; attempt++) {
    (void)ra8_usb_host_bus_reset(k_ra8_usb_speed_hs, true);
    ra8_delay_ms(k_hid_reset_hold_ms);
    (void)ra8_usb_host_bus_reset(k_ra8_usb_speed_hs, false);
    (void)ra8_usb_host_set_uact(k_ra8_usb_speed_hs, true);
    ra8_delay_ms(k_hid_recovery_ms);
    (void)ra8_usb_host_set_target(k_ra8_usb_speed_hs, 0U);
    err = hid_ctrl_get_dev_desc(desc);
    if (err == k_ra8_ok) {
      return k_ra8_ok;
    }
  }
  return err;
}

/**
 * @brief SET_ADDRESS to ::k_hid_dev_addr, then retarget the DCP.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The DCP now targets the operating address.
 *
 * @pre ::hid_enum_hunt succeeded (device answering at address 0).
 * @pre The bus is active (UACT on).
 * @post Later transfers carry tokens to ::k_hid_dev_addr.
 * @post The set-address recovery delay has elapsed.
 *
 * @note Blocking (one control transfer + settle).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_enum_set_address(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_hid_bm_std_dev_out,
    .b_request       = (uint8_t)k_hid_breq_set_addr,
    .w_value         = (uint16_t)k_hid_dev_addr,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  ra8_err_t err = ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_hid_addr_settle_ms);
  return ra8_usb_host_set_target(k_ra8_usb_speed_hs, (uint8_t)k_hid_dev_addr);
}

/**
 * @brief SET_CONFIGURATION(::k_hid_config_value) on the addressed device.
 *
 * @return Control-transfer outcome.
 * @retval k_ra8_ok The device entered the Configured state.
 *
 * @pre ::hid_enum_set_address succeeded.
 * @pre The DCP targets ::k_hid_dev_addr.
 * @post On success the device's endpoints are usable.
 * @post No global state changes.
 *
 * @note Blocking (one control transfer).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_enum_set_config(void)
{
  const ra8_usb_setup_t setup = {
    .bm_request_type = (uint8_t)k_hid_bm_std_dev_out,
    .b_request       = (uint8_t)k_hid_breq_set_config,
    .w_value         = (uint16_t)k_hid_config_value,
    .w_index         = 0U,
    .w_length        = 0U,
  };
  return ra8_usb_host_control_xfer(k_ra8_usb_speed_hs, &setup, nullptr, 0U, nullptr);
}

/**
 * @brief Open the host receive pipe for the device's HID interrupt-IN endpoint.
 *
 * @details Pipe ::k_hid_pipe_in -> device EP1 IN (host receives), 64-byte
 * MPS. The endpoint is interrupt on the device; the host SIE drives it as a
 * receive pipe (issues IN tokens on demand), which is sufficient for the
 * polled report read. HID is input-only, so there is no OUT pipe.
 *
 * @return The pipe-setup error, or k_ra8_ok.
 * @retval k_ra8_ok The IN pipe is configured and parked NAK.
 *
 * @pre ::hid_enum_set_config succeeded.
 * @pre The pipe is not currently armed.
 * @post The pipe targets ::k_hid_dev_addr EP1 with DATA0 forced.
 * @post The pipe's status bits are cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_open_pipes(void)
{
  return ra8_usb_host_pipe_setup(k_ra8_usb_speed_hs,
                                 (uint8_t)k_hid_pipe_in,
                                 (uint8_t)k_hid_dev_addr,
                                 (uint8_t)k_hid_ep_in_num,
                                 true,
                                 (uint16_t)k_hid_mps);
}

/**
 * @brief Run the full enumeration ladder and open the bulk pipes.
 *
 * @details hunt -> SET_ADDRESS -> SET_CONFIGURATION -> open pipes, and
 * extract idProduct from the device descriptor. Each step prints its own
 * failure tag. The caller deinitializes the host controller on error.
 *
 * @param[out] out_pid Receives the device's idProduct on success.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok Device enumerated; bulk pipes open.
 *
 * @pre ::ra8_usb_host_init has succeeded on this pass.
 * @pre @p out_pid is non-NULL.
 * @post @p out_pid holds the device idProduct on success.
 * @post On failure the offending step printed its tag.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_enumerate(uint32_t* out_pid)
{
  uint8_t   desc[k_hid_dev_desc_len] = {};
  ra8_err_t err                      = hid_enum_hunt(desc);
  if (err != k_ra8_ok) {
    (void)hid_print_fail("enumerate", err);
    return err;
  }
  *out_pid = (uint32_t)desc[k_hid_off_dev_pid] |
             ((uint32_t)desc[(uint32_t)k_hid_off_dev_pid + 1U] << (uint32_t)k_hid_byte_bits);
  err      = hid_enum_set_address();
  if (err != k_ra8_ok) {
    (void)hid_print_fail("set_address", err);
    return err;
  }
  err = hid_enum_set_config();
  if (err != k_ra8_ok) {
    (void)hid_print_fail("set_config", err);
    return err;
  }
  err = hid_open_pipes();
  if (err != k_ra8_ok) {
    (void)hid_print_fail("open_pipes", err);
  }
  return err;
}

/**
 * @brief Print "enumerated pid=0xNNNN" for the looped device.
 *
 * @param[in] pid The device idProduct to report.
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The line is queued.
 *
 * @pre ::hid_enumerate succeeded.
 * @pre SCI8 init already ran.
 * @post One ASCII line is in the SCI8 TX FIFO.
 * @post No other state changes.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_print_enum(uint32_t pid)
{
  ra8_err_t err = hid_print("ra8d2 hid: enumerated pid=0x");
  if (err != k_ra8_ok) {
    return err;
  }
  err = hid_print_hex(pid, (uint8_t)k_hid_hex_chars_u16);
  if (err != k_ra8_ok) {
    return err;
  }
  return hid_print("\r\n");
}

/**
 * @brief Print the decoded keys + "USB HOST KEYBOARD PASS".
 *
 * @details "host decoded keys \"RA8D2\" over N reports -- USB HOST KEYBOARD PASS".
 *
 * @return ra8_err_t propagated from the SCI helpers.
 * @retval k_ra8_ok The verdict line is queued.
 *
 * @pre All ::k_hid_rounds report rounds verified; ::s_typed is populated.
 * @pre SCI8 init already ran.
 * @post One ASCII verdict line is in the SCI8 TX FIFO.
 *
 * @note Blocking polled TX.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_print_pass(void)
{
  ra8_err_t err = hid_print("ra8d2 hid: host decoded keys \"");
  if (err == k_ra8_ok) {
    err = hid_print(s_typed);
  }
  if (err == k_ra8_ok) {
    err = hid_print("\" over ");
  }
  if (err == k_ra8_ok) {
    err = hid_print_dec((uint32_t)k_hid_rounds);
  }
  if (err == k_ra8_ok) {
    err = hid_print(" reports -- USB HOST KEYBOARD PASS\r\n");
  }
  return err;
}

/**
 * @brief One report round: read an interrupt-IN report, check its body.
 *
 * @details Polls one report off the device's interrupt-IN endpoint, checks
 * the read length equals ::k_hid_report_len, and byte-checks the fixed body
 * (bytes 1..N-1) against ::hid_fill_report_body. Byte 0 (the seq) is
 * recorded in ::s_dbg_last_seq as a liveness witness. Each report is well
 * under the endpoint MPS, so it arrives as one short packet.
 *
 * @param[in] round The report round index (0..::k_hid_rounds-1).
 *
 * @return ra8_err_t verdict.
 * @retval k_ra8_ok               The report body matched.
 * @retval k_ra8_err_invalid_size The report length differed.
 * @retval k_ra8_err_invalid_state The report body bytes differed.
 *
 * @pre The interrupt-IN pipe was opened by ::hid_open_pipes.
 * @pre The device send worker is queuing reports.
 * @post ::s_dbg_mismatch records @p round on any mismatch.
 * @post ::s_dbg_last_seq holds the read report's seq on success.
 *
 * @note Blocking; one interrupt-IN read over the self-loop.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_read_round(uint32_t round)
{
  static uint8_t s_expect[k_hid_read_buf] = {};
  static uint8_t s_rx[k_hid_read_buf]     = {};
  hid_fill_report_body(s_expect, (uint32_t)k_hid_report_len);
  uint16_t        rx  = 0U;
  const ra8_err_t err = ra8_usb_host_bulk_in(k_ra8_usb_speed_hs,
                                             (uint8_t)k_hid_pipe_in,
                                             s_rx,
                                             (uint16_t)k_hid_read_buf,
                                             &rx);
  if (err != k_ra8_ok) {
    (void)hid_print_fail("report read", err);
    return err;
  }
  if (rx != (uint16_t)k_hid_report_len) {
    s_dbg_mismatch = round;
    (void)hid_print_fail("report length", k_ra8_err_invalid_size);
    return k_ra8_err_invalid_size;
  }
  const size_t body = (size_t)((uint32_t)k_hid_report_len - (uint32_t)k_hid_body_idx);
  if (memcmp(&s_rx[k_hid_body_idx], &s_expect[k_hid_body_idx], body) != 0) {
    s_dbg_mismatch = round;
    (void)hid_print_fail("report pattern mismatch", k_ra8_err_invalid_state);
    return k_ra8_err_invalid_state;
  }
  s_dbg_last_seq = (uint32_t)s_rx[k_hid_seq_idx];
  /* Decode the boot-keyboard keycodes (bytes 2..) back to ASCII for the verdict.
   * The report is constant, so doing it each round is idempotent. */
  for (uint32_t i = 0U; i < (uint32_t)k_hid_nkeys; i++) {
    s_typed[i] = hid_keycode_to_ascii(s_rx[(uint32_t)k_hid_key0_idx + i]);
  }
  s_typed[(uint32_t)k_hid_nkeys] = '\0';
  return k_ra8_ok;
}

/**
 * @brief One full host-side pass: enumerate, then read + check reports.
 *
 * @details Phases mirror ::hid_phase_t. On any failure the host controller
 * is deinitialized so the next retry starts from a clean attach.
 *
 * @return First failing step's error, or k_ra8_ok.
 * @retval k_ra8_ok The pass printed HID PASS.
 *
 * @pre Device-side HID class is registered and sending (other thread).
 * @pre The self-loop cable connects J7 to J11.
 * @post On success ::s_dbg_pass_count advanced and LED2 is on.
 * @post On failure the host controller is deinitialized.
 *
 * @note Blocking; runs on the low-priority host thread.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t hid_host_pass(void)
{
  s_dbg_phase   = (uint32_t)k_hid_phase_init;
  ra8_err_t err = hid_print("ra8d2 hid: host up on USB-HS, probing the loop...\r\n");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_usb_host_init(k_ra8_usb_speed_hs);
  if (err != k_ra8_ok) {
    (void)hid_print_fail("host init", err);
    return err;
  }

  s_dbg_phase  = (uint32_t)k_hid_phase_enum;
  uint32_t pid = 0U;
  err          = hid_enumerate(&pid);
  if (err != k_ra8_ok) {
    (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
    return err;
  }
  s_dbg_pid = pid;
  err       = hid_print_enum(pid);
  if (err != k_ra8_ok) {
    return err;
  }

  s_dbg_phase     = (uint32_t)k_hid_phase_verify;
  s_dbg_rounds_ok = 0U;
  for (uint32_t r = 0U; r < (uint32_t)k_hid_rounds; r++) {
    err = hid_read_round(r);
    if (err != k_ra8_ok) {
      (void)ra8_usb_host_deinit(k_ra8_usb_speed_hs);
      return err;
    }
    s_dbg_rounds_ok++;
  }

  s_dbg_phase = (uint32_t)k_hid_phase_pass;
  s_dbg_pass_count++;
  err = hid_print_pass();
  if (err != k_ra8_ok) {
    return err;
  }
  (void)ra8_board_led_on(k_ra8_board_led2);
  return k_ra8_ok;
}

/**
 * @brief Host-side worker: retry the full pass until it succeeds.
 *
 * @details Waits for the device side to attach, then loops ::hid_host_pass
 * with a retry pause until every report round verifies; afterwards parks so
 * the verdict stays on the wire.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre The HS host pins, expander switch, and PLL are up (main).
 * @post On success the pass counter and LED2 are latched.
 * @post Retries forever otherwise; each failure prints its step.
 *
 * @note Blocking calls; ms timeouts via ra8_time.
 * @since 0.1.0
 */
static VOID hid_host_worker(ULONG arg)
{
  (void)arg;

  tx_thread_sleep(k_hid_boot_wait_ticks);
  for (;;) {
    const ra8_err_t err = hid_host_pass();
    if (err == k_ra8_ok) {
      break;
    }
    tx_thread_sleep(k_hid_retry_ticks);
  }
  while (1) {
    tx_thread_sleep(k_hid_idle_ticks);
  }
}

/**
 * @brief ThreadX application-define hook. Spawns both workers.
 *
 * @details Creates the activation semaphore, then the device worker at
 * priority 8 and the host worker at 24 (below the USBX class threads).
 * Sets ::s_tx_kernel_up so SysTick may feed ThreadX from here on.
 *
 * @param[in] first_unused_memory Sentinel (unused; static stacks).
 *
 * @pre Called from ``tx_kernel_enter`` after scheduler init.
 * @pre Static stacks are reserved at file scope.
 * @post ::s_usb_host_keyboard_hid_active_sem exists and two auto-start workers
 *       are queued.
 * @post ``s_tx_kernel_up`` is true.
 *
 * @note Called once at boot; not thread-safe.
 * @since 0.1.0
 */
VOID tx_application_define(VOID* first_unused_memory)
{
  (void)first_unused_memory;
  s_tx_kernel_up = true;
  (void)tx_semaphore_create(&s_usb_host_keyboard_hid_active_sem, (CHAR*)"hid_active", 0U);
  usb_host_keyboard_device_thread_create();
  (void)tx_thread_create(&s_host_thread,
                         "hid_host",
                         hid_host_worker,
                         0UL,
                         s_host_stack,
                         k_hid_host_stack,
                         (UINT)k_hid_host_priority,
                         (UINT)k_hid_host_priority,
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
static void hid_panic_halt(void)
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
 * @pre Called once from ::hid_setup_or_halt.
 * @post FS pins carry the device role, HS pins the host role.
 * @post PD07 is HIGH (J7 powered).
 *
 * @note Panic-halts on any routing failure.
 * @since 0.1.0
 */
static void hid_route_usb_or_halt(void)
{
  if (ra8_pfs_route_peripheral(k_hid_pin_fs_vbus, k_ra8_psel_usb_fs, "hid.fs_vbus") != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_gpio_output_init(k_hid_pin_fs_vbusen, k_ra8_level_low) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_hid_pin_fs_dp, k_ra8_psel_usb_fs, "hid.fs_dp") != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_hid_pin_fs_dm, k_ra8_psel_usb_fs, "hid.fs_dm") != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_board_io_expander_set_usbhs_host_mode() != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_gpio_output_init(k_hid_pin_hs_pwr, k_ra8_level_high) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_pfs_route_peripheral(k_hid_pin_hs_vbus, k_ra8_psel_usb_hs, "hid.hs_vbus") != k_ra8_ok) {
    hid_panic_halt();
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
static void hid_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if (ra8_cgc_init() != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_cgc_usbfs_clock_enable() != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_cgc_usbhs_pll_enable() != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_hid_baud) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    hid_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    hid_panic_halt();
  }
  hid_route_usb_or_halt();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry: bring the board up, then hand off to ThreadX.
 *
 * @details Both USB controllers' clocks and pins come up before the
 * kernel so the workers only deal with stack bring-up.
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
  hid_setup_or_halt();

  ra8_isr_globals_enable();

#ifndef RA8_OFF_TARGET
  tx_kernel_enter();
#endif

  hid_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
