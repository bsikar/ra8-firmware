/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph.h
 * @brief Register-accurate peripheral-model framework for the board emulator
 *
 * @details
 * A registry that maps RA8D2 peripheral-register address ranges to per-block
 * read/write handlers backed by real state, dispatched from ra8_emulator's MMIO
 * callbacks. It SUPERSEDES the sparse reflect-then-settle fallback for the
 * blocks modelled here (the fallback still answers every UNmodelled address),
 * so a non-display example produces real peripheral data instead of faked
 * ready-bit handshakes.
 *
 * The first blocks modelled are GPIO/PORT (the board LEDs become observable),
 * the AGT and GPT timers (counters that advance on their configured clock and
 * raise compare-match / overflow / underflow events), the ICU/NVIC (a
 * peripheral event linked through IELSR pends the matching NVIC IRQ, taken as a
 * real Cortex-M exception by the engine's exception layer), and the SCI_B
 * UART (TDR writes are captured to a host console sink, RDR reads return a
 * host-supplied byte stream, and TXI / TEI / RXI route through the same ICU
 * event path so interrupt-driven serial works as well as polled). The block
 * table is the extension point: I2C / SPI / USB slot in as new entries later.
 *
 * Design: this module owns no Unicorn engine of its own and takes no AppKit
 * dependency. main.c passes the engine in where the model must read or write
 * emulated memory / pend an NVIC line, so board_periph stays plain C and the
 * exception delivery stays in the one place that already models it.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Board user-LED identity, mirrored from the EK-RA8D2 BSP.
 *
 * @details
 * Pin assignments per libs/ra8_board_ek_ra8d2 (EK-RA8D2 v1 UM Table 24, p 31):
 * LED1 BLUE = P600, LED2 GREEN = P303, LED3 RED = PA07. All three are
 * active-high. board_periph traces these specific port/pin output latches so
 * the run summary / --trace can report each LED transition.
 */
typedef enum : uint8_t {
  k_board_led1      = 0U, /**< LED1, BLUE,  P600 (port 6, pin 0).  */
  k_board_led2      = 1U, /**< LED2, GREEN, P303 (port 3, pin 3).  */
  k_board_led3      = 2U, /**< LED3, RED,   PA07 (port 10, pin 7). */
  k_board_led_count = 3U, /**< Board led count.                    */
} board_led_id_t;

/**
 * @brief The RA8 device the emulator models for this run.
 *
 * @details
 * ra8_emulator's peripheral models were written for the RA8D2; the RA8P1
 * (R7KA8P1KFLCAC) shares the RA8D2's entire register map and memory map, so the
 * same models serve both parts. Only one RA8P1-only block differs -- the Arm
 * Ethos-U55 NPU (0x40140000) -- and it is gated with ::k_board_block_dev_ra8p1
 * so it is dispatched only when the active device is the RA8P1. The default is
 * the RA8D2, so a run with no ``--device`` flag behaves exactly as before this
 * device knob existed.
 *
 * @invariant Exactly one value is active per run; set once before the run loop.
 * @see board_periph_set_device
 * @see board_block_device_t
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_board_device_ra8d2 = 0U, /**< Renesas RA8D2 (default): no NPU.       */
  k_board_device_ra8p1 = 1U, /**< Renesas RA8P1: adds the Ethos-U55 NPU. */
} board_device_t;

/**
 * @brief Select which RA8 device the peripheral model emulates.
 *
 * @details
 * Sets the active device used to gate the RA8P1-only NPU block. Called once by
 * main.c after parsing ``--device`` and before the run loop; the selection
 * persists across warm reboots (the emulated silicon does not change part
 * between resets). An out-of-range value is clamped
 * to ::k_board_device_ra8d2 so a malformed flag can never leave the model in an
 * undefined device state.
 *
 * @param[in] device Device to emulate (::k_board_device_ra8d2 /
 *                   ::k_board_device_ra8p1); out-of-range clamps to RA8D2.
 * @return Nothing.
 * @pre The peripheral registry constructors have run (they always do, pre-main).
 * @pre Called once during single-threaded setup, before the run loop (not re-entrant).
 * @post ::board_periph_device reports the clamped selection.
 * @post RA8P1-only blocks are dispatched iff @p device is ::k_board_device_ra8p1.
 * @note Not thread-safe; call once from the single-threaded setup path.
 * @see board_periph_device
 * @since 0.1.0
 */
