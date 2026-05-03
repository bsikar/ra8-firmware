# RA_* Annotation Contracts

This document is the authoritative reference for the `RA_*` annotation
macros defined in `libs/ra_core/inc/ra_attributes.h`. The macros are
metadata-only: they expand to `__attribute__((annotate("...")))` under
clang and to a comment-only no-op under other toolchains. They produce
no codegen and have zero runtime cost.

The libclang-based enforcement script that consumes these annotations
will land in a follow-up. Until then, the annotations are documentation
contracts that humans (and the citation gate) verify by hand.

## Citation policy

Every example in this document references targets by **function name**
or **symbol name** -- never by line number -- per
[`docs/CITATION_POLICY.md`](CITATION_POLICY.md). The
`RA_MCDC_DEACTIVATED(reason)` macro additionally enforces this rule
mechanically: the citation gate (`scripts/utils/check_line_citations.py`)
rejects any reason text containing a `<file>.<ext>:<line>` token.

## Reference

### 1. `RA_TEST_HELPER`

- **Purpose:** externally-linked symbol callable only from `tests/`.
- **Enforcement:** libclang call-graph analyzer.
- **Example:**

  ```c
  RA_TEST_HELPER ra_err_t ra_pin_validator_reset_for_test(void);
  ```

  `ra_pin_validator_reset_for_test` may only be called from test
  entry points such as `test_ra_pin_validator`.

### 2. `RA_INTERNAL`

- **Purpose:** marker that a function is intended to be `static`.
- **Enforcement:** libclang verifies storage class is `static`.
- **Example:**

  ```c
  RA_INTERNAL static ra_err_t internal_validate_pin_range(uint8_t pin);
  ```

### 3. `RA_PRIV`

- **Purpose:** module-private helper shared across TUs inside one library.
- **Enforcement:** libclang verifies callers reside in the same
  `libs/<module>/` directory.
- **Example:**

  ```c
  RA_PRIV ra_err_t priv_ra_log_emit(const char* tag, const char* msg);
  ```

  `priv_ra_log_emit` is callable only from inside `libs/ra_core/`.

### 4. `RA_DI_SLOT(role)`

- **Purpose:** mark a function as an explicit Dependency Injection seam.
- **Enforcement:** libclang verifies a mock exists under `tests/mocks/`
  for the role and that production callers go through the vtable.
- **Example:**

  ```c
  RA_DI_SLOT("bus_read")
  ra_err_t ra_bus_i2c_read(void* ctx, uint8_t* dst, uint32_t len);
  ```

### 5. `RA_NSC_VENEER`

- **Purpose:** TrustZone Secure-to-Non-Secure entry-point veneer; pairs
  with `__attribute__((cmse_nonsecure_entry))`. Lives in `.gnu.sgstubs`.
- **Enforcement:**
  - `scripts/utils/check_world_tags.py` restricts `cmse_nonsecure_entry`
    to files under `libs/ra_nsc/`.
 - libclang checker verifies every pointer parameter passes
    through a `RA_NSC_CHECK_NS_RANGE_*` helper before being dereferenced.
- **Example:**

  ```c
  RA_NSC_VENEER
  __attribute__((cmse_nonsecure_entry))
  ra_err_t ra_nsc_secure_storage_read(uint8_t* ns_buf, uint32_t len);
  ```

  `ra_nsc_secure_storage_read` must call
  `RA_NSC_CHECK_NS_RANGE_RW(ns_buf, len)` before touching `ns_buf`.

### 6. `RA_HW_REGISTER_ACCESS`

- **Purpose:** MMIO accessor function (returns `volatile` register pointer).
- **Enforcement:** libclang verifies the function is `inline` and its
  return type is `volatile`-qualified, and scans call sites to confirm
  writes go through `RA_PROTECTED_WRITE` or carry a per-line
  `// CITES-OK: read-only` justification.
- **Example:**

  ```c
  RA_HW_REGISTER_ACCESS
  static inline volatile ra_sci_regs_t* ra_sci0_regs(void);
  ```

### 7. `RA_NASA_RULE_3_OK`

- **Purpose:** documented exception to NASA P10 Rule 3 (no dynamic alloc).
- **Enforcement:** `scripts/utils/check_no_dynamic_alloc.py` plus
  libclang call-graph walk. Untagged callers of tagged functions must
  themselves be tagged or carry a deviation entry.
- **Example:**

  ```c
  RA_NASA_RULE_3_OK
  ra_err_t ra_test_harness_alloc_scratch(uint32_t bytes, void** out);
  ```

### 8. `RA_MCDC_DEACTIVATED(reason)`

- **Purpose:** mark a decision as MC/DC-deactivated, replacing the
  legacy `// mcdc-deactivated:` line comment.
