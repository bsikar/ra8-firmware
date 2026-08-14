/**
 * @file ra8_board_ek_ra8d2_peripherals.h
 * @brief I/O-expander, USB, camera, external-memory, MIPI-DSI, UART console,
 *        and Ethernet portion of the EK-RA8D2 v1 board-support layer.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Sub-header of ``ra8_board_ek_ra8d2.h`` (the thin umbrella). Carries the
 * U15 PI4IOE5V6408 I/O-expander SW4 override functions (Section 6c), the
 * USB-HS / USB-FS routing (Section 7), the parallel-camera connector J35
 * (Section 8), the Octo-SPI flash + SDRAM external memory (Section 9), the
 * MIPI-DSI graphics expansion port J32 (Section 10), the J-Link OB VCOM
 * serial bridge (Section 11), and the on-board RGMII Ethernet PHY
 * (Section 12).
 *
 * Every declaration here was moved VERBATIM out of ``ra8_board_ek_ra8d2.h``;
 * no contract, Doxygen block, or HUM/UM citation has changed. The enum
 * that backs the SW4-layout output bytes
 * (``ra8_board_pi4ioe_project_t``) lives in
 * ``ra8_board_ek_ra8d2_connectors.h``, pulled in below so the I/O-expander
 * functions resolve their referenced constants. Consumers keep including
 * ``ra8_board_ek_ra8d2.h``; this file is pulled in by that umbrella and
 * should not be included directly.
 *
 * Authoritative source: ``docs/reference/ek-ra8d2-v1-users-manual.pdf``
 * (Rev 1.01, R20UT5523EG0101, October 2025).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_err.h"
#include "ra8_port_constants.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Clock rates published by the standard EK-RA8D2 bring-up. */
typedef struct {
  uint32_t cpuclk0_hz; /**< Main Cortex-M85 clock rate in hertz. */
  uint32_t pclka_hz;   /**< Peripheral clock A rate in hertz.   */
} ra8_board_clock_rates_t;

/**
 * @brief Initialize the standard EK-RA8D2 clock tree and return its rates.
 * @details Keeps chip-specific CGC selection inside the board-composition
 *          layer while exposing the fixed rates needed by application-owned
 *          timebases and peripheral configurations.
 *
 * @param[out] out_rates Initialized CPUCLK0 and PCLKA rates.
 * @return Error code from the board clock bring-up.
 * @retval k_ra8_ok Clock initialization succeeded and @p out_rates is valid.
 * @retval k_ra8_err_invalid_arg @p out_rates is null.
 * @retval other Propagated clock-generator initialization error.
 *
 * @pre Called from single-threaded board initialization.
 * @post On success, the standard PLL1 clock tree is active.
 * @post On failure, @p out_rates is unchanged when nonnull.
 * @note Not thread-safe; call once during board initialization.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_clocks_init(ra8_board_clock_rates_t* out_rates);

/* =============================================================================
 * 6c. Project SW4 layout (UM Section 4.3 Table 3 p 16 + Table 18 p 26)
 *     -- I/O-expander override functions (the ra8_board_pi4ioe_project_t
 *        enum that names the output bytes lives in the connectors header).
 * =============================================================================
 */

/**
 * @brief Program the U15 I/O expander to apply the project's SW4 layout.
 *
 * @details
 * Equivalent to ``ra8_board_io_expander_set_usbhs_device_mode`` but
 * writes ``k_ra8_board_pi4ioe_output_project_default`` (0xF2) to the
 * expander's output register instead of 0xFF. After the write the
 * board is forced into "Pmod1 UART, Octo-SPI inactive, Arduino +
 * mikroBUS active, I2C on mikroBUS, USBHS Device" regardless of the
 * physical DIP positions.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                All three U15 register writes succeeded.
 * @retval k_ra8_err_gpio_conflict P512/P511 already owned by another driver.
 * @retval k_ra8_err_hw_init_failed RIIC1 initialization failed.
 * @retval k_ra8_err_nack          U15 didn't ACK the register write.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra8_mstp_init`` has run.
 * @post P512/P511 are routed to SCL1/SDA1; RIIC1 is initialized at
 *       100 kHz; U15.P0..P7 are configured as outputs driven to
 *       ``k_ra8_board_pi4ioe_output_project_default``.
 *
 * @note Not thread-safe; call once from the boot context before any
 *       Pmod / Arduino / mikroBUS init that depends on SW4 routing.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_io_expander_apply_project_sw4_defaults(void);

/**
 * @brief Program an exact U15 SW4 override byte in one bring-up sequence.
 *
 * @details Writes the requested output latch before enabling U15's outputs,
 * avoiding an intermediate board-mux layout. Bit n = 1 requests SW4-(n+1)
 * OFF and bit n = 0 requests it ON.
 *
 * @param[in] output_byte Exact byte for U15's output register.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok U15 is driving @p output_byte.
 * @retval k_ra8_err_gpio_conflict RIIC1 pins are already owned.
 * @retval k_ra8_err_hw_init_failed RIIC1 initialization failed.
 * @retval k_ra8_err_nack U15 did not acknowledge a register write.
 *
 * @pre IOPORT is powered and `ra8_mstp_init` has run.
 * @post RIIC1 is initialized at 100 kHz and U15 drives @p output_byte.
 * @note Not thread-safe; call once from boot context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_io_expander_apply_sw4(uint8_t output_byte);

/**
 * @brief Override selected SW4 positions while releasing every other position.
 *
 * @param[in] output_byte Output latch values using the SW4 active-low convention.
 * @param[in] output_mask Bit mask of U15 pins to drive; clear bits remain inputs.
 * @return ra8_err_t Error code from RIIC1 or U15 programming.
 * @post U15's output latch is @p output_byte and IODIR is @p output_mask.
 * @note Not thread-safe; call once from boot context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_io_expander_apply_sw4_mask(uint8_t output_byte,
                                                             uint8_t output_mask);

/**
 * @brief Program the U15 I/O expander to request the Octo-SPI SW4 layout.
 *
 * @details
 * Writes ``k_ra8_board_pi4ioe_output_octospi_active`` (0xF8, the all-SW4-OFF
 * layout with OSPI_OE_L asserted -- see that enum's definition) to the U15
 * output register with the port set to outputs.
 *
 * IMPORTANT -- this does NOT, and CANNOT, connect the flash. A firmware
 * sweep over the entire U15 output space (all 256 output bytes, plus
 * released-inputs / Hi-Z / per-bit single-line overrides; 2026-06, issue
 * #44, docs/HARDWARE_BRINGUP.md) re-read the 1S JEDEC ID after each config
 * and saw zero change: the flash stays silent (bus at the board pull-ups)
 * for every U15 state. The U15 expander is therefore a pure SW4 *sense /
 * override* whose GPIOs are NOT in the OSPI DQ/CK/CS path; the flash is
 * gated only by the SW4-3 **analog mux**, which is hardware-only and not
 * reachable from firmware. (When U15 is released to inputs, reg 0x0F reads
 * 0xF8, but the exact U15<->SW4 bit mapping is not published in the UM -- it
 * lives in the EK-RA8D2 Design Package schematic -- and has not yet been
 * verified on this EVM, so that value is NOT actionable.) This call is kept
 * only as an inert courtesy write; it has no bearing on flash reachability.
 * See ``examples/.../flash_journal/README.md`` and issue #44.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                All U15 register writes succeeded.
 * @retval k_ra8_err_busy          RIIC1 bus was held by another controller.
 * @retval k_ra8_err_hw_timeout    U15 did not respond on RIIC1.
 * @retval k_ra8_err_nack          U15 didn't ACK the register write.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra8_mstp_init`` has run.
 * @post P512/P511 are routed to SCL1/SDA1; RIIC1 is initialized at
 *       100 kHz; U15.P0..P7 are outputs driven to
 *       ``k_ra8_board_pi4ioe_output_octospi_active`` (inert w.r.t. the OSPI
 *       bus, which is gated by the hardware-only SW4-3 mux -- see @details).
 *
 * @note Not thread-safe; call once from the boot context before the
 *       Octo-SPI bring-up.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_io_expander_set_octospi_active(void);

/* =============================================================================
 * 7. USB (UM Section 5.4.1 + 6.2, Tables 22 + 28, p 30 + 34)
 * =============================================================================
 */