void board_periph_set_device(board_device_t device);

/**
 * @brief Report which RA8 device the peripheral model is emulating.
 *
 * @details Read-only accessor over the active-device selection last set by
 *          ::board_periph_set_device (defaults to ::k_board_device_ra8d2).
 *
 * @return The active device (::k_board_device_ra8d2 / ::k_board_device_ra8p1).
 * @retval k_board_device_ra8d2 Default, or the last RA8D2 selection.
 * @retval k_board_device_ra8p1 Last selected via ::board_periph_set_device.
 * @pre None; safe at any time -- the selection is statically initialized to RA8D2.
 * @post No model state is modified (read-only accessor).
 * @post The returned value is a valid ::board_device_t enumerator.
 * @note Not thread-safe; single-threaded run-loop / setup use.
 * @see board_periph_set_device
 * @since 0.1.0
 */
board_device_t board_periph_device(void);

/**
 * @brief Enable the chip-internal USBHS-host self-loop model (--usbhs-loop).
 *
 * @details Gates every ::board_periph_block_t whose @c loop_only flag is set: a
 * loop-only block (the USBHS host controller model, board_periph_usbhs_host.c)
 * owns its register window ONLY when this is enabled. Off (the default), such a
 * block is skipped and its window falls through to the sparse fallback, exactly
 * as an unmodelled reserved region does -- so a run WITHOUT the flag is
 * byte-for-behaviour unchanged (the USBHS host apps that rely on the function
 * seam are untouched). main.c sets this once, after argument parsing, for an app
 * declared as a chip-internal self-loop.
 *
 * @param[in] on true to activate loop-only blocks for this run.
 * @return Nothing.
 * @post Loop-only blocks own their windows iff @p on; the dispatch cache is
 *       invalidated so the change takes effect immediately.
 * @note Not thread-safe; single-threaded setup use.
 * @see board_periph_usbhs_loop
 * @since 0.1.0
 */
void board_periph_set_usbhs_loop(bool on);

/**
 * @brief Report whether the USBHS-host self-loop model is enabled.
 *
 * @return true when ::board_periph_set_usbhs_loop last enabled it, else false.
 * @pre None; safe at any time (defaults to false).
 * @post No model state is modified (read-only accessor).
 * @note Not thread-safe; single-threaded run-loop / setup use.
 * @see board_periph_set_usbhs_loop
 * @since 0.1.0
 */
bool board_periph_usbhs_loop(void);

/**
 * @brief One-time reset of all peripheral-model state.
 *
 * @details
 * Clears every modelled block (PORT latches/direction, AGT/GPT counters and
 * status, ICU event-link table and NVIC pend records) and the observability
 * counters. Call once after the memory map is created and before the run loop.
 *
 * @param[in] trace When true, each LED / GPIO transition and each taken IRQ is
 *                   logged to stderr as it happens (the --trace flag).
 * @return Nothing.
 * @post All counters read zero and every block is in its reset state.
 * @since 0.1.0
 */
void board_periph_init(bool trace);

