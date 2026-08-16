/**
 * @file examples/ek_ra8d2/hw_validated/hil/iic_b_facade_demo/main.c
 * @brief IIC_B (I3C block in I2C-compat mode) facade controller + peripheral demo
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Exercises the ``ra8_io_i2c_bus`` facade over the I3C block's legacy
 * I2C-compatibility mode (IIC_B, the twin of the RIIC ``ra8_i2c`` path) on
 * channel 0, in BOTH roles the peripheral supports:
 *
 *  1. **Controller** -- binds the facade with
 *     ``ra8_io_i2c_bus_bind_i3c_compat`` and runs the classic
 *     write-register-pointer / repeated-START / read pattern via
 *     ``ra8_io_i2c_bus_transfer`` against the on-board GoodIX GT911 touch
 *     controller (7-bit 0x5D). It reads the 4-byte PRODUCT_ID (register
 *     0x8140 -> "911\0"), the exact bring-up probe ``ra8_touch`` performs --
 *     so a real device answers on a stock EK-RA8D2 (see ``touch_demo``). The
 *     facade makes the physical peripheral (IIC_B vs RIIC) a bind-time choice,
 *     not a call-site rewrite.
 *  2. **Peripheral (target)** -- reprograms the same channel as an addressed
 *     I2C target (own address 0x42) via ``ra8_i3c_peripheral_open`` and then
 *     services controller write-then-read round-trips: it drains a byte the
 *     bus controller wrote (NTST.RDBFF0) and echoes it back on the following
 *     controller-read (NTST.TDBEF0).
 *
 * Each second the SCI8 J-Link console prints one banner that always leads with
 * the gate substring ``iic_b facade: up`` (reaching the loop proves
 * ``ra8_i3c_init`` + facade bind succeeded on silicon -- a bring-up failure
 * BKPT-halts first), followed by the informational controller outcome and the
 * completed peripheral round-trip count:
 *
 *   ``iic_b facade: up ctrl=OK id0=0x39 periph=4``
 *
 * @note **EIL vs bench.** ``tools/ra8_emulator`` models the IIC_B controller
 * bus (GT911 answers PRODUCT_ID) AND plays the EXTERNAL I2C controller that
 * drives the firmware's target role (``board_periph_i2c.c``), so both halves
 * run headless and the peripheral count is non-zero in EIL. On a bare bench
 * the controller half still ACKs the real GT911, but the target half needs an
 * external I2C controller wired to the bus (the stock rig has none), so
 * ``periph=0`` there -- informational, never gated. See ``README.md``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_i2c_bus_ops.h"
#include "ra8_i3c.h"
#include "ra8_io_i2c_bus.h"
#include "ra8_io_i2c_bus_i3c_compat.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @enum iic_facade_tunable_t @brief Console / bus / loop knobs (no magic numbers). */
typedef enum : uint32_t {
  k_iic_baud      = 115200U, /**< SCI8 J-Link console baud.    */
  k_iic_bus_hz    = 100000U, /**< IIC_B bit rate (100 kHz Sm). */
  k_iic_period_ms = 1000U,   /**< Banner period.               */
} iic_facade_tunable_t;

/** @enum iic_facade_layout_t @brief Channel + on-bus addresses + register map. */
typedef enum : uint8_t {
  k_iic_channel        = 0U,    /**< I3C/IIC_B channel (only 0 on RA8D2).   */
  k_iic_ctrl_addr_7b   = 0x5DU, /**< On-board GT911 touch 7-bit address.    */
  k_iic_periph_addr_7b = 0x42U, /**< Own 7-bit address in target mode.      */
  k_iic_prod_ptr_hi    = 0x81U, /**< GT911 PRODUCT_ID pointer MSB (0x8140). */
  k_iic_prod_ptr_lo    = 0x40U, /**< GT911 PRODUCT_ID pointer LSB.          */
  k_iic_ptr_len        = 2U,    /**< 16-bit register-pointer width.         */
  k_iic_prod_len       = 4U,    /**< PRODUCT_ID payload length ("911\0").   */
} iic_facade_layout_t;