- **Enforcement:**
  - `scripts/utils/check_line_citations.py` rejects any reason text
    containing a `<file>.<ext>:<line>` token.
  - `scripts/utils/regen_mcdc_gaps.py` tallies `RA_MCDC_DEACTIVATED`
    annotations into `docs/MCDC_DEACTIVATIONS.md`.
- **Example:**

  ```c
  RA_MCDC_DEACTIVATED("defensive guard, ra_pin_validator_check is "
                      "the sole caller and asserts non-null first")
  static inline bool internal_validate_handle(const ra_pin_t* h);
  ```

### 9. `RA_MAX_STACK(bytes)`

- **Purpose:** per-function stack-frame budget.
- **Enforcement:** `scripts/utils/stack_usage_check.py` cross-checks
  against GCC `-fstack-usage` `.su` files.
- **Example:**

  ```c
  RA_MAX_STACK(128)
  ra_err_t ra_uart_isr_drain_fifo(ra_uart_handle_t* h);
  ```

### 10. `RA_ISR_SAFE`

- **Purpose:** the function is safe to call from interrupt context.
- **Enforcement:** libclang call-graph walk: every callee reachable from
  an ISR-tagged function (file matches `*_isr.c`, or function is tagged
  `RA_ISR_HANDLER`) must itself be `RA_ISR_SAFE`.
- **Example:**

  ```c
  RA_ISR_SAFE
  void ra_ringbuf_push_byte(ra_ringbuf_t* rb, uint8_t b);
  ```

  `ra_ringbuf_push_byte` is safe to call from `ra_uart0_rxi_handler`.

### 11. `RA_EXPECTS_LOCK(name)`

- **Purpose:** function expects the named lock to be held on entry.
- **Enforcement:** libclang verifies the caller wraps the call in
  `RA_TAKE_LOCK(name)` / `RA_RELEASE_LOCK(name)`, or is itself tagged
  `RA_EXPECTS_LOCK(name)` (propagating the contract upward).
- **Example:**

  ```c
  RA_EXPECTS_LOCK("i2c0_bus")
  ra_err_t ra_i2c0_write_locked(const uint8_t* buf, uint32_t len);
  ```

### 12. `RA_HOST_FRIENDLY`

- **Purpose:** the function works under `RA_SIMULATOR_MODE` on the host.
- **Enforcement:** libclang AST walk: no unmocked `volatile` MMIO inside
  the call subtree.
- **Example:**

  ```c
  RA_HOST_FRIENDLY
  ra_err_t ra_pid_step(ra_pid_state_t* s, float setpoint, float measured);
  ```

  `ra_pid_step` is pure math and runs identically on hardware and host.

### 13. `RA_LATENCY_BUDGET_NS(n)`

- **Purpose:** real-time deadline; function must complete within `n` ns.
- **Enforcement:** future WCET analysis pass cross-checks the budget.
- **Example:**

  ```c
  RA_LATENCY_BUDGET_NS(2000)
  void ra_servo_pwm_update(uint16_t duty_q8);
  ```

### 14. `RA_NO_RECURSION`

- **Purpose:** NASA P10 Rule 1; no direct or indirect self-call.
- **Enforcement:** libclang call-graph cycle detection.
- **Example:**

  ```c
  RA_NO_RECURSION
  ra_err_t ra_fs_walk_directory(const char* path, ra_fs_visitor_fn visit);
  ```

### 15. `RA_BOUNDED_LOOP(symbol)`

- **Purpose:** NASA P10 Rule 2; every loop has a constant upper bound
  named by `symbol`.
- **Enforcement:** libclang loop analyzer verifies each loop's
  termination condition references a constant or the named symbol.
- **Example:**

  ```c
  RA_BOUNDED_LOOP(k_max_retries)
  ra_err_t ra_i2c_send_with_retry(const uint8_t* buf, uint32_t len);
  ```

### 16. `RA_VALIDATES(n)`

- **Purpose:** NASA P10 Rule 5; function body has at least `n`
  `RA_CHECK_*` / `RA_VALIDATE_*` / `RA_ASSERT` calls.
- **Enforcement:** libclang AST walk counts validation invocations.
- **Example:**

  ```c
  RA_VALIDATES(3)
  ra_err_t ra_gpio_output_init(ra_port_t port, uint8_t pin, ra_level_t lvl);
  ```

### 17. `RA_OWNS_RESOURCE(kind)`

- **Purpose:** RAII-style resource-ownership contract.
- **Enforcement:** libclang control-flow walk verifies every return path
  releases the resource via a matching `RA_RELEASES_RESOURCE(kind)` call.
- **Example:**

  ```c
  RA_OWNS_RESOURCE("dma_channel")
  ra_err_t ra_dma_acquire_channel(ra_dma_channel_t* out_ch);
  ```

  Every caller of `ra_dma_acquire_channel` must, on success, call
  `ra_dma_release_channel` before any `return`.

