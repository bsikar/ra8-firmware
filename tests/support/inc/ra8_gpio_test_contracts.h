/**
 * @file ra8_gpio_test_contracts.h
 * @brief File-local contracts for the GPIO unit-test vectors.
 * @details Keeps complete test-helper contracts separate from the behavioral
 *          implementation so both responsibility-focused files remain below
 *          the repository's 1,000-line ceiling.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */
#pragma once

#include "ra8_attributes.h"
#include "ra8_gpio_constants.h"
#include "ra8_port_utils.h"

/**
 * @brief Reset fake GPIO hardware and pin ownership.
 * @details Clears mapped register state and the validator claim bitmap before a
 * vector runs.
 * @pre Fake MMIO support has been initialized by the test executable.
 * @pre No test vector is concurrently accessing the shared fixture.
 * @post Every fake register returns to its reset value.
 * @post Every pin claim is released.
 * @note Test-fixture helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_state(void);

/**
 * @brief Verify low-level output initialization at a low level.
 * @details Claims P000, initializes it low, and checks its PFS direction and
 * level bits.
 * @pre The fake register map can be reset and addressed.
 * @pre The pin validator can accept a fresh claim.
 * @post P000 is proven configured as a low output.
 * @post The validator is proven to own P000.
 * @note Assertions terminate the vector on the first mismatch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_output_init_happy_low(void);

/**
 * @brief Verify output initialization at the highest valid port and pin.
 * @details Claims P1415 high and checks the exact direction and output-data
 * mask.
 * @pre The fake register map can be reset and addressed.
 * @pre The pin validator can accept a fresh claim.
 * @post P1415 is proven configured as a high output.
 * @post No unrelated PFS expectation is accepted.
 * @note Exercises the upper valid packed-pin boundary.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_output_init_happy_high(void);

/**
 * @brief Verify output initialization rejects an invalid port.
 * @details Passes packed port 15 and pins the public invalid-port error.
 * @pre The shared fixture can be reset.
 * @pre The invalid packed identifier remains outside the supported port range.
 * @post The invalid-port status is observed.
 * @post No valid pin is claimed.
 * @note This is a public argument-validation vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_output_init_invalid_port(void);

/**
 * @brief Verify output initialization rejects an invalid pin number.
 * @details Passes pin 16 on port 0 and pins the public invalid-pin error.
 * @pre The shared fixture can be reset.
 * @pre The invalid packed identifier remains outside the per-port pin range.
 * @post The invalid-pin status is observed.
 * @post No valid pin is claimed.
 * @note This is a public argument-validation vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_output_init_invalid_pin(void);

/**
 * @brief Verify duplicate output claims are rejected.
 * @details Claims P102 once and confirms a second output initialization reports
 * conflict.
 * @pre The shared fixture can be reset.
 * @pre The validator records exclusive ownership.
 * @post The first claim succeeds.
 * @post The duplicate claim returns the GPIO conflict status.
 * @note The original claim intentionally remains live until the next reset.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_output_init_conflict(void);

/**
 * @brief Verify input initialization without a pull resistor.
 * @details Configures P000 as a no-pull input and checks that its PFS word
 * stays clear.
 * @pre The shared fixture can be reset.
 * @pre P000 is unclaimed.
 * @post Input initialization succeeds.
 * @post The pull-up and output bits remain clear.
 * @note Uses the zero-valued input configuration boundary.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_input_init_no_pull(void);

/**
 * @brief Verify input initialization enables pull-up selection.
 * @details Configures P102 with pull-up and checks the exact PCR bit.
 * @pre The shared fixture can be reset.
 * @pre P102 is unclaimed.
 * @post Input initialization succeeds.
 * @post P102 contains the pull-up mask and no unrelated bits.
 * @note Assertions compare the complete emulated PFS word.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_input_init_pull_up(void);

/**
 * @brief Verify input initialization rejects an invalid port.
 * @details Passes packed port 15 to the input API and checks the exact error.
 * @pre The shared fixture can be reset.
 * @pre The invalid port identifier is unchanged.
 * @post Invalid-port is returned.
 * @post No valid PFS word is modified.
 * @note This vector isolates port validation from pin validation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_input_init_invalid_port(void);

/**
 * @brief Verify input initialization rejects an invalid pin.
 * @details Passes pin 16 on port 0 to the input API and checks the exact error.
 * @pre The shared fixture can be reset.
 * @pre The invalid pin identifier is unchanged.
 * @post Invalid-pin is returned.
 * @post No valid PFS word is modified.
 * @note This vector isolates pin validation from port validation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_input_init_invalid_pin(void);

/**
 * @brief Verify a high write targets the POSR half-word.
 * @details Writes P102 high and compares PCNTR3 with the single expected set
 * bit.
 * @pre The fake port register map is resettable.
 * @pre P102 is a valid packed pin.
 * @post The write returns success.
 * @post Only the expected POSR bit is asserted.
 * @note The API permits direct writes independent of initialization in this
 * fixture.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_high_sets_posr(void);

/**
 * @brief Verify a low write targets the PORR half-word.
 * @details Writes P102 low and compares PCNTR3 with the shifted reset bit.
 * @pre The fake port register map is resettable.
 * @pre P102 is a valid packed pin.
 * @post The write returns success.
 * @post Only the expected PORR bit is asserted.
 * @note The complete PCNTR3 word is checked.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_low_sets_porr(void);

/**
 * @brief Verify writes reject an invalid port.
 * @details Calls the write API with packed port 15 and checks its error
 * contract.
 * @pre The shared fixture can be reset.
 * @pre The invalid port identifier remains out of range.
 * @post Invalid-port is returned.
 * @post No valid port write register is accepted as output.
 * @note This vector exercises validation before MMIO access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_invalid_port(void);

/**
 * @brief Verify writes reject an invalid pin.
 * @details Calls the write API with pin 16 on port 0 and checks its error
 * contract.
 * @pre The shared fixture can be reset.
 * @pre The invalid pin identifier remains out of range.
 * @post Invalid-pin is returned.
 * @post No valid port write register is accepted as output.
 * @note This vector exercises validation before MMIO access.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_write_invalid_pin(void);

/**
 * @brief Verify toggling a low output emits a set operation.
 * @details Seeds P102 low, toggles it, and checks the POSR bit in PCNTR3.
 * @pre The fake port register map can be reset and seeded.
 * @pre P102 is a valid packed pin.
 * @post Toggle returns success.
 * @post The expected POSR bit is asserted.
 * @note Models the low-to-high branch of the toggle decision.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_toggle_when_low(void);

/**
 * @brief Verify toggling a high output emits a reset operation.
 * @details Seeds P102 high, toggles it, and checks the PORR bit in PCNTR3.
 * @pre The fake port register map can be reset and seeded.
 * @pre P102 is a valid packed pin.
 * @post Toggle returns success.
 * @post The expected PORR bit is asserted.
 * @note Models the high-to-low branch of the toggle decision.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_toggle_when_high(void);

/**
 * @brief Verify toggle rejects an invalid port.
 * @details Calls toggle with packed port 15 and checks the exact error.
 * @pre The shared fixture can be reset.
 * @pre The invalid port identifier remains out of range.
 * @post Invalid-port is returned.
 * @post No valid port register change is accepted.
 * @note Validation occurs before reading the output latch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_toggle_invalid_port(void);

/**
 * @brief Verify toggle rejects an invalid pin.
 * @details Calls toggle with pin 16 on port 0 and checks the exact error.
 * @pre The shared fixture can be reset.
 * @pre The invalid pin identifier remains out of range.
 * @post Invalid-pin is returned.
 * @post No valid port register change is accepted.
 * @note Validation occurs before reading the output latch.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_toggle_invalid_pin(void);

/**
 * @brief Verify reads decode both high and low input levels.
 * @details Seeds and clears the P102 PIDR bit and checks both published enum
 * values.
 * @pre The fake port register map can be reset and seeded.
 * @pre Both output destinations are writable.
 * @post A set PIDR bit publishes high.
 * @post A clear PIDR bit publishes low.
 * @note Exercises both outcomes without changing the packed pin.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_read_high_and_low(void);

/**
 * @brief Verify reads reject a null output pointer.
 * @details Calls the read API with a valid pin and null destination.
 * @pre The shared fixture can be reset.
 * @pre P000 is a valid packed pin.
 * @post Null-pointer is returned.
 * @post No caller output object is accessed.
 * @note This pins the public output-ownership contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_read_null_out(void);

/**
 * @brief Verify reads reject an invalid port.
 * @details Reads packed port 15 into a sentinel level destination.
 * @pre The shared fixture can be reset.
 * @pre The destination object is writable.
 * @post Invalid-port is returned.
 * @post The call does not accept an unrelated valid port value.
 * @note The vector isolates port validation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_read_invalid_port(void);

/**
 * @brief Verify reads reject an invalid pin.
 * @details Reads pin 16 on port 0 into a sentinel level destination.
 * @pre The shared fixture can be reset.
 * @pre The destination object is writable.
 * @post Invalid-pin is returned.
 * @post The call does not accept an unrelated valid pin value.
 * @note The vector isolates pin validation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_read_invalid_pin(void);

/**
 * @brief Verify releasing a claimed GPIO pin clears ownership.
 * @details Claims P102 as an output, releases it, and checks the validator
 * bitmap.
 * @pre The shared fixture can be reset.
 * @pre P102 can be claimed by output initialization.
 * @post Release returns success.
 * @post P102 is no longer marked claimed.
 * @note The PFS reset itself is outside this ownership assertion.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_release_pin(void);

/**
 * @brief Verify peripheral routing programs PMR and PSEL.
 * @details Routes P102 to SCI asynchronous mode and checks the exact PFS word.
 * @pre The shared fixture can be reset.
 * @pre P102 is unclaimed and the owner string is valid.
 * @post Routing returns success.
 * @post PMR and the selected PSEL field are asserted exactly.
 * @note The owner is used only by the validator claim.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_route_peripheral_happy(void);

/**
 * @brief Verify peripheral routing rejects a null owner.
 * @details Routes a valid pin and selector with no ownership identity.
 * @pre The shared fixture can be reset.
 * @pre P102 and the selector are otherwise valid.
 * @post Null-pointer is returned.
 * @post P102 remains unclaimed.
 * @note This pins the required ownership metadata contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_route_peripheral_null_owner(void);

/**
 * @brief Verify peripheral routing rejects an invalid port.
 * @details Routes packed port 15 with a valid selector and owner.
 * @pre The shared fixture can be reset.
 * @pre The selector and owner are valid.
 * @post Invalid-port is returned.
 * @post No valid PFS word is modified.
 * @note The vector isolates packed-port validation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_route_peripheral_invalid_port(void);

/**
 * @brief Verify peripheral routing rejects an invalid pin.
 * @details Routes pin 16 on port 0 with a valid selector and owner.
 * @pre The shared fixture can be reset.
 * @pre The selector and owner are valid.
 * @post Invalid-pin is returned.
 * @post No valid PFS word is modified.
 * @note The vector isolates per-port pin validation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_route_peripheral_invalid_pin(void);

/**
 * @brief Verify duplicate peripheral routes are rejected.
 * @details Claims P102 for one owner and retries it with a different owner.
 * @pre The shared fixture can be reset.
 * @pre The selected pin and peripheral function are valid.
 * @post The initial route succeeds.
 * @post The second route returns GPIO conflict.
 * @note The first ownership record remains authoritative.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_route_peripheral_conflict(void);

/**
 * @brief Verify the injected vtable forwards output initialization and writes.
 * @details Calls both operations through ::g_ra8_gpio_pin_interface and checks
 * success.
 * @pre The shared fixture can be reset.
 * @pre The concrete GPIO interface is linked.
 * @post Output initialization succeeds through the interface.
 * @post A high write succeeds through the same interface.
 * @note Exercises dependency-injection forwarding rather than direct calls.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_vtable_output_init_and_write(void);

/**
 * @brief Verify the injected vtable forwards reads and toggles.
 * @details Reads and toggles P102 through ::g_ra8_gpio_pin_interface.
 * @pre The shared fixture can be reset.
 * @pre The concrete GPIO interface is linked.
 * @post The reset input reads low through the interface.
 * @post Toggle returns success through the interface.
 * @note Exercises non-owning interface context forwarding.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_vtable_read_and_toggle(void);

/**
 * @brief Record one synthetic GPIO interrupt callback.
 * @details Increments the fire counter and stores the caller context for later
 * assertions.
 * @param[in] ctx Opaque context supplied to the interrupt attachment API.
 * @pre The IRQ fixture counters are initialized.
 * @pre The callback executes serially in the hosted test.
 * @post The fire count increases by one.
 * @post The latest context equals @p ctx.
 * @note Test-only ISR substitute; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_stub_irq_handler(void* ctx);

/**
 * @brief Reset GPIO, ICU, ISR, and callback fixture state.
 * @details Clears shared hardware state, callback observations, and initializes
 * IRQ services.
 * @pre Fake MMIO support has been initialized.
 * @pre No IRQ vector is concurrently executing.
 * @post GPIO and callback state return to defaults.
 * @post ICU and ISR registries are initialized for a fresh vector.
 * @note Test-fixture helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_irq_state(void);

/**
 * @brief Construct the common external-IRQ configuration.
 * @details Returns the pull-up, falling-edge, filtered priority configuration
 * used by all vectors.
 * @return Complete caller-owned IRQ configuration.
 * @retval ra8_gpio_irq_cfg_t Deterministic configuration value.
 * @pre The GPIO and ICU enum values retain their documented encodings.
 * @pre No runtime fixture state is required.
 * @post The returned value has every field initialized.
 * @post No shared state is modified.
 * @note Pure value-construction helper.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_gpio_irq_cfg_t internal_make_irq_cfg(void);

/**
 * @brief Verify successful GPIO external-IRQ attachment.
 * @details Attaches IRQ3 to P102 and checks ICU configuration plus pin
 * ownership.
 * @pre The IRQ fixture can be reset and initialized.
 * @pre IRQ3 and P102 are unused.
 * @post Attachment succeeds and IRQCR becomes nonzero.
 * @post P102 is marked claimed.
 * @note Callback dispatch itself is covered by ISR-specific tests.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gpio_attach_irq_happy(void);

/**
 * @brief Verify IRQ attachment rejects a null configuration.
 * @details Passes a valid pin, number, and handler with no configuration
 * object.
 * @pre The IRQ fixture can be reset and initialized.
 * @pre IRQ3 and P102 are unused.
 * @post Null-pointer is returned.
 * @post No IRQ or pin ownership is published.
 * @note This pins configuration ownership validation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gpio_attach_irq_null_cfg(void);

/**
 * @brief Verify IRQ attachment rejects a null handler.
 * @details Passes a complete configuration with no callback function.
 * @pre The IRQ fixture can be reset and initialized.
 * @pre IRQ3 and P102 are unused.
 * @post Null-pointer is returned.
 * @post No IRQ or pin ownership is published.
 * @note A null callback context remains valid when a handler exists.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gpio_attach_irq_null_handler(void);

/**
 * @brief Verify IRQ attachment rejects an out-of-range IRQ number.
 * @details Uses IRQ16 with an otherwise valid pin, configuration, and handler.
 * @pre The IRQ fixture can be reset and initialized.
 * @pre P102 is unused.
 * @post Invalid-argument is returned.
 * @post P102 remains unclaimed.
 * @note The vector exercises the upper IRQ bound.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gpio_attach_irq_bad_num(void);

/**
 * @brief Verify IRQ attachment rejects an invalid packed pin.
 * @details Uses packed port 15 with a valid IRQ number, configuration, and
 * handler.
 * @pre The IRQ fixture can be reset and initialized.
 * @pre IRQ3 is unused.
 * @post Invalid-port is returned.
 * @post IRQ3 remains unbound.
 * @note The vector isolates GPIO validation from ICU validation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gpio_attach_irq_bad_pin(void);

/**
 * @brief Verify successful IRQ detachment reverses attachment state.
 * @details Attaches and detaches IRQ3/P102, then checks pin ownership and IRQCR
 * clearing.
 * @pre The IRQ fixture can be reset and initialized.
 * @pre IRQ3 and P102 are unused before the vector.
 * @post Detachment succeeds and P102 is released.
 * @post IRQ3 configuration reads back as zero.
 * @note The vector covers the complete attach/detach lifecycle.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gpio_detach_irq_happy(void);

/**
 * @brief Verify IRQ detachment rejects an out-of-range number.
 * @details Attempts to detach IRQ16 from an otherwise valid pin.
 * @pre The IRQ fixture can be reset and initialized.
 * @pre P102 is a valid packed pin.
 * @post Invalid-argument is returned.
 * @post No valid IRQ binding is changed.
 * @note The vector exercises detachment's IRQ bound check.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gpio_detach_irq_bad_num(void);

/**
 * @brief Verify detaching an unattached IRQ reports not found.
 * @details Resets the fixture and detaches IRQ3/P102 without a preceding
 * attachment.
 * @pre The IRQ fixture can be reset and initialized.
 * @pre IRQ3 and P102 are unused.
 * @post Not-found is returned.
 * @post The pin and IRQ registries remain empty.
 * @note Distinguishes absent ownership from invalid arguments.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_gpio_detach_irq_not_attached(void);
