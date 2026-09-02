/**
 * @file ra8_board_ek_ra8d2_audio_usb.c
 * @brief EK-RA8D2 BSP -- DA7212 audio CODEC, USB-HS, and U15 I/O expander
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Sibling translation unit of ``ra8_board_ek_ra8d2.c`` carrying the
 * board's audio + USB sub-responsibilities:
 *   - DA7212 audio CODEC link over SSIE0 + the IIC control pair
 *     (UM Table 32 p 38).
 *   - USB-HS device/host bring-up (CGC PHY clock + MSTP ungate) plus
 *     the PD07 role-select strap.
 *   - The U15 PI4IOE5V6408 I2C I/O expander used to override the SW4
 *     configuration switches (UM Section 5.5.3).
 *
 * The BSP itself never touches MCU registers; ``ra8_ssie_*`` /
 * ``ra8_i2c_*`` / ``ra8_mpc_*`` / ``ra8_usb_*`` / ``ra8_pfs_route_peripheral``
 * carry every register write. Source-of-truth for the pin tables is
 * ``docs/reference/ek-ra8d2-v1-users-manual.pdf`` (R20UT5523EG0101
 * Rev 1.01, October 2025).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_board_ek_ra8d2_audio_usb_internal.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_i2c.h"
#include "ra8_mpc.h"
#include "ra8_mstp.h"
#include "ra8_pfs_regs.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_ssie.h"
#include "ra8_time.h"
#include "ra8_usb.h"

/* =============================================================================
 * 4. Audio CODEC (UM Table 32, page 38)
 * =============================================================================
 */

/**
 * @brief Tracks whether ``ra8_board_audio_init`` has succeeded.
 */
static bool s_audio_initialized = false;

/**
 * @brief Supported PCM significant-bit-depth values for the DA7212 path.
 *
 * @details
 * Names every bit depth the BSP knows how to map to an SSIE DWL/SWL pair.
 * The DA7212 supports 16/24/32-bit native; 8/18/20/22 are accepted because
 * the SSIE itself supports those data-word widths and an application may
 * want to feed pre-padded streams.
 */
typedef enum : uint8_t {
  k_ra8_audio_bits_8  = 8U,  /**< RA8 audio bits 8.  */
  k_ra8_audio_bits_16 = 16U, /**< RA8 audio bits 16. */
  k_ra8_audio_bits_18 = 18U, /**< RA8 audio bits 18. */
  k_ra8_audio_bits_20 = 20U, /**< RA8 audio bits 20. */
  k_ra8_audio_bits_22 = 22U, /**< RA8 audio bits 22. */
  k_ra8_audio_bits_24 = 24U, /**< RA8 audio bits 24. */
  k_ra8_audio_bits_32 = 32U, /**< RA8 audio bits 32. */
} ra8_audio_bit_depth_t;

/**
 * @brief Sample-frame channel counts the BSP accepts.
 */
typedef enum : uint8_t {
  k_ra8_audio_channels_mono   = 1U, /**< RA8 audio channels mono.   */
  k_ra8_audio_channels_stereo = 2U, /**< RA8 audio channels stereo. */
} ra8_audio_channels_t;

/**
 * @brief Stereo-frame packing factor (two int16_t samples per 32-bit word).
 */
typedef enum : uint8_t {
  k_ra8_audio_samples_per_word = 2U, /**< RA8 audio samples per word. */
} ra8_audio_pack_t;

/* Map ``bit_depth`` (significant bits) to SSIE DWL / SWL pair -- see implementation for details. */
RA8_INTERNAL static ra8_err_t internal_audio_bits_to_word(uint8_t                 bit_depth,
                                                          ra8_ssie_data_word_t*   out_dwl,
                                                          ra8_ssie_system_word_t* out_swl)
{
  switch ((ra8_audio_bit_depth_t)bit_depth) {
    case k_ra8_audio_bits_8:
      *out_dwl = k_ra8_ssie_dwl_8;
      *out_swl = k_ra8_ssie_swl_8;
      return k_ra8_ok;
    case k_ra8_audio_bits_16:
      *out_dwl = k_ra8_ssie_dwl_16;
      *out_swl = k_ra8_ssie_swl_16;
      return k_ra8_ok;
    case k_ra8_audio_bits_18:
      *out_dwl = k_ra8_ssie_dwl_18;
      *out_swl = k_ra8_ssie_swl_24;
      return k_ra8_ok;
    case k_ra8_audio_bits_20:
      *out_dwl = k_ra8_ssie_dwl_20;
      *out_swl = k_ra8_ssie_swl_24;
      return k_ra8_ok;
    case k_ra8_audio_bits_22:
      *out_dwl = k_ra8_ssie_dwl_22;
      *out_swl = k_ra8_ssie_swl_24;
      return k_ra8_ok;
    case k_ra8_audio_bits_24:
      *out_dwl = k_ra8_ssie_dwl_24;
      *out_swl = k_ra8_ssie_swl_24;
      return k_ra8_ok;
    case k_ra8_audio_bits_32:
      *out_dwl = k_ra8_ssie_dwl_32;
      *out_swl = k_ra8_ssie_swl_32;
      return k_ra8_ok;
    default:
      return k_ra8_err_invalid_arg;
  }
}