/**
 * @brief USBHS host/device routing per UM Table 28 p 34.
 *
 * @details
 * USBHS (J7, USB Type-C). The HS PHY DM/DP differential pair
 * (USBH_P/USBH_N) is internal to the chip and not a board-routed
 * port pin. VBUS sense (USBHS_cVBUS_CON) and CC pins are PHY-side.
 * Only the bus signals listed below are exposed to user firmware.
 *
 * USB-FS (J11, USB Type-C, UM Table 22 p 30) is the host/dev port
 * for the full-speed peripheral and follows the same arrangement.
 */
typedef enum : uint16_t {
  /* The HS D+/D- differential pair (USBH_P/USBH_N) is internal to the chip,
   * so only the two MCU-driven board signals below are port pins: the VBUS
   * sense input and the J7 host-power-switch enable. */
  k_ra8_board_usbhs_pin_vbus =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_8), /**< P4_08 USBHS_VBUS sense. UM Table 28 p 34. */
  k_ra8_board_usbhs_pin_pwr = (uint16_t)RA8_PIN(
    k_ra8_port_13,
    k_ra8_pin_7), /**< PD07 J7 host-power switch (HIGH = U18 drives 5 V VBUS). UM 6.2. */
} ra8_board_usbhs_pin_t;

/**
 * @brief USB-FS (J11) board-routed port pins, UM Table 22 p 30.
 *
 * @details
 * The full-speed peripheral on J11 (USB Type-C) exposes four board-routed
 * pins: the D+/D- differential pair, the VBUS sense input, and the
 * MCU-driven VBUS-enable GPIO. Route D+/D-/VBUS to the USBFS peripheral
 * function (``k_ra8_psel_usb_fs``) and drive VBUSEN as a GPIO output
 * (LOW = device role, HIGH = supply 5 V VBUS for host role).
 *
 * These are the single source of truth for the EK-RA8D2 USB-FS pinout:
 * applications reference these names instead of re-encoding the port/pin
 * pair, so the board fact lives in exactly one place.
 */
typedef enum : uint16_t {
  k_ra8_board_usbfs_pin_dp =
    (uint16_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_14), /**< P8_14 D+. UM Table 22 p 30. */
  k_ra8_board_usbfs_pin_dm =
    (uint16_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_15), /**< P8_15 D-. UM Table 22 p 30. */
  k_ra8_board_usbfs_pin_vbus =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_7), /**< P4_07 VBUS sense. UM Table 22 p 30. */
  k_ra8_board_usbfs_pin_vbusen =
    (uint16_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_0), /**< P5_00 VBUSEN GPIO. UM Table 22 p 30. */
} ra8_board_usbfs_pin_t;

/**
 * @brief IIC_B / I3C channel-0 board pins (SCL0 / SDA0), UM Table 20 p 28.
 *
 * @details
 * The I3C ch0 I2C-compatibility bus is routed to P400 (SCL0) and P401 (SDA0)
 * when SW4-5 selects I3C mode (UM Table 20 p 28 + Section 5.5.3 p 32). These
 * two pads double as SDHI0 CMD/CLK under the default I2C-mode routing, so use
 * these names only when the board is jumpered for the I3C bus.
 */
typedef enum : uint16_t {
  k_ra8_board_i3c0_pin_scl =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_0), /**< P400 SCL0. UM Table 20 p 28. */
  k_ra8_board_i3c0_pin_sda =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_1), /**< P401 SDA0. UM Table 20 p 28. */
} ra8_board_i3c0_pin_t;