/** @enum iic_facade_periph_t @brief Target-mode round-trip + poll budget. */
typedef enum : uint16_t {
  k_iic_periph_rounds   = 4U,    /**< Controller write+read round-trips to accept. */
  k_iic_periph_poll_max = 4000U, /**< Bounded target-service polls (NASA P10 R2).  */
} iic_facade_periph_t;

/** @enum iic_facade_fmt_t @brief Console number-formatting constants. */
typedef enum : uint8_t {
  k_iic_nib_shift  = 4U,    /**< High-nibble shift for a hex byte. */
  k_iic_nib_mask   = 0x0FU, /**< Low-nibble mask.                  */
  k_iic_dec_ten    = 10U,   /**< Decimal radix.                    */
  k_iic_dec_digits = 5U,    /**< Decimal digit-buffer capacity.    */
} iic_facade_fmt_t;

/**
 * @enum iic_facade_ctrl_result_t
 * @brief Outcome classification of the facade controller transfer.
 *
 * @details Maps the ``ra8_io_i2c_bus_transfer`` return code to a banner token.
 * A completed-but-unacknowledged bus transaction (NAK) is distinguished from a
 * bus that never clocked (idle / floated), matching how ``i3c_loopback``
 * reports a bare bus.
 *
 * @invariant ::k_iic_ctrl_ok is the only value that carries valid id bytes.
 * @see iic_facade_classify_ctrl
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_iic_ctrl_ok   = 0U, /**< Device ACKed; PRODUCT_ID bytes are valid.   */
  k_iic_ctrl_nak  = 1U, /**< Bus clocked but no device acknowledged.     */
  k_iic_ctrl_idle = 2U, /**< No transaction completed (bus fault/float). */
} iic_facade_ctrl_result_t;

/**
 * @enum iic_facade_svc_t
 * @brief What one target-mode service poll did.
 *
 * @details Reported by ::iic_facade_service so the caller can count only a
 * FULL round-trip (a received byte followed by its echo), not either half in
 * isolation.
 *
 * @invariant ::k_iic_svc_none means the status poll found nothing to service.
 * @see iic_facade_service
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_iic_svc_none = 0U, /**< No RX-full / TX-empty event this poll. */
  k_iic_svc_rx   = 1U, /**< Drained one controller-written byte.   */
  k_iic_svc_tx   = 2U, /**< Echoed one byte back (round-trip end). */
} iic_facade_svc_t;

/**
 * @var s_iic_bus
 * @brief Facade handle bound to IIC_B channel 0 in I2C-compat controller mode.
 *
 * @details File-scope so it out-lives every facade call made through it during
 * the controller phase.
 *
 * @note Written once during bring-up, then read-only.
 * @warning Do not rebind while a transfer is in flight.
 * @since 0.1.0
 */
static ra8_io_i2c_bus_t s_iic_bus;

static const uint8_t k_msg_boot[] = "iic_b facade: boot\r\n";
static const uint8_t k_msg_fail[] = "iic_b facade: FAIL init\r\n";
static const uint8_t k_msg_open[] = "iic_b facade: FAIL open\r\n";
static const uint8_t k_msg_up[]   = "iic_b facade: up ctrl=";
static const uint8_t k_msg_id0[]  = " id0=0x";
static const uint8_t k_msg_peri[] = " periph=";
static const uint8_t k_msg_eol[]  = "\r\n";
static const uint8_t k_tok_ok[]   = "OK";
static const uint8_t k_tok_nak[]  = "NAK";
static const uint8_t k_tok_idle[] = "IDLE";

