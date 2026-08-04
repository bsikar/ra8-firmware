/**
 * @file c6_probe.h
 * @brief Shared contract for the EK-RA8D2 <-> ESP32-C6 esp-hosted SPI probe.
 *
 * @details
 * This header is the **probe's own** contract: its budgets, the Pmod1
 * side-band and muxed-net maps, the evidence it accumulates, and the entry
 * point of each module. The esp-hosted **wire format** it decodes is a
 * separate concern and lives in `c6_proto.h`, which this header includes.
 *
 * The probe is split into one module per concern, all driven by `main.c`:
 *
 *   - `src/c6_console.c`  -- bounded console formatters (no newlib printf).
 *   - `src/c6_sideband.c` -- Pmod1 side-band sampling, the muxed-net wire
 *                            test, the pull-up contest that identifies
 *                            DATA_READY, and the chip-select hunt that
 *                            identifies HANDSHAKE. None of the three needs
 *                            a working data path.
 *   - `src/c6_frame.c`    -- the `c6_proto.h` half: payload-header decode,
 *                            checksum and classification.
 *   - `src/c6_xfer.c`     -- one full 1600-byte esp-hosted transaction.
 *
 * Nothing from esp-hosted-mcu is vendored; every protocol constant is
 * hand-decoded from the pinned upstream tree (commit `949bb30`, firmware
 * `2.12.11`) and cites its source in `c6_proto.h`. The side-band semantics
 * this file relies on come from upstream's `docs/spi_full_duplex.md` and
 * `slave/main/spi_slave_api.c` -- what the C6 drives. LEGACY-OK: upstream path
 *
 *
 * [Ring 6 / App] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "c6_proto.h"
#include "ra8_err.h"
#include "ra8_spi.h"

/* =============================================================================
 * 1. Tunables and budgets
 * =============================================================================
 */

/**
 * @enum c6_probe_tunable_t
 * @brief Timing and clock budgets for the probe.
 *
 * @details
 * ``k_c6_probe_sck_hz`` starts an order of magnitude below the 5 MHz that
 * esp-hosted-mcu ``docs/spi_full_duplex.md`` section 4.1 recommends for
 * evaluation, because this is the first time the link has ever been
 * clocked. ``k_c6_probe_boot_wait_ms`` covers the C6 booting its own
 * bootloader plus the esp-hosted application after a shared power cycle --
 * the RA8D2 is out of reset long before the C6 has queued anything.
 *
 * @invariant Every value is a positive, statically known bound.
 *
 * @par Example:
 * @code
 * ra8_delay_ms((uint32_t)k_c6_probe_boot_wait_ms);
 * @endcode
 *
 * @see c6_probe_budget_t
 */
typedef enum : uint32_t {
  k_c6_probe_uart_baud    = 115200U,  /**< Console baud (J-Link OB VCOM).     */
  k_c6_probe_sck_hz       = 1000000U, /**< Conservative first-light SCK rate. */
  k_c6_probe_boot_wait_ms = 1500U,    /**< Let the C6 finish booting.         */
  k_c6_probe_settle_ms    = 20U,      /**< Post-transfer side-band settle.    */
  k_c6_probe_gap_ms       = 100U,     /**< Gap between transactions.          */
  k_c6_probe_cs_hold_ms   = 2U,       /**< Chip-select setup / hold.          */
  k_c6_probe_idle_ms      = 500U,     /**< Background heartbeat period.       */
} c6_probe_tunable_t;

/**
 * @enum c6_probe_budget_t
 * @brief Statically provable loop bounds (NASA Power of 10 Rule 2).
 *
 * @details
 * Every loop in the probe counts against one of these, so no loop bound
 * depends on data received from the C6 -- a peripheral that answers with
 * garbage can waste a bounded amount of time and nothing more.
 *
 * ``k_c6_probe_min_votes`` is an evidence bar rather than a loop bound, and
 * it exists because of a real mis-identification on the bench: an
 * unconnected side-band pin floats, and a single noise transition on it is
 * indistinguishable from one real edge, so a mapping claimed on one vote
 * named a floating pin while the truly-connected pin sat unresolved. A
 * mapping is therefore claimed only from repeated, agreeing evidence.
 *
 * @invariant Every value is non-zero.
 *
 * @par Example:
 * @code
 * for (uint8_t i = 0U; i < (uint8_t)k_c6_probe_xfer_per_mode; i++) { ... }
 * @endcode
 *
 * @see c6_probe_tunable_t
 */