/**
 * @brief Drive the U15 PI4IOE5V6408 I/O expander to select USB-HS device mode.
 *
 * @details
 * The EK-RA8D2 v1 carries a PI4IOE5V6408 8-bit I2C I/O expander at U15
 * (I2C address 0x43, EK-RA8D2 v1 UM Rev 1.01 Section 5.5.3 p 32 +
 * Section 4 p 16). U15 sits in parallel with the eight DIP switches of
 * SW4 -- when configured as outputs, U15's port pins override SW4 and
 * gate the same on-board mux that SW4 drives, including SW4-8 which
 * selects USB function on J7 (USB-HS): OFF = Device, ON = Host.
 *
 * U15 register convention (from the PI4IOE5V6408 datasheet register map):
 *  - 0x01 Device-ID  (expect 0xA0 or 0xA2)
 *  - 0x03 I/O direction      (1 = output)
 *  - 0x05 Output state       (1 = HIGH = SW4 OFF, 0 = LOW = SW4 ON)
 *  - 0x07 Output Hi-Z        (1 = Hi-Z)
 *  - 0x0D Pull-up / pull-down select
 *
 * Polarity: the PI4IOE5V6408 datasheet defines a HIGH output bit as the
 * released (pulled-up) level, so SW4-8 OFF (the silk-screen "Device"
 * position) corresponds to U15.P7 = 1. The exact bit<->SW4-channel mapping is
 * not in the UM (it is in the Design Package schematic) and is pending
 * on-hardware verification on this EVM. We write 0xFF (all bits HIGH = all
 * SW4 channels in their
 * default OFF position) which puts USB-HS into Device mode and leaves
 * the other muxed peripherals at their EK-RA8D2 default routing.
 *
 * Routes P512 -> SCL1 and P511 -> SDA1 and initializes RIIC channel 1 at
 * 100 kHz.
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok All three register writes succeeded; U15 is driving
 *                 SW4-8 = OFF (Device mode).
 * @retval k_ra8_err_gpio_conflict P512/P511 already owned.
 * @retval k_ra8_err_hw_init_failed RIIC1 initialization failed.
 * @retval k_ra8_err_nack U15 didn't ACK the register write.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra8_mstp_init`` has run.
 * @post P512/P511 are routed to SCL1/SDA1; RIIC1 is initialized at
 *       100 kHz; U15.P0..P7 are configured as outputs driven HIGH.
 *
 * @note Not thread-safe; call once from the boot context immediately
 *       before ``ra8_board_usbhs_device_init``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_io_expander_set_usbhs_device_mode(void);

/**
 * @brief Drive the U15 I/O expander to select USB-HS HOST mode.
 *
 * @details
 * Same U15 mechanism as ``ra8_board_io_expander_set_usbhs_device_mode``
 * (see that function for the expander register convention), but writes
 * ``k_ra8_board_pi4ioe_output_usbhs_host`` (0x72): the project-default
 * SW4 layout with bit 7 (SW4-8, USBHS role) driven LOW = ON = Host.
 * In the host position the board's J7 VBUS switch supplies bus power
 * to an attached device; in the default OFF/device position J7 expects
 * VBUS from an external host and an attached USB stick stays dark.
 *
 * @return ``ra8_err_t`` Error code.
 * @retval k_ra8_ok U15 is driving SW4-8 = ON (Host mode).
 * @retval k_ra8_err_gpio_conflict P512/P511 already owned.
 * @retval k_ra8_err_hw_init_failed RIIC1 initialization failed.
 * @retval k_ra8_err_nack U15 didn't ACK a register write.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra8_mstp_init`` has run.
 * @post U15.P0..P7 are outputs driving 0x72; J7 supplies VBUS.
 * @post P512/P511 are routed to SCL1/SDA1 with RIIC1 at 100 kHz.
 *
 * @note Not thread-safe; call once from the boot context before the
 *       USB host bring-up.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_io_expander_set_usbhs_host_mode(void);

/**
 * @brief Bring the chip USBHS module up in device mode (HS PHY).
 *
 * @retval k_ra8_ok / k_ra8_err_not_supported (until USBHS HAL lands)
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_usbhs_device_init(void);

/**
 * @brief Bring the chip USBHS module up in host mode.
 *
 * @retval k_ra8_ok / k_ra8_err_not_supported (until USBHS HAL lands)
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_usbhs_host_init(void);

/* =============================================================================
 * 8. Camera connector J35 (UM Section 8.3, Tables 35 + 36, p 48 + 49)
 * =============================================================================
 */

/**
 * @enum ra8_board_camera_pin_t
 * @brief Pins routed to the parallel-camera connector J35 (DVP mode).
 *
 * @details
 * UM Table 35 ("Camera Expansion Port Assignments in Parallel mode,
 * SW4-6 ON"). Goes into the chip CEU peripheral. P405/P406 are
 * shared with the audio CODEC -- see ``ra8_board_audio_pin_t`` and
 * jumper J41. P902/PB02/PB03/PB04 are also shared with the parallel
 * graphics expansion port, so DVP camera and parallel TFT are
 * mutually exclusive.
 */