/* Route the six DA7212 audio pins (4 SSIE + 2 IIC) to their alt fns -- see implementation for details. */
RA8_INTERNAL static ra8_err_t internal_audio_route_pins(void)
{
  /* k_ra8_psel_ssie comes from HUM Ch 19 PFS PSEL field. MCLK on PD06
   * stays in GPIO mode -- the application picks SSIE EXTAL or CGC
   * clock-out explicitly. */
  const struct {
    ra8_port_pin_t pin;   /**< Pin.   */
    ra8_psel_t     psel;  /**< Psel.  */
    const char*    owner; /**< Owner. */
  } routes[] = {
    {(ra8_port_pin_t)k_ra8_board_audio_pin_bclk, k_ra8_psel_ssie, "ra8_board.audio.bclk"},
    {(ra8_port_pin_t)k_ra8_board_audio_pin_wclk, k_ra8_psel_ssie, "ra8_board.audio.wclk"},
    {(ra8_port_pin_t)k_ra8_board_audio_pin_datin, k_ra8_psel_ssie, "ra8_board.audio.datin"},
    {(ra8_port_pin_t)k_ra8_board_audio_pin_datout, k_ra8_psel_ssie, "ra8_board.audio.datout"},
    {(ra8_port_pin_t)k_ra8_board_audio_pin_i2c_sda, k_ra8_psel_iic, "ra8_board.audio.i2c.sda"},
    {(ra8_port_pin_t)k_ra8_board_audio_pin_i2c_scl, k_ra8_psel_iic, "ra8_board.audio.i2c.scl"},
  };
  for (uint32_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); ++i) {
    const ra8_err_t err = ra8_pfs_route_peripheral(routes[i].pin, routes[i].psel, routes[i].owner);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Build the SSIE0 controller-mode I2S config for the DA7212 link.
 *
 * @details
 * AUDIO_MCK / 4 lands close to 12.288 MHz / 4 = 3.072 MHz BCK for
 * 48 kHz x 2ch x 32-bit frames; finer rate matching is left to
 * applications that re-call ra8_ssie_init() with a tuned divider.
 *
 * @param[in,out] channels See function signature.
 * @param[in,out] dwl See function signature.
 * @param[in,out] swl See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_ssie_cfg_t internal_audio_build_ssie_cfg(uint8_t                channels,
                                                                 ra8_ssie_data_word_t   dwl,
                                                                 ra8_ssie_system_word_t swl)
{
  return (ra8_ssie_cfg_t){
    .role          = k_ra8_ssie_role_controller,
    .format        = (channels == (uint8_t)k_ra8_audio_channels_mono) ? k_ra8_ssie_format_monaural
                                                                      : k_ra8_ssie_format_i2s,
    .data_word     = dwl,
    .system_word   = swl,
    .bclk_div      = k_ra8_ssie_bclk_div_4,
    .use_gpt_clk   = false,
    .long_frame    = false,
    .bckp_rising   = false,
    .lrckp_low     = false,
    .spdp_high     = false,
    .byte_swap     = false,
    .lr_continue   = false,
    .bck_idle_stop = false,
    .enable_aucke  = true,
    .tx_threshold  = 0U,
    .rx_threshold  = 0U,
  };
}

ra8_err_t ra8_board_audio_init(uint32_t sample_rate_hz, uint8_t bit_depth, uint8_t channels)
{
  if (sample_rate_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (channels != (uint8_t)k_ra8_audio_channels_mono &&
      channels != (uint8_t)k_ra8_audio_channels_stereo) {
    return k_ra8_err_invalid_arg;
  }
  ra8_ssie_data_word_t   dwl  = k_ra8_ssie_dwl_16;
  ra8_ssie_system_word_t swl  = k_ra8_ssie_swl_16;
  ra8_err_t              berr = internal_audio_bits_to_word(bit_depth, &dwl, &swl);
  if (berr != k_ra8_ok) {
    return berr;
  }

  /* Step 1: route the four DAI pins to SSIE0 + the I2C control pair
   * to IIC1 (UM Table 32 p 38). */
  ra8_err_t rerr = internal_audio_route_pins();
  if (rerr != k_ra8_ok) {
    return rerr;
  }

  /* Step 2: bring up SSIE0 in I2S controller mode. */
  (void)sample_rate_hz;
  const ra8_ssie_cfg_t cfg  = internal_audio_build_ssie_cfg(channels, dwl, swl);
  const ra8_err_t      serr = ra8_ssie_init((uint8_t)k_ra8_board_audio_ssie_channel, &cfg);
  if (serr != k_ra8_ok) {
    return k_ra8_err_hw_init_failed;
  }
  s_audio_initialized = true;
  return k_ra8_ok;
}

ra8_err_t ra8_board_audio_play_sample_block(const int16_t* buf, uint32_t len)
{
  if (buf == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if (len == 0U || (len % (uint32_t)k_ra8_audio_samples_per_word) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (!s_audio_initialized) {
    return k_ra8_err_not_initialized;
  }
  /* Two int16_t samples (one stereo frame) pack into one 32-bit SSIE
   * FIFO word; len is guaranteed even by the check above. The buffer
   * is a contiguous PCM stream the SSIE FIFO consumes 32 bits at a
   * time. Reinterpret via uintptr_t so we neither (a) double-hop
   * through void* (bugprone-casting-through-void) nor (b) trip
   * cast-align by going int16_t* -> uint32_t* directly. The caller
   * contract requires ``buf`` to be 32-bit aligned (one stereo frame
   * naturally aligns); see ra8_board_audio_play_sample_block docs. */
  const uint32_t        words   = len / (uint32_t)k_ra8_audio_samples_per_word;
  const uintptr_t       addr    = (uintptr_t)buf;
  const uint32_t* const packed  = (const uint32_t*)addr;
  uint16_t              written = 0U;
  const ra8_err_t       err     = ra8_ssie_write_buffer((uint8_t)k_ra8_board_audio_ssie_channel,
                                                        packed,
                                                        (uint16_t)words,
                                                        &written);
  if (err != k_ra8_ok) {
    return err;
  }
  if (written != (uint16_t)words) {
    return k_ra8_err_hw_timeout;
  }
  return k_ra8_ok;
}

/* =============================================================================
 * 7. USB stubs (HS HAL not yet wired to BSP)
 * =============================================================================
 */

/**
 * @var g_usbhs_probe
 * @brief Temporary bisect probe for USBHS bring-up HardFault.
 *
 * @details
 * Stepped through the sub-calls of internal_usbhs_clock_and_mstp +
 * ra8_usb_device_init so a JLink session can read which sub-step is in
 * flight when the worker thread takes the IACCVIOL/PRECISERR fault.
 * Values: 1 = pre cgc_usbhs_pll_enable, 2 = pre mstp_init, 3 = pre
 * mstp_enable(usbhs), 4 = pre ra8_usb_device_init(hs), 5 = post
 * ra8_usb_device_init returned. Remove once the offending sub-call is
 * identified.
 *
 * @note File-scope, single-writer (worker thread).
 * @since 0.1.0
 */
volatile uint32_t g_usbhs_probe = 0U;

/**
 * @enum usbhs_probe_step_t
 * @brief Bisect-probe step values for g_usbhs_probe.
 */
typedef enum : uint32_t {
  k_usbhs_probe_pre_pll_enable    = 1U, /**< before ra8_cgc_usbhs_pll_enable. */
  k_usbhs_probe_pre_mstp_init     = 2U, /**< before ra8_mstp_init.            */
  k_usbhs_probe_pre_mstp_enable   = 3U, /**< before ra8_mstp_enable(usbhs).   */
  k_usbhs_probe_pre_usb_dev_init  = 4U, /**< before ra8_usb_device_init(hs).  */
  k_usbhs_probe_post_usb_dev_init = 5U, /**< after ra8_usb_device_init.       */
} usbhs_probe_step_t;

/**
 * @brief Shared CGC + MSTP bring-up for both USBHS modes.
 *
 * @details
 * Sequence per HUM Ch 9 (CGC, USBCKCR / USBCKDIVCR, p 365) and Ch 11
 * (MSTP, MSTPCRB12 USBHS, p 469):
 *   1. ``ra8_cgc_usbhs_pll_enable()`` -- arms the PHY 12 MHz reference
 *      derived from the main XTAL.
 *   2. ``ra8_mstp_init()`` -- ensures the MSTP refcount table exists
 *      (idempotent across BSP veneers).
 *   3. ``ra8_mstp_enable(k_ra8_mstp_usbhs)`` -- ungates MSTPCRB.MSTPB12
 *      so the controller's SYSCFG block is reachable.
 *
 * @return ra8_err_t First non-OK result, or k_ra8_ok on a clean run.
 *
 * @pre  ra8_cgc_init() has been called.
 * @post On k_ra8_ok, USBHS controller registers are clocked and ready
 *       for ra8_usb_device_init / ra8_usb_host_init.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
RA8_INTERNAL static ra8_err_t internal_usbhs_clock_and_mstp(void)
{
  g_usbhs_probe = (uint32_t)k_usbhs_probe_pre_pll_enable;
  ra8_err_t err = ra8_cgc_usbhs_pll_enable();
  if (err != k_ra8_ok) {
    return err;
  }
  /* DO NOT call ra8_mstp_init() here -- it gates ALL modules (IOPORT,
   * SCI, etc.) and the chip faults trying to access already-running
   * peripherals. ra8_mstp_init must run exactly once per boot, before
   * any peripheral is brought up; the boot-time path handles that.
   * ra8_mstp_enable below uses the existing ref-counted state. */
  g_usbhs_probe = (uint32_t)k_usbhs_probe_pre_mstp_enable;
  return ra8_mstp_enable(k_ra8_mstp_usbhs);
}

/* =============================================================================
 * U15 PI4IOE5V6408 I/O expander -- SW4 override (J7 USB role select etc.)
 *
 * EK-RA8D2 v1 UM Rev 1.01 Section 5.5.3 + Section 4 p 16:
 *   "The EK-RA8D2 features an I2C I/O Port Expander (PI4IOE5V6408) at
 *    U15 which has the I2C address 0x43. The port expander is connected
 *    to the configuration switches SW4 and allows the settings to be
 *    read (when the I/O expander port is set to input) or overridden
 *    (when the I/O expander port is set to output) by software."
 *
 * Register map and polarity convention come from Renesas's reference
 * driver in ``ra-fsp-examples/ek_ra8t2/board_cfg_switch.c`` (the EK-RA8T2
 * sister board uses an identical wiring).
 * =============================================================================
 */

/** @brief PI4IOE5V6408 7-bit I2C address on EK-RA8D2 v1 (SW4 sibling). */
typedef enum : uint8_t {
  k_ra8_board_io_expander_addr = 0x43U, /**< RA8 board io expander address. */
} ra8_board_io_expander_addr_t;

/**
 * @brief PI4IOE5V6408 register addresses.
 *
 * @details
 * The expander is auto-incrementing; we always do single-byte writes.
 * Values cite ``ra-fsp-examples/ek_ra8t2/board_cfg_switch.c``.
 */
typedef enum : uint8_t {
  k_ra8_board_pi4ioe_reg_devid     = 0x01U, /**< Device ID register.    */
  k_ra8_board_pi4ioe_reg_iodir     = 0x03U, /**< 1 = output.            */
  k_ra8_board_pi4ioe_reg_output    = 0x05U, /**< 1 = HIGH (== SW4 OFF). */
  k_ra8_board_pi4ioe_reg_hiz       = 0x07U, /**< 1 = output Hi-Z.       */
  k_ra8_board_pi4ioe_reg_pud_sel   = 0x0DU, /**< Pull-up/down select.   */
  k_ra8_board_pi4ioe_reg_input_lvl = 0x0FU, /**< Input state.           */
} ra8_board_pi4ioe_reg_t;

/**
 * @brief Magic-value writes for ``ra8_board_io_expander_set_usbhs_device_mode``.
 *
 * @details
 * Polarity per FSP reference: ``OFF == output bit HIGH``. SW4-8 OFF
 * selects USB-HS device mode on J7, so bit 7 must be HIGH. We write
 * 0xFF (every SW4 channel in its mechanical-default OFF position),
 * which keeps every other SW4-gated peripheral at its EK-RA8D2 default
 * routing while still forcing SW4-8 = OFF.
 */
typedef enum : uint8_t {
  k_ra8_board_pi4ioe_iodir_all_outputs = 0xFFU, /**< RA8 board pi4ioe iodir all outputs. */
  k_ra8_board_pi4ioe_output_all_high   = 0xFFU, /**< RA8 board pi4ioe output all high.   */
  k_ra8_board_pi4ioe_hiz_none          = 0x00U, /**< RA8 board pi4ioe hiz none.          */
} ra8_board_pi4ioe_magic_t;

/** @brief IIC1 (system I2C) configuration shared by every U15 access. */
typedef enum : uint32_t {
  k_ra8_board_io_expander_bus_hz   = 100000U,   /**< 100 kHz Sm.                */
  k_ra8_board_io_expander_pclkb_hz = 62500000U, /**< PCLKB = PLL1P/16 post-CGC. */
} ra8_board_io_expander_clk_t;

/** @brief IIC channel index used for the U15 expander (RIIC channel 1). */
typedef enum : uint8_t {
  k_ra8_board_io_expander_iic_channel = 1U, /**< RA8 board io expander iic channel. */
} ra8_board_io_expander_channel_t;

/**
 * @brief Pins that carry SCL1 / SDA1 to U15 (the system I2C bus).
 *
 * @details
 * The EK-RA8D2 v1 wires U15 (and the Grove / Pmod / mikroBUS / Arduino
 * I2C) to the SYS_I2C bus = RIIC channel 1 on P512 (SCL1) / P511 (SDA1)
 * -- confirmed against the FSP EK-RA8D2 quickstart board_cfg_switch
 * (R_IIC_MASTER channel 1, iic1.scl1.p512 / iic1.sda1.p511) and HUM
 * Ch 39. NOT P400/P401 (those are the I3C bus). The earlier P400/P401
 * + ra8_i3c_i2c (I3C) wiring talked on the wrong peripheral, which is why
 * every U15 access timed out.
 */
static const ra8_port_pin_t s_ra8_board_io_expander_pin_scl =
  (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_12); /**< SCL1 (P512). */
static const ra8_port_pin_t s_ra8_board_io_expander_pin_sda =
  (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_11); /**< SDA1 (P511). */

/**
 * @brief Pull-up enable pins for the system I2C bus (P109 / P311).
 *
 * @details
 * EK-RA8D2 v1 UM Table 23 ("I2C/I3C Pullup Configuration") routes the
 * SCL1/SDA1 bus pull-up resistors through P109 and P311: in I2C mode
 * (SW4-5 OFF, P512/P511) these two pins must be driven as push-pull
 * outputs HIGH for the bus to have pull-ups at all. Without them SCL1/
 * SDA1 float, the open-drain bus never reaches a valid high level, and
 * U15 (and every other I2C peripheral) cannot ACK -- the symptom that
 * made every RIIC1 transaction time out.
 */
static const ra8_port_pin_t s_ra8_board_io_expander_pin_pullup_a =
  (ra8_port_pin_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_9); /**< P109 pull-up enable. */
static const ra8_port_pin_t s_ra8_board_io_expander_pin_pullup_b =
  (ra8_port_pin_t)RA8_PIN(k_ra8_port_3, k_ra8_pin_11); /**< P311 pull-up enable. */

/** @brief Bit-bang bus-recovery tunables for the system I2C bus. */
typedef enum : uint32_t {
  k_ra8_board_i2c_recover_pulses = 9U,    /**< SCL clocks to flush one stuck byte + ACK.    */
  k_ra8_board_i2c_recover_spins  = 2000U, /**< Busy-loop iterations ~= one SCL half-period. */
} ra8_board_i2c_recover_t;

/* internal_io_expander_write_reg -- see implementation for details. */
RA8_INTERNAL static ra8_err_t internal_io_expander_write_reg(uint8_t reg, uint8_t val)
{
  const uint8_t buf[2] = {reg, val};
  /* Each PI4IOE5V6408 register write is a complete START..STOP
   * transaction. Chaining them with repeated-START (send_stop = false)
   * leaves the bus held and the device's command parser misaligned on the
   * third back-to-back write, so always close the transaction with a STOP. */
  return ra8_i2c_write((uint8_t)k_ra8_board_io_expander_iic_channel,
                       (uint8_t)k_ra8_board_io_expander_addr,
                       buf,
                       sizeof(buf),
                       true);
}

/**
 * @var s_io_expander_probe
 * @brief Bisect-probe progress counter for ra8_board_io_expander_set_usbhs_device_mode.
 *
 * @details
 * JLink-readable counter that records the last step the U15 bring-up
 * function reached before either succeeding or returning an error.
 * Mirrors the g_usbhs_probe pattern used by internal_usbhs_clock_and_mstp.
 *
 * Step values:
 *   1 = pre-PFS (entered function, before SCL/SDA route)
 *   2 = pre-init (PFS + open-drain done, before ra8_i2c_init)
 *   3 = pre-write-output (init done, before output-latch write)
 *   4 = pre-write-hiz (output-latch written, before Hi-Z clear)
 *   5 = pre-write-iodir (Hi-Z cleared, before iodir write)
 *   6 = success (all three U15 writes acknowledged)
 *
 * @note Volatile so the optimiser cannot lift the writes out of the
 *       step sequence; declared at file scope so JLink can resolve it
 *       by symbol name.
 * @since 0.1.0
 */
static volatile uint32_t s_io_expander_probe = 0U;

/**
 * @brief Step values for s_io_expander_probe (see variable docs).
 */
typedef enum : uint32_t {
  k_io_exp_probe_pre_pfs       = 1U, /**< Io exp probe pre PFS.       */
  k_io_exp_probe_pre_init      = 2U, /**< Io exp probe pre init.      */
  k_io_exp_probe_pre_write_out = 3U, /**< Io exp probe pre write out. */
  k_io_exp_probe_pre_write_hiz = 4U, /**< Io exp probe pre write hiz. */
  k_io_exp_probe_pre_write_dir = 5U, /**< Io exp probe pre write dir. */
  k_io_exp_probe_success       = 6U, /**< Io exp probe success.       */
} io_exp_probe_step_t;

/**
 * @brief Crude busy-wait for roughly one I2C SCL half-period.
 * @details Used only by the bit-bang bus-recovery, which does not need
 *          precise timing -- any peripheral that clock-stretches will
 *          release on the slow clocks this produces. Avoids a dependency
 *          on ra8_time being initialized this early in board bring-up.
 * @pre None.
 * @pre None.
 * @post Approximately one SCL half-period of wall-clock time has passed.
 * @post No register or pin state is modified.
 * @note Not thread-safe (timing only).
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_io_expander_bus_settle(void)
{
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra8_board_i2c_recover_spins; i++) {
    __asm__ volatile("nop");
  }
}

/**
 * @brief Bit-bang up to 9 SCL clocks to free a wedged system I2C bus.
 * @details If a previous, aborted transfer left a peripheral (e.g. U15)
 *          holding SDA low mid-byte, the bus is stuck and every START
 *          fails. Drive SCL (P512) as a GPIO output and pulse it -- with
 *          SDA (P511) released as an input -- until SDA reads high or the
 *          pulse budget is spent, then frame a STOP. Finally release both
 *          pins so the caller can route them to the IIC mux. Runs before
 *          every U15 access so a wedged bus self-recovers on the next boot.
 * @return ra8_err_t Error from the first failing GPIO sub-call, else k_ra8_ok.
 * @retval k_ra8_ok Recovery sequence completed and pins released.
 * @pre IOPORT module powered (reset default).
 * @pre P512/P511 are not currently claimed by another driver.
 * @post P512/P511 are released (unclaimed), ready for the IIC route.
 * @post SDA has been clocked toward release and a STOP edge was framed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_io_expander_bus_recover(void)
{
  const ra8_port_pin_t scl = s_ra8_board_io_expander_pin_scl;
  const ra8_port_pin_t sda = s_ra8_board_io_expander_pin_sda;
  ra8_err_t            err = ra8_gpio_output_init(scl, k_ra8_level_high);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_gpio_input_init(sda, k_ra8_pull_up);
  if (err != k_ra8_ok) {
    (void)ra8_gpio_release(scl);
    return err;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_board_i2c_recover_pulses; i++) {
    ra8_level_t     sda_lvl = k_ra8_level_low;
    const ra8_err_t rd      = ra8_gpio_read(sda, &sda_lvl);
    if (rd == k_ra8_ok) {
      if (sda_lvl == k_ra8_level_high) {
        break; /* peripheral released SDA -- bus is free */
      }
    }
    (void)ra8_gpio_write(scl, k_ra8_level_low);
    internal_io_expander_bus_settle();
    (void)ra8_gpio_write(scl, k_ra8_level_high);
    internal_io_expander_bus_settle();
  }
  /* Frame a STOP (SDA low->high while SCL high) to end any partial frame,
   * then release both pins for the IIC mux to re-claim. */
  (void)ra8_gpio_release(sda);
  (void)ra8_gpio_output_init(sda, k_ra8_level_low);
  internal_io_expander_bus_settle();
  (void)ra8_gpio_write(scl, k_ra8_level_high);
  (void)ra8_gpio_write(sda, k_ra8_level_high);
  internal_io_expander_bus_settle();
  (void)ra8_gpio_release(sda);
  (void)ra8_gpio_release(scl);
  return k_ra8_ok;
}