typedef enum : uint8_t {
  k_c6_probe_xfer_per_mode  = 4U,   /**< Transactions attempted per SPI mode.  */
  k_c6_probe_mode_count     = 4U,   /**< SPI modes swept.                      */
  k_c6_probe_hs_poll_max    = 100U, /**< Handshake polls before giving up.     */
  k_c6_probe_hs_poll_ms     = 5U,   /**< Delay between handshake polls.        */
  k_c6_probe_dump_bytes     = 32U,  /**< Payload bytes hex-dumped per frame.   */
  k_c6_probe_str_max        = 96U,  /**< Longest console literal accepted.     */
  k_c6_probe_pull_samples   = 8U,   /**< Reads per pin in the pull-up contest. */
  k_c6_probe_pull_settle_ms = 5U,   /**< Settle between pull-up contest reads. */
  k_c6_probe_min_votes      = 2U,   /**< Votes required to claim a mapping.    */
} c6_probe_budget_t;

/**
 * @enum c6_probe_fmt_t
 * @brief Radix and field-width constants used by the console formatters.
 *
 * @details
 * Kept at file scope rather than inside each formatter: a function-local
 * ``typedef`` that a body only reads through its enumerators trips
 * ``-Werror=unused-local-typedefs`` under the project warning profile.
 *
 * @invariant ``k_c6_fmt_dec_digits`` is wide enough for ``UINT32_MAX``.
 *
 * @par Example:
 * @code
 * c6_probe_put_hex(header_byte, (uint8_t)k_c6_fmt_hex_byte);
 * @endcode
 *
 * @see c6_probe_put_hex
 */
typedef enum : uint8_t {
  k_c6_fmt_dec_radix  = 10U,   /**< Base ten.                          */
  k_c6_fmt_dec_digits = 10U,   /**< "4294967295" is ten digits.        */
  k_c6_fmt_hex_max    = 8U,    /**< A uint32_t is eight nibbles.       */
  k_c6_fmt_hex_bits   = 4U,    /**< Bits per nibble.                   */
  k_c6_fmt_hex_mask   = 0x0FU, /**< Nibble mask.                       */
  k_c6_fmt_hex_alpha  = 10U,   /**< First nibble printed as a letter.  */
  k_c6_fmt_hex_byte   = 2U,    /**< Nibbles printed for one byte.      */
  k_c6_fmt_hex_word   = 4U,    /**< Nibbles printed for a 16-bit word. */
} c6_probe_fmt_t;

/* =============================================================================
 * 2. Side-band pins
 * =============================================================================
 */

/**
 * @enum c6_sideband_idx_t
 * @brief Index into the Pmod1 side-band pin table.
 *
 * @details
 * Pmod1 exposes four signals besides the four SPI lines, and unlike SPI
 * pins 1..4 they are not affected by the board's Pmod1 mode mux. Which of
 * them the C6's DATA_READY (its GPIO4) and HANDSHAKE (its GPIO6) are
 * soldered to is exactly what this app determines, so all four are sampled
 * and none is assumed.
 *
 * @invariant ``k_c6_sb_count`` equals the length of the side-band pin table.
 *
 * @par Example:
 * @code
 * c6_sideband_sample_t s = {};
 * c6_probe_sample_sideband(&s);
 * @endcode
 *
 * @see c6_sideband_sample_t
 */
typedef enum : uint8_t {
  k_c6_sb_irq    = 0U, /**< Pmod1.7  IRQ,   P006. */
  k_c6_sb_reset  = 1U, /**< Pmod1.8  RESET, P402. */
  k_c6_sb_gpio_a = 2U, /**< Pmod1.9  GPIO,  P412. */
  k_c6_sb_gpio_b = 3U, /**< Pmod1.10 GPIO,  P413. */
  k_c6_sb_count  = 4U, /**< Side-band pin count.  */
} c6_sideband_idx_t;

/**
 * @struct c6_sideband_sample_t
 * @brief One simultaneous reading of every side-band pin.
 * @invariant Each entry is 0 or 1.
 *
 * @par Example:
 * @code
 * c6_sideband_sample_t s = {};
 * c6_probe_sample_sideband(&s);
 * const bool irq_high = (s.level[k_c6_sb_irq] != 0U);
 * @endcode
 *
 * @see c6_probe_sample_sideband
 */