typedef enum : uint16_t {
  k_ra8_board_cam_d0 =
    (uint16_t)RA8_PIN(k_ra8_port_4,
                      k_ra8_pin_0), /**< CAM D0,    P400. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_d1 =
    (uint16_t)RA8_PIN(k_ra8_port_9,
                      k_ra8_pin_2), /**< CAM D1,    P902. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_d2 =
    (uint16_t)RA8_PIN(k_ra8_port_4,
                      k_ra8_pin_5), /**< CAM D2,    P405 (J41). EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_d3 =
    (uint16_t)RA8_PIN(k_ra8_port_4,
                      k_ra8_pin_6), /**< CAM D3,    P406 (J41). EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_d4 =
    (uint16_t)RA8_PIN(k_ra8_port_7,
                      k_ra8_pin_0), /**< CAM D4,    P700. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_d5 =
    (uint16_t)RA8_PIN(k_ra8_port_7,
                      k_ra8_pin_1), /**< CAM D5,    P701. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_d6 =
    (uint16_t)RA8_PIN(k_ra8_port_7,
                      k_ra8_pin_2), /**< CAM D6,    P702. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_d7 =
    (uint16_t)RA8_PIN(k_ra8_port_7,
                      k_ra8_pin_3), /**< CAM D7,    P703. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_vsync =
    (uint16_t)RA8_PIN(k_ra8_port_11,
                      k_ra8_pin_2), /**< CAM VSYNC, PB02. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_hsync =
    (uint16_t)RA8_PIN(k_ra8_port_11,
                      k_ra8_pin_3), /**< CAM HSYNC, PB03. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_pclk =
    (uint16_t)RA8_PIN(k_ra8_port_11,
                      k_ra8_pin_4), /**< CAM PCLK,  PB04. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_xclk =
    (uint16_t)RA8_PIN(k_ra8_port_5,
                      k_ra8_pin_1), /**< CAM XCLK,  P501. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_rst =
    (uint16_t)RA8_PIN(k_ra8_port_7,
                      k_ra8_pin_9), /**< CAM RST,   P709. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_int =
    (uint16_t)RA8_PIN(k_ra8_port_0,
                      k_ra8_pin_10), /**< CAM INT (IRQ-14), P010. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_i2c_sda =
    (uint16_t)RA8_PIN(k_ra8_port_5,
                      k_ra8_pin_11), /**< CAM I2C SDA (SDA1), P511. EK-RA8D2 UM Table 35 p 48. */
  k_ra8_board_cam_i2c_scl =
    (uint16_t)RA8_PIN(k_ra8_port_5,
                      k_ra8_pin_12), /**< CAM I2C SCL (SCL1), P512. EK-RA8D2 UM Table 35 p 48. */
} ra8_board_camera_pin_t;

/** @brief RIIC controller channel wired to the J35 camera SCCB bus. */
typedef enum : uint8_t {
  k_ra8_board_camera_i2c_channel = 1U, /**< RIIC1 serves J35 SCCB. */
} ra8_board_camera_bus_t;

/**
 * @brief Start the J35 camera sensor input clock (XCLK) on P501/GTIOC12A.
 * @details Configures GPT12 saw-PWM from PCLKD and routes its A output to P501.
 * @param[in] frequency_hz Requested non-zero camera input-clock frequency.
 * @return Error code.
 * @retval k_ra8_ok GPT12 is running and P501 is routed.
 * @retval k_ra8_err_invalid_arg The requested divisor is not representable.
 * @retval other Propagated clock, GPT, or pin-routing error.
 * @pre CGC and module-stop services are initialized.
 * @pre P501 is available for the camera clock function.
 * @post On success GPT12 runs continuously at the nearest integer divisor.
 * @post On success P501 uses the high-speed GPT peripheral drive setting.
 * @note Init-context only; this claims shared board resources.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_camera_xclk_start(uint32_t frequency_hz);

/**
 * @brief Drive only SW4-6 ON through U15, selecting J35 parallel DVP.
 * @details Applies a masked U15 override without changing unrelated switches.
 * @return Error code.
 * @retval k_ra8_ok U15 selected the parallel-camera path.
 * @retval other Propagated expander or RIIC error.
 * @pre Board I/O-expander services and RIIC1 are available.
 * @pre The Camera Expansion Board is connected at J35.
 * @post On success SW4-6 is overridden to its ON state.
 * @post Other U15 SW4 override bits retain their prior values.
 * @note Init-context only; this changes shared physical routing.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_camera_select_parallel(void);

/**
 * @brief Route J35 D[7:0], VSYNC, HSYNC and PCLK to the CEU peripheral.
 * @details Claims and muxes the eleven parallel-camera pads in board order.
 * @return Error code.
 * @retval k_ra8_ok Every parallel-camera pin was routed.
 * @retval other First propagated PFS routing error.
 * @pre SW4-6 selects the parallel-camera path.
 * @pre The eleven J35 CEU pins are not claimed by another peripheral.
 * @post On success every listed pin uses the CEU peripheral function.
 * @post On failure no later pin in the routing list is touched.
 * @note Init-context only; partial routing may remain after an error.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_camera_route_parallel_pins(void);

/**
 * @brief Pulse the J35 camera reset input and leave the sensor released.
 *
 * @details Claims P709 as an output, holds reset low, then releases it high.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok P709 was driven low, then high.
 * @retval other Forwarded GPIO initialization/write error.
 *
 * @pre The IOPORT module is powered.
 * @pre P709 is available for the camera reset function.
 * @post CAM_RST is high after two 20 ms settle intervals.
 * @post On success the reset pin remains claimed as a GPIO output.
 * @note Not thread-safe; call from camera initialization context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_camera_reset(void);

/**
 * @brief Read one 16-bit-addressed SCCB register on the J35 camera bus.
 *
 * @details Signature intentionally matches the transport callback used by
 * `ra8_ov5640`; the BSP remains independent of that optional sensor library.
 *
 * @param[in] ctx Unused transport context; may be `nullptr`.
 * @param[in] address 7-bit SCCB target address.
 * @param[in] reg 16-bit sensor register address.
 * @param[out] out_value Register value on success.
 * @return ra8_err_t Forwarded RIIC result.
 * @retval k_ra8_ok One register byte was read.
 * @retval k_ra8_err_null_ptr @p out_value was `nullptr`.
 * @retval other Propagated RIIC transfer error.
 * @pre RIIC1 is initialized for the J35 SCCB bus.
 * @pre The sensor is clocked and released from reset.
 * @post On success @p out_value contains the addressed register byte.
 * @post The RIIC bus is idle after the completed transfer.
 * @note Not thread-safe with respect to RIIC1.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_board_camera_sccb_read_reg(void* ctx, uint8_t address, uint16_t reg, uint8_t* out_value);

/**
 * @brief Write one 16-bit-addressed SCCB register on the J35 camera bus.
 *
 * @param[in] ctx Unused transport context; may be `nullptr`.
 * @param[in] address 7-bit SCCB target address.
 * @param[in] reg 16-bit sensor register address.
 * @param[in] value Register value to write.
 * @return ra8_err_t Forwarded RIIC result.
 * @retval k_ra8_ok The complete address and value payload was written.
 * @retval other Propagated RIIC write error.
 * @pre RIIC1 is initialized for the J35 SCCB bus.
 * @pre The sensor is clocked and released from reset.
 * @post On success the target accepted the addressed register byte.
 * @post The RIIC bus is idle after the completed transfer.
 * @note Not thread-safe with respect to RIIC1.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_board_camera_sccb_write_reg(void* ctx, uint8_t address, uint16_t reg, uint8_t value);

/**
 * @brief Millisecond delay adapter for transport-independent camera drivers.
 * @details Ignores @p ctx and delegates the bounded delay to `ra8_delay_ms`.
 * @param[in] ctx Unused transport context; may be `nullptr`.
 * @param[in] milliseconds Delay duration.
 * @return Nothing.
 * @retval none This function cannot report an error.
 * @pre The SysTick timebase is initialized.
 * @pre @p milliseconds is acceptable to the platform delay service.
 * @post At least the requested delay interval has elapsed.
 * @post No camera or transport state is modified directly.
 * @note Blocking and not suitable for interrupt context.
 * @since 0.1.0
 */
