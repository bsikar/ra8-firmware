/**
 * @file ra8_attributes.h
 * @brief Annotation-attribute framework macros for ra8-firmware
 * @ingroup grp_core
 *
 * @details
 * This header defines the `RA8_*` annotation macros that decorate function
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
 * Each RA8_* annotation macro lowers to `[[clang::annotate("...")]]` under
 * clang and is exposed through libclang as `clang_Cursor_getAnnotations`. The
 * C23/C++11 attribute-specifier form is used (not the GNU
 * `__attribute__((annotate(...)))` form) because it is the spelling libclang
 * surfaces on a declaration. GCC parses the same syntax silently but does not
 * surface it; either way codegen is unaffected. We gate on `__clang__` so
 * non-clang toolchains compile a literal no-op (a pure comment placeholder).
 *
 * These annotations attach to a DECLARATION -- a function, variable, or type --
 * and NOWHERE ELSE. `[[clang::annotate]]` is not valid in statement position:
 * clang rejects `[[clang::annotate("x")]] for (...)` as "'annotate' attribute
 * cannot be applied to a statement", and under a non-clang toolchain the
 * comment no-op evaporates, binding to nothing. A per-LOOP bound therefore
 * cannot be one of these annotations, and a statement-position
 * `RA8_BOUNDED_LOOP(x);` was for that reason a hard clang error and a silent
 * GCC no-op that bound to no loop at all. `RA8_BOUNDED_LOOP` (section 15) is a
 * FUNCTION-level annotation: it decorates the function declaration and the
 * checker walks that function's loops. To bind a bound to ONE specific loop in
 * statement position, use `RA8_LOOP_BOUND` / `RA8_LOOP_BOUND_RUNTIME`
 * (sections 15b/15c) -- these are real C (a `static_assert`, or a symbol
 * reference), not annotations, so they compile and are enforced identically
 * under every toolchain and cannot degrade to a no-op.
 *
 * `__CPPCHECK__` is excluded because cppcheck's C parser cannot represent a
 * scoped attribute carrying a string argument: on `[[clang::annotate("x")]]`
 * it mis-parses the declaration that follows, and the damaged parse surfaces
 * as phantom MISRA findings in the annotated function (14.2 / 16.2 / 17.3
 * appearing where the source has no loop, no switch and no implicit
 * declaration, plus inflated 15.5 exit-point counts). cppcheck explores both
 * arms of the `#ifdef` above, so without this exclusion the audit reports the
 * defects of a configuration that is never built. The firmware ships compiled
 * by arm-none-eabi-gcc, where these macros are already comments -- pinning
 * cppcheck to that arm analyses the code that actually ships, and keeps the
 * annotation visible to clang-tidy and to the libclang annotation gate
 * (`scripts/checks/check_annotations.py`), neither of which defines
 * `__CPPCHECK__`. Suppressing the rules or absorbing the findings into the
 * MISRA baseline instead would blind the ratchet to real defects in every
 * annotated file.
 * =============================================================================
 */

#if defined(__clang__) && !defined(__CPPCHECK__)
/** @brief RA8 INTERNAL ANNOTATE. */
#define RA8_INTERNAL_ANNOTATE(tag) [[clang::annotate(tag)]]
#else
/** @brief RA8 INTERNAL ANNOTATE. */
#define RA8_INTERNAL_ANNOTATE(tag) /* annotation: tag */
#endif

/* =============================================================================
 * 1. RA8_TEST_HELPER
 * =============================================================================
 */

/**
 * @brief Mark a symbol as externally-linked but only callable from tests.
 *
 * @details
 * The symbol must have external linkage so unit tests under `tests/` can
 * link against it, but production callers in `libs/`, `src/`, and
 * `examples/` must not invoke it. the libclang checker walks the
 * call graph and rejects any call to a `RA8_TEST_HELPER` from a
 * non-test translation unit.
 *
 * @par Enforcement:
 * libclang call-graph analyzer. Callers must reside under `tests/`.
 *
 * @par Example:
 * @code
 * RA8_TEST_HELPER ra8_err_t ra8_pin_validator_reset_for_test(void);
 * @endcode
 *
 * `ra8_pin_validator_reset_for_test` may only be called from
 * `test_ra8_pin_validator` and similar test entry points.
 */