/**
 * @brief Drive the system-I2C pull-up enable pins (P109/P311) high.
 * @details EK-RA8D2 v1 UM Table 23 routes the SCL1/SDA1 bus pull-ups
 *          through P109 and P311; in I2C mode they must be push-pull
 *          outputs driven high or the open-drain bus has no pull-ups and
 *          no peripheral can ACK. Run before routing SCL1/SDA1 so the
 *          bus is pulled up the instant the IIC mux takes the pins.
 * @return ra8_err_t Error code from the first failing sub-call, else k_ra8_ok.
 * @retval k_ra8_ok Both pull-up enables are driven high.
 * @pre IOPORT module powered (reset default).
 * @pre Caller has not already claimed P109/P311.
 * @post On success P109 and P311 are push-pull outputs at level high.
 * @post On failure the first pin may remain claimed (caller aborts boot).
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_io_expander_enable_pullups(void)
{
  ra8_err_t err = ra8_gpio_output_init(s_ra8_board_io_expander_pin_pullup_a, k_ra8_level_high);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_output_init(s_ra8_board_io_expander_pin_pullup_b, k_ra8_level_high);
}

/**
 * @brief Route P512/P511 to SCL1/SDA1 with N-channel open-drain enabled.
 * @details I2C requires NCODR on both signals; ra8_pfs_route_peripheral
 *          leaves NCODR clear, so the route+open-drain pair runs per pin.
 * @return ra8_err_t Error code from the first failing sub-call, else k_ra8_ok.
 * @retval k_ra8_ok All four register writes succeeded.
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller has not already claimed P512/P511.
 * @post On success, P512/P511 are owned by the IIC1 mux in open-drain mode.
 * @post On failure, partially-claimed pins remain claimed (caller aborts boot).
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_io_expander_route_pins(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral(s_ra8_board_io_expander_pin_scl,
                                           k_ra8_psel_iic,
                                           "ra8_board.io_exp.scl1");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_mpc_set_open_drain(k_ra8_port_5, k_ra8_pin_12, true);
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral(s_ra8_board_io_expander_pin_sda,
                                 k_ra8_psel_iic,
                                 "ra8_board.io_exp.sda1");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_mpc_set_open_drain(k_ra8_port_5, k_ra8_pin_11, true);
}

/**
 * @brief Issue the three U15 register writes that latch a given SW4 layout.
 * @details Order matches the FSP reference (output-latch -> Hi-Z clear ->
 *          iodir) so each pin only flips to output once the latch is already
 *          driving the desired level. Updates s_io_expander_probe between
 *          writes so JLink can pinpoint a NACK.
 * @param[in] output_byte Bit-pattern written to PI4IOE5V6408 reg 0x05
 *                        (Output state). Bit n = 1 -> SW4-(n+1) reads OFF;
 *                        bit n = 0 -> reads ON. Use ``0xFF`` to mirror the
 *                        all-DIPs-OFF mechanical default or
 *                        ``k_ra8_board_pi4ioe_output_project_default``
 *                        (0xF2) for this project's wiring.
 * @param[in] output_mask Bit n = 1 drives U15 pin n as an output; clear bits
 *                        release that SW4 position to the physical switch.
 * @return ra8_err_t Error code; k_ra8_ok if all three writes ack.
 * @retval k_ra8_ok U15 driving exactly the positions selected by @p output_mask.
 * @retval k_ra8_err_nack U15 didn't ACK one of the register writes.
 * @pre ra8_i2c_init has already configured channel 1.
 * @pre I2C bus is idle.
 * @post On success, U15 IODIR=output_mask, OUTPUT=output_byte, HIZ=0x00.
 * @post On failure, s_io_expander_probe records the failing step.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_io_expander_program_u15(uint8_t output_byte,
                                                               uint8_t output_mask)
{
  s_io_expander_probe = (uint32_t)k_io_exp_probe_pre_write_out;
  ra8_err_t err =
    internal_io_expander_write_reg((uint8_t)k_ra8_board_pi4ioe_reg_output, output_byte);
  if (err != k_ra8_ok) {
    return err;
  }
  s_io_expander_probe = (uint32_t)k_io_exp_probe_pre_write_hiz;
  err                 = internal_io_expander_write_reg((uint8_t)k_ra8_board_pi4ioe_reg_hiz,
                                                       (uint8_t)k_ra8_board_pi4ioe_hiz_none);
  if (err != k_ra8_ok) {
    return err;
  }
  s_io_expander_probe = (uint32_t)k_io_exp_probe_pre_write_dir;
  return internal_io_expander_write_reg((uint8_t)k_ra8_board_pi4ioe_reg_iodir, output_mask);
}

/**
 * @brief Shared U15 bring-up: route SCL1/SDA1, initialize RIIC1, then load
 *        @p output_byte into the expander's output register.
 *
 * @details The public entry points differ only in the SW4 layout they
 *          program, so factor the PFS route + RIIC1 init + U15 write sequence
 *          into one helper. The probe word
 *          ``s_io_expander_probe`` is updated between steps so a JLink
 *          halt can pinpoint which step NACKed.
 *
 * @param[in] output_byte Value to latch into U15's output register
 *                        (see ``internal_io_expander_program_u15``).
 * @param[in] output_mask Value to latch into U15's IODIR register.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok All bring-up steps completed.
 * @retval k_ra8_err_gpio_conflict P512/P511 already owned.
 * @retval k_ra8_err_hw_init_failed RIIC1 initialization failed.
 * @retval k_ra8_err_nack U15 didn't ACK one of the register writes.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra8_mstp_init`` has run.
 * @post On success P512/P511 are routed to SCL1/SDA1; RIIC1 is at
 *       100 kHz; selected U15 pins drive @p output_byte.
 * @post ``s_io_expander_probe`` ends at ``k_io_exp_probe_success`` on OK
 *       or at the failing step on error.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_io_expander_apply_mask(uint8_t output_byte,
                                                              uint8_t output_mask)
{
  /* Step -1: clock out any wedged peripheral (stuck SDA) before touching
   * the bus, so a prior aborted transfer cannot deadlock every START. */
  ra8_err_t err = internal_io_expander_bus_recover();
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 0: drive P109/P311 high so the SCL1/SDA1 bus has pull-ups
   * (EK-RA8D2 v1 UM Table 23). Without this the open-drain bus floats
   * and U15 cannot ACK. */
  err = internal_io_expander_enable_pullups();
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 1: route P512 -> SCL1 and P511 -> SDA1 with NCODR on both
   * (HUM Ch 20.6 PSEL table p 859). */
  s_io_expander_probe = (uint32_t)k_io_exp_probe_pre_pfs;
  err                 = internal_io_expander_route_pins();
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 2: bring RIIC channel 1 up at 100 kHz. ra8_i2c_init ungates
   * the per-channel MSTP gate (IIC1 = MSTPB8) itself, so no separate
   * MSTP enable is needed here. */
  s_io_expander_probe     = (uint32_t)k_io_exp_probe_pre_init;
  const ra8_i2c_cfg_t cfg = {
    .bus_hz   = (uint32_t)k_ra8_board_io_expander_bus_hz,
    .pclkb_hz = (uint32_t)k_ra8_board_io_expander_pclkb_hz,
  };
  err = ra8_i2c_init((uint8_t)k_ra8_board_io_expander_iic_channel, &cfg);
  if (err != k_ra8_ok) {
    return err;
  }

  /* Step 4: program U15 in FSP-reference order. */
  err = internal_io_expander_program_u15(output_byte, output_mask);
  if (err != k_ra8_ok) {
    return err;
  }
  s_io_expander_probe = (uint32_t)k_io_exp_probe_success;
  return k_ra8_ok;
}

