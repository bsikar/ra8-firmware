/**
 * @file ra_attributes.h
 * @brief Annotation-attribute framework macros for ra8d2-firmware
 *
 * @details
 * This header defines the `RA_*` annotation macros that decorate function
 * declarations, definitions, and prototypes with metadata describing
 * their architectural contracts: test-only linkage, dependency-injection
 * slots, NSC veneer placement, MMIO accessor contracts, NASA Power-of-10
 * exemptions, MC/DC deactivation reasons, ISR-safety, lock requirements,
 * stack budgets, latency budgets, recursion bans, loop-bound contracts,
 * validation-count contracts, RAII-style ownership, safety reviewer
 * sign-off, and register-bank grouping.
 *
 * ## Why annotations?
 *
 * The macros expand to `__attribute__((annotate("...")))` under clang,
 * which preserves the metadata in the IR for libclang-based enforcement
 * scripts to inspect. They produce no codegen and have no runtime cost.
 * Under non-clang toolchains the macros expand to a comment-only
 * no-op so portable builds compile without warning.
 *
 * The full reference (purpose, enforcement script, examples) lives in
 * `docs/ANNOTATIONS.md`. will introduce the libclang-based
 * checker that reads these annotations from the AST.
 *
 * ## Citation policy
 *
 * Every example in this header references targets by FUNCTION NAME or
 * SYMBOL NAME -- never by line number -- per the rule in
 * `docs/CITATION_POLICY.md`. The `MCDC_DEACTIVATED` macro's reason
 * argument is enforced by the citation gate: it must not contain a
 * `<file>.<ext>:<line>` token.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Backend selection
 *
 * Clang preserves `__attribute__((annotate("...")))` on declarations and
 * exposes it through libclang as `clang_Cursor_getAnnotations`. GCC parses
 * the same syntax silently but does not surface it; either way, codegen
 * is unaffected. We still gate on `__clang__` so non-clang toolchains
 * compile a literal no-op (a pure comment placeholder).
 * =============================================================================
 */

#ifdef __clang__
#define RA_INTERNAL_ANNOTATE(tag) __attribute__((annotate(tag)))
#else
#define RA_INTERNAL_ANNOTATE(tag) /* annotation: tag */
#endif

/* =============================================================================
 * 1. RA_TEST_HELPER
 * =============================================================================
 */

/**
 * @brief Mark a symbol as externally-linked but only callable from tests.
 *
 * @details
 * The symbol must have external linkage so unit tests under `tests/` can
 * link against it, but production callers in `libs/`, `src/`, and
 * `examples/` must not invoke it. the libclang checker walks the
 * call graph and rejects any call to a `RA_TEST_HELPER` from a
 * non-test translation unit.
 *
 * @par Enforcement:
 * libclang call-graph analyzer. Callers must reside under `tests/`.
 *
 * @par Example:
 * @code
 * RA_TEST_HELPER ra_err_t ra_pin_validator_reset_for_test(void);
 * @endcode
 *
 * `ra_pin_validator_reset_for_test` may only be called from
 * `test_ra_pin_validator` and similar test entry points.
 */
#define RA_TEST_HELPER RA_INTERNAL_ANNOTATE("ra_test_helper")

/* =============================================================================
 * 2. RA_INTERNAL
 * =============================================================================
 */

/**
 * @brief Marker that a function is intended to be `static` (file-local).
 *
 * @details
 * Records the file-static discipline so the libclang checker can verify
 * the symbol's declared linkage actually is `static`. Pairs with the
 * `internal_` name prefix from the project naming convention.
 *
 * @par Enforcement:
 * libclang checks that the symbol's storage class is `static`.
 *
 * @par Example:
 * @code
 * RA_INTERNAL static ra_err_t internal_validate_pin_range(uint8_t pin);
 * @endcode
 */
#define RA_INTERNAL RA_INTERNAL_ANNOTATE("ra_internal")

/* =============================================================================
 * 3. RA_PRIV
 * =============================================================================
 */

/**
 * @brief Module-private helper: shared across TUs but only inside one library.
 *
 * @details
 * The function has external linkage (so other `.c` files within the same
 * `libs/<module>/` directory can call it) but is not part of the module's
 * public API. Pairs with the `priv_` name prefix.
 *
 * @par Enforcement:
 * libclang verifies callers reside in the same `libs/<module>/` directory
 * as the definition.
 *
 * @par Example:
 * @code
 * RA_PRIV ra_err_t priv_ra_log_emit(const char* tag, const char* msg);
 * @endcode
 *
 * `priv_ra_log_emit` is callable only from other files inside
 * `libs/ra_core/`.
 */