#define RA8_TEST_HELPER RA8_INTERNAL_ANNOTATE("ra8_test_helper")

/* =============================================================================
 * 2. RA8_INTERNAL
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
 * RA8_INTERNAL static ra8_err_t internal_validate_pin_range(uint8_t pin);
 * @endcode
 */
#define RA8_INTERNAL RA8_INTERNAL_ANNOTATE("ra8_internal")

/* =============================================================================
 * 3. RA8_PRIV
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
 * RA8_PRIV ra8_err_t priv_ra8_log_emit(const char* tag, const char* msg);
 * @endcode
 *
 * `priv_ra8_log_emit` is callable only from other files inside
 * `libs/ra8_core/`.
 */
#define RA8_PRIV RA8_INTERNAL_ANNOTATE("ra8_priv")

/* =============================================================================
 * 4. RA8_DI_SLOT(role)
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
 * @param[in] role String literal naming the DI role (e.g. `"bus_read"`,
 *                 `"clock_get_ticks"`).
 *
 * @par Enforcement:
 * libclang verifies (a) a mock exists under `tests/mocks/` for the role
 * and (b) production callers go through the vtable rather than naming
 * the implementation symbol directly.
 *
 * @par Example:
 * @code
 * // The device driver consumes an injected bus seam (see ra8_i2c_bus_ops.h);
 * // the app binds it to RIIC or I3C via the ra8_io_i2c_bus_t facade binders.
 * RA8_DI_SLOT("i2c_bus_ops")
 * ra8_err_t ra8_touch_init(const ra8_touch_cfg_t* cfg); // cfg carries ra8_i2c_bus_ops_t
 * @endcode
 */
#define RA8_DI_SLOT(role) RA8_INTERNAL_ANNOTATE("ra8_di_slot:" role)

/* =============================================================================
 * 5. RA8_NSC_VENEER
 * =============================================================================
 */

/**
 * @brief Mark a TrustZone Secure-to-Non-Secure entry-point veneer.
 *
 * @details
 * Pairs with the Arm CMSE non-secure-entry attribute. The function must
 * live in `libs/ra8_nsc/`, must validate every pointer argument with the
 * `RA8_NSC_CHECK_NS_RANGE_*` family of helpers, and is placed in the
 * `.gnu.sgstubs` linker section so the SG instruction lands at a valid
 * Non-Secure entry address.
 *
 * @par Enforcement:
 * - `check_world_tags.py` already restricts the CMSE non-secure-entry
 *   attribute to files under `libs/ra8_nsc/`.
 * - libclang checker verifies every pointer parameter is fed
 *   to a `RA8_NSC_CHECK_NS_RANGE_*` call before being dereferenced.
 *
 * @warning
 * The definition below is the **annotation-only default**, used by
 * translation units that never cross the NSC boundary. It is
 * deliberately overridden by `libs/ra8_nsc/inc/ra8_nsc_veneer.h`,
 * which `#undef`s it and re-defines it to carry
 * the Arm CMSE non-secure-entry attribute **as well as** this
 * annotation. `ra8_nsc_veneer.h` is the single authority; never add
 * the CMSE attribute here, and never let this definition reach an NSC
 * translation unit after that header (it would drop the
 * secure-gateway veneer and silently break the S/NS boundary).
 *
 * @par Example:
 * @code
 * RA8_NSC_VENEER
 * ra8_err_t ra8_nsc_secure_storage_read(uint8_t* ns_buf, uint32_t len);
 * @endcode
 *
 * `ra8_nsc_secure_storage_read` must call
 * `RA8_NSC_CHECK_NS_RANGE_RW(ns_buf, len)` before touching `ns_buf`.
 *
 * @see ra8_nsc_veneer.h  Secure-world form (cmse attribute + annotation).
 */
#define RA8_NSC_VENEER RA8_INTERNAL_ANNOTATE("ra8_nsc_veneer")

/* =============================================================================
 * 6. RA8_HW_REGISTER_ACCESS
 * =============================================================================
 */