ra8_err_t ra8_board_io_expander_set_usbhs_device_mode(void)
{
  return internal_io_expander_apply_mask((uint8_t)k_ra8_board_pi4ioe_output_all_high,
                                         (uint8_t)k_ra8_board_pi4ioe_iodir_all_outputs);
}

/**
 * @brief Implementation of `ra8_board_io_expander_set_usbhs_host_mode()`.
 * @details See the public header for the documented contract; applies the
 *          project SW4 layout with SW4-8 (USBHS role) driven ON so the
 *          board supplies VBUS on J7.
 * @return Result code.
 * @retval k_ra8_ok U15 is driving the host-role layout.
 * @pre `ra8_mstp_init` has run.
 * @pre The I2C bus to U15 is reachable.
 * @post U15 outputs 0x72; J7 is in host role.
 * @post RIIC1 is initialized as a side effect of the shared bring-up.
 * @note Not thread-safe; boot context only.
 * @since 0.1.0
 */
ra8_err_t ra8_board_io_expander_set_usbhs_host_mode(void)
{
  return internal_io_expander_apply_mask((uint8_t)k_ra8_board_pi4ioe_output_usbhs_host,
                                         (uint8_t)k_ra8_board_pi4ioe_iodir_all_outputs);
}

/**
 * @brief Program U15 with this project's SW4 override pattern.
 * @details Writes ``k_ra8_board_pi4ioe_output_project_default`` (0xF2) to
 *          the expander's output register, which forces SW4-1 ON +
 *          SW4-2 OFF (Pmod1 UART) + SW4-3 ON (Octo-SPI inactive) +
 *          SW4-4 ON (Arduino/mikroBUS active) + SW4-5 OFF (I2C),
 *          regardless of the physical DIP positions. See the header
 *          for the full per-bit table.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok All bring-up steps completed; SW4 layout latched.
 * @retval k_ra8_err_gpio_conflict P512/P511 already owned.
 * @retval k_ra8_err_hw_init_failed RIIC1 initialization failed.
 * @retval k_ra8_err_nack U15 didn't ACK one of the register writes.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra8_mstp_init`` has run.
 * @post On success U15 outputs are 0xF2; the project's expected SW4
 *       layout is active.
 * @post P512/P511 are routed to SCL1/SDA1 and RIIC1 is brought up at
 *       100 kHz, ready for subsequent in-tree I2C work.
 *
 * @note Not thread-safe; call once early in main() before any code that
 *       depends on Pmod1 / Arduino / mikroBUS routing.
 * @since 0.1.0
 */
