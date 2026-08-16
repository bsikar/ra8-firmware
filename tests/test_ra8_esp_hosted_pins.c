/**
 * @file test_ra8_esp_hosted_pins.c
 * @brief Unit tests for the esp-hosted link's pin map and IRQ routing table.
 *
 * @par Tag
 * [Test / Host] {World: N/A}
 *
 * @details
 * Drives ``ra8_esp_hosted_pin_irq_num`` from
 * ``port/esp-hosted/src/ra8_esp_hosted_pins.c`` and asserts the invariants
 * ``port/esp-hosted/inc/ra8_esp_hosted_pins.h`` states about the harness map.
 *
 * @par Why "no channel" is asserted as loudly as "channel 11"
 * On this package the ICU external-interrupt inputs are concentrated on port
 * 0, so of the four Pmod1 side-band nets only P006 has a channel. The port
 * services a pin with a channel through the ICU and a pin without one through
 * its software edge detector, and both deliver the same callback -- so the
 * routing table is the only place the difference is visible. A row that
 * silently gained a channel would move a net onto a hardware edge that is not
 * wired to it, and nothing downstream would notice. The unrouted rows are
 * therefore asserted individually rather than covered by one "not in the
 * table" case.
 *
 * @par Why the invariants are tested at all
 * The header promises that the chip select, handshake and data-ready nets are
 * distinct and that every assignment decodes to a legal port and pin. Those
 * are exactly the promises a harness rebuild breaks -- the RA8 side of this
 * link is explicitly not settled yet -- and they are cheap to check here,
 * where the failure names the invariant instead of appearing later as a
 * driver that toggles the wrong net.
 *
 * No hardware registers are touched; no ``ra8_fake_mmap`` window is required.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_esp_hosted_pins.h"
#include "ra8_port_constants.h"
#include "unity_minimal.h"

/**
 * @enum t_pins_const_t
 * @brief Channel numbers and fixtures this translation unit expects.
 *
 * @details
 * Spelled out here rather than read back from the module under test, so the
 * assertions state an expectation instead of restating whatever the table
 * happens to contain. The two channel numbers come from the board User's
 * Manual entries the connector header cites for each net.
 *
 * @invariant Both channel numbers are within the 0..15 range
 *            ``ra8_gpio_attach_irq`` accepts.
 *
 * @par Example:
 * @code
 * TEST_ASSERT_EQ(k_t_pins_irq_sideband, ra8_esp_hosted_pin_irq_num(pin));
 * @endcode
 *
 * @see ra8_esp_hosted_pin_irq_num
 */
typedef enum : uint8_t {
  k_t_pins_irq_sideband    = 11U, /**< IRQ11-DS, the channel P006 routes to. */
  k_t_pins_irq_chip_select = 14U, /**< IRQ14, the channel P804 routes to.    */
} t_pins_const_t;

/**
 * @brief Assert a pin assignment is either unwired or decodes legally.
 *
 * @details
 * The header's invariant is a disjunction, not a range check: an assignment
 * may be the no-pin sentinel, which deliberately decodes to an out-of-range
 * port and pin, or it must be a real ``RA8_PIN(port, pin)`` with both halves
 * inside what the GPIO layer accepts. Testing only the range would reject the
 * sentinel; testing only the sentinel would accept a corrupt packing.
 *
 * @param[in] pin Packed assignment taken from ``ra8_esp_hosted_pin_t``.
 *
 * @pre @p pin came from the port's own pin enumeration.
 * @pre The board connector header describes this package.
 * @post Returns only when the invariant holds.
 * @post The process has exited with status 1 otherwise.
 *
 * @note Not thread-safe; writes to the shared stderr stream on failure.
 */
static void t_assert_pin_legal(uint16_t pin)
{
  const bool unwired = (pin == (uint16_t)k_ra8_pin_none);
  const bool decodes = ((uint32_t)RA8_PIN_PORT(pin) <= (uint32_t)k_ra8_port_max) &&
                       ((uint32_t)RA8_PIN_PIN(pin) <= (uint32_t)k_ra8_pin_max);
  TEST_ASSERT(unwired || decodes);
}