/**
 * @brief Mark an MMIO accessor function (returns `volatile` register pointer).
 *
 * @details
 * Tags the inline accessor functions that wrap memory-mapped peripheral
 * register banks (per the "Hardware Register Access" rule in
 * `CLAUDE.md`). The accessor must be `inline`, must return a `volatile`
 * pointer, and writes through it must be wrapped in `RA8_PROTECTED_WRITE`
 * (or carry a per-line `// CITES-OK: read-only` justification) so the
 * register-protection auditor can prove the write was intentional.
 *
 * @par Enforcement:
 * libclang verifies the function is `inline` and its return type is
 * `volatile`-qualified, and scans call sites to confirm the accessor is
 * either dereferenced for read-only, dereferenced inside
 * `RA8_PROTECTED_WRITE`, or carries the `CITES-OK` exemption.
 *
 * @par Example:
 * @code
 * RA8_HW_REGISTER_ACCESS
 * static inline volatile ra8_sci_regs_t* ra8_sci0_regs(void);
 * @endcode
 */
#define RA8_HW_REGISTER_ACCESS RA8_INTERNAL_ANNOTATE("ra8_hw_register_access")

/* =============================================================================
 * 7. RA8_NASA_RULE_3_OK
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
 * RA8_NASA_RULE_3_OK
 * ra8_err_t ra8_test_harness_alloc_scratch(uint32_t bytes, void** out);
 * @endcode
 *
 * Only `ra8_test_harness_alloc_scratch` and similarly tagged scaffolding
 * may invoke `malloc`.
 */
#define RA8_NASA_RULE_3_OK RA8_INTERNAL_ANNOTATE("ra8_nasa_rule_3_ok")

/* =============================================================================
 * 8. RA8_MCDC_DEACTIVATED(reason)
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
 * @param[in] reason String literal explaining the deactivation. The citation
 *                   gate (`check_line_citations.py`) rejects any reason that
 *                   contains a `<file>.<ext>:<line>` token; reference target
 *                   functions or symbols by name instead.
 *
 * @par Enforcement:
 * - `check_line_citations.py` scans the macro's reason argument for
 *   `*.[ch]:NNN` tokens and rejects them.
 * - `regen_mcdc_gaps.py` tallies `RA8_MCDC_DEACTIVATED` annotations into
 *   `docs/MCDC_DEACTIVATIONS.md`.
 *
 * @par Example:
 * @code
 * RA8_MCDC_DEACTIVATED("defensive guard, ra8_pin_validator_check is "
 *                     "the sole caller and asserts non-null first")
 * static inline bool internal_validate_handle(const ra8_pin_t* h);
 * @endcode
 */
#define RA8_MCDC_DEACTIVATED(reason) RA8_INTERNAL_ANNOTATE("ra8_mcdc_deactivated:" reason)

/* =============================================================================
 * 9. RA8_MAX_STACK(bytes)
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
 * @param[in] bytes Integer literal: maximum stack-frame size in bytes.
 *
 * @par Enforcement:
 * `scripts/checks/stack_usage_check.py` cross-checks against `.su` files.
 *
 * @par Example:
 * @code
 * RA8_MAX_STACK(128)
 * ra8_err_t ra8_uart_isr_drain_fifo(ra8_uart_handle_t* h);
 * @endcode
 *
 * `ra8_uart_isr_drain_fifo` must keep its frame under 128 bytes.
 */
#define RA8_MAX_STACK(bytes) RA8_INTERNAL_ANNOTATE("ra8_max_stack:" #bytes)

/* =============================================================================
 * 10. RA8_ISR_SAFE
 * =============================================================================
 */

/**
 * @brief The function is callable from interrupt context.
 *
 * @details
 * ISR-context callers (functions defined in files matching `*_isr.c`
 * or themselves tagged `RA8_ISR_HANDLER`) may only invoke functions that
 * also carry `RA8_ISR_SAFE`. This catches accidental calls to logging,
 * blocking I/O, or non-reentrant helpers from within an interrupt.
 *
 * @par Enforcement:
 * libclang call-graph walk: every callee reachable from an ISR-tagged
 * function must itself be `RA8_ISR_SAFE`.
 *
 * @par Example:
 * @code
 * RA8_ISR_SAFE
 * void ra8_ringbuf_push_byte(ra8_ringbuf_t* rb, uint8_t b);
 * @endcode
 *
 * `ra8_ringbuf_push_byte` is safe to call from `ra8_uart0_rxi_handler`.
 */