ra8_err_t ra8_board_io_expander_apply_project_sw4_defaults(void)
{
  return internal_io_expander_apply_mask((uint8_t)k_ra8_board_pi4ioe_output_project_default,
                                         (uint8_t)k_ra8_board_pi4ioe_iodir_all_outputs);
}

ra8_err_t ra8_board_io_expander_apply_sw4(uint8_t output_byte)
{
  return internal_io_expander_apply_mask(output_byte,
                                         (uint8_t)k_ra8_board_pi4ioe_iodir_all_outputs);
}

ra8_err_t ra8_board_io_expander_apply_sw4_mask(uint8_t output_byte, uint8_t output_mask)
{
  return internal_io_expander_apply_mask(output_byte, output_mask);
}

ra8_err_t ra8_board_io_expander_set_octospi_active(void)
{
  return internal_io_expander_apply_mask((uint8_t)k_ra8_board_pi4ioe_output_octospi_active,
                                         (uint8_t)k_ra8_board_pi4ioe_iodir_all_outputs);
}

/**
 * @var g_usbhs_role_pin_probe
 * @brief Bisect-probe progress counter for the PD07 USB-HS role-select drive.
 *
 * @details
 * EK-RA8D2 v1 UM Rev 1.01 Section 6.2 p 34 ("USB High Speed"):
 *   "For a USB Device configuration, set PD07 to low and configure the RA
 *    MCU firmware to use the USB High Speed ports in device mode."
 * PD07 is a direct-drive MCU GPIO (PORT 13, pin 7) that selects the J7
 * role; the U15 I/O expander is an alternate override path but is not
 * required when PD07 is driven explicitly. JLink-readable so a bench
 * reflash can confirm the pin was reached and driven low.
 *
 * Step values:
 *   0 = not yet entered
 *   1 = pre ra8_gpio_output_init(PD07, low)
 *   2 = output-init returned (success or failure recorded in g_usbhs_role_pin_err)
 *   3 = drive confirmed (gpio_init returned k_ra8_ok)
 *
 * @since 0.1.0
 */