/**
 * @brief Record the cause of a warm reboot in the sticky RSTSRn flags.
 *
 * @details Called by the ra8_emulator reboot path (main.c) just before it
 * re-enters the firmware from the reset vector, so the next boot reads the
 * reset cause it expects. For a power-on reboot, leaves RSTSR0.PORF set and
 * asserts nothing else. For any other reset, clears PORF and latches the
 * specific cause in RSTSR1: SWRF (software reset / AIRCR.SYSRESETREQ), WDTRF
 * (watchdog-0 reset), or IWDTRF (independent-watchdog reset). The reset block's
 * reset hook preserves these flags across the warm reboot. Exactly one of the
 * four booleans should be true for a well-formed reset cause.
 *
 * @param[in] power_on true for a power-on / cold reboot (RSTSR0.PORF).
 * @param[in] software true to latch RSTSR1.SWRF (software reset).
 * @param[in] watchdog true to latch RSTSR1.WDTRF (watchdog-0 reset).
 * @param[in] iwdt     true to latch RSTSR1.IWDTRF (independent-watchdog reset).
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_reset_set_cause(bool power_on, bool software, bool watchdog, bool iwdt);

/**
 * @brief Request a warm reboot from a peripheral model (e.g. the watchdog).
 *
 * @details A peripheral block cannot perform the reboot itself (the run loop in
 * main.c owns that), so it records a request here and the run loop polls
 * ::board_periph_reset_take_request once per chunk. Used by the WDT model when
 * its down-counter underflows in reset mode. Exactly one of the flags should be
 * true.
 *
 * @param[in] watchdog true for a watchdog-0 reset (RSTSR1.WDTRF).
 * @param[in] iwdt     true for an independent-watchdog reset (RSTSR1.IWDTRF).
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_reset_request_reboot(bool watchdog, bool iwdt);

/**
 * @brief Consume a pending peripheral reboot request, if any (run-loop side).
 *
 * @details Polled by the run loop each chunk. If a peripheral requested a warm
 * reboot (::board_periph_reset_request_reboot), reports which cause and clears
 * the request so it fires once.
 *
 * @param[out] out_watchdog Set true if the request was a watchdog-0 reset.
 * @param[out] out_iwdt     Set true if the request was an independent-WDT reset.
 * @return true if a request was pending (and consumed); false otherwise.
 * @since 0.1.0
 */
bool board_periph_reset_take_request(bool* out_watchdog, bool* out_iwdt);

/**
 * @brief Wire a host sink that receives every byte the firmware transmits.
 *
 * @details
 * The SCI_B model calls @p sink once per byte written to a channel's TDR (the
 * transmit-data register, used by both the polled and interrupt TX paths and by
 * FIFO mode, which also writes TDR). main.c installs a sink that prints each
 * byte to stdout with a clear @c [uart] prefix so a console example's output is
 * captured and greppable. When no sink is installed, transmitted bytes are
 * still counted for the end-of-run summary but not echoed.
 *
 * @param[in] sink Callback invoked as @c sink(channel, byte) per TX byte, or
 *                 NULL to detach. The model owns no copy of @p byte.
 * @return Nothing.
 * @post Subsequent TDR writes are delivered to @p sink.
 * @since 0.1.0
 */
void board_periph_sci_set_tx_sink(void (*sink)(uint8_t channel, uint8_t byte));

/**
 * @brief Queue host->firmware bytes for a channel's receive path.
 *
 * @details
 * Appends @p len bytes to the channel's RX queue. The SCI_B model asserts
 * CSR.RDRF (and the FIFO RX data flags) while the queue is non-empty, returns
 * queued bytes from reads of RDR oldest-first, and -- if the firmware armed
 * RXI (CCR0.RIE) and routed the channel's RXI event through the ICU -- pends the
 * RXI interrupt so interrupt-driven receive also runs. main.c feeds this from
 * @c --input and/or stdin so a console example sees real input. Bytes beyond
 * the per-channel queue capacity are dropped (reported on @p --trace).
 *
 * @param[in] channel SCI channel index (0..9). Out-of-range is ignored.
 * @param[in] data    Source bytes (copied into the queue); ignored if NULL.
 * @param[in] len     Number of bytes to queue.
 * @return Nothing.
 * @post Up to the queue's free space of @p data is readable via RDR and RDRF
 *       reflects availability.
 * @since 0.1.0
 */
void board_periph_sci_feed_rx(uint8_t channel, const uint8_t* data, uint32_t len);

/**
 * @brief Arm a pending touch contact for the modelled GT911 device.
 *
 * @details
 * ra8_emulator turns a @c --click argument or a live board_view mouse-down into a
 * single pending contact here. The contact is answered through the REAL firmware
 * path: ra8_touch_read issues a GT911 status read over ra8_i3c_transfer (the I3C
 * peripheral in legacy I2C mode), and the modelled GT911 device -- registered on
 * the modelled I3C/I2C bus at its 7-bit address -- reports a status byte with one
 * point plus a point0 record carrying @p x / @p y. The contact is one-shot: once
 * the firmware reads the point record it is cleared and ::board_periph_touch_reported
 * is incremented, so the next frame reads "no frame ready" exactly as the real
 * controller would after a tap is drained. There is no function-level touch hook;
 * the firmware's ra8_touch -> I3C -> GT911 code runs unchanged.
 *
 * @param[in] x Panel X coordinate of the contact (GT911-native units).
 * @param[in] y Panel Y coordinate of the contact.
 * @return Nothing.
 * @post The next GT911 status read reports a buffer-ready frame with one point.
 * @since 0.1.0
 */