#define RA8_ISR_SAFE RA8_INTERNAL_ANNOTATE("ra8_isr_safe")

/* =============================================================================
 * 11. RA8_EXPECTS_LOCK(name)
 * =============================================================================
 */

/**
 * @brief The function expects the named thread/IRQ lock to be held on entry.
 *
 * @details
 * Documents the lock contract so the libclang checker can verify that every
 * caller already holds `name`. A caller proves it in one of exactly two ways:
 *
 *  1. it TAKES the lock for the length of its own body, and says so with
 *     `RA8_OWNS_RESOURCE(name)` -- which is separately required to reach a
 *     matching `RA8_RELEASES_RESOURCE(name)` on every return path, so the
 *     pair is what makes "held across this call" checkable; or
 *  2. it was itself entered under the lock, and propagates the contract
 *     upward by carrying the same `RA8_EXPECTS_LOCK(name)`.
 *
 * Anything else is a violation reported at the call site.
 *
 * @param[in] name String literal naming the lock (e.g. `"i2c0_bus"`,
 *                 `"global_irq"`). It must be spelled identically on the
 *                 `RA8_OWNS_RESOURCE` / `RA8_RELEASES_RESOURCE` pair that
 *                 discharges it.
 *
 * @par Enforcement:
 * libclang call-graph walk verifies every caller holds `name`.
 *
 * @par Example:
 * @code
 * RA8_EXPECTS_LOCK("i2c0_bus")
 * ra8_err_t ra8_i2c0_write_locked(const uint8_t* buf, uint32_t len);
 *
 * RA8_OWNS_RESOURCE("i2c0_bus")
 * ra8_err_t ra8_i2c0_write(const uint8_t* buf, uint32_t len)
 * {
 *   ra8_i2c0_lock();                              // takes "i2c0_bus"
 *   const ra8_err_t e = ra8_i2c0_write_locked(buf, len);
 *   ra8_i2c0_unlock();                            // RA8_RELEASES_RESOURCE
 *   return e;
 * }
 * @endcode
 *
 * Callers of `ra8_i2c0_write_locked` must hold the `"i2c0_bus"` lock.
 */
#define RA8_EXPECTS_LOCK(name) RA8_INTERNAL_ANNOTATE("ra8_expects_lock:" name)

/* =============================================================================
 * 12. RA8_HOST_FRIENDLY
 * =============================================================================
 */

/**
 * @brief The function is safe to invoke under `RA8_OFF_TARGET` on the host.
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
 * RA8_HOST_FRIENDLY
 * ra8_err_t ra8_pid_step(ra8_pid_state_t* s, float setpoint, float measured);
 * @endcode
 *
 * `ra8_pid_step` is pure math and runs identically on hardware and host.
 */
#define RA8_HOST_FRIENDLY RA8_INTERNAL_ANNOTATE("ra8_host_friendly")

/* =============================================================================
 * 13. RA8_LATENCY_BUDGET_NS(n)
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
 * @param[in] n Integer literal: maximum execution time in nanoseconds.
 *
 * @par Enforcement:
 * Future WCET analysis pass (planned ).
 *
 * @par Example:
 * @code
 * RA8_LATENCY_BUDGET_NS(2000)
 * void ra8_servo_pwm_update(uint16_t duty_q8);
 * @endcode
 *
 * `ra8_servo_pwm_update` must complete within 2 microseconds.
 */
#define RA8_LATENCY_BUDGET_NS(n) RA8_INTERNAL_ANNOTATE("ra8_latency_budget_ns:" #n)

/* =============================================================================
 * 14. RA8_NO_RECURSION
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
 * RA8_NO_RECURSION
 * ra8_err_t ra8_fs_walk_directory(const char* path, ra8_fs_visitor_fn visit);
 * @endcode
 *
 * `ra8_fs_walk_directory` must not call itself, directly or transitively.
 */
#define RA8_NO_RECURSION RA8_INTERNAL_ANNOTATE("ra8_no_recursion")