/** @brief Emit a byte run on the SCI8 console (best-effort). */
static void iic_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner then trap (ra8_emulator halts on the BKPT). */
static void iic_panic_halt(const uint8_t* msg, uint32_t len)
{
  iic_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a byte as two uppercase hex nibbles. */
static void iic_print_hex_byte(uint8_t value)
{
  static const uint8_t k_hex[] = "0123456789ABCDEF";
  const uint8_t        hi = k_hex[(value >> (uint8_t)k_iic_nib_shift) & (uint8_t)k_iic_nib_mask];
  const uint8_t        lo = k_hex[value & (uint8_t)k_iic_nib_mask];
  iic_print(&hi, 1U);
  iic_print(&lo, 1U);
}

/** @brief Print a small unsigned integer (0..9999) in decimal. */
static void iic_print_dec(uint32_t value)
{
  uint8_t  buf[k_iic_dec_digits];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = (uint8_t)'0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_iic_dec_digits)) {
    buf[n] = (uint8_t)((uint32_t)'0' + (value % (uint32_t)k_iic_dec_ten));
    n++;
    value /= (uint32_t)k_iic_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    iic_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Classify the facade controller transfer return code into a banner token.
 *
 * @details Pure mapping used by the controller phase and mirrored by the host
 * test. ``k_ra8_ok`` means the target ACKed and the PRODUCT_ID bytes are valid;
 * ``k_ra8_err_nack`` means the bus clocked but nothing answered; any other error
 * means no transaction completed (a floated / wedged bus).
 *
 * @param[in] err Return code from ``ra8_io_i2c_bus_transfer``.
 *
 * @return ra8_facade controller result classification.
 * @retval k_iic_ctrl_ok   @p err was ``k_ra8_ok``.
 * @retval k_iic_ctrl_nak  @p err was ``k_ra8_err_nack``.
 * @retval k_iic_ctrl_idle @p err was any other value.
 *
 * @pre @p err is a valid ``ra8_err_t`` value.
 * @post The return value names exactly one banner token.
 * @note Thread-safe (pure function of @p err).
 * @see iic_facade_controller_phase
 * @since 0.1.0
 */
[[nodiscard]] static iic_facade_ctrl_result_t iic_facade_classify_ctrl(ra8_err_t err)
{
  if (err == k_ra8_ok) {
    return k_iic_ctrl_ok;
  }
  if (err == k_ra8_err_nack) {
    return k_iic_ctrl_nak;
  }
  return k_iic_ctrl_idle;
}

/**
 * @brief Did the target-mode status mask flag anything to service this poll?
 *
 * @details The compound decision at the heart of the peripheral service loop:
 * a poll has work iff the RX-buffer-full OR TX-buffer-empty event is latched.
 * Mirrored under MC/DC by the host test.
 *
 * @param[in] mask ``ra8_i3c_peripheral_status`` event mask.
 *
 * @return true when a controller-write or controller-read is pending.
 * @retval true  RX-full or TX-empty (or both) is set.
 * @retval false Neither event is set (idle poll).
 *
 * @pre @p mask is a valid ::ra8_i3c_peripheral_status_t OR-mask.
 * @post No state is mutated.
 * @note Thread-safe (pure function of @p mask).
 * @see iic_facade_service
 * @since 0.1.0
 */
[[nodiscard]] static bool iic_facade_periph_acted(uint8_t mask)
{
  const bool rx = (mask & (uint8_t)k_ra8_i3c_peripheral_status_rx_full) != 0U;
  const bool tx = (mask & (uint8_t)k_ra8_i3c_peripheral_status_tx_empty) != 0U;
  return rx || tx;
}

/**
 * @brief Service one target-mode event: drain a written byte or echo it back.
 *
 * @details Polls ``ra8_i3c_peripheral_status`` once. On RX-full it drains one
 * byte into @p last_byte; on TX-empty it re-sends @p last_byte (echo). The
 * ra8_emulator external-controller model paces these so a receive is always
 * followed by the matching echo.
 *
 * @param[in,out] last_byte Latched controller-write byte; echoed on the next read.
 * @param[out]    out_svc   What this poll did (::iic_facade_svc_t).
 *
 * @return ra8_err_t status of the HAL calls.
 * @retval k_ra8_ok           Poll serviced (see @p out_svc).
 * @retval k_ra8_err_hw_error A HAL status / receive / send call failed.
 *
 * @pre The channel was opened in target mode via ``ra8_i3c_peripheral_open``.
 * @pre @p last_byte and @p out_svc are non-NULL.
 * @post @p out_svc names the serviced half (or ::k_iic_svc_none).
 * @post On RX @p last_byte holds the freshly received byte.
 * @note Not thread-safe with respect to the channel.
 * @see iic_facade_periph_acted
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t iic_facade_service(uint8_t* last_byte, iic_facade_svc_t* out_svc)
{
  uint8_t mask = 0U;
  if (ra8_i3c_peripheral_status((uint8_t)k_iic_channel, &mask) != k_ra8_ok) {
    return k_ra8_err_hw_error;
  }
  *out_svc = k_iic_svc_none;
  if (!iic_facade_periph_acted(mask)) {
    return k_ra8_ok; /* Idle poll: no RX-full / TX-empty event to service. */
  }
  if ((mask & (uint8_t)k_ra8_i3c_peripheral_status_rx_full) != 0U) {
    uint8_t b = 0U;
    if (ra8_i3c_peripheral_receive((uint8_t)k_iic_channel, &b, 1U) != k_ra8_ok) {
      return k_ra8_err_hw_error;
    }
    *last_byte = b;
    *out_svc   = k_iic_svc_rx;
  }
  if ((mask & (uint8_t)k_ra8_i3c_peripheral_status_tx_empty) != 0U) {
    if (ra8_i3c_peripheral_send((uint8_t)k_iic_channel, last_byte, 1U) != k_ra8_ok) {
      return k_ra8_err_hw_error;
    }
    *out_svc = k_iic_svc_tx;
  }
  return k_ra8_ok;
}