#define RA_PRIV RA_INTERNAL_ANNOTATE("ra_priv")

/* =============================================================================
 * 4. RA_DI_SLOT(role)
 * =============================================================================
 */

/**
 * @brief Mark a function as an explicit Dependency Injection slot.
 *
 * @details
 * The function is the seam at which a runtime mock can be substituted for
 * the real implementation. Public callers must reach the implementation
 * through the published vtable / function-pointer interface, not by
 * naming the symbol directly.
 *
 * @param role String literal naming the DI role (e.g. `"bus_read"`,
 *             `"clock_get_ticks"`).
 *
 * @par Enforcement:
 * libclang verifies (a) a mock exists under `tests/mocks/` for the role
 * and (b) production callers go through the vtable rather than naming
 * the implementation symbol directly.
 *
 * @par Example:
 * @code
 * RA_DI_SLOT("bus_read")
 * ra_err_t ra_bus_i2c_read(void* ctx, uint8_t* dst, uint32_t len);
 * @endcode
 */
#define RA_DI_SLOT(role) RA_INTERNAL_ANNOTATE("ra_di_slot:" role)

/* =============================================================================
 * 5. RA_NSC_VENEER
 * =============================================================================
 */

/**
 * @brief Mark a TrustZone Secure-to-Non-Secure entry-point veneer.
 *
 * @details
 * Pairs with the Arm CMSE non-secure-entry attribute. The function must
 * live in `libs/ra_nsc/`, must validate every pointer argument with the
 * `RA_NSC_CHECK_NS_RANGE_*` family of helpers, and is placed in the
 * `.gnu.sgstubs` linker section so the SG instruction lands at a valid
 * Non-Secure entry address.
 *
 * @par Enforcement:
 * - `check_world_tags.py` already restricts the CMSE non-secure-entry
 *   attribute to files under `libs/ra_nsc/`.
 * - libclang checker verifies every pointer parameter is fed
 *   to a `RA_NSC_CHECK_NS_RANGE_*` call before being dereferenced.
 *
 * @par Example:
 * @code
 * RA_NSC_VENEER
 * RA_CMSE_NS_ENTRY  // expands to the CMSE non-secure entry attribute
 * ra_err_t ra_nsc_secure_storage_read(uint8_t* ns_buf, uint32_t len);
 * @endcode
 *
 * `ra_nsc_secure_storage_read` must call
 * `RA_NSC_CHECK_NS_RANGE_RW(ns_buf, len)` before touching `ns_buf`.
 */
#define RA_NSC_VENEER RA_INTERNAL_ANNOTATE("ra_nsc_veneer")

/* =============================================================================
 * 6. RA_HW_REGISTER_ACCESS
 * =============================================================================
 */

/**
 * @brief Mark an MMIO accessor function (returns `volatile` register pointer).
 *
 * @details
 * Tags the inline accessor functions that wrap memory-mapped peripheral
 * register banks (per the "Hardware Register Access" rule in
 * `CLAUDE.md`). The accessor must be `inline`, must return a `volatile`
 * pointer, and writes through it must be wrapped in `RA_PROTECTED_WRITE`
 * (or carry a per-line `// CITES-OK: read-only` justification) so the
 * register-protection auditor can prove the write was intentional.
 *
 * @par Enforcement:
 * libclang verifies the function is `inline` and its return type is
 * `volatile`-qualified, and scans call sites to confirm the accessor is
 * either dereferenced for read-only, dereferenced inside
 * `RA_PROTECTED_WRITE`, or carries the `CITES-OK` exemption.
 *
 * @par Example:
 * @code
 * RA_HW_REGISTER_ACCESS
 * static inline volatile ra_sci_regs_t* ra_sci0_regs(void);
 * @endcode
 */
#define RA_HW_REGISTER_ACCESS RA_INTERNAL_ANNOTATE("ra_hw_register_access")

/* =============================================================================
 * 7. RA_NASA_RULE_3_OK
 * =============================================================================
 */

/**
 * @brief Documented exception to NASA Power-of-10 Rule 3 (no dynamic alloc).
 *
 * @details
 * The function is permitted to call `malloc`, `calloc`, `realloc`, or
 * `free`. Tagging records the exception in the AST so the
 * `check_no_dynamic_alloc.py` helper (and the call-graph checker)
 * can verify only tagged functions touch the allocator and that every
 * caller chain that reaches them is itself tagged or carries an explicit
 * deviation entry.
 *
 * @par Enforcement:
 * `check_no_dynamic_alloc.py` plus libclang call-graph walk. Untagged
 * functions calling tagged ones must be covered by a deviation entry.
 *
 * @par Example:
 * @code
 * RA_NASA_RULE_3_OK
 * ra_err_t ra_test_harness_alloc_scratch(uint32_t bytes, void** out);
 * @endcode
 *
 * Only `ra_test_harness_alloc_scratch` and similarly tagged scaffolding
 * may invoke `malloc`.
 */