/* =============================================================================
 * 15. RA8_BOUNDED_LOOP(symbol)
 * =============================================================================
 */

/**
 * @brief NASA Power-of-10 Rule 2: every loop in a FUNCTION has a constant bound.
 *
 * @details
 * This is a FUNCTION-level annotation. It decorates a function DECLARATION and
 * names the symbol (typically a typed enum value) that bounds every loop inside
 * that function; the libclang checker walks the function body and verifies each
 * loop's termination condition references the named symbol. Because it is an
 * `[[clang::annotate]]` attribute it may appear ONLY before a declaration --
 * never in statement position inside a body (see the Backend-selection note
 * above). To bind a bound to ONE specific loop, reach for `RA8_LOOP_BOUND` or
 * `RA8_LOOP_BOUND_RUNTIME` instead: those are real statements that sit directly
 * above the loop they describe.
 *
 * @param[in] symbol Bare token naming the bounding constant (e.g.
 *                   `k_max_retries`, `k_ringbuf_capacity`).
 *
 * @par Enforcement:
 * libclang loop analyzer.
 *
 * @warning
 * Not a statement. `RA8_BOUNDED_LOOP(x);` inside a function body is a hard
 * clang error and a silent GCC no-op; use `RA8_LOOP_BOUND(x);` there.
 *
 * @par Example:
 * @code
 * RA8_BOUNDED_LOOP(k_max_retries)
 * ra8_err_t ra8_i2c_send_with_retry(const uint8_t* buf, uint32_t len);
 * @endcode
 *
 * Every loop in `ra8_i2c_send_with_retry` is bounded by `k_max_retries`.
 *
 * @see RA8_LOOP_BOUND  Per-loop, statement-position bound (compile-time ceiling).
 * @see RA8_LOOP_BOUND_RUNTIME  Per-loop bound whose ceiling is a runtime symbol.
 */
#define RA8_BOUNDED_LOOP(symbol) RA8_INTERNAL_ANNOTATE("ra8_bounded_loop:" #symbol)

/* =============================================================================
 * 15b. RA8_LOOP_BOUND(ceiling)
 * =============================================================================
 */

/**
 * @def RA8_LOOP_BOUND
 * @brief NASA Power-of-10 Rule 2: bind ONE loop to a compile-time ceiling.
 *
 * @details
 * The per-loop counterpart to `RA8_BOUNDED_LOOP`. Placed on the statement line
 * immediately above a `for` / `while` / `do` loop, it asserts that @p ceiling
 * is a positive compile-time constant -- exactly the property NASA Rule 2
 * requires of a statically-bounded loop -- and it is the marker
 * `scripts/checks/check_annotations.py` pairs to that immediately-following
 * loop.
 *
 * Unlike the `RA8_*` annotation macros this is NOT an `[[clang::annotate]]`
 * attribute: it lowers to a `static_assert`, which is real C valid in statement
 * position under every toolchain. That is the whole point -- it cannot degrade
 * to a comment no-op the way a statement-position annotation did. If @p ceiling
 * is not a positive compile-time constant the build fails under both
 * arm-none-eabi-gcc and clang. A loop whose ceiling is a link-time / runtime
 * symbol (not a compile-time constant) cannot use this macro; use
 * `RA8_LOOP_BOUND_RUNTIME` instead rather than fake a `static_assert`.
 *
 * @param[in] ceiling A positive integer compile-time constant expression (typically
 *                    a typed enum value) that upper-bounds the loop's iteration
 *                    count. Cast to `uint32_t` inside the assert, so it must fit.
 *
 * @par Enforcement:
 * Two ways at once. (1) The `static_assert` fails the compile if @p ceiling is
 * not a positive constant. (2) `check_annotations.py` (loop-bound scan) fails
 * the gate if this marker is not immediately followed by a loop.
 *
 * @note No codegen, no runtime cost; the assert is evaluated at compile time.
 *
 * @par Example:
 * @code
 * RA8_LOOP_BOUND(k_max_retries);
 * for (uint32_t i = 0U; i < (uint32_t)k_max_retries; i++) {
 *   ...
 * }
 * @endcode
 *
 * @see RA8_LOOP_BOUND_RUNTIME  When the ceiling is a runtime / linker symbol.
 * @see RA8_BOUNDED_LOOP  Function-level form (all loops in one function).
 * @since 0.1.0
 */