typedef struct {
  uint8_t level[k_c6_sb_count]; /**< Per-pin logic level (0 or 1). */
} c6_sideband_sample_t;

/**
 * @enum c6_wire_idx_t
 * @brief Index into the Pmod1 muxed-net table.
 *
 * @details
 * EK-RA8D2 UM Table 17 p 26 shows Pmod1 pins 1..4 are *muxed* on the
 * board: which MCU pin reaches J26 depends on SW4-1 / SW4-2 (UM Table 18
 * p 26) and, for the Octo-SPI overlap, on SW4-3 (UM Table 3 p 16). Only in
 * the SPI position does the set {P804, P801, P802, P803} reach J26 pins
 * 1..4; the UART position swaps J26-1 to P800 and J26-4 to P804, and the
 * I2C position routes J26-3 / J26-4 to P512 / P511 entirely. All five
 * candidates are exercised so the log states which mux position the board
 * is actually in rather than assuming one.
 *
 * @invariant ``k_c6_wire_count`` equals the length of the muxed-net table.
 *
 * @par Example:
 * @code
 * c6_probe_wire_test();   // prints one line per candidate
 * @endcode
 *
 * @see c6_wire_kind_t
 */
typedef enum : uint8_t {
  k_c6_wire_p800  = 0U, /**< CTS2  -- J26-1 only in the UART position. */
  k_c6_wire_p801  = 1U, /**< COPI2 -- J26-2 in SPI and UART positions. */
  k_c6_wire_p802  = 2U, /**< CIPO2 -- J26-3 in SPI and UART positions. */
  k_c6_wire_p803  = 3U, /**< SCK2  -- J26-4 only in the SPI position.  */
  k_c6_wire_p804  = 4U, /**< SS2   -- J26-1 in SPI, J26-4 in UART.     */
  k_c6_wire_count = 5U, /**< Muxed-net candidate count.                */
} c6_wire_idx_t;

/**
 * @enum c6_wire_kind_t
 * @brief What the drive-and-release test says is on the far end of a net.
 *
 * @details
 * Each net is driven high, released to a no-pull input and sampled, then
 * driven low, released and sampled again. A net with nothing on it holds
 * whatever it was last driven to on its own parasitic capacitance; a net
 * terminated by a pull resistor or an active driver snaps back to that
 * termination's level instead.
 *
 * @invariant Exactly one kind describes any sample pair.
 *
 * @par Example:
 * @code
 * const c6_wire_kind_t k = c6_probe_wire_kind(after_high, after_low);
 * @endcode
 *
 * @see c6_probe_wire_test
 */
typedef enum : uint8_t {
  k_c6_wire_floating  = 0U, /**< Held both levels: no termination.      */
  k_c6_wire_low_side  = 1U, /**< Snapped to 0: pull-down or driven low. */
  k_c6_wire_high_side = 2U, /**< Snapped to 1: pull-up or driven high.  */
  k_c6_wire_odd       = 3U, /**< Inverted result: not physically sane.  */
} c6_wire_kind_t;

/**
 * @struct c6_probe_stats_t
 * @brief Running evidence gathered across every transaction.
 *
 * @details
 * Two independent kinds of evidence live here and they are not equally
 * strong. The vote counters are *behavioural*: they need the C6 to move a
 * pin while the probe happens to be sampling. ``pull_low`` is *electrical*:
 * a pin read as an input with the internal pull-up engaged can only read low
 * if something off-chip is sinking the pull-up current, which no floating
 * pin can fake. ::c6_probe_resolve_map weighs them in that order.
 *
 * @invariant ``hs_vote`` / ``dr_vote`` are only ever incremented.
 * @invariant ``pull_low[i] <= pull_samples`` for every ``i``.
 *
 * @par Example:
 * @code
 * c6_probe_stats_t st = {};
 * (void)c6_probe_sweep_mode(k_ra8_spi_mode_3, &st, &hs_idx);
 * @endcode
 *
 * @see c6_probe_vote
 */
typedef struct {
  uint32_t attempts;                 /**< Transactions clocked.                */
  uint32_t idle_frames;              /**< Idle filler frames decoded.          */
  uint32_t data_frames;              /**< Real frames with a good checksum.    */
  uint32_t bad_csum_frames;          /**< Sane shape, checksum mismatched.     */
  uint32_t hs_vote[k_c6_sb_count];   /**< Pin dropped while CS was asserted.   */
  uint32_t dr_vote[k_c6_sb_count];   /**< Pin dropped once the queue drained.  */
  uint8_t  ever_high[k_c6_sb_count]; /**< Pin was seen high at least once.     */
  uint8_t  ever_low[k_c6_sb_count];  /**< Pin was seen low at least once.      */
  uint8_t  pull_low[k_c6_sb_count];  /**< Reads that lost to an external sink. */
  uint8_t  pull_samples;             /**< Pull-up reads per pin; 0 = not run.  */
} c6_probe_stats_t;