void ra8_board_camera_delay_ms(void* ctx, uint32_t milliseconds);

/* =============================================================================
 * 9. Octo-SPI flash + SDRAM (UM Section 6.3 + 6.4, Tables 29 + 30, p 35 + 36)
 * =============================================================================
 */

/**
 * @brief Memory-mapped base addresses for off-chip XSPI / SDRAM.
 *
 * @details
 * Per UM Section 6.3 (Octo-SPI flash, IS25LX512M-JHLE 64 MB on the
 * Octo-SPI peripheral) and Section 6.4 (IS42S32160F-6BLI 64 MB
 * SDRAM on the SDRAMC bus). Bases are documented in the chip
 * Hardware User's Manual memory map; the board UM only specifies
 * the parts and their pin connections.
 */
typedef enum : uintptr_t {
  k_ra8_board_xspi_flash_base = 0x80000000UL, /**< 64 MB IS25LX512M (chip HUM memory map). */
  k_ra8_board_sdram_base =
    0x68000000UL, /**< 64 MB IS42S32160F (per linker scripts + ra8_sdramc.c). */
} ra8_board_extmem_base_t;

/** @brief Sizes of the off-chip memories, in bytes. */
typedef enum : uint32_t {
  k_ra8_board_xspi_flash_size = 0x04000000UL, /**< 64 MiB. UM Section 6.3 p 35. */
  k_ra8_board_sdram_size      = 0x04000000UL, /**< 64 MiB. UM Section 6.4 p 36. */
} ra8_board_extmem_size_t;

/**
 * @enum ra8_board_xspi_pin_t
 * @brief Pins routed to the on-board IS25LX512M Octo-SPI flash.
 *
 * @details
 * UM Table 29 ("Octo-SPI Flash Assignments"). Selection is gated by
 * SW4-3 (Octo-SPI vs Arduino/Pmod1). The chip-select strobe is
 * OSPI_FLASH_S_L, mapped to P104.
 */
typedef enum : uint16_t {
  k_ra8_board_xspi_cs =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_4), /**< OSPI_FLASH_S_L, P104. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_clk =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_8), /**< OSPI_FLASH_C,   P808. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_dqs =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_1), /**< OSPI_FLASH_DQS, P801. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_reset =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_6), /**< OSPI_FLASH_RESET_L, P106. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_dq0 =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_0), /**< OSPI_FLASH_DQ0, P100. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_dq1 =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_3), /**< OSPI_FLASH_DQ1, P803. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_dq2 =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_3), /**< OSPI_FLASH_DQ2, P103. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_dq3 =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_1), /**< OSPI_FLASH_DQ3, P101. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_dq4 =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_2), /**< OSPI_FLASH_DQ4, P102. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_dq5 =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_0), /**< OSPI_FLASH_DQ5, P800. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_dq6 =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_2), /**< OSPI_FLASH_DQ6, P802. EK-RA8D2 UM Table 29 p 35. */
  k_ra8_board_xspi_dq7 =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_4), /**< OSPI_FLASH_DQ7, P804. EK-RA8D2 UM Table 29 p 35. */
} ra8_board_xspi_pin_t;

/**
 * @brief Route the 12 OCTA bus pins to the xSPI peripheral and pulse RESET_L.
 *
 * @details
 * Configures CS / CK / DQS / DQ0..DQ7 under PSEL=k_ra8_psel_qspi (0x1C)
 * per EK-RA8D2 UM Table 29 (p 35), then drives the active-low RESET
 * strap as a GPIO output: low for >= tRLRH, high for >= tRHSL before
 * the first xSPI command. Must run BEFORE any `ra8_xspi_*` call.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok All pins routed and reset strap pulsed.
 * @retval k_ra8_err_invalid_arg PFS programming rejected an entry.
 *
 * @pre IOPORT MSTP cleared (ra8_pfs_init has run).
 * @pre Single-threaded init context.
 * @post 12 OCTA pins are under PSEL=0x1C.
 * @post RESET_L is high; the IS25LX512M is ready for its first command.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_xspi_pins_init(void);

/* =============================================================================
 * 10. MIPI-DSI graphics expansion port J32
 *     (UM Section 8.2, Table 34, page 45)
 * =============================================================================
 */