void board_periph_touch_inject(uint16_t x, uint16_t y);

/**
 * @brief Count of touch contacts the firmware has drained from the GT911 model.
 *
 * @details
 * Incremented each time the firmware reads the GT911 point0 record for an armed
 * contact (i.e. a real ra8_touch_read -> I3C -> GT911 point fetch completed). The
 * run loop uses this -- instead of a stub-side counter -- to know a headless
 * @c --click has flowed all the way through the real touch path before it drains
 * the post-click settle window.
 *
 * @return Number of contacts reported through the modelled GT911.
 * @since 0.1.0
 */
uint32_t board_periph_touch_reported(void);

/**
 * @brief Clear the modelled GT911 injected-touch sequence FIFO.
 *
 * @details
 * The FIFO is the multi-tap analogue of ::board_periph_touch_inject: instead of
 * one re-armed contact it queues a SEQUENCE of distinct raw points, delivering
 * the next queued point on each ``ra8_touch_read`` frame the firmware drains.
 * It exists so an interactive N-point flow -- e.g. the touch-calibration example
 * (touch_cal, #262), which must collect one raw sample per on-screen target --
 * can run headless in ra8_emulator: on silicon a human taps N cross-hairs; in EIL
 * the CLI (@c --touch-seq, ::board_periph_touch_seq_push) supplies N synthetic
 * raw taps that return through the genuine ``ra8_touch_read`` decode. Resetting
 * empties the queue and drops any point armed from it.
 *
 * @return Nothing.
 * @post The sequence FIFO is empty; the next status read reports "no frame".
 * @since 0.1.0
 */
void board_periph_touch_seq_reset(void);

/**
 * @brief Queue one raw touch point onto the modelled GT911 injection FIFO.
 *
 * @details
 * Appends (@p x, @p y) to the sequence the GT911 model serves one point per
 * drained frame (see ::board_periph_touch_seq_reset). While the FIFO is
 * non-empty every GT911 status read reports a buffer-ready frame with one
 * contact, and the matching point0 read returns the head point and advances the
 * queue -- exactly as a real GT911 latches the next physical touch after the
 * controller drains and acks the current one. Points are consumed in push
 * order, so callers push them in the same order the firmware presents targets.
 *
 * @param[in] x Panel X coordinate of the queued contact (GT911-native units).
 * @param[in] y Panel Y coordinate of the queued contact.
 * @return true if the point was queued; false if the FIFO is full.
 * @post On true the queued depth grows by one.
 * @since 0.1.0
 */
bool board_periph_touch_seq_push(uint16_t x, uint16_t y);

/**
 * @brief Read the last driven output level of a board LED.
 *
 * @details
 * Read-only accessor over the GPIO/PORT model's per-LED latch shadow, so the
 * graphical board view can light each indicator without reaching into module
 * internals. The level is the active-high pin drive recorded by the PORT write
 * path: 1 once the firmware drives the LED's pin high, 0 once it drives it low.
 *
 * @param[in] led Board LED identity (::board_led_id_t).
 * @return 1 if the LED's pin is currently driven high, else 0 (0 for an
 *         out-of-range @p led).
 * @since 0.1.0
 */
uint32_t board_periph_led_level(board_led_id_t led);

/**
 * @brief Drive a GPIO pin's input level from outside the firmware.
 *
 * @details
 * Lets the harness inject a pin level that the firmware reads back through PIDR
 * (PCNTR2) -- the model for a physical input such as a user push-button. The
 * board's active-low switches SW1 (P009) / SW2 (P008) idle high (released) and
 * are pulled low to model a press, so ``--button`` (and a live-view key) can
 * exercise button-driven firmware paths (e.g. gpio_input_demo: SW1 -> LED1).
 * Only the named pin's input is affected; output pins still read their latch.
 *
 * @param[in] port  PORT index (0-based; PORT0 == 0).
 * @param[in] pin   Pin number within the port (0..15).
 * @param[in] level Injected level: true = high, false = low.
 * @return Nothing.
 *
 * @pre The peripheral model has been initialised.
 * @pre @p port / @p pin are within range (out-of-range is ignored).
 * @post Subsequent PIDR reads of @p pin (when configured as input) see @p level.
 * @post Output pins are unaffected (they read their driven latch).
 * @note Not thread-safe; single-threaded harness use.
 * @since 0.1.0
 */