#define RA8_LOOP_BOUND(ceiling)                                                                    \
  static_assert((uint32_t)(ceiling) > 0U,                                                          \
                "RA8_LOOP_BOUND requires a positive compile-time constant bound")

/* =============================================================================
 * 15c. RA8_LOOP_BOUND_RUNTIME(ceiling_ref)
 * =============================================================================
 */

/**
 * @def RA8_LOOP_BOUND_RUNTIME
 * @brief NASA Power-of-10 Rule 2: bind ONE loop to a runtime / linker ceiling.
 *
 * @details
 * The honest form for a loop whose upper bound is real but NOT a compile-time
 * constant -- the canonical case being a `.data` / `.bss` copy loop bounded by
 * a linker-provided end symbol (`while (dst < &g_ra8_ls_cpu1_data_end)`). Such
 * a loop is statically provably terminating (the pointer marches monotonically
 * to a fixed, link-time address), but no `static_assert` can evaluate that
 * address, so `RA8_LOOP_BOUND` cannot be used and faking one would be
 * dishonest.
 *
 * Placed on the statement line immediately above the loop, this macro names the
 * ceiling symbol so a typo fails to compile, and it is the marker
 * `check_annotations.py` pairs to the immediately-following loop. It lowers to
 * `((void)sizeof(&(ceiling_ref)))`: the operand is unevaluated, so it emits no
 * code (safe in a reset handler that runs before `.data` is copied) yet still
 * requires @p ceiling_ref to be a declared, addressable object.
 *
 * @param[in] ceiling_ref An addressable object whose address upper-bounds the loop
 *                        (e.g. a linker end symbol). Must be declared and in scope.
 *
 * @par Enforcement:
 * `((void)sizeof(&(ceiling_ref)))` fails the compile if @p ceiling_ref is not a
 * declared addressable object; `check_annotations.py` (loop-bound scan) fails
 * the gate if this marker is not immediately followed by a loop.
 *
 * @note No codegen, no runtime cost; `sizeof`'s operand is unevaluated.
 *
 * @par Example:
 * @code
 * RA8_LOOP_BOUND_RUNTIME(g_ra8_ls_cpu1_data_end);
 * while (dst < &g_ra8_ls_cpu1_data_end) {
 *   *dst = *src;
 *   dst++;
 *   src++;
 * }
 * @endcode
 *
 * @see RA8_LOOP_BOUND  When the ceiling is a compile-time constant.
 * @since 0.1.0
 */
#define RA8_LOOP_BOUND_RUNTIME(ceiling_ref) ((void)sizeof(&(ceiling_ref)))

/* =============================================================================
 * 16. RA8_VALIDATES(n)
 * =============================================================================
 */

/**
 * @brief NASA Power-of-10 Rule 5: function body has at least `n` `RA8_CHECK_*` calls.
 *
 * @details
 * Records the validation-count contract so the libclang checker can
 * count `RA8_CHECK_*` / `RA8_VALIDATE_*` / `RA8_ASSERT` invocations in the
 * function body and fail if the count drops below `n`.
 *
 * @param[in] n Integer literal: minimum number of validation calls.
 *
 * @par Enforcement:
 * libclang AST walk counts `RA8_CHECK_*` invocations.
 *
 * @par Example:
 * @code
 * RA8_VALIDATES(3)
 * ra8_err_t ra8_gpio_output_init(ra8_port_t port, uint8_t pin, ra8_level_t lvl);
 * @endcode
 *
 * `ra8_gpio_output_init` must contain at least three `RA8_CHECK_*` calls.
 */
#define RA8_VALIDATES(n) RA8_INTERNAL_ANNOTATE("ra8_validates:" #n)

/* =============================================================================
 * 17. RA8_OWNS_RESOURCE(kind)
 * =============================================================================
 */