/**
 * @enum ra8_board_mipi_dsi_pin_t
 * @brief MIPI-DSI display-connector control pins (J32).
 *
 * @details
 * UM Table 34 ("MIPI Graphics Expansion Port Assignments"). The
 * high-speed DSI clock + 2 data lanes (MIPI_DSI_CL_P/N,
 * MIPI_DSI_DL0_P/N, MIPI_DSI_DL1_P/N) are dedicated MIPI-PHY balls
 * and are not exposed as general-purpose port pins; only the
 * low-speed control side (TE / reset / backlight / I2C touch /
 * touch INT) is BSP-addressable.
 */
typedef enum : uint16_t {
  k_ra8_board_mipi_dsi_te =
    (uint16_t)RA8_PIN(k_ra8_port_4,
                      k_ra8_pin_11), /**< DSI tearing-effect, P411. UM Table 34 p 45. */
  k_ra8_board_mipi_dsi_reset_n =
    (uint16_t)RA8_PIN(k_ra8_port_6, k_ra8_pin_6), /**< DSI DISP_RST, P606. UM Table 34 p 45. */
  k_ra8_board_mipi_dsi_backlight =
    (uint16_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_14), /**< DSI DISP_BLEN, P514. UM Table 34 p 45. */
  k_ra8_board_mipi_dsi_touch_int =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_11), /**< DSI DISP_INT (IRQ-19), P111. UM Table 34 p 45. */
  k_ra8_board_mipi_dsi_i2c_sda =
    (uint16_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_11), /**< DSI I2C SDA1, P511. UM Table 34 p 45. */
  k_ra8_board_mipi_dsi_i2c_scl =
    (uint16_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_12), /**< DSI I2C SCL1, P512. UM Table 34 p 45. */
} ra8_board_mipi_dsi_pin_t;

/**
 * @brief Bring the MIPI-DSI link up (PHY + DSI controller).
 *
 * @retval k_ra8_ok / k_ra8_err_not_supported (until MIPI-DSI HAL lands)
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_mipi_dsi_init(void);

/* =============================================================================
 * 11. J-Link OB VCOM serial bridge (UM Section 5.2.4, Table 13, page 24)
 * =============================================================================
 */

/**
 * @enum ra8_board_uart_t
 * @brief Identifiers for board-routed UART consoles.
 *
 * @details
 * The EK-RA8D2 v1 carries the J-Link OB debug MCU's USB-CDC virtual-COM
 * bridge on chip pins PD02 (TXD) / PD03 (RXD) per UM Table 13 p 24.
 * Hardware-flow-control lines PD04 (RTS) and PD05 (CTS) are present on
 * the same connector but are gated by trace-cut links E17 / E9 and are
 * not used by the basic console wiring.
 *
 * On the RA8D2 PD02/PD03 are the SCI8 TXD8/RXD8 alternate functions
 * (chip HUM "Multiplexed Pin Function Selector"); the BSP forwards to
 * SCI channel 8 via ``ra8_sci_init`` / ``ra8_sci_write_polling`` /
 * ``ra8_sci_getc_polling``.
 */
typedef enum : uint8_t {
  k_ra8_board_uart_console = 0U, /**< J-Link OB VCOM bridge: PD02 TXD / PD03 RXD on SCI8.
                                 *   EK-RA8D2 UM Table 13 p 24. */
} ra8_board_uart_t;

/**
 * @brief SCI channel that backs ``k_ra8_board_uart_console``.
 *
 * @details
 * Exposed as an enum (not a macro) so test code and applications can
 * reference the channel without re-encoding the magic number. The
 * value comes from PD02/PD03's Multiplexed Pin Function Selector
 * row in the chip HUM I/O Ports chapter (TXD8/RXD8 alternate).
 */
typedef enum : uint8_t {
  k_ra8_board_uart_console_sci_channel =
    8U, /**< PD02/PD03 -> SCI8 (verified on real EK-RA8D2 v1 silicon). */
} ra8_board_uart_sci_channel_t;

/**
 * @brief Pin assignments for the J-Link OB VCOM bridge (UM Table 13 p 24).
 *
 * @details
 * PD02 / PD03 are always wired to the debug MCU's CDC bridge. PD04 /
 * PD05 are the optional RTS / CTS lines (links E17 / E9, both closed
 * by default). Chip-coordinate names: P1302 / P1303 / P1304 / P1305
 * (port 13, pins 2..5).
 */
typedef enum : uint16_t {
  k_ra8_board_uart_console_pin_txd =
    (uint16_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_2), /**< PD02 TXD. UM Table 13 p 24. */
  k_ra8_board_uart_console_pin_rxd =
    (uint16_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_3), /**< PD03 RXD. UM Table 13 p 24. */
  k_ra8_board_uart_console_pin_rts =
    (uint16_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_4), /**< PD04 RTS (link E17). UM Table 13 p 24. */
  k_ra8_board_uart_console_pin_cts =
    (uint16_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_5), /**< PD05 CTS (link E9).  UM Table 13 p 24. */
} ra8_board_uart_console_pin_t;

