/**
 * @file ra8_touch_cal_test_contracts.h
 * @brief Private contracts for touch-calibration test helpers.
 * @details Declares the file-local shims, affine helpers, workflow vectors,
 * persistence checks, and MC/DC cases implemented by `test_ra8_touch_cal.c`.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @brief Record a requested calibration target in the LCD stub.
 * @details Propagates the configured fault or appends within the fixed draw log.
 * @param[in,out] ctx Pointer to the active `stub_state_t` fixture.
 * @param[in] target Screen coordinate requested by the calibration routine.
 * @return The configured fault or success.
 * @retval k_ra8_ok The target was accepted by the stub.
 * @pre `ctx` points to a live fixture.
 * @pre The draw count is bounded by the published target count.
 * @post Success records at most one target and advances the count accordingly.
 * @post Failure preserves the recorded draw sequence.
 * @note This is the injected draw callback used only by host tests.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_stub_draw(void* ctx, ra8_touch_cal_point_t target);

/**
 * @brief Replay one raw touch sample from the fixture.
 * @details Reads samples in source order and rejects exhaustion explicitly.
 * @param[in,out] ctx Pointer to the active `stub_state_t` fixture.
 * @param[out] out_raw Receives the next raw coordinate on success.
 * @return Sample-read status.
 * @retval k_ra8_ok A raw sample was published.
 * @pre `ctx` and `out_raw` are non-null.
 * @pre The fixture's sample count does not exceed its backing array.
 * @post Success advances the read index exactly once.
 * @post Exhaustion leaves the output and index unchanged.
 * @note This is the injected read callback used only by host tests.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_stub_read(void* ctx, ra8_touch_cal_point_t* out_raw);

/**
 * @brief Apply a known affine transform to one raw fixture point.
 * @details Evaluates both affine rows and rounds each coordinate to nearest.
 * @param[in] raw Raw coordinate to transform.
 * @param[in] m Ground-truth affine matrix.
 * @return Rounded screen coordinate.
 * @retval ra8_touch_cal_point_t The transformed fixture coordinate.
 * @pre `m` is non-null.
 * @pre The transformed coordinates are representable by `int32_t`.
 * @post The matrix and input point are unchanged.
 * @post The return value contains one rounded result per affine row.
 * @note This helper deliberately mirrors, but does not call, production apply.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_touch_cal_point_t internal_apply_truth(ra8_touch_cal_point_t         raw,
                                                               const ra8_touch_cal_matrix_t* m);

/**
 * @brief Verify an exact three-point affine fit.
 * @details Synthesizes a nondegenerate triangle and checks recovered coefficients.
 * @pre The production solver is linked.
 * @pre Floating-point arithmetic follows the host C implementation.
 * @post The recovered scale and translation terms meet fixed tolerances.
 * @post No shared fixture state is modified.
 * @note Supplies the minimum supported target-count happy path.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_compute_three_point(void);

/**
 * @brief Verify a five-point least-squares affine fit.
 * @details Fits a bounded ground truth and round-trips every integer sample.
 * @pre All synthetic screen points lie within the configured panel.
 * @pre The five raw points form a nonsingular calibration set.
 * @post Every recovered screen point is within five pixels of its target.
 * @post No shared fixture state is modified.
 * @note Integer source rounding explains the deliberately bounded residual.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_compute_five_point(void);

/**
 * @brief Verify compute rejects null, count, and singular inputs.
 * @details Exercises each public pointer guard, both count bounds, and collinearity.
 * @pre Writable point and matrix fixtures are available.
 * @pre The collinear fixture remains exactly rank deficient.
 * @post Every malformed vector returns its documented error.
 * @post Rejected calls do not escape the bounded fixtures.
 * @note Complements the dedicated count-range MC/DC vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_compute_bad_inputs(void);

/**
 * @brief Verify apply guards and panel-edge clipping.
 * @details Uses an identity transform to isolate null, size, low, and high arms.
 * @pre The output point is writable for valid vectors.
 * @pre The identity matrix coefficients are finite.
 * @post Invalid inputs return the documented error.
 * @post Valid out-of-range coordinates clamp to the nearest panel edge.
 * @note The paired height guard is closed by a separate MC/DC vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_apply_clip_and_null(void);

/**
 * @brief Check that a recovered matrix round-trips five samples.
 * @details Applies the matrix and asserts each coordinate is within five pixels.
 * @param[in] raws Five synthetic raw coordinates.
 * @param[in] targets Five corresponding screen coordinates.
 * @param[in] got Recovered affine matrix.
 * @pre All three pointers designate five-element or scalar live objects.
 * @pre The screen fixture dimensions match the calibration run.
 * @post Every sample satisfies the fixed error bound.
 * @post All caller inputs remain unchanged.
 * @note Assertion failure terminates the current test process.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_verify_roundtrip(const ra8_touch_cal_point_t   raws[k_t_cal_points],
                          const ra8_touch_cal_point_t   targets[k_t_cal_points],
                          const ra8_touch_cal_matrix_t* got);

/**
 * @brief Invert a diagonal ground-truth transform for five targets.
 * @details Divides each target coordinate by its corresponding scale term.
 * @param[in] targets Five on-screen calibration targets.
 * @param[in] truth Ground-truth affine matrix with nonzero diagonal scales.
 * @param[out] raws Receives five synthesized raw coordinates.
 * @pre `truth->a` and `truth->e` are nonzero.
 * @pre The divided coordinates are representable by `int32_t`.
 * @post Every output point maps back to its corresponding target.
 * @post The target and matrix inputs remain unchanged.
 * @note The workflow fixture uses zero shear and translation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_synth_raws(const ra8_touch_cal_point_t   targets[k_t_cal_points],
                                             const ra8_touch_cal_matrix_t* truth,
                                             ra8_touch_cal_point_t         raws[k_t_cal_points]);

/**
 * @brief Verify the calibration targets were drawn in source order.
 * @details Compares every recorded coordinate with the expected five-point path.
 * @param[in] targets Expected target sequence.
 * @param[in] state Completed draw-callback fixture.
 * @pre `state` contains exactly five successful draw records.
 * @pre `targets` designates five expected points.
 * @post Every pair of coordinates compares equal.
 * @post The recorded fixture remains unchanged.
 * @note The target count is fixed by the public calibration contract.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_verify_draw_order(const ra8_touch_cal_point_t targets[k_t_cal_points],
                           const stub_state_t*         state);

/**
 * @brief Verify the full callback-driven calibration workflow.
 * @details Draws, replays, fits, and round-trips all five target samples.
 * @pre Callback fixtures and production calibration entry points are linked.
 * @pre The configured panel and inset leave positive target margins.
 * @post Five draws and five reads occur in the documented order.
 * @post The fitted matrix round-trips every synthetic sample.
 * @note This is the primary end-to-end host vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_run_full_sequence(void);

/**
 * @brief Verify run propagates callback faults and rejects invalid inputs.
 * @details Forces a shim error, tests null outputs, and rejects a wide inset.
 * @pre The fixture callback signatures match the public injected seams.
 * @pre A writable matrix exists for non-null vectors.
 * @post The callback error is preserved exactly.
 * @post Every invalid vector returns before publishing a calibration.
 * @note Margin-condition independence is exercised separately.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_run_shim_error(void);

/**
 * @brief Compare all six affine coefficients exactly.
 * @details Uses value comparisons so structure padding is irrelevant.
 * @param[in] lhs First matrix.
 * @param[in] rhs Second matrix.
 * @return Whether every coefficient compares equal.
 * @retval true All six values compare equal.
 * @pre `lhs` and `rhs` are non-null.
 * @pre Both matrices contain initialized coefficients.
 * @post Neither matrix is modified.
 * @post The result reflects all and only the six public coefficients.
 * @note Signed zero follows normal floating-point equality.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_cal_matrix_equal(const ra8_touch_cal_matrix_t* lhs,
                                                   const ra8_touch_cal_matrix_t* rhs);

/**
 * @brief Verify calibration serialization round-trips exactly.
 * @details Checks header bytes, coefficient equality, size guards, and nulls.
 * @pre A fixed-size blob and initialized matrix are available.
 * @pre The published storage offsets describe the current format version.
 * @post Saving then loading reproduces every coefficient.
 * @post Invalid-size and null vectors return the documented errors.
 * @note The value comparison avoids ABI padding assumptions.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_save_load_roundtrip(void);

/**
 * @brief Verify corrupted serialized fields fail closed.
 * @details Mutates magic, version, reserved, and coefficient bytes independently.
 * @pre A valid serialized calibration blob has been produced.
 * @pre Each corrupt copy retains the original fixed extent.
 * @post Structural corruptions return invalid argument.
 * @post Payload corruption returns CRC mismatch without publishing output.
 * @note Per-byte magic and reserved independence is covered separately.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_load_corruption(void);

/**
 * @brief Prove every magic and reserved-byte OR term independently controls load.
 * @details Flips each trailing byte while other predicates remain false.
 * @pre A canonical serialized blob is available.
 * @pre The corruption mask changes every selected fixture byte.
 * @post Every mutated blob returns invalid argument.
 * @post The canonical blob remains unchanged for the next vector.
 * @note Completes the N+1 MC/DC sets started by the corruption test.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_load_magic_and_reserved_byte_pairs(void);

/**
 * @brief Prove the lower and upper target-count bounds are independent.
 * @details Supplies below, within, and above-range sample counts.
 * @pre Six bounded point slots and a writable matrix are available.
 * @pre The in-range control is intentionally singular after the count guard.
 * @post Both bound violations return invalid argument at the count decision.
 * @post The in-range vector proceeds to the singularity check.
 * @note This is the minimal three-vector MC/DC set for a two-term OR.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_compute_n_range_mcdc(void);

/**
 * @brief Prove width and height margin predicates are independent.
 * @details Varies panel geometry while holding the alternate predicate false.
 * @pre The callbacks are non-null and the empty read fixture is bounded.
 * @pre The three geometries preserve the documented predicate truth table.
 * @post Both rejecting arms return invalid argument.
 * @post The all-false control reaches the callback and returns hardware error.
 * @note Supplies N+1 vectors for the run margin OR.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_run_margin_mcdc(void);

/**
 * @brief Prove all three compute pointer guards independently control rejection.
 * @details Uses the canonical four-vector short-circuit OR set.
 * @pre The non-null raw and screen controls form a valid triangle.
 * @pre The output matrix is writable for the all-false control.
 * @post The control succeeds and each single-null vector returns null pointer.
 * @post Rejected vectors do not dereference the missing operand.
 * @note The shared-determinant solve decision is separately deactivated.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_compute_null_or3(void);

/**
 * @brief Prove the draw and read callback guards are independent.
 * @details Supplies each single-null callback and a non-null later-error control.
 * @pre The configuration object and output matrix are writable.
 * @pre Non-null sentinels use the correctly typed callback functions.
 * @post Each missing callback returns null pointer.
 * @post The both-present control advances to the screen-size guard.
 * @note Supplies N+1 vectors for the callback OR.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_run_cb_null_or(void);

/**
 * @brief Prove the run configuration and output guards are independent.
 * @details Supplies null configuration, null output, and an all-present control.
 * @pre The non-null configuration contains typed callbacks.
 * @pre The matrix storage is writable for the control.
 * @post Each single-null vector returns null pointer.
 * @post The all-present path is covered by the full workflow test.
 * @note Supplies N+1 vectors for the entry-point OR.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_run_cfg_out_null_or(void);

/**
 * @brief Complete height-zero independence for apply and run dimension guards.
 * @details Supplies height-zero vectors paired with existing valid/width-zero arms.
 * @pre The identity matrix and output point are initialized.
 * @pre The callback sentinels are not reached after dimension rejection.
 * @post Apply rejects a zero screen height.
 * @post Run rejects a zero configured screen height.
 * @note Completes both two-term screen-dimension OR decisions.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_mcdc_apply_run_screen_dim_pair(void);