### 18. `RA_REVIEWED_BY(name)`

- **Purpose:** safety-critical reviewer sign-off.
- **Enforcement:** the qualification toolchain rolls annotations up into
  `docs/qualification/SVR.md`.
- **Example:**

  ```c
  RA_REVIEWED_BY("bsikar")
  ra_err_t ra_secure_storage_commit(const uint8_t* key_blob, uint32_t len);
  ```

### 19. `RA_REGISTER_BANK(peripheral)`

- **Purpose:** group MMIO accessors by parent peripheral.
- **Enforcement:** the documentation generator groups accessors by
  `peripheral` name when emitting per-peripheral reference pages.
- **Example:**

  ```c
  RA_REGISTER_BANK("sci0")
  RA_HW_REGISTER_ACCESS
  static inline volatile ra_sci_regs_t* ra_sci0_regs(void);
  ```

## Backend notes

The macros expand under clang to `__attribute__((annotate("ra_<tag>")))`
or `__attribute__((annotate("ra_<tag>:<arg>")))`. Clang preserves the
annotation in the AST and exposes it through libclang
(`clang_Cursor_getAnnotations`). GCC parses the same syntax silently and
emits no warning, but does not surface it through a public API. The
header therefore gates on `__clang__` and falls back to a literal
comment placeholder under other toolchains so portable builds compile
cleanly.

## Enforcement script

The static enforcement framework lives at
[`scripts/utils/check_annotations.py`](../scripts/utils/check_annotations.py).
It walks the AST of every C/C++ TU under `libs/`, `src/`, `examples/`,
`tests/`, and `port/` via the Python `libclang` bindings and applies
the 19 rules documented above. Excluded subtrees: `build/`, `_deps/`,
`third_party/`, and the various `build-*/` host-test directories.

### Running it

```sh
# Warn-only (default; mirrors cite_check / world-tag pattern)
python3 scripts/utils/check_annotations.py

# CI gate -- exits non-zero on any non-warn-only violation
python3 scripts/utils/check_annotations.py --check

# Or via the convenience target
make check-annotations

# Dump every annotated symbol in the project (no enforcement)
python3 scripts/utils/check_annotations.py --list
```

### Dependency

The script depends on the `libclang` Python wheel, which ships its own
`libclang.{so,dylib}` so no system-wide LLVM install is required:

```sh
python3 -m pip install --user --break-system-packages libclang
```

The wheel is `libclang-18.x` on macOS arm64 / Linux x86_64.

### mode

The script header sets `WARN_ONLY_MODE = True`. While the flag is
on, the pre-commit hook and the bare CLI never exit non-zero; only
explicit `--check` invocations are fatal. Promotion to strict
mirrors the `cite_check` / `check_world_tags` schedule documented
in `CLAUDE.md`.

### Pre-commit wiring

The hook at [`scripts/git/pre-commit`](../scripts/git/pre-commit)
invokes the script after the existing static gates (`cite_check`,
`check_world_tags`, etc.) and before the stack-usage aggregator. The
hook respects the warn-only flag, so flipping the flag is the single
switch that turns the gate strict project-wide.

### Rule-by-rule notes

| # | Rule                              | Implementation kind                    |
|---|-----------------------------------|----------------------------------------|
| 1 | `ra_test_helper`                  | caller path must contain `/tests/`     |
| 2 | `ra_internal`                     | linkage check on definition            |
| 3 | `ra_priv`                         | `libs/<module>/` path comparison       |
| 4 | `ra_di_slot:<role>`               | address-of vs direct-call heuristic    |
| 5 | `ra_nsc_veneer`                   | path + body-call + section attr        |
| 6 | `ra_hw_mmio`                      | inline + return-type volatile check    |
| 7 | `ra_p10_rule3_exception`          | global malloc/free sweep               |
| 8 | `ra_mcdc_deactivated:<reason>`    | reason-string regex                    |
| 9 | `ra_stack_max:<bytes>`            | reads `examples/*/build*/*.su`         |
| 10| `ra_isr_safe`                     | call-graph reachability from handlers  |
| 11| `ra_expects_lock:<name>`          | preceding `RA_TAKE_LOCK("<name>")`     |
| 12| `ra_host_friendly`                | rejects calls into MMIO accessors      |
| 13| `ra_latency_max_ns:<n>`           | warn-only TODO until WCET pass         |
| 14| `ra_no_recursion`                 | transitive call-closure walk           |
| 15| `ra_bounded_loop:<symbol>`        | textual condition-expression check     |
| 16| `ra_validates:<n>`                | counts `RA_CHECK_*` calls in body      |
| 17| `ra_owns_resource:<kind>`         | matching `ra_releases_resource:<kind>` |
| 18| `ra_reviewed_by:<name>`           | informational rollup only              |
| 19| `ra_register_bank:<periph>`       | informational only (doc-gen feed)      |