/**
 * @brief Configure SCI8 + PD02/PD03 as the debug-console UART.
 *
 * @details
 * Routes PD02 -> TXD8 and PD03 -> RXD8 (PSEL = SCI async) and brings
 * SCI8 up via ``ra8_sci_init`` with 8N1 framing at the requested baud.
 * The SCI module operating clock on RA8D2 is PCLKA (chip HUM Ch 38.2
 * "SCI registers"); the BRR divisor is computed from the **current**
 * PCLKA frequency reported by ``ra8_cgc_get_clock_hz``, so callers must
 * call ``ra8_cgc_init()`` **before** ``ra8_board_uart_console_init``.
 * Applications that retune CGC afterwards must additionally call
 * ``ra8_sci_set_baud((uint8_t)k_ra8_board_uart_console_sci_channel,
 * baud, new_pclka_hz)`` to recompute the divisor.
 *
 * @param[in] baud Target line rate in bps (e.g. 115200).
 *
 * @retval k_ra8_ok                  Console up, ready to TX/RX.
 * @retval k_ra8_err_invalid_arg     baud == 0.
 * @retval k_ra8_err_not_initialized ``ra8_cgc_init`` has not yet published
 *                                  a usable PCLKA value (chip still on
 *                                  MOCO / pre-PLL).
 * @retval k_ra8_err_gpio_conflict   PD02 or PD03 already owned.
 * @retval k_ra8_err_hw_init_failed  Underlying ``ra8_sci_init`` failed.
 *
 * @pre HAL pin validator initialized (single-threaded boot context).
 * @pre ``ra8_cgc_init()`` has run and PCLKA is post-PLL.
 * @pre ra8_mstp_init() has run.
 * @post SCI8 is enabled with TE=RE=1; PD02/PD03 are routed to SCI8.
 * @post BRR computed against the live PCLKA value (no hardcoded clock).
 *
 * @note Not thread-safe; call once during board bring-up.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_uart_console_init(uint32_t baud);

/**
 * @brief Polled blocking write to the J-Link OB VCOM console.
 *
 * @param[in] data Bytes to transmit; must be non-NULL when ``len`` > 0.
 * @param[in] len  Number of bytes in ``data``.
 *
 * @retval k_ra8_ok                  All bytes pushed to TDR.
 * @retval k_ra8_err_invalid_arg     ``data`` NULL with non-zero ``len``.
 * @retval k_ra8_err_not_initialized ``ra8_board_uart_console_init`` not called.
 *
 * @pre ra8_board_uart_console_init succeeded.
 * @post All ``len`` bytes have been handed to the SCI8 TDR shift register.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_uart_console_write(const uint8_t* data, size_t len);

/**
 * @brief Polled non-blocking read from the J-Link OB VCOM console.
 *
 * @details
 * Drains up to ``cap`` bytes from SCI8 RDR while RDRF stays set. Stops
 * (without error) the first time RDRF clears so the call never blocks
 * waiting for a host that is silent.
 *
 * @param[out] out      Destination buffer (non-NULL when ``cap`` > 0).
 * @param[in]  cap      Capacity of ``out`` in bytes.
 * @param[out] out_len  Number of bytes actually read; non-NULL.
 *
 * @retval k_ra8_ok                  Read complete; *out_len in [0, cap].
 * @retval k_ra8_err_invalid_arg     out / out_len NULL with non-zero cap.
 * @retval k_ra8_err_not_initialized Console not initialized.
 *
 * @pre ra8_board_uart_console_init succeeded.
 * @post 0 <= *out_len <= cap.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_uart_console_read(uint8_t* out, size_t cap, size_t* out_len);

/**
 * @brief Block until every byte queued on the J-Link OB VCOM console
 *        has finished clocking out on the wire.
 *
 * @details
 * Thin wrapper around ``ra8_sci_flush(k_ra8_board_uart_console_sci_channel)``
 * that polls CSR.TEND on the SCI8 channel that backs the J-Link OB VCOM
 * bridge (HUM Ch 38.2.17 "CSR : Common Status Register", p 2225). The
 * intended caller is a panic-handler that needs the failure log to
 * reach the host before WFI gates the SCI clock and silently drops the
 * remaining FIFO contents.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok TEND observed (or fake stub).
 * @retval k_ra8_err_not_initialized ``ra8_board_uart_console_init`` not called.
 * @retval k_ra8_err_hw_timeout Spin budget elapsed without TEND.
 *
 * @pre ``ra8_board_uart_console_init`` succeeded.
 * @post On success, every byte previously passed to
 *       ``ra8_board_uart_console_write`` has been transmitted.
 *
 * @note Not thread-safe with respect to a concurrent
 *       ``ra8_board_uart_console_write`` -- the writer may refill the
 *       shift register while the flush is polling.
 *
 * @see ra8_sci_flush
 * @see ra8_board_uart_console_write
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_uart_console_flush(void);

/* =============================================================================
 * 12. On-board RGMII Ethernet PHY (UM Section 6.1, Tables 26 + 27, p 33-34)
 * =============================================================================
 */

/**
 * @brief Pin assignments for the on-board MaxLinear PEF7071 (GPY111) PHY.
 *
 * @details
 * Per UM Table 26 ("Ethernet Port Assignments") p 33. The PHY is wired
 * to the RA8D2 ETHERC0 / RMAC0 in **RGMII** mode (UM 6.1: "RGMII
 * Ethernet Physical Layer Transceiver"). The 25 MHz reference clock
 * is sourced from a discrete oscillator (ECS-250-10-37B-CTN-TR, UM
 * Table 27 p 34) connected directly to the PHY -- the MCU does not
 * drive REFCLK. Ethernet data rails are gated by trace-cut links
 * E18..E24, E33, E34, E36..E38 (all closed by default).
 */
