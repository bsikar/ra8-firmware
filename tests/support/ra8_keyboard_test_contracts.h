/**
 * @file ra8_keyboard_test_contracts.h
 * @brief Private contracts for keyboard unit-test helpers.
 * @details Declares lookup, input, reachability, and vector helpers while the
 * implementation remains file-local in `test_ra8_keyboard.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @brief Find a character key in the active layout.
 * @details Scans the bounded key array for a matching lowercase character.
 * @param[in] ch Character to locate.
 * @return Matching key index or the published no-hit sentinel.
 * @retval k_ra8_kbd_no_hit No active character key matches.
 * @pre `s_kb` contains a layout with a bounded count.
 * @pre `ch` is the deliberate lookup character for the vector.
 * @post No layout or text state is modified.
 * @post Any returned index is less than `s_kb.count`.
 * @note Matching is against the unshifted character field.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_key_of(char ch);

/**
 * @brief Find the first active key of a requested kind.
 * @details Performs a bounded source-order scan of the active layout.
 * @param[in] kind Non-character key kind to locate.
 * @return Matching key index or the no-hit sentinel.
 * @retval k_ra8_kbd_no_hit No key has the requested kind.
 * @pre `s_kb` contains a layout with a bounded count.
 * @pre `kind` is representable by `ra8_kbd_key_kind_t`.
 * @post No layout or text state is modified.
 * @post The lowest matching index is returned.
 * @note Used for shift, backspace, and commit actions.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_key_of_kind(ra8_kbd_key_kind_t kind);

/**
 * @brief Find the layer-toggle key targeting a requested layer.
 * @details Matches both the layer kind and its auxiliary target value.
 * @param[in] aux Layer identifier to locate.
 * @return Matching key index or the no-hit sentinel.
 * @retval k_ra8_kbd_no_hit No toggle targets `aux`.
 * @pre `s_kb` contains a layout with a bounded count.
 * @pre `aux` is a deliberate layer fixture value.
 * @post No layout or text state is modified.
 * @post Any returned index designates a layer key.
 * @note The auxiliary byte is part of the public key descriptor.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_key_of_layer(uint8_t aux);

/**
 * @brief Tap one laid-out key at its center and apply it.
 * @details Verifies hit-testing selects `idx` before invoking the text action.
 * @param[in,out] t Text editor state receiving the key action.
 * @param[in] idx Active key index to tap.
 * @pre `idx` is within the fixed key-array capacity.
 * @pre The selected key has valid laid-out geometry.
 * @post Hit testing returns exactly `idx`.
 * @post Applying the selected key returns `k_ra8_ok`.
 * @note Assertions make invalid layout geometry fail immediately.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_tap(ra8_kbd_text_t* t, uint8_t idx);

/**
 * @brief Type a lowercase string through active character keys.
 * @details Looks up and taps each byte until the terminating null character.
 * @param[in,out] t Text editor state receiving characters.
 * @param[in] s Null-terminated lowercase string reachable on the active layer.
 * @pre `t` and `s` are non-null.
 * @pre Every non-null byte in `s` has an active character key.
 * @post Each input byte has been applied in order.
 * @post The layout remains unchanged.
 * @note Intended for short fixed test strings only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_type_lc(ra8_kbd_text_t* t, const char* s);

/**
 * @brief Verify the letters layer key inventory and frame containment.
 * @details Builds the default layout and checks count, characters, controls,
 * and every key rectangle against the configured frame.
 * @pre The fixed frame has positive width and height.
 * @pre The layout output object is writable.
 * @post Exactly the expected 31 keys are present.
 * @post Every key lies fully inside the frame.
 * @note The false frame arms are covered by a dedicated guard vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_layout_letters(void);

/**
 * @brief Verify case changes, layer toggles, typing, and commit.
 * @details Drives text state through letters and numeric layers using hit tests.
 * @pre All required action keys are present in their expected layers.
 * @pre The text buffer has capacity for the test phrase.
 * @post Shift and layer actions update the visible layout correctly.
 * @post Commit reports the expected final text state.
 * @note Exercises the user-facing keyboard workflow end to end.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_typing_layers(void);

/**
 * @brief Report whether one character is reachable on the active layer.
 * @details Performs a bounded scan of character-key lower and upper glyphs.
 * @param[in] c Character whose reachability is queried.
 * @return True when any active key emits the character.
 * @retval false No active key emits `c`.
 * @pre `s_kb` contains a bounded active layout.
 * @pre `c` is a printable ASCII fixture character.
 * @post No layout or text state is modified.
 * @post The result reflects both shifted and unshifted glyph fields.
 * @note Used to prove complete printable-symbol coverage.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_reachable_here(char c);

/**
 * @brief Verify every printable ASCII symbol is reachable across layers.
 * @details Builds each keyboard layer and checks its assigned character set.
 * @pre Layer construction succeeds for letters, numbers, and symbols.
 * @pre Printable ASCII fixture bounds are stable.
 * @post Every digit and symbol is reachable on at least one layer.
 * @post No duplicate action changes the text state during reachability scans.
 * @note Pins the published keyboard character repertoire.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_all_ascii_symbols(void);

/**
 * @brief Verify glyph selection and boundary text actions.
 * @details Checks shift glyphs, empty backspace, full-buffer append, and edges.
 * @pre The letters layout and text fixture are initialized.
 * @pre The fixed text capacity is known to the vector.
 * @post Key glyphs reflect shift state.
 * @post Backspace and overflow no-op paths preserve valid text state.
 * @note Covers visible labels as well as mutation edges.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_glyph_and_edges(void);

/**
 * @brief Prove both frame-dimension guards reject independently.
 * @details Supplies zero width, zero height, and a valid control frame.
 * @pre The output layout is writable.
 * @pre Frame coordinates remain representable during the test.
 * @post Each nonpositive dimension returns invalid argument.
 * @post The positive control frame builds successfully.
 * @note Supplies the N+1 vectors for the frame rejection OR.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_frame_reject_mcdc(void);

/**
 * @brief Prove both key-glyph guard conditions control fallback.
 * @details Compares null layout, high index, and valid-key requests.
 * @pre The valid control layout contains at least one key.
 * @pre The output glyph buffer is writable where required.
 * @post Each invalid input returns the documented fallback safely.
 * @post The valid input returns its active key glyph.
 * @note Covers the public glyph trust boundary.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_key_glyph_guard_mcdc(void);

/**
 * @brief Verify public keyboard entry points reject null arguments.
 * @details Exercises build, hit, apply, and glyph guards with absent storage.
 * @pre The production keyboard implementation is linked.
 * @pre Valid control objects are available for single-null arms.
 * @post Each invalid call returns its documented error or sentinel.
 * @post No valid fixture object is corrupted by a rejected call.
 * @note Consolidates simple public null guards.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_null_guards(void);