volatile uint32_t g_usbhs_role_pin_probe = 0U;

/**
 * @var g_usbhs_role_pin_err
 * @brief Last ra8_err_t returned by the PD07 GPIO drive (0 = k_ra8_ok).
 * @details JLink-readable so the bench operator can disambiguate a pin
 *          conflict from a port-mux failure.
 * @since 0.1.0
 */
volatile uint32_t g_usbhs_role_pin_err = 0U;

/** @brief Probe step values for g_usbhs_role_pin_probe. */
typedef enum : uint32_t {
  k_usbhs_role_probe_pre_init  = 1U, /**< Usbhs role probe pre init.  */
  k_usbhs_role_probe_post_init = 2U, /**< Usbhs role probe post init. */
  k_usbhs_role_probe_success   = 3U, /**< Usbhs role probe success.   */
} usbhs_role_probe_step_t;

/**
 * @brief Drive PD07 low to strap the J7 USB-HS role line to "Device".
 *
 * @details
 * EK-RA8D2 v1 UM Rev 1.01 Section 6.2 p 34: PD07 is the J7 USB-HS role
 * select. Low = Device, High = Host. This is a plain MCU GPIO; the U15
 * I/O expander only matters if the firmware needs to override the SW4-8
 * strap from a different code path.
 *
 * @return ra8_err_t Result of ra8_gpio_output_init.
 * @retval k_ra8_ok PD07 owned and driven low.
 * @pre IOPORT module clock is on (always-on after reset).
 * @pre Pin validator initialized.
 * @post On success, PD07 is GPIO-output low and owned by GPIO tag.
 * @post On any return, g_usbhs_role_pin_probe is updated and
 *       g_usbhs_role_pin_err records the gpio-init result.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_usbhs_role_select_device(void)
{
  g_usbhs_role_pin_probe = (uint32_t)k_usbhs_role_probe_pre_init;
  /* PD07 (port 13, pin 7). The ra8_port_pin_t enum only pre-defines LED
   * pins, so any other packed value lands outside the enumerator set
   * and trips clang-analyzer EnumCastOutOfRange; suppress in the same
   * pattern used elsewhere in this file for board-specific pins. */
  const ra8_port_pin_t pd07 = (ra8_port_pin_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_7);
  const ra8_err_t      err  = ra8_gpio_output_init(pd07, k_ra8_level_low);
  g_usbhs_role_pin_err      = (uint32_t)err;
  g_usbhs_role_pin_probe    = (uint32_t)k_usbhs_role_probe_post_init;
  if (err == k_ra8_ok) {
    g_usbhs_role_pin_probe = (uint32_t)k_usbhs_role_probe_success;
  }
  return err;
}