/**
 * @brief Run the facade CONTROLLER phase: read the GT911 PRODUCT_ID.
 *
 * @details Issues one ``ra8_io_i2c_bus_transfer`` -- write the 2-byte register
 * pointer 0x8140, repeated START, read 4 bytes -- against 0x5D and classifies
 * the outcome. The first PRODUCT_ID byte is returned for the banner.
 *
 * @param[out] out_id0 First PRODUCT_ID byte on ACK, else 0.
 *
 * @return Controller-phase classification (::iic_facade_ctrl_result_t).
 *
 * @pre The facade is bound to IIC_B channel 0 (::s_iic_bus).
 * @pre @p out_id0 is non-NULL.
 * @post @p out_id0 holds the first read byte (0 unless the result is OK).
 * @note Not thread-safe with respect to the channel.
 * @see iic_facade_classify_ctrl
 * @since 0.1.0
 */
[[nodiscard]] static iic_facade_ctrl_result_t iic_facade_controller_phase(uint8_t* out_id0)
{
  const uint8_t   ptr[k_iic_ptr_len]    = {(uint8_t)k_iic_prod_ptr_hi, (uint8_t)k_iic_prod_ptr_lo};
  uint8_t         id[k_iic_prod_len]    = {};
  const ra8_err_t err                   = ra8_io_i2c_bus_transfer(&s_iic_bus,
                                                                  (uint8_t)k_iic_ctrl_addr_7b,
                                                                  ptr,
                                                                  (uint32_t)k_iic_ptr_len,
                                                                  id,
                                                                  (uint32_t)k_iic_prod_len);
  const iic_facade_ctrl_result_t result = iic_facade_classify_ctrl(err);
  *out_id0                              = (result == k_iic_ctrl_ok) ? id[0] : 0U;
  return result;
}

/**
 * @brief Run the PERIPHERAL (target) phase and count completed round-trips.
 *
 * @details Opens the channel as an addressed I2C target (0x42) then services a
 * bounded number of polls, counting only a full round-trip (a received byte
 * immediately echoed back). In ra8_emulator the external-controller model drives
 * these; on a bare bench with no external controller the count stays 0.
 *
 * @return Completed controller write+read round-trips (0 .. ::k_iic_periph_rounds).
 *
 * @pre The channel was initialised in I2C mode via ``ra8_i3c_init``.
 * @pre The controller phase has finished (no transfer is in flight).
 * @post The channel is left in target mode answering 0x42.
 * @note Not thread-safe with respect to the channel.
 * @see iic_facade_service
 * @since 0.1.0
 */
[[nodiscard]] static uint32_t iic_facade_peripheral_phase(void)
{
  const ra8_i3c_peripheral_cfg_t cfg = {
    .peripheral_addr_7b = (uint8_t)k_iic_periph_addr_7b,
    .general_call       = 0U,
  };
  if (ra8_i3c_peripheral_open((uint8_t)k_iic_channel, &cfg) != k_ra8_ok) {
    return 0U;
  }
  uint8_t  last    = 0U;
  bool     have_rx = false;
  uint32_t rounds  = 0U;
  for (uint32_t i = 0U;
       (i < (uint32_t)k_iic_periph_poll_max) && (rounds < (uint32_t)k_iic_periph_rounds);
       i++) {
    iic_facade_svc_t svc = k_iic_svc_none;
    if (iic_facade_service(&last, &svc) != k_ra8_ok) {
      break;
    }
    if (svc == k_iic_svc_rx) {
      have_rx = true;
    } else if ((svc == k_iic_svc_tx) && have_rx) {
      rounds++;
      have_rx = false;
    } else {
      /* Idle poll or an echo without a preceding receive: not a round-trip. */
    }
  }
  return rounds;
}