/* =============================================================================
 * 3. Module entry points
 * =============================================================================
 */

/**
 * @brief Emit a bounded, NUL-terminated ASCII literal on the console.
 *
 * @details
 * Measures the literal with a statically bounded scan
 * (::k_c6_probe_str_max) rather than calling ``strlen``, keeping NASA
 * Power of 10 Rule 2 provable without linking newlib string code.
 *
 * @param[in] text NUL-terminated ASCII string; ignored when NULL.
 *
 * @pre The board UART console has been initialised.
 * @pre ``text`` is NUL-terminated within ::k_c6_probe_str_max bytes.
 * @post At most ::k_c6_probe_str_max bytes were queued to the console.
 * @post ``text`` is unmodified.
 *
 * @note Not thread-safe; single-threaded logging only.
 *
 * @see c6_probe_put_u32
 * @since 0.1.0
 */
void c6_probe_puts(const char* text);

/**
 * @brief Emit an unsigned integer in decimal.
 *
 * @param[in] value Value to print; zero prints as a single ``0``.
 *
 * @pre The board UART console has been initialised.
 * @pre ``value`` fits in 32 bits by construction.
 * @post Between one and ten ASCII digits were queued.
 * @post No trailing separator was emitted.
 *
 * @note Not thread-safe.
 *
 * @see c6_probe_put_hex
 * @since 0.1.0
 */
void c6_probe_put_u32(uint32_t value);

/**
 * @brief Emit a value as fixed-width lowercase hexadecimal.
 *
 * @param[in] value  Value to print.
 * @param[in] digits Number of nibbles to emit (1..8).
 *
 * @pre The board UART console has been initialised.
 * @pre ``digits`` is between one and eight inclusive.
 * @post Exactly ``digits`` characters were queued when in range.
 * @post Nothing is queued when ``digits`` is out of range.
 *
 * @note Not thread-safe.
 *
 * @see c6_probe_put_u32
 * @since 0.1.0
 */
void c6_probe_put_hex(uint32_t value, uint8_t digits);

/**
 * @brief Claim the four Pmod1 side-band pins as no-pull digital inputs.
 *
 * @details
 * No internal pull is applied for *sampling*: the C6 drives HANDSHAKE and
 * DATA_READY push-pull, and holding a pull-up during the run would make an
 * unconnected pin indistinguishable from an asserted one -- precisely the
 * distinction this app exists to make. ::c6_probe_pull_contest engages the
 * pull-up briefly and on purpose, then restores the pins to this state.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok All four side-band pins are inputs.
 * @retval k_ra8_err_gpio_conflict A pin is already owned elsewhere.
 * @retval k_ra8_err_gpio_invalid_port Board pin table disagrees with the HAL.
 *
 * @pre ``ra8_mstp_init`` has run so PFS writes land.
 * @pre No other driver owns the Pmod1 side-band pins.
 * @post On success every side-band pin is a digital input.
 * @post No side-band pin is left driven.
 *
 * @note Not thread-safe; boot-time only.
 *
 * @see c6_probe_sample_sideband
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t c6_probe_sideband_init(void);

/**
 * @brief Read every Pmod1 side-band pin into one sample.
 *
 * @param[out] out Destination sample; ignored when NULL.
 *
 * @pre Every side-band pin was configured as an input.
 * @pre ``out`` is non-NULL for the sample to be stored.
 * @post On success every ``out->level`` entry is 0 or 1.
 * @post A pin whose read fails is recorded as 0 rather than left stale.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * c6_sideband_sample_t s = {};
 * c6_probe_sample_sideband(&s);
 * @endcode
 *
 * @see c6_probe_print_sideband
 * @since 0.1.0
 */
void c6_probe_sample_sideband(c6_sideband_sample_t* out);