typedef enum : uint16_t {
  k_ra8_board_eth_pin_mdint =
    (uint16_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_7), /**< MDINT, P107. UM Table 26 p 33. */
  k_ra8_board_eth_pin_mdc =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_15), /**< MDC,   P415. UM Table 26 p 33. */
  k_ra8_board_eth_pin_mdio =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_14), /**< MDIO,  P414. UM Table 26 p 33. */
  k_ra8_board_eth_pin_txd0 =
    (uint16_t)RA8_PIN(k_ra8_port_3, k_ra8_pin_7), /**< TXD0,  P307 (E21). UM Table 26 p 33. */
  k_ra8_board_eth_pin_txd1 =
    (uint16_t)RA8_PIN(k_ra8_port_3, k_ra8_pin_6), /**< TXD1,  P306 (E20). UM Table 26 p 33. */
  k_ra8_board_eth_pin_txd2 =
    (uint16_t)RA8_PIN(k_ra8_port_3, k_ra8_pin_5), /**< TXD2,  P305 (E19). UM Table 26 p 33. */
  k_ra8_board_eth_pin_txd3 =
    (uint16_t)RA8_PIN(k_ra8_port_3, k_ra8_pin_4), /**< TXD3,  P304 (E18). UM Table 26 p 33. */
  k_ra8_board_eth_pin_tx_ctl =
    (uint16_t)RA8_PIN(k_ra8_port_3, k_ra8_pin_10), /**< TX_CTL, P310 (E22). UM Table 26 p 33. */
  k_ra8_board_eth_pin_tx_clk =
    (uint16_t)RA8_PIN(k_ra8_port_3, k_ra8_pin_9), /**< TX_CLK, P309 (E23). UM Table 26 p 33. */
  k_ra8_board_eth_pin_rxd0 =
    (uint16_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_6), /**< RXD0,  P906 (E36). UM Table 26 p 33. */
  k_ra8_board_eth_pin_rxd1 =
    (uint16_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_7), /**< RXD1,  P907 (E34). UM Table 26 p 33. */
  k_ra8_board_eth_pin_rxd2 =
    (uint16_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_8), /**< RXD2,  P908 (E33). UM Table 26 p 33. */
  k_ra8_board_eth_pin_rxd3 =
    (uint16_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_9), /**< RXD3,  P909 (E24). UM Table 26 p 33. */
  k_ra8_board_eth_pin_rx_ctl =
    (uint16_t)RA8_PIN(k_ra8_port_2, k_ra8_pin_6), /**< RX_CTL, P206 (E38). UM Table 26 p 33. */
  k_ra8_board_eth_pin_rx_clk =
    (uint16_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_5), /**< RX_CLK, P905 (E37). UM Table 26 p 33. */
  k_ra8_board_eth_pin_rstn =
    (uint16_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_8), /**< RSTN,   P708.        UM Table 26 p 33. */
} ra8_board_eth_pin_t;

/**
 * @brief ETHA / RMAC port and PHY MDIO address for the on-board PHY.
 *
 * @details
 * Per UM 6.1 ("Ethernet interface") and the canonical FSP example
 * project ``ethernet_ek_ra8d2_ep`` (ra8_cfg.txt: "Channel: 1" for both
 * g_ether0 and g_rmac_phy0). The on-board PHY (MaxLinear PEF7071VV16-
 * LLHU, UM Table 27 p 34) is the only device on the MDIO bus and
 * powers up at MDIO address 0; the RA8D2 has two ETHA / RMAC
 * instances, of which **port 1** (not port 0) is the one wired to the
 * RJ45 J33 on the EK-RA8D2. Earlier revisions of this header pointed
 * at port 0 -- that was wrong; the chip-side MDIO controller never saw a
 * reply because the MDC / MDIO pads on RMAC0 are not routed to the
 * board's PHY.
 */
typedef enum : uint8_t {
  k_ra8_board_eth_etha_port = 1U, /**< ETHA1 (k_ra8_etha_port_1). UM 6.1. */
  k_ra8_board_eth_rmac_port = 1U, /**< RMAC1 (k_ra8_rmac_port_1). UM 6.1. */
  k_ra8_board_eth_phy_addr  = 0U, /**< MDIO addr of the on-board PHY (HW
                                  *   strap on PEF7071, UM Table 27 p 34). */
} ra8_board_eth_index_t;

/**
 * @brief Bring up the on-board RGMII Ethernet PHY (PEF7071) and RMAC0.
 *
 * @details
 * 1. Routes all sixteen Ethernet data / control pins (UM Table 26 p 33)
 *    to their ETHERC RGMII alternate functions via
 *    ``ra8_pfs_route_peripheral`` (PSEL = ``k_ra8_psel_ether_rmii`` --
 *    same PSEL slot covers RMII and RGMII on RA8D2; the per-pin mux
 *    is identical and the ESWM MIICR1.MIISEL field picks RGMII).
 * 2. Initialises ETHA0 with the default ``ra8_etha_config_t`` (RESET
 *    mode, no IRQs enabled) via ``ra8_etha_init``.
 * 3. Initialises RMAC0 with ``phy_interface = k_ra8_rmac_pis_gmii``,
 *    ``link_speed = k_ra8_rmac_lsc_1000mbit``, ``duplex = full`` via
 *    ``ra8_rmac_init`` (auto-negotiation overrides this once link
 *    comes up; RMAC.MPIC.PIS is GMII for 1 Gbps and MII for
 *    10/100 Mbps per HUM Table 29.11).
 *
 * The on-board PHY's 25 MHz reference is provided by an external
 * crystal oscillator (UM Table 27 p 34); no chip-side REFCLK output
 * programming is needed. Caller is responsible for driving RSTN
 * (P708) low for >= 10 us before this function and for starting
 * auto-negotiation via ``ra8_rmac_phy_auto_neg_start`` after it
 * returns.
 *
 * @retval k_ra8_ok                  PHY pins routed; ETHA0 + RMAC0 up.
 * @retval k_ra8_err_gpio_conflict   At least one Ethernet pin is owned.
 * @retval k_ra8_err_hw_init_failed  ETHA or RMAC init failed.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ra8_mstp_init() has run.
 * @post All sixteen Ethernet pins are in RGMII alternate-function mode.
 * @post ETHA0 and RMAC0 are initialized and ready for descriptor-ring
 *       configuration / auto-negotiation.
 *
 * @note Not thread-safe; call once during board bring-up.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_ethernet_init(void);

#ifdef __cplusplus
}
#endif