void board_periph_gpio_set_input(uint8_t port, uint8_t pin, bool level);

/**
 * @brief Read the externally-injected input level of a GPIO pin.
 *
 * @details Returns the level last set by ::board_periph_gpio_set_input for @p
 * pin (the board's switches idle high). Lets an interactive caller toggle a
 * push-button by reading the current state and writing its inverse -- e.g. an
 * on-screen SW1 click flips P009 between released (high) and pressed (low).
 *
 * @param[in] port PORT index (0-based; PORT0 == 0).
 * @param[in] pin  Pin number within the port (0..15).
 * @return The injected level (true = high, false = low); false if out of range.
 *
 * @pre The peripheral model has been initialised.
 * @note Not thread-safe; single-threaded harness use.
 * @since 0.1.0
 */
bool board_periph_gpio_get_input(uint8_t port, uint8_t pin);

/**
 * @brief The on-colour of a board LED as a packed RGB565 value.
 *
 * @details
 * Returns the real EK-RA8D2 indicator colour the LED emits when driven high --
 * LED1 blue (P600), LED2 green (P303), LED3 red (PA07) per the BSP -- encoded
 * as RGB565 so the board view can fill the indicator in the genuine colour
 * (and the @c --ppm composite, also RGB565, captures it for verification). The
 * value is the lit colour regardless of the live level; pair it with
 * ::board_periph_led_level to decide lit vs dark.
 *
 * @param[in] led Board LED identity (::board_led_id_t).
 * @return RGB565 on-colour (0 for an out-of-range @p led).
 * @since 0.1.0
 */
uint16_t board_periph_led_color_rgb565(board_led_id_t led);

/**
 * @brief The most recent complete line the firmware transmitted over any SCI.
 *
 * @details
 * The SCI_B TX path captures each transmitted byte into a line buffer that is
 * latched on newline (CR is dropped), so the board view can show the last
 * console line a non-display example printed -- e.g. @c "hello, ra8d2!" from
 * uart_hello -- on its "UART:" status line. Returns a pointer to static
 * storage holding the last completed line (empty string before the first
 * newline); the pointer must not be freed and is valid until the next TX byte.
 *
 * @return NUL-terminated last UART line (never NULL; empty until one is sent).
 * @since 0.1.0
 */
const char* board_periph_uart_last_line(void);

/**
 * @brief Total bytes the firmware has transmitted over all SCI channels.
 *
 * @details Sum of every modelled SCI channel's TX byte counter -- a coarse
 * "how chatty is this firmware" figure the board view shows beside the console
 * panel. Counts raw TDR-write bytes (including CR/LF), not completed lines.
 *
 * @return Total SCI TX byte count since reset.
 * @since 0.1.0
 */
uint32_t board_periph_uart_tx_total(void);

/**
 * @brief Number of times a given NVIC line was taken in this run.
 *
 * @param[in] irq NVIC line number (0-based).
 * @return Times the engine vectored in @p irq (0 if never, or out of range).
 * @since 0.1.0
 */
uint32_t board_periph_irq_count(uint32_t irq);

/**
 * @brief Total NVIC interrupts the ICU has delivered in this run.
 *
 * @return Sum of every taken IRQ (the board view's "IRQ" activity total).
 * @since 0.1.0
 */
uint32_t board_periph_irq_total(void);

/**
 * @brief Report the coordinates of the most recently drained touch contact.
 *
 * @details
 * Read-only accessor over the GT911 model's last-reported point, so the board
 * view can show @c "touch x,y" for the last tap the firmware drained through
 * the real ra8_touch -> I3C -> GT911 path. Writes nothing when no contact has
 * been reported yet.
 *
 * @param[out] x Receives the last contact's X coordinate (unchanged if none).
 * @param[out] y Receives the last contact's Y coordinate (unchanged if none).
 * @return true if at least one contact has been drained (and @p x / @p y were
 *         written), false otherwise.
 * @since 0.1.0
 */
bool board_periph_touch_last(uint16_t* x, uint16_t* y);