/**
 * @brief Print a labelled side-band sample as ``label P006=1 P402=0 ...``.
 *
 * @param[in] label Short prefix, already indented by the caller.
 * @param[in] s     Sample to print; ignored when NULL.
 *
 * @pre The board UART console has been initialised.
 * @pre ``s`` is non-NULL for anything to be printed.
 * @post Exactly one console line was emitted when ``s`` is non-NULL.
 * @post Neither argument is modified.
 *
 * @note Not thread-safe.
 *
 * @see c6_probe_sample_sideband
 * @since 0.1.0
 */
void c6_probe_print_sideband(const char* label, const c6_sideband_sample_t* s);

/**
 * @brief Fold one transaction's three samples into the classification votes.
 *
 * @details
 * HANDSHAKE is the pin that is high before the transfer and low while the
 * chip-select is asserted, because the C6 is built with
 * ``CONFIG_ESP_SPI_DEASSERT_HS_ON_CS=y`` and clears it from its
 * chip-select edge interrupt (``gpio_disable_hs_isr_handler`` in
 * the C6's peripheral-side SPI driver). That is a genuine identification:
 * the pin moves, twice per transaction, in step with a line the probe
 * itself drives.
 *
 * The DATA_READY vote is far weaker and must not be read as its equal. It
 * counts a pin that was high before the transfer and still low well after
 * it, which is what the C6 does when it *drains* a queued frame
 * (``get_next_tx_buffer``). That is a once-per-boot event: the C6 raises
 * DATA_READY for its queued INIT event and lowers it when the first
 * transaction takes it, after which the pin stays low and every later
 * ``pre`` sample is already low, so the vote cannot fire again. Worse, that
 * single transition usually lands *between* transactions -- during the
 * chip-select hunt, say -- where nothing is sampling, so in practice the
 * counter often stays at zero for the correctly-wired pin. The vote proves
 * DATA_READY when it fires and says nothing at all when it does not, which
 * is why ::c6_probe_resolve_map prefers the pull-up contest and keeps this
 * counter only as a fallback.
 *
 * @param[in]     pre  Sample taken with the chip-select released.
 * @param[in]     mid  Sample taken with the chip-select asserted.
 * @param[in]     post Sample taken after the chip-select was released again.
 * @param[in,out] st   Statistics block to accumulate into.
 *
 * @pre All four pointers are non-NULL.
 * @pre The three samples come from the same transaction, in order.
 * @post Vote counters only ever grow.
 * @post ``ever_high`` / ``ever_low`` reflect all three samples.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * c6_probe_vote(&pre, &mid, &post, &stats);
 * @endcode
 *
 * @see c6_probe_best
 * @since 0.1.0
 */
void c6_probe_vote(const c6_sideband_sample_t* pre,
                   const c6_sideband_sample_t* mid,
                   const c6_sideband_sample_t* post,
                   c6_probe_stats_t*           st);

/**
 * @brief Pick the side-band pin that won the vote outright, or report none.
 *
 * @details
 * A winner must clear two bars, and both exist because the bench produced
 * the failure they prevent. It must reach ``min_votes``, so one transition
 * on a floating pin cannot name a mapping; and it must be a *strict*
 * winner, so a tie is reported as unresolved rather than silently broken by
 * table order. Reporting "unresolved" is a useful answer here -- claiming
 * the wrong pin is not.
 *
 * @param[in] votes     Per-pin vote counters.
 * @param[in] ignore    Index to skip, or ::k_c6_sb_count to skip nothing.
 * @param[in] min_votes Smallest vote count that may claim a mapping; must
 *                      be at least one.
 *
 * @return Winning pin index, or ::k_c6_sb_count when none qualifies.
 * @retval k_c6_sb_count No pin reached ``min_votes``, the top score was
 *                       tied, ``votes`` was NULL, or ``min_votes`` was zero.
 *
 * @pre ``votes`` is non-NULL for a winner to be reported.
 * @pre ``ignore`` is a valid index or ::k_c6_sb_count.
 * @post The returned index is either a strict winner or ::k_c6_sb_count.
 * @post ``votes`` is unmodified.
 *
 * @note Pure function; safe from any context.
 *
 * @par Example:
 * @code
 * const uint8_t hs = c6_probe_best(stats.hs_vote,
 *                                  (uint8_t)k_c6_sb_count,
 *                                  (uint32_t)k_c6_probe_min_votes);
 * @endcode
 *
 * @see c6_probe_resolve_map
 * @since 0.1.0
 */
uint8_t c6_probe_best(const uint32_t* votes, uint8_t ignore, uint32_t min_votes);