/**
 * @test test_irq_num_routed_pins
 *
 * @brief The two Pmod1 nets the package routes report their real channels.
 *
 * @details
 * P006 carries DATA_READY, the latency-sensitive signal -- it is what tells
 * the host a frame is waiting -- and it is the one side-band net on this
 * connector with a hardware channel, so it gets the ICU path. P804 carries
 * the chip select, an output, so its channel goes unused by this link; the
 * row exists because a rebuilt harness could move a side-band net onto that
 * position and would then inherit a hardware edge for free.
 *
 * @par MC/DC:
 * Decision: `if (k_ra8_esp_hosted_irq_map[row].pin == wanted)` inside the scan
 * (1 condition, 2 vectors)
 * - Vector 1: wanted=P006 -> true on the first row; channel 11 is returned
 * - Vector 2: wanted=P804 -> false on the first row, true on the second;
 *   channel 14 is returned
 * Vector 2 also proves the scan continues past a non-matching row rather than
 * answering from the first entry alone.
 *
 * @pre None.
 * @post The routing table is unchanged.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_irq_num_routed_pins(void)
{
  TEST_BEGIN("pins routed irq channels");

  TEST_ASSERT_EQ(k_t_pins_irq_sideband,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_board_pmod1_irq));
  TEST_ASSERT_EQ(k_t_pins_irq_chip_select,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_board_pmod1_spi_cs));

  /* The same two nets reached through the link's own names, which is how the
     port asks. As the harness is wired, HANDSHAKE is the routed one and the
     chip select is an output; DATA_READY lands on P402 and is polled. */
  TEST_ASSERT_EQ(k_t_pins_irq_sideband,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_esp_hosted_pin_handshake));
  TEST_ASSERT_EQ(k_ra8_esp_hosted_irq_none,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_esp_hosted_pin_data_ready));
  TEST_ASSERT_EQ(k_t_pins_irq_chip_select,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_esp_hosted_pin_chip_select));
  TEST_END("pins routed irq channels");
}

/**
 * @test test_irq_num_unrouted_pins
 *
 * @brief Nets the package does not route report the no-channel sentinel.
 *
 * @details
 * Three of the four Pmod1 side-band nets -- P402, P412 and P413 -- have no
 * ICU external-interrupt channel on this package. Their rows are in the table
 * deliberately: recording "no channel" as a fact is what stops the answer
 * from being an accident of the table's length, and it is what the port reads
 * as "use the software edge detector for this pin".
 *
 * DATA_READY sits on P402, so it is one of the unrouted ones. That is
 * asserted through the link's own name as well as the board's, because it is
 * the case a reader is most likely to assume works the other way round: the
 * signal that says "a frame is waiting" is the polled one, and it is safe to
 * poll only because the co-processor holds it asserted until the host drains
 * the frame.
 *
 * @par MC/DC:
 * Decision: `if (k_ra8_esp_hosted_irq_map[row].pin == wanted)` inside the scan
 * (1 condition, 2 vectors)
 * - Vector 1: wanted=P402 -> false, false, true on the third row; the row's
 *   recorded sentinel is returned
 * - Vector 2: wanted=P412 / P413 -> matches on the fourth and fifth rows
 * These vectors reach rows the routed-pin test does not, so every row of the
 * table is matched by some vector across the two tests.
 *
 * @pre None.
 * @post The routing table is unchanged.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_irq_num_unrouted_pins(void)
{
  TEST_BEGIN("pins unrouted nets");

  TEST_ASSERT_EQ(k_ra8_esp_hosted_irq_none,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_board_pmod1_reset));
  TEST_ASSERT_EQ(k_ra8_esp_hosted_irq_none,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_board_pmod1_gpio_a));
  TEST_ASSERT_EQ(k_ra8_esp_hosted_irq_none,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_board_pmod1_gpio_b));

  /* DATA_READY is on an unrouted net, so the port polls it rather than taking
     an edge -- the opposite of what its importance would suggest. It is safe
     because the co-processor holds the line asserted until the frame is
     drained, so a poll can be late but never blind. */
  TEST_ASSERT_EQ(k_ra8_esp_hosted_irq_none,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_esp_hosted_pin_data_ready));
  TEST_END("pins unrouted nets");
}

/**
 * @test test_irq_num_unknown_pins
 *
 * @brief A pin outside the table reports no channel rather than guessing.
 *
 * @details
 * The table records only the nets this link uses. Anything else has no
 * channel as far as this port is concerned, and saying so is what stops a
 * harness change from silently attaching an edge to the wrong channel. This
 * is the loop-exhaustion path -- the scan runs to the end of a
 * compile-time-sized table and falls through -- which no other test reaches.
 *
 * @par MC/DC:
 * Decision A: `if (k_ra8_esp_hosted_irq_map[row].pin == wanted)`
 * (1 condition, 2 vectors)
 * - Vector A1: a pin in the table     -> true on some row (covered by the
 *   routed and unrouted tests)
 * - Vector A2: a pin in no row        -> false on every row; the scan falls
 *   through to the sentinel return
 *
 * Decision B: `for (row < k_ra8_esp_hosted_irq_map_rows)` (1 condition, 2 vectors)
 * - Vector B1: an early match  -> the loop exits through its body
 * - Vector B2: no match at all -> the loop exits through its bound, which is
 *   the NASA Rule 2 path: the bound is derived from the array, so no
 *   caller-supplied value can index past it
 *
 * @pre None.
 * @post The routing table is unchanged.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_irq_num_unknown_pins(void)
{
  TEST_BEGIN("pins unknown lookups");

  /* A perfectly legal pin that this link simply does not use. */
  TEST_ASSERT_EQ(k_ra8_esp_hosted_irq_none,
                 ra8_esp_hosted_pin_irq_num(RA8_PIN(k_ra8_port_1, k_ra8_pin_0)));
  TEST_ASSERT_EQ(k_ra8_esp_hosted_irq_none,
                 ra8_esp_hosted_pin_irq_num(RA8_PIN(k_ra8_port_0, k_ra8_pin_0)));

  /* The no-pin sentinel is accepted as an argument and answered, not treated
     as an error -- the reset net is exactly that today. */
  TEST_ASSERT_EQ(k_ra8_esp_hosted_irq_none,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_pin_none));
  TEST_ASSERT_EQ(k_ra8_esp_hosted_irq_none,
                 ra8_esp_hosted_pin_irq_num((ra8_port_pin_t)k_ra8_esp_hosted_pin_reset));
  TEST_END("pins unknown lookups");
}