/**
 * @brief RAII-style resource ownership contract.
 *
 * @details
 * The function acquires a resource of `kind` (a typed-enum-style label
 * such as `"i2c_bus"`, `"dma_channel"`, `"trng_handle"`). The libclang
 * checker walks every return path and requires a matching
 * `RA8_RELEASES_RESOURCE(kind)` call before the function returns -- on
 * the success path and every error path.
 *
 * @param[in] kind String literal naming the resource kind.
 *
 * @par Enforcement:
 * libclang control-flow walk: every return path releases the resource.
 *
 * @par Example:
 * @code
 * RA8_OWNS_RESOURCE("dma_channel")
 * ra8_err_t ra8_dma_acquire_channel(ra8_dma_channel_t* out_ch);
 * @endcode
 *
 * Every caller of `ra8_dma_acquire_channel` must, on success, call
 * `ra8_dma_release_channel` before any `return`.
 */
#define RA8_OWNS_RESOURCE(kind) RA8_INTERNAL_ANNOTATE("ra8_owns_resource:" kind)

/**
 * @brief Release side of the `RA8_OWNS_RESOURCE(kind)` pair.
 *
 * @details
 * Marks the function that hands a resource of `kind` back. The checker
 * looks for a call to a function carrying this annotation when deciding
 * whether an `RA8_OWNS_RESOURCE(kind)` function has discharged its
 * ownership, so `kind` must be spelled identically on both halves.
 *
 * Without this macro the acquire side has no counterpart to look for and
 * the rule can only ever report that nothing releases anything, which is
 * why it is defined here rather than left to each caller to invent.
 *
 * @param[in] kind String literal naming the resource kind. Must match the
 *                 `RA8_OWNS_RESOURCE(kind)` it pairs with, character for
 *                 character.
 *
 * @par Enforcement:
 * libclang control-flow walk, paired with `RA8_OWNS_RESOURCE`.
 *
 * @par Example:
 * @code
 * RA8_RELEASES_RESOURCE("dma_channel")
 * ra8_err_t ra8_dma_release_channel(ra8_dma_channel_t ch);
 * @endcode
 *
 * `ra8_dma_release_channel` discharges the ownership that
 * `ra8_dma_acquire_channel` took.
 */
#define RA8_RELEASES_RESOURCE(kind) RA8_INTERNAL_ANNOTATE("ra8_releases_resource:" kind)

/* =============================================================================
 * 18. RA8_REVIEWED_BY(name)
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
 * @param[in] name String literal: reviewer identity (e.g. `"bsikar"`).
 *
 * @par Enforcement:
 * `docs/qualification/SVR.md` auto-rollup.
 *
 * @par Example:
 * @code
 * RA8_REVIEWED_BY("bsikar")
 * ra8_err_t ra8_secure_storage_commit(const uint8_t* key_blob, uint32_t len);
 * @endcode
 *
 * `ra8_secure_storage_commit` has been reviewed by `bsikar` for the
 * safety-critical key-commit path.
 */
#define RA8_REVIEWED_BY(name) RA8_INTERNAL_ANNOTATE("ra8_reviewed_by:" name)

/* =============================================================================
 * 19. RA8_REGISTER_BANK(peripheral)
 * =============================================================================
 */

/**
 * @brief Group MMIO accessor functions by peripheral register bank.
 *
 * @details
 * The annotation tags each `RA8_HW_REGISTER_ACCESS`-style accessor with
 * its parent peripheral so the auto-generated peripheral documentation
 * tool can list every accessor belonging to a given block (e.g. SCI0,
 * IIC1, GPT3).
 *
 * @param[in] peripheral String literal naming the peripheral (e.g.
 *                       `"sci0"`, `"iic1"`, `"gpt3"`).
 *
 * @par Enforcement:
 * Documentation generator groups accessors by `peripheral` name.
 *
 * @par Example:
 * @code
 * RA8_REGISTER_BANK("sci0")
 * RA8_HW_REGISTER_ACCESS
 * static inline volatile ra8_sci_regs_t* ra8_sci0_regs(void);
 * @endcode
 *
 * `ra8_sci0_regs` is grouped under the `sci0` register bank in the
 * generated peripheral documentation.
 */
#define RA8_REGISTER_BANK(peripheral) RA8_INTERNAL_ANNOTATE("ra8_register_bank:" peripheral)

#ifdef __cplusplus
}
#endif