/**
 * @brief Find which side-band pins an external driver is holding low.
 *
 * @details
 * Re-claims each side-band pin as an input with the RA8D2's internal
 * pull-up engaged, reads it ::k_c6_probe_pull_samples times, and restores
 * it to a no-pull input. A pin with nothing on it is pulled high and reads
 * high every time; a pin that keeps reading low is losing a current fight
 * to something off-chip, which nothing floating can imitate. That is the
 * strongest "this pin is connected" evidence the probe can gather, and it
 * needs no cooperation from the C6 whatsoever.
 *
 * It is what identifies DATA_READY. An esp-hosted peripheral with an empty
 * transmit queue holds DATA_READY low by design, so across a whole probe run
 * that pin moves once at most, and usually where nothing is sampling. The
 * pin that matters most is therefore the one a transition-counting rule
 * cannot see -- and the one this rule reads without ambiguity.
 *
 * One line per pin is printed, so the raw count is in the log even when the
 * verdict is unresolved.
 *
 * @param[in,out] st Statistics block; ``pull_low`` and ``pull_samples`` are
 *                   written.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Every pin was tested and restored to a no-pull input.
 * @retval k_ra8_err_null_ptr ``st`` was NULL.
 * @retval k_ra8_err_gpio_conflict A side-band pin is owned elsewhere.
 *
 * @pre ::c6_probe_sideband_init has claimed the side-band pins.
 * @pre The chip-select is released and no transaction is in flight.
 * @post On success every side-band pin is a no-pull input again.
 * @post On success ``st->pull_samples`` is ::k_c6_probe_pull_samples.
 *
 * @warning Timing is part of the measurement, in both directions. Run it
 *          only with the chip-select released -- HANDSHAKE is legitimately
 *          low while it is asserted, and would read as a sunk pin. Run it
 *          only *after* the transaction sweep as well: a freshly-booted C6
 *          holds DATA_READY high for its queued INIT event until the first
 *          transaction drains it, so an early contest finds nothing sunk.
 *
 * @note Not thread-safe; boot-time diagnostic only.
 *
 * @par Example:
 * @code
 * if (c6_probe_pull_contest(&stats) != k_ra8_ok) { panic(); }
 * @endcode
 *
 * @see c6_probe_sunk_pin
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t c6_probe_pull_contest(c6_probe_stats_t* st);

/**
 * @brief Name the one pin that lost every read of the pull-up contest.
 *
 * @details
 * Reads the verdict out of the evidence ::c6_probe_pull_contest gathered: a
 * pin qualifies only when ``pull_low`` equals ``pull_samples``, i.e. it read
 * low on every single sample taken against the pull-up.
 *
 * Two conditions are deliberately reported as "no answer" rather than as a
 * winner. **Unanimity** is required because a pin that reads low only
 * sometimes is being driven by something that is not an idle DATA_READY --
 * a coupled edge, a shared ground bounce -- and naming it would be a guess.
 * **Uniqueness** is required because two sunk pins mean the caller's
 * exclusion (typically the resolved HANDSHAKE) has not narrowed the field to
 * one, and picking either by table order is the exact failure this whole
 * mechanism exists to prevent.
 *
 * @param[in] st     Statistics block filled by ::c6_probe_pull_contest.
 * @param[in] ignore Index to skip, or ::k_c6_sb_count to skip nothing.
 *
 * @return Index of the uniquely sunk pin, or ::k_c6_sb_count.
 * @retval k_c6_sb_count No pin was sunk, more than one was, the contest was
 *                       never run, or ``st`` was NULL.
 *
 * @pre ``st`` is non-NULL for a verdict to be reported.
 * @pre ``ignore`` is a valid index or ::k_c6_sb_count.
 * @post ``st`` is unmodified.
 * @post The returned index is unique, or ::k_c6_sb_count.
 *
 * @note Pure function; safe from any context.
 *
 * @par Example:
 * @code
 * const uint8_t dr = c6_probe_sunk_pin(&stats, hs_idx);
 * @endcode
 *
 * @see c6_probe_pull_contest
 * @since 0.1.0
 */
uint8_t c6_probe_sunk_pin(const c6_probe_stats_t* st, uint8_t ignore);