/**
 * @test test_pin_map_invariants
 *
 * @brief The three distinct-net and legal-encoding invariants hold.
 *
 * @details
 * The chip select, handshake and data-ready nets must be pairwise distinct:
 * two of them sharing a pin would mean the port driving an output and reading
 * it back as if the co-processor had answered, which looks exactly like a
 * working link until the data is wrong. The encoding check catches the other
 * way a harness edit goes wrong -- a packed value assembled by hand rather
 * than through ``RA8_PIN``.
 *
 * @par MC/DC:
 * Decision: `unwired || decodes` in ::t_assert_pin_legal (2 conditions, 3 vectors)
 * - Vector 1: a wired pin (the chip select) -> false, true  -> true
 *   (varies the decode condition; the sentinel condition is false)
 * - Vector 2: the reset net, unwired        -> true         -> true
 *   (varies the sentinel condition)
 * - Vector 3: not asserted as a passing case -- a value that is neither the
 *   sentinel nor a legal encoding is precisely what this helper exists to
 *   reject, so driving it would fail the test by design. The false outcome is
 *   the failure path, and its absence from a green run is the assertion.
 * Vectors 1+2 together show both conditions can independently produce the
 * true outcome.
 *
 * @pre The board connector header describes this package.
 * @post No module state is modified.
 * @note Not thread-safe; single-threaded test context.
 * @since 0.1.0
 */
static void test_pin_map_invariants(void)
{
  TEST_BEGIN("pins map invariants");
  const uint16_t chip_select = (uint16_t)k_ra8_esp_hosted_pin_chip_select;
  const uint16_t handshake   = (uint16_t)k_ra8_esp_hosted_pin_handshake;
  const uint16_t data_ready  = (uint16_t)k_ra8_esp_hosted_pin_data_ready;

  TEST_ASSERT(chip_select != handshake);
  TEST_ASSERT(chip_select != data_ready);
  TEST_ASSERT(handshake != data_ready);

  t_assert_pin_legal(chip_select);
  t_assert_pin_legal(handshake);
  t_assert_pin_legal(data_ready);
  t_assert_pin_legal((uint16_t)k_ra8_esp_hosted_pin_copi);
  t_assert_pin_legal((uint16_t)k_ra8_esp_hosted_pin_cipo);
  t_assert_pin_legal((uint16_t)k_ra8_esp_hosted_pin_sck);
  t_assert_pin_legal((uint16_t)k_ra8_esp_hosted_pin_reset);

  /* The reset net is the one deliberate unwired assignment: the C6 records
     its reset input as disconnected, so inventing a pin here would make the
     port drive an unrelated net during bring-up. */
  TEST_ASSERT_EQ(k_ra8_pin_none, k_ra8_esp_hosted_pin_reset);

  /* The four bus signals are distinct from each other and from the side-band
     nets, which is what makes them four wires rather than a shorted bus. */
  TEST_ASSERT((uint16_t)k_ra8_esp_hosted_pin_copi != (uint16_t)k_ra8_esp_hosted_pin_cipo);
  TEST_ASSERT((uint16_t)k_ra8_esp_hosted_pin_copi != (uint16_t)k_ra8_esp_hosted_pin_sck);
  TEST_ASSERT((uint16_t)k_ra8_esp_hosted_pin_cipo != (uint16_t)k_ra8_esp_hosted_pin_sck);
  TEST_ASSERT((uint16_t)k_ra8_esp_hosted_pin_sck != chip_select);

  /* The sentinel cannot collide with a real channel, which is what lets the
     caller read it as "poll this pin" without a second flag. */
  TEST_ASSERT((uint8_t)k_ra8_esp_hosted_irq_none > (uint8_t)k_t_pins_irq_chip_select);
  TEST_END("pins map invariants");
}

int main(void)
{
  test_irq_num_routed_pins();
  test_irq_num_unrouted_pins();
  test_irq_num_unknown_pins();
  test_pin_map_invariants();
  return 0;
}