/**
 * @brief Set the emulated battery state surfaced by the MAX17048 fuel gauge.
 *
 * @details
 * The firmware reads state-of-charge + charge direction from a MAX17048-class
 * fuel gauge at I2C 0x36; this drives that device's register file. Set from the
 * CLI (``--battery <pct>`` / ``--charge``) before the run. @p soc_pct is clamped
 * to 0..100.
 *
 * @param[in] soc_pct  State-of-charge percent (clamped to [0, 100]).
 * @param[in] charging true marks the charger attached (CRATE reads positive).
 * @since 0.1.0
 */
void board_periph_battery_set(uint8_t soc_pct, bool charging);

/**
 * @brief Read back the emulated battery state (for the status overlay).
 *
 * @param[out] out_soc      Receives the state-of-charge percent (NULL ok).
 * @param[out] out_charging Receives the charging flag (NULL ok).
 * @since 0.1.0
 */
void board_periph_battery_get(uint8_t* out_soc, bool* out_charging);

/**
 * @brief The SCI channel the EK-RA8D2 console (J-Link OB VCOM) uses.
 *
 * @details
 * PD02 TXD / PD03 RXD route to SCI8 on the EK-RA8D2 v1, surfaced as the board's
 * debug-console UART (mirrored from libs/ra8_board_ek_ra8d2). main.c feeds
 * @c --input / stdin to this channel by default.
 *
 * @return The console SCI channel index (8 on the EK-RA8D2).
 * @since 0.1.0
 */
uint8_t board_periph_sci_console_channel(void);

/**
 * @brief Dispatch an MMIO read to the owning block, if any.
 *
 * @details
 * Looks up @p addr in the block table; on a hit the block's read handler
 * returns the register value and @p *handled is set true. On a miss @p *handled
 * is false and the caller falls back to the sparse model.
 *
 * @param[in,out] uc      Unicorn engine (handlers may read emulated memory).
 * @param[in]     addr    Absolute peripheral address being read.
 * @param[in]     size    Access width in bytes (1/2/4).
 * @param[out]    handled True iff a modelled block answered the read.
 * @return The register value when @p *handled is true, else 0.
 * @since 0.1.0
 */
uint64_t board_periph_read(uc_engine* uc, uint64_t addr, unsigned size, bool* handled);

/**
 * @brief Dispatch an MMIO write to the owning block, if any.
 *
 * @param[in,out] uc      Unicorn engine (handlers may read emulated memory).
 * @param[in]     addr    Absolute peripheral address being written.
 * @param[in]     size    Access width in bytes (1/2/4).
 * @param[in]     value   Value being written.
 * @param[out]    handled True iff a modelled block consumed the write.
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value, bool* handled);

/**
 * @brief Advance every modelled timer by one emulation chunk and raise events.
 *
 * @details
 * Called once per run-loop chunk (the same cadence as one SysTick period). Each
 * running AGT / GPT counter steps by its per-chunk increment; a wrap past the
 * period sets the block's status flag (overflow / underflow / compare-match)
 * and, if that event is linked through the ICU with its NVIC line enabled,
 * records a pending IRQ for the engine to take. Stopped timers do not advance.
 * The SCI_B model is also serviced here: with TX always drained in the model,
 * an enabled TXI / TEI re-pends each tick so an interrupt-driven transmitter
 * keeps streaming, and an enabled RXI pends while queued RX bytes remain.
 *
 * @param[in,out] uc Unicorn engine (the ICU reads IELSR / NVIC ISER from PPB).
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_tick(uc_engine* uc);

/**
 * @brief Set or clear a NVIC line's enable in the model's set-enable shadow.
 *
 * @details
 * The Cortex-M NVIC ISER / ICER registers are set-enable / clear-enable: a
 * written 1 sets (ISER) or clears (ICER) that interrupt line and a written 0 is
 * ignored, so several independent stores accumulate. ra8_emulator maps the PPB as
 * plain RAM, where a raw @c "1 << bit" store to ISER would instead overwrite the
 * whole word and drop every other enabled line. main.c decodes ISER / ICER
 * writes and calls this so the ICU model sees the correct accumulated enable
 * state -- essential once firmware enables more than one line at once (SCI
 * RXI + TXI + TEI, and the USB controller lines in Phase 3).
 *
 * @param[in] irq    NVIC line number (0-based).
 * @param[in] enable true to set the line's enable, false to clear it.
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_nvic_set_enable(uint32_t irq, bool enable);

/**
 * @brief Pop the next pending, enabled NVIC IRQ number the ICU has queued.
 *
 * @details
 * The software half of "the ICU asserts a line and the NVIC latches it". The
 * run loop calls this at an instruction boundary; the returned IRQ number is
 * vectored in by the engine's exception layer as a real Cortex-M exception
 * (vector 16 + IRQn from VTOR). Priority and PRIMASK are handled by that layer,
 * so this only reports a line that is event-linked and NVIC-enabled.
 *
 * @param[out] out_irq Receives the IRQ number (0-based, NVIC line) on success.
 * @return true if a pending IRQ was popped into @p out_irq.
 * @since 0.1.0
 */