#define RA_NASA_RULE_3_OK RA_INTERNAL_ANNOTATE("ra_nasa_rule_3_ok")

/* =============================================================================
 * 8. RA_MCDC_DEACTIVATED(reason)
 * =============================================================================
 */

/**
 * @brief Mark a decision as MC/DC-deactivated, with a free-text reason.
 *
 * @details
 * Replaces the legacy `// mcdc-deactivated:` line comment. The `reason`
 * argument is a string literal explaining why the decision is exempt
 * from MC/DC coverage (e.g. defensive programming guard, hardware
 * fault-injection-only path, unreachable in normal operation).
 *
 * @param reason String literal explaining the deactivation. The citation
 *               gate (`check_line_citations.py`) rejects any reason that
 *               contains a `<file>.<ext>:<line>` token; reference target
 *               functions or symbols by name instead.
 *
 * @par Enforcement:
 * - `check_line_citations.py` scans the macro's reason argument for
 *   `*.[ch]:NNN` tokens and rejects them.
 * - `regen_mcdc_gaps.py` tallies `RA_MCDC_DEACTIVATED` annotations into
 *   `docs/MCDC_DEACTIVATIONS.md`.
 *
 * @par Example:
 * @code
 * RA_MCDC_DEACTIVATED("defensive guard, ra_pin_validator_check is "
 *                     "the sole caller and asserts non-null first")
 * static inline bool internal_validate_handle(const ra_pin_t* h);
 * @endcode
 */
#define RA_MCDC_DEACTIVATED(reason) RA_INTERNAL_ANNOTATE("ra_mcdc_deactivated:" reason)

/* =============================================================================
 * 9. RA_MAX_STACK(bytes)
 * =============================================================================
 */

/**
 * @brief Per-function stack-budget contract.
 *
 * @details
 * The function promises to consume no more than `bytes` of stack frame
 * (excluding callees). `stack_usage_check.py` reads the annotation
 * alongside GCC's `-fstack-usage` `.su` files and fails the build if
 * the actual frame exceeds the budget.
 *
 * @param bytes Integer literal: maximum stack-frame size in bytes.
 *
 * @par Enforcement:
 * `scripts/utils/stack_usage_check.py` cross-checks against `.su` files.
 *
 * @par Example:
 * @code
 * RA_MAX_STACK(128)
 * ra_err_t ra_uart_isr_drain_fifo(ra_uart_handle_t* h);
 * @endcode
 *
 * `ra_uart_isr_drain_fifo` must keep its frame under 128 bytes.
 */
#define RA_MAX_STACK(bytes) RA_INTERNAL_ANNOTATE("ra_max_stack:" #bytes)

/* =============================================================================
 * 10. RA_ISR_SAFE
 * =============================================================================
 */

/**
 * @brief The function is callable from interrupt context.
 *
 * @details
 * ISR-context callers (functions defined in files matching `*_isr.c`
 * or themselves tagged `RA_ISR_HANDLER`) may only invoke functions that
 * also carry `RA_ISR_SAFE`. This catches accidental calls to logging,
 * blocking I/O, or non-reentrant helpers from within an interrupt.
 *
 * @par Enforcement:
 * libclang call-graph walk: every callee reachable from an ISR-tagged
 * function must itself be `RA_ISR_SAFE`.
 *
 * @par Example:
 * @code
 * RA_ISR_SAFE
 * void ra_ringbuf_push_byte(ra_ringbuf_t* rb, uint8_t b);
 * @endcode
 *
 * `ra_ringbuf_push_byte` is safe to call from `ra_uart0_rxi_handler`.
 */
#define RA_ISR_SAFE RA_INTERNAL_ANNOTATE("ra_isr_safe")

/* =============================================================================
 * 11. RA_EXPECTS_LOCK(name)
 * =============================================================================
 */