/**
 * @brief Resolve the HANDSHAKE and DATA_READY side-band map from the
 *        accumulated evidence.
 *
 * @details
 * The whole identification policy lives here, in one place, ranked by how
 * hard the evidence is to fake:
 *
 *   1. **HANDSHAKE** is the pin that wins the vote outright -- more
 *      chip-select-tracking transitions than any other pin, and at least
 *      ::k_c6_probe_min_votes of them. The C6 drives that edge itself, so
 *      repeated transitions in step with a line the probe controls are not
 *      something noise produces.
 *   2. **DATA_READY** is the pin that loses the pull-up contest outright,
 *      excluding whichever pin took HANDSHAKE. An idle esp-hosted
 *      peripheral holds DATA_READY low, and only a real connection can hold
 *      a pin down against the pull-up.
 *   3. Failing that, DATA_READY falls back to the drain-transition vote
 *      under the same threshold -- the only evidence available if the C6
 *      had a frame queued and drained it during the run.
 *
 * Any step that cannot decide yields ::k_c6_sb_count, and the caller prints
 * ``unresolved``. That is deliberate: the previous policy took the highest
 * vote unconditionally, and a single noise transition on an unconnected pin
 * was enough to publish a wrong pin map that read exactly like a measured
 * one.
 *
 * @param[in]  st     Accumulated evidence.
 * @param[out] hs_idx Resolved HANDSHAKE index, or ::k_c6_sb_count.
 * @param[out] dr_idx Resolved DATA_READY index, or ::k_c6_sb_count.
 *
 * @pre All three pointers are non-NULL.
 * @pre ``st`` holds the evidence of a completed run.
 * @post ``*hs_idx`` and ``*dr_idx`` are valid indices or ::k_c6_sb_count.
 * @post ``*hs_idx != *dr_idx`` unless both are ::k_c6_sb_count.
 *
 * @note Pure with respect to ``st``; safe from any context.
 *
 * @par Example:
 * @code
 * uint8_t hs = 0U;
 * uint8_t dr = 0U;
 * c6_probe_resolve_map(&stats, &hs, &dr);
 * @endcode
 *
 * @see c6_probe_pull_contest
 * @since 0.1.0
 */
void c6_probe_resolve_map(const c6_probe_stats_t* st, uint8_t* hs_idx, uint8_t* dr_idx);

/**
 * @brief Name a side-band pin for the console.
 *
 * @param[in] idx Side-band index, or ::k_c6_sb_count for "unresolved".
 *
 * @return Static label for the pin.
 * @retval "unresolved" ``idx`` was out of range.
 *
 * @pre ``idx`` is a valid index or ::k_c6_sb_count.
 * @pre The returned pointer is never freed by the caller.
 * @post The returned string is NUL-terminated.
 * @post No state is modified.
 *
 * @note Pure function; safe from any context.
 *
 * @see c6_probe_print_sideband
 * @since 0.1.0
 */
const char* c6_probe_sideband_name(uint8_t idx);

/**
 * @brief Classify one Pmod1 net from its two drive-and-release samples.
 *
 * @param[in] after_high Level read after driving the net high and releasing.
 * @param[in] after_low  Level read after driving the net low and releasing.
 *
 * @return The termination classification.
 * @retval k_c6_wire_floating  Held both driven levels: nothing attached.
 * @retval k_c6_wire_low_side  Snapped low both times: pull-down or driver.
 * @retval k_c6_wire_high_side Snapped high both times: pull-up or driver.
 * @retval k_c6_wire_odd       Inverted pair; not physically expected.
 *
 * @pre Both samples come from the same net, high test first.
 * @pre Each sample is 0 or 1.
 * @post Exactly one classification is returned.
 * @post Neither argument is modified.
 *
 * @note Pure function; safe from any context.
 *
 * @par Example:
 * @code
 * const c6_wire_kind_t k = c6_probe_wire_kind(1U, 0U);  // floating
 * @endcode
 *
 * @see c6_probe_wire_test
 * @since 0.1.0
 */
c6_wire_kind_t c6_probe_wire_kind(uint8_t after_high, uint8_t after_low);

/**
 * @brief Run the drive-and-release test over every Pmod1 muxed net.
 *
 * @details
 * Prints one line per candidate so the log states, without inference, what
 * terminates each MCU pin that the board's Pmod1 mux might connect to J26.
 *
 * @pre The console is up and the Pmod1 SPI pins are unclaimed.
 * @pre ``ra8_time_init`` has run so the settle delays are real.
 * @post Every tested pin is left unclaimed, ready for PFS routing.
 * @post Exactly ::k_c6_wire_count result lines were printed.
 *
 * @note Not thread-safe; boot-time diagnostic only.
 *
 * @par Example:
 * @code
 * c6_probe_wire_test();
 * @endcode
 *
 * @see c6_probe_cs_hunt
 * @since 0.1.0
 */