bool board_periph_next_irq(uint32_t* out_irq);

/**
 * @brief Record that NVIC IRQ @p irq was actually taken (for the summary).
 *
 * @param[in] irq IRQ number that the engine just vectored in.
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_note_irq_taken(uint32_t irq);

/**
 * @brief Active GLCDC graphics-layer framebuffer descriptor.
 *
 * @details
 * The decode the GLCDC model (board_periph_glcdc.c) recovers from the graphics
 * layer registers the firmware programmed: where the scanned-out framebuffer
 * lives in emulated memory and how to read it. ::board_periph_glcdc_get_framebuffer
 * fills this so a harness can fetch the rendered pixels out of modelled RAM
 * (on-chip SRAM or external SDRAM at 0x68000000) and checksum them.
 *
 * @invariant @c stride >= @c width * (bytes-per-pixel of @c format).
 * @invariant @c base points into a modelled RAM window whenever this descriptor
 *            is returned; @c enabled additionally reflects the BG output stage.
 * @see board_periph_glcdc_get_framebuffer
 */
typedef struct {
  uint32_t base;    /**< Framebuffer base address (GRn FLM2.BASE).            */
  uint32_t width;   /**< Width in pixels (line stride / bytes-per-pixel).     */
  uint32_t height;  /**< Height in pixels (GRn FLM5.LNNUM + 1).               */
  uint32_t stride;  /**< Line stride in bytes (GRn FLM3.LNOFF[31:16]).        */
  uint8_t  format;  /**< Pixel format code (GRn FLM6.FORMAT[30:28]).          */
  uint8_t  layer;   /**< Source layer: 1 = GR1 (upper), 2 = GR2 (lower).      */
  bool     enabled; /**< True when the layer is fetching (FLMRD set + BG EN). */
} board_glcdc_fb_t;

/**
 * @brief Report the active GLCDC graphics-layer framebuffer, if one is programmed.
 *
 * @details
 * Reads the descriptor the GLCDC model snooped from the firmware's graphics-layer
 * register writes (GR1 preferred; GR2 if only it is enabled). Lets a ra8_emulator
 * harness locate and checksum the rendered framebuffer in emulated memory without
 * re-deriving the layout from raw registers. The pixels themselves are read with
 * @c uc_mem_read at @c base; this only returns the layout.
 *
 * @param[out] out Receives the active framebuffer descriptor on success; left
 *                 unchanged when no layer is programmed. Ignored if NULL.
 * @return true when a graphics layer has a framebuffer programmed (and @p out was
 *         written), false otherwise.
 * @post On true, @p out->base points into a modelled RAM window.
 * @post On false, @p out is untouched.
 * @note Not thread-safe; single-threaded run-loop / report use.
 * @since 0.1.0
 */
bool board_periph_glcdc_get_framebuffer(board_glcdc_fb_t* out);

/**
 * @brief Print the peripheral-model section of the end-of-run summary.
 *
 * @details
 * Reports the final driven level of each board LED and its transition count,
 * each modelled timer's final counter / event totals, the per-IRQ taken count,
 * and each active SCI channel's transmitted / received byte totals -- the
 * observability the epic asks for (GPIO/LED transitions + per-IRQ interrupt
 * counts + captured serial), beyond the generic MMIO table main.c already
 * prints.
 *
 * @param[in,out] uc Unicorn engine (read for any final register state).
 * @return Nothing.
 * @since 0.1.0
 */
void board_periph_report(uc_engine* uc);

#ifdef __cplusplus
}
#endif