/**
 * @brief The function expects the named thread/IRQ lock to be held on entry.
 *
 * @details
 * Documents the lock contract so the libclang checker can verify every
 * caller wraps the call in a matching `RA_TAKE_LOCK(name)` /
 * `RA_RELEASE_LOCK(name)` pair (or itself carries the same
 * `RA_EXPECTS_LOCK(name)` annotation, propagating the contract upward).
 *
 * @param name String literal naming the lock (e.g. `"i2c0_bus"`,
 *             `"global_irq"`).
 *
 * @par Enforcement:
 * libclang call-graph walk verifies caller has acquired `name` first.
 *
 * @par Example:
 * @code
 * RA_EXPECTS_LOCK("i2c0_bus")
 * ra_err_t ra_i2c0_write_locked(const uint8_t* buf, uint32_t len);
 * @endcode
 *
 * Callers of `ra_i2c0_write_locked` must hold the `"i2c0_bus"` lock.
 */
#define RA_EXPECTS_LOCK(name) RA_INTERNAL_ANNOTATE("ra_expects_lock:" name)

/* =============================================================================
 * 12. RA_HOST_FRIENDLY
 * =============================================================================
 */

/**
 * @brief The function is safe to invoke under `RA_SIMULATOR_MODE` on the host.
 *
 * @details
 * Tagged functions either avoid all `volatile`-qualified MMIO access or
 * route every such access through a mock. The libclang checker walks the
 * AST for raw `volatile` dereferences in tagged functions (and their
 * callees) and rejects any that lack a corresponding mock binding.
 *
 * @par Enforcement:
 * libclang AST walk: no unmocked `volatile` MMIO inside the call subtree.
 *
 * @par Example:
 * @code
 * RA_HOST_FRIENDLY
 * ra_err_t ra_pid_step(ra_pid_state_t* s, float setpoint, float measured);
 * @endcode
 *
 * `ra_pid_step` is pure math and runs identically on hardware and host.
 */
#define RA_HOST_FRIENDLY RA_INTERNAL_ANNOTATE("ra_host_friendly")

/* =============================================================================
 * 13. RA_LATENCY_BUDGET_NS(n)
 * =============================================================================
 */

/**
 * @brief Real-time deadline contract: function must complete within `n` ns.
 *
 * @details
 * Records the worst-case-execution-time (WCET) budget so a future WCET
 * analysis pass can cross-check against the measured / computed bound
 * for the function's call subtree.
 *
 * @param n Integer literal: maximum execution time in nanoseconds.
 *
 * @par Enforcement:
 * Future WCET analysis pass (planned ).
 *
 * @par Example:
 * @code
 * RA_LATENCY_BUDGET_NS(2000)
 * void ra_servo_pwm_update(uint16_t duty_q8);
 * @endcode
 *
 * `ra_servo_pwm_update` must complete within 2 microseconds.
 */
#define RA_LATENCY_BUDGET_NS(n) RA_INTERNAL_ANNOTATE("ra_latency_budget_ns:" #n)

/* =============================================================================
 * 14. RA_NO_RECURSION
 * =============================================================================
 */

/**
 * @brief NASA Power-of-10 Rule 1: no direct or indirect self-call.
 *
 * @details
 * Most firmware functions implicitly have this property; the annotation
 * makes the contract explicit and lets the libclang checker prove it via
 * a call-graph walk that rejects any cycle reaching back to the tagged
 * function.
 *
 * @par Enforcement:
 * libclang call-graph cycle detection.
 *
 * @par Example:
 * @code
 * RA_NO_RECURSION
 * ra_err_t ra_fs_walk_directory(const char* path, ra_fs_visitor_fn visit);
 * @endcode
 *
 * `ra_fs_walk_directory` must not call itself, directly or transitively.
 */
#define RA_NO_RECURSION RA_INTERNAL_ANNOTATE("ra_no_recursion")

/* =============================================================================
 * 15. RA_BOUNDED_LOOP(symbol)
 * =============================================================================
 */

/**
 * @brief NASA Power-of-10 Rule 2: every loop has a constant upper bound.
 *
 * @details
 * Names the symbol (typically a typed enum value) that bounds every loop
 * inside the function. The libclang checker walks the function body and
 * verifies each loop's termination condition references a constant (or
 * the named symbol).
 *
 * @param symbol Bare token naming the bounding constant (e.g.
 *               `k_max_retries`, `k_ringbuf_capacity`).
 *
 * @par Enforcement:
 * libclang loop analyzer.
 *
 * @par Example:
 * @code
 * RA_BOUNDED_LOOP(k_max_retries)
 * ra_err_t ra_i2c_send_with_retry(const uint8_t* buf, uint32_t len);
 * @endcode
 *
 * Every loop in `ra_i2c_send_with_retry` is bounded by `k_max_retries`.
 */