/** @brief Print one controller-result token for @p result. */
static void iic_print_ctrl_token(iic_facade_ctrl_result_t result)
{
  if (result == k_iic_ctrl_ok) {
    iic_print(k_tok_ok, (uint32_t)sizeof(k_tok_ok) - 1U);
  } else if (result == k_iic_ctrl_nak) {
    iic_print(k_tok_nak, (uint32_t)sizeof(k_tok_nak) - 1U);
  } else {
    iic_print(k_tok_idle, (uint32_t)sizeof(k_tok_idle) - 1U);
  }
}

/** @brief Print the once-per-period summary banner. */
static void iic_print_banner(iic_facade_ctrl_result_t result, uint8_t id0, uint32_t rounds)
{
  iic_print(k_msg_up, (uint32_t)sizeof(k_msg_up) - 1U);
  iic_print_ctrl_token(result);
  iic_print(k_msg_id0, (uint32_t)sizeof(k_msg_id0) - 1U);
  iic_print_hex_byte(id0);
  iic_print(k_msg_peri, (uint32_t)sizeof(k_msg_peri) - 1U);
  iic_print_dec(rounds);
  iic_print(k_msg_eol, (uint32_t)sizeof(k_msg_eol) - 1U);
}

/** @brief Bring up clocks / MSTP / SysTick + the SCI8 console; halt on failure. */
static void iic_setup_or_halt(uint32_t* out_pclka_hz)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    iic_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if ((ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) ||
      (ra8_cgc_get_clock_hz(k_ra8_clock_id_pclka, out_pclka_hz) != k_ra8_ok)) {
    iic_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    iic_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_iic_baud) != k_ra8_ok) {
    iic_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    iic_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/**
 * @brief App entry: facade controller round-trip, then target-mode responder.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The ``iic_b facade: up`` banner is emitted every period in the loop.
 * @post On any bring-up failure the CPU BKPT-halts before the loop.
 * @since 0.1.0
 */
void main(void)
{
  uint32_t pclka_hz = 0U;
  iic_setup_or_halt(&pclka_hz);
  ra8_isr_globals_enable();
  iic_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  /* App-owned bus bring-up: IIC_B in I2C-compat controller mode, bound through
   * the ra8_io facade. A future board revision that routes this bus to a RIIC
   * channel only swaps the bind call for ra8_io_i2c_bus_bind_riic. */
  const ra8_i3c_cfg_t iic_cfg = {
    .mode     = k_ra8_i3c_mode_i2c,
    .bus_hz   = (uint32_t)k_iic_bus_hz,
    .pclka_hz = pclka_hz,
  };
  if (ra8_i3c_init((uint8_t)k_iic_channel, &iic_cfg) != k_ra8_ok) {
    iic_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }
  if (ra8_io_i2c_bus_bind_i3c_compat(&s_iic_bus, (uint8_t)k_iic_channel) != k_ra8_ok) {
    iic_panic_halt(k_msg_open, (uint32_t)sizeof(k_msg_open) - 1U);
  }

  /* (1) Controller: facade write-pointer / repeated-START / read of the GT911
   * PRODUCT_ID. (2) Peripheral: reopen the channel as target 0x42 and echo
   * controller round-trips. Both complete once, before the banner loop. */
  uint8_t                        id0    = 0U;
  const iic_facade_ctrl_result_t ctrl   = iic_facade_controller_phase(&id0);
  const uint32_t                 periph = iic_facade_peripheral_phase();

  while (1) {
    iic_print_banner(ctrl, id0, periph);
    (void)ra8_board_led_toggle(k_ra8_board_led1);
    ra8_delay_ms((uint32_t)k_iic_period_ms);
  }
}
