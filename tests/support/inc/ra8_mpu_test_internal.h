/**
 * @file ra8_mpu_test_internal.h
 * @brief Private contracts for the hosted MPU unit-test vectors.
 * @details Declares the file-local fixture helpers and behavior/MC/DC vectors
 * used by test_ra8_mpu.c so their complete contracts remain separate from the
 * scenario narratives and the test translation unit stays below 1,000 lines.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

/**
 * @brief Publish a hosted MPU region-count value.
 * @details Encodes the supplied count in MPU_TYPE.DREGION for validation tests.
 * @param[in] n Implemented region count to expose.
 * @pre The fake MPU register window is mapped.
 * @pre n is representable by the architectural DREGION field.
 * @post MPU_TYPE contains the requested DREGION value.
 * @post Other MPU registers are unchanged.
 * @note This helper is file-local to the MPU test translation unit.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_set_dregion(uint8_t n);

/**
 * @brief Reset the hosted MPU fixture.
 * @details Clears the MPU registers explicitly and restores a 16-region TYPE value.
 * @pre The fake hardware mappings were installed by the test constructor.
 * @pre No MPU test vector is executing concurrently.
 * @post The MPU is disabled and all programmable registers are zero.
 * @post MPU_TYPE.DREGION reports the test region count.
 * @note The explicit clear also covers ASan shadow-gap mappings.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_setup(void);

/**
 * @brief Verify the architectural MPU register layout.
 * @details Checks every public offset and the complete register-block size.
 * @pre The compile-time MPU register type is available.
 * @pre Unity assertions are operational.
 * @post Every layout mismatch is reported by the test harness.
 * @post No hosted MPU register is modified.
 * @note This is a pure type-layout vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_register_layout(void);

/**
 * @brief Verify null configuration rejection.
 * @details Calls ra8_mpu_configure with a null configuration and checks the exact error.
 * @pre The hosted MPU fixture can be reset.
 * @pre Unity assertions are operational.
 * @post The null-pointer result is asserted.
 * @post No successful MPU configuration is published.
 * @note The production diagnostic path is intentionally exercised.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_configure_null_cfg(void);

/**
 * @brief Verify excessive region-count rejection.
 * @details Requests more descriptors than MPU_TYPE.DREGION advertises.
 * @pre The hosted MPU fixture can be reset.
 * @pre The local descriptor table is fully initialized.
 * @post The invalid-argument result is asserted.
 * @post The rejected table is not installed.
 * @note The vector models an eight-region implementation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_configure_too_many_regions(void);

/**
 * @brief Verify non-power-of-two size rejection.
 * @details Supplies a 0x1500-byte region and checks validation fails before programming.
 * @pre The hosted MPU fixture can be reset.
 * @pre The candidate descriptor is otherwise valid.
 * @post The invalid-argument result is asserted.
 * @post No invalid region is enabled.
 * @note This isolates the region-size rule.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_configure_invalid_size(void);

/**
 * @brief Verify misaligned-base rejection.
 * @details Supplies a base that violates the selected region-size alignment.
 * @pre The hosted MPU fixture can be reset.
 * @pre The candidate region size is a valid power of two.
 * @post The invalid-argument result is asserted.
 * @post No misaligned region is programmed.
 * @note The vector isolates base alignment from size validation.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_configure_misaligned_base(void);

/**
 * @brief Verify unrepresentable permission rejection.
 * @details Requests privileged read-only with unprivileged read-write access.
 * @pre The hosted MPU fixture can be reset.
 * @pre The candidate descriptor is otherwise valid.
 * @post The invalid-argument result is asserted.
 * @post No unsupported AP encoding is programmed.
 * @note The requested permission pair has no Armv8-M AP code.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_configure_unrepresentable_perms(void);

/**
 * @brief Verify successful region-zero configuration.
 * @details Checks MAIR, control, selector, and encoded region fields after configure.
 * @pre The hosted MPU fixture can be reset.
 * @pre The candidate descriptor and MAIR values are valid.
 * @post The expected register values are asserted.
 * @post The MPU is left enabled with privileged default access.
 * @note Unused implemented regions are also cleared by this vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_configure_programs_region_zero(void);

/**
 * @brief Verify unused-region clearing.
 * @details Pre-poisons a high region and confirms configure clears it.
 * @pre The hosted MPU fixture can be reset.
 * @pre The candidate one-region configuration is valid.
 * @post The poisoned high-region RLAR is zero.
 * @post The requested region remains configured successfully.
 * @note RNR selection is exercised through the hosted register model.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_configure_clears_unused_regions(void);

/**
 * @brief Verify enable and disable transitions.
 * @details Toggles MPU_CTRL.ENABLE through the two public lifecycle operations.
 * @pre The hosted MPU fixture can be reset.
 * @pre MPU_CTRL starts cleared.
 * @post Enable sets the architectural bit.
 * @post Disable clears the architectural bit.
 * @note Both operations are expected to return success.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_enable_disable(void);

/**
 * @brief Verify null region rejection.
 * @details Calls ra8_mpu_set_region with a null descriptor and checks the exact error.
 * @pre The hosted MPU fixture can be reset.
 * @pre Region zero is architecturally available.
 * @post The null-pointer result is asserted.
 * @post No region registers are programmed.
 * @note The production diagnostic path is intentionally exercised.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_set_region_null(void);

/**
 * @brief Verify out-of-range selector rejection.
 * @details Requests selector 16 from a fixture advertising regions zero through 15.
 * @pre The hosted MPU fixture can be reset.
 * @pre The candidate descriptor is valid.
 * @post The invalid-argument result is asserted.
 * @post No out-of-range selector is programmed.
 * @note The boundary is exactly one past the highest valid selector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_set_region_out_of_range(void);

/**
 * @brief Verify direct region-pair programming.
 * @details Checks selector, RBAR, RLAR, access, execute, and attribute encodings.
 * @pre The hosted MPU fixture can be reset.
 * @pre The candidate region-seven descriptor is valid.
 * @post The expected RBAR and RLAR fields are asserted.
 * @post Region selector seven remains selected.
 * @note This vector does not enable the whole MPU.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_set_region_writes_pair(void);

/**
 * @brief Construct a baseline MC/DC region descriptor.
 * @details Returns a 4-KiB-style descriptor with caller-selected permissions and size.
 * @param[in] priv Privileged access permission.
 * @param[in] unpriv Unprivileged access permission.
 * @param[in] size Region size used by the validation vector.
 * @return Fully initialized region descriptor.
 * @retval ra8_mpu_region_t Value suitable for ra8_mpu_set_region tests.
 * @pre priv is a declared MPU permission value.
 * @pre unpriv is a declared MPU permission value.
 * @post The descriptor base and attributes use the shared fixture defaults.
 * @post The returned size and permissions equal the caller inputs.
 * @note Invalid combinations are deliberately preserved for rejection tests.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_mpu_region_t
internal_mcdc_region(ra8_mpu_perm_t priv, ra8_mpu_perm_t unpriv, uint32_t size);

/**
 * @brief Cover both conditions in the power-of-two predicate.
 * @details Exercises zero, non-power-of-two, and minimum valid sizes through the public setter.
 * @pre The hosted MPU fixture can be reset.
 * @pre The shared MC/DC descriptor builder is available.
 * @post Both rejecting predicate arms are asserted.
 * @post The valid minimum-size arm is asserted.
 * @note The source block records the exact independence pairs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_validate_region_mcdc_is_pow2(void);

/**
 * @brief Cover the compound region-size guard.
 * @details Varies predicate validity and the architectural minimum independently.
 * @pre The hosted MPU fixture can be reset.
 * @pre The shared MC/DC descriptor builder is available.
 * @post Non-power-of-two and below-minimum sizes are rejected.
 * @post The minimum legal size is accepted.
 * @note The source block records the exact independence pairs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_validate_region_mcdc_size_guard(void);

/**
 * @brief Cover the read-write AP encoding branch.
 * @details Varies privileged and unprivileged read-write conditions independently.
 * @pre The hosted MPU fixture can be reset.
 * @pre The shared MC/DC descriptor builder is available.
 * @post Unsupported permission pairs are rejected.
 * @post The read-write/read-write pair is accepted.
 * @note The source block records the exact independence pairs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_encode_ap_mcdc_priv_rw_unpriv_rw(void);

/**
 * @brief Cover the privileged-read-only AP branches.
 * @details Varies privileged and unprivileged read-only/none conditions independently.
 * @pre The hosted MPU fixture can be reset.
 * @pre The shared MC/DC descriptor builder is available.
 * @post The read-only/none and read-only/read-only encodings are accepted.
 * @post The unsupported comparison arm remains rejected.
 * @note The source block records the exact independence pairs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_encode_ap_mcdc_priv_ro_unpriv_none(void);

/**
 * @brief Cover the configuration pointer/count guard.
 * @details Varies region count and region-table presence through ra8_mpu_configure.
 * @pre The hosted MPU fixture can be reset.
 * @pre The zero-region and one-region configurations are initialized.
 * @post A missing nonempty table is rejected.
 * @post Both valid independence arms are accepted.
 * @note The source block records the exact independence pairs.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_validate_cfg_mcdc_region_count_and_regions(void);

/**
 * @brief Verify the canonical boot-map table.
 * @details Checks count, bases, sizes, permissions, attributes, and execution policy.
 * @pre The production boot-map accessor is linked.
 * @pre Unity assertions are operational.
 * @post Every canonical descriptor field is asserted.
 * @post The immutable boot-map table is not modified.
 * @note The vector also verifies the non-power-of-two SHRAM extent.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_boot_map_table(void);

/**
 * @brief Verify null boot-map count handling.
 * @details Calls the accessor without a count destination and checks it returns null.
 * @pre The production boot-map accessor is linked.
 * @pre Unity assertions are operational.
 * @post The null result is asserted.
 * @post No caller output is written.
 * @note This isolates the accessor guard.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_boot_map_null(void);

/**
 * @brief Verify successful boot-map installation.
 * @details Applies the canonical map and checks MAIR, control, and final region encoding.
 * @pre The hosted MPU fixture advertises 16 regions.
 * @pre The hosted MPU fixture begins disabled.
 * @post The MPU is enabled with privileged default access.
 * @post The expected canonical register values are asserted.
 * @note The final selected region proves installation order.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_apply_boot_map_enables(void);

/**
 * @brief Verify insufficient boot-map capacity rejection.
 * @details Advertises four regions for a five-region map and checks failure atomicity.
 * @pre The hosted MPU fixture begins disabled.
 * @pre MPU_TYPE can be rewritten by the fixture.
 * @post The invalid-argument result is asserted.
 * @post MPU_CTRL remains disabled.
 * @note This is the exact one-region-short boundary vector.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_apply_boot_map_insufficient_regions(void);

/**
 * @brief Verify the enabled-state accessor.
 * @details Observes the accessor before enable, after enable, and after disable.
 * @pre The hosted MPU fixture begins disabled.
 * @pre Unity assertions are operational.
 * @post Both enabled and disabled observations are asserted.
 * @post The MPU ends disabled.
 * @note The accessor is exercised only through public lifecycle calls.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_is_enabled_tracks_ctrl(void);