#define RA_BOUNDED_LOOP(symbol) RA_INTERNAL_ANNOTATE("ra_bounded_loop:" #symbol)

/* =============================================================================
 * 16. RA_VALIDATES(n)
 * =============================================================================
 */

/**
 * @brief NASA Power-of-10 Rule 5: function body has at least `n` `RA_CHECK_*` calls.
 *
 * @details
 * Records the validation-count contract so the libclang checker can
 * count `RA_CHECK_*` / `RA_VALIDATE_*` / `RA_ASSERT` invocations in the
 * function body and fail if the count drops below `n`.
 *
 * @param n Integer literal: minimum number of validation calls.
 *
 * @par Enforcement:
 * libclang AST walk counts `RA_CHECK_*` invocations.
 *
 * @par Example:
 * @code
 * RA_VALIDATES(3)
 * ra_err_t ra_gpio_output_init(ra_port_t port, uint8_t pin, ra_level_t lvl);
 * @endcode
 *
 * `ra_gpio_output_init` must contain at least three `RA_CHECK_*` calls.
 */
#define RA_VALIDATES(n) RA_INTERNAL_ANNOTATE("ra_validates:" #n)

/* =============================================================================
 * 17. RA_OWNS_RESOURCE(kind)
 * =============================================================================
 */

/**
 * @brief RAII-style resource ownership contract.
 *
 * @details
 * The function acquires a resource of `kind` (a typed-enum-style label
 * such as `"i2c_bus"`, `"dma_channel"`, `"trng_handle"`). The libclang
 * checker walks every return path and requires a matching
 * `RA_RELEASES_RESOURCE(kind)` call before the function returns -- on
 * the success path and every error path.
 *
 * @param kind String literal naming the resource kind.
 *
 * @par Enforcement:
 * libclang control-flow walk: every return path releases the resource.
 *
 * @par Example:
 * @code
 * RA_OWNS_RESOURCE("dma_channel")
 * ra_err_t ra_dma_acquire_channel(ra_dma_channel_t* out_ch);
 * @endcode
 *
 * Every caller of `ra_dma_acquire_channel` must, on success, call
 * `ra_dma_release_channel` before any `return`.
 */
#define RA_OWNS_RESOURCE(kind) RA_INTERNAL_ANNOTATE("ra_owns_resource:" kind)

/* =============================================================================
 * 18. RA_REVIEWED_BY(name)
 * =============================================================================
 */

/**
 * @brief Safety-critical review sign-off marker.
 *
 * @details
 * Records the name of the reviewer who signed off on the safety-critical
 * function. The qualification toolchain rolls these annotations up into
 * `docs/qualification/SVR.md` (Software Verification Report) so each
 * reviewed item has a traceable owner.
 *
 * @param name String literal: reviewer identity (e.g. `"bsikar"`).
 *
 * @par Enforcement:
 * `docs/qualification/SVR.md` auto-rollup.
 *
 * @par Example:
 * @code
 * RA_REVIEWED_BY("bsikar")
 * ra_err_t ra_secure_storage_commit(const uint8_t* key_blob, uint32_t len);
 * @endcode
 *
 * `ra_secure_storage_commit` has been reviewed by `bsikar` for the
 * safety-critical key-commit path.
 */
#define RA_REVIEWED_BY(name) RA_INTERNAL_ANNOTATE("ra_reviewed_by:" name)

/* =============================================================================
 * 19. RA_REGISTER_BANK(peripheral)
 * =============================================================================
 */

/**
 * @brief Group MMIO accessor functions by peripheral register bank.
 *
 * @details
 * The annotation tags each `RA_HW_REGISTER_ACCESS`-style accessor with
 * its parent peripheral so the auto-generated peripheral documentation
 * tool can list every accessor belonging to a given block (e.g. SCI0,
 * IIC1, GPT3).
 *
 * @param peripheral String literal naming the peripheral (e.g.
 *                   `"sci0"`, `"iic1"`, `"gpt3"`).
 *
 * @par Enforcement:
 * Documentation generator groups accessors by `peripheral` name.
 *
 * @par Example:
 * @code
 * RA_REGISTER_BANK("sci0")
 * RA_HW_REGISTER_ACCESS
 * static inline volatile ra_sci_regs_t* ra_sci0_regs(void);
 * @endcode
 *
 * `ra_sci0_regs` is grouped under the `sci0` register bank in the
 * generated peripheral documentation.
 */
#define RA_REGISTER_BANK(peripheral) RA_INTERNAL_ANNOTATE("ra_register_bank:" peripheral)

#ifdef __cplusplus
}
#endif