ra8_err_t ra8_board_usbhs_device_init(void)
{
  /* HUM Ch 9 (CGC) USBCKCR p 365 + HUM Ch 11 (MSTP) MSTPCRB12 p 469
   * stage the PHY clock and ungate the controller block; then the
   * generic device-mode entry in libs/ra8_hal/inc/ra8_usb.h drives
   * SYSCFG.SCKE / USBE / HSE and arms the device IRQ set.
   *
   * EK-RA8D2 v1 UM Rev 1.01 Section 6.2 p 34 ("USB High Speed"): J7's
   * role is selected by the MCU GPIO PD07 -- LOW = Device, HIGH = Host.
   * Drive PD07 low directly (no U15 I/O-expander dependency required;
   * U15 is only an SW4 override path, see Section 4.3.4 p 16 / Table 23
   * pullup-config p 31). The U15 helper is invoked best-effort below
   * for boards where PD07 alone is insufficient (e.g. SW4-8 mechanical
   * position contradicts the firmware intent), but its failure is
   * non-fatal because PD07 already strapped the role line. */
  const ra8_err_t pd07_err = internal_usbhs_role_select_device();
  if (pd07_err != k_ra8_ok) {
    return pd07_err;
  }

  /* Best-effort U15 override (logged via s_io_expander_probe). The U15
   * I2C path may NACK if SW4-5 is OFF (I2C/I3C-Select strap routes the
   * shared bus elsewhere) -- non-fatal because PD07 already strapped
   * the role. */
  const ra8_err_t io_err = ra8_board_io_expander_set_usbhs_device_mode();
  (void)io_err;
  ra8_err_t err = internal_usbhs_clock_and_mstp();
  if (err != k_ra8_ok) {
    return err;
  }
  g_usbhs_probe    = (uint32_t)k_usbhs_probe_pre_usb_dev_init;
  ra8_err_t rc_dev = ra8_usb_device_init(k_ra8_usb_speed_hs);
  g_usbhs_probe    = (uint32_t)k_usbhs_probe_post_usb_dev_init;
  return rc_dev;
}

ra8_err_t ra8_board_usbhs_host_init(void)
{
  /* Symmetric to ra8_board_usbhs_device_init. EK-RA8D2 v1 UM Rev 1.01
   * Table 28 lists USBHS_VBUSEN / USBHS_OVRCUR as PHY-controller
   * pins not routed to RA8D2 port pins, so VBUS sourcing is left to
   * the on-board USB-PD controller. ra8_usb_host_init drives
   * SYSCFG.DCFM=1 / DRPD=1 / USBE=1 plus the DCP setup. */
  ra8_err_t err = internal_usbhs_clock_and_mstp();
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_usb_host_init(k_ra8_usb_speed_hs);
}