void c6_probe_wire_test(void);

/**
 * @brief Find which MCU pin the board's Pmod1 mux has wired to the C6's
 *        chip-select, and identify HANDSHAKE at the same time.
 *
 * @details
 * The C6 image sets ``CONFIG_ESP_SPI_DEASSERT_HS_ON_CS=y``, so its
 * chip-select edge interrupt drops HANDSHAKE the moment chip-select is
 * asserted and re-raises it once the next transaction is queued
 * (``gpio_disable_hs_isr_handler`` / ``spi_post_setup_cb`` in
 * the C6's peripheral-side SPI driver). Asserting each muxed-net candidate in
 * turn and watching for a side-band pin to drop therefore needs no clock,
 * no payload and no working data path: the pin that provokes the drop is
 * the real chip-select, and the pin that drops is HANDSHAKE.
 *
 * @param[in,out] st Statistics block; handshake votes are accumulated here.
 *
 * @return Index into the muxed-net table of the pin that reached the C6.
 * @retval k_c6_wire_count No candidate provoked a side-band response.
 *
 * @pre The console is up and the Pmod1 SPI pins are unclaimed.
 * @pre ``st`` is non-NULL.
 * @post Every tested pin is left unclaimed.
 * @post ``st->hs_vote`` records every observed drop.
 *
 * @note Safe against contention: every candidate is an input on the C6.
 *
 * @par Example:
 * @code
 * const uint8_t cs = c6_probe_cs_hunt(&stats);
 * @endcode
 *
 * @see c6_probe_wire_test
 * @since 0.1.0
 */
uint8_t c6_probe_cs_hunt(c6_probe_stats_t* st);

/**
 * @brief Route the Pmod1 SPI pins to SCI2 and own the chip-select as GPIO.
 *
 * @details
 * The three clocked signals go to their SCI function (``PSEL = 00100b``,
 * which HUM Ch 20.6 "Multiplexed Pin Function Selector" maps to SCI
 * 0/2/4/6/8), while the chip-select stays a GPIO so one assertion can span
 * the whole 1600-byte esp-hosted frame.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok All four pins routed or claimed.
 * @retval k_ra8_err_gpio_conflict A pin is already owned elsewhere.
 * @retval k_ra8_err_gpio_invalid_port Board pin table disagrees with the HAL.
 *
 * @pre ``ra8_mstp_init`` has run so PFS writes land.
 * @pre The diagnostics released every Pmod1 pin they claimed.
 * @post On success the chip-select is an output driven high (deasserted).
 * @post On success SCK / CIPO / COPI carry their SCI2 function.
 *
 * @note Not thread-safe; boot-time only.
 *
 * @par Example:
 * @code
 * if (c6_probe_spi_pins_init() != k_ra8_ok) { panic(); }
 * @endcode
 *
 * @see c6_probe_sweep_mode
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t c6_probe_spi_pins_init(void);

/**
 * @brief Run the transaction burst for one SPI mode.
 *
 * @param[in]     mode     Clock polarity / phase to open the channel with.
 * @param[in]     pclka_hz SCI baud-clock source, in hertz.
 * @param[in,out] st       Statistics block to accumulate into.
 * @param[in,out] hs_idx   Handshake index, refined as evidence accumulates.
 *
 * @return ``true`` when the C6 answered with a recognisable frame.
 * @retval true  At least one idle or data frame decoded in this mode.
 * @retval false The mode produced nothing recognisable.
 *
 * @pre ``pclka_hz`` is non-zero.
 * @pre ``st`` and ``hs_idx`` are non-NULL.
 * @post The SPI channel is closed again before returning.
 * @post ``*hs_idx`` names the best handshake candidate seen so far.
 *
 * @note Not thread-safe.
 *
 * @par Example:
 * @code
 * const bool up = c6_probe_sweep_mode(k_ra8_spi_mode_3, pclka, &st, &hs);
 * @endcode
 *
 * @see c6_probe_spi_pins_init
 * @since 0.1.0
 */
bool c6_probe_sweep_mode(ra8_spi_mode_t    mode,
                         uint32_t          pclka_hz,
                         c6_probe_stats_t* st,
                         uint8_t*          hs_idx);
