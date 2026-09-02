# RA8_* Annotation Contracts

This document is the authoritative reference for the `RA8_*` annotation
macros defined in `libs/ra8_core/inc/ra8_attributes.h`. The macros are
metadata-only: they expand to `[[clang::annotate("...")]]` under clang
and to a comment-only no-op under other toolchains. They produce
no codegen and have zero runtime cost.

The libclang-based enforcement script consumes these annotations in CI and
cross-checks their storage, naming, declaration, and call-graph contracts.

## Citation policy

Every example in this document references targets by **function name**
or **symbol name** -- never by line number -- per
[`docs/CITATION_POLICY.md`](CITATION_POLICY.md). The
`RA8_MCDC_DEACTIVATED(reason)` macro additionally enforces this rule
mechanically: the citation gate (`scripts/checks/check_line_citations.py`)
rejects any reason text containing a `<file>.<ext>:<line>` token.

## Reference

### 1. `RA8_TEST_HELPER`

- **Purpose:** externally-linked symbol callable only from `tests/`.
- **Enforcement:** libclang call-graph analyzer.
- **Example:**

  ```c
  RA8_TEST_HELPER ra8_err_t ra8_pin_validator_reset_for_test(void);
  ```

  `ra8_pin_validator_reset_for_test` may only be called from test
  entry points such as `test_ra8_pin_validator`.

### 2. `RA8_INTERNAL`

- **Purpose:** marker that a function is intended to be `static`.
- **Enforcement:** libclang verifies storage class is `static` and the
  function uses the `internal_` prefix.
- **Example:**

  ```c
  RA8_INTERNAL static ra8_err_t internal_validate_pin_range(uint8_t pin);
  ```

### 3. `RA8_PRIV`

- **Purpose:** module-private helper shared across TUs inside one library.
- **Enforcement:** libclang verifies the definition is non-`static`, uses the
  `priv_` prefix, is declared in that module's `*_internal.h`, and has no
  production caller outside its `libs/<module>/` or `tools/<tool>/` boundary.
- **Example:**

  ```c
  RA8_PRIV ra8_err_t priv_ra8_log_emit(const char* tag, const char* msg);
  ```

  `priv_ra8_log_emit` is callable only from inside `libs/ra8_core/`.

### 4. `RA8_DI_SLOT(role)`

- **Purpose:** mark a function as an explicit Dependency Injection seam.
- **Enforcement:** libclang verifies a mock exists under `tests/mocks/`
  for the role and that production callers go through the vtable.
- **Example:**

  ```c
  RA8_DI_SLOT("bus_read")
  ra8_err_t ra8_bus_i2c_read(void* ctx, uint8_t* dst, uint32_t len);
  ```

### 5. `RA8_NSC_VENEER`

- **Purpose:** TrustZone Secure-to-Non-Secure entry-point veneer; pairs
  with `__attribute__((cmse_nonsecure_entry))`. Lives in `.gnu.sgstubs`.
- **Enforcement:**
  - `scripts/checks/check_world_tags.py` restricts `cmse_nonsecure_entry`
    to files under `libs/ra8_nsc/`.
 - libclang checker verifies every pointer parameter passes
    through a `RA8_NSC_CHECK_NS_RANGE_*` helper before being dereferenced.
- **Example:**

  ```c
  RA8_NSC_VENEER
  __attribute__((cmse_nonsecure_entry))
  ra8_err_t ra8_nsc_secure_storage_read(uint8_t* ns_buf, uint32_t len);
  ```

  `ra8_nsc_secure_storage_read` must call
  `RA8_NSC_CHECK_NS_RANGE_RW(ns_buf, len)` before touching `ns_buf`.

### 6. `RA8_HW_REGISTER_ACCESS`

- **Purpose:** MMIO accessor function (returns `volatile` register pointer).
- **Enforcement:** libclang verifies the function is `inline` and its
  return type is `volatile`-qualified, and scans call sites to confirm
  writes go through `RA8_PROTECTED_WRITE` or carry a per-line
  `// CITES-OK: read-only` justification.
- **Example:**

  ```c
  RA8_HW_REGISTER_ACCESS
  static inline volatile ra8_sci_regs_t* ra8_sci0_regs(void);
  ```

### 7. `RA8_NASA_RULE_3_OK(reason)`

- **Purpose:** documented exception to NASA P10 Rule 3 (no dynamic alloc).
- **Reason:** a narrow string literal naming the unavoidable allocation
  boundary is mandatory.
- **Enforcement:** `scripts/checks/check_no_dynamic_alloc.py` plus
  libclang call-graph walk over firmware-linkable translation units. Host
  products and files in any `tests/` directory are outside the firmware rule;
  untagged firmware callers of tagged functions must themselves be tagged or
  carry a deviation entry.
- **Example:**

  ```c
  RA8_NASA_RULE_3_OK("vendor TLS session owns opaque handshake storage")
  ra8_err_t priv_target_tls_open(void);
  ```

### 8. `RA8_MCDC_DEACTIVATED(reason)`

- **Purpose:** mark a decision as MC/DC-deactivated, replacing the
  legacy `// mcdc-deactivated:` line comment.
- **Enforcement:**
  - `scripts/checks/check_line_citations.py` rejects any reason text
    containing a `<file>.<ext>:<line>` token.
  - `scripts/fix/regen_mcdc_gaps.py` tallies `RA8_MCDC_DEACTIVATED`
    annotations into `docs/MCDC_DEACTIVATIONS.md`.
- **Example:**

  ```c
  RA8_MCDC_DEACTIVATED("defensive guard, ra8_pin_validator_check is "
                      "the sole caller and asserts non-null first")
  static inline bool internal_validate_handle(const ra8_pin_t* h);
  ```

### 9. `RA8_MAX_STACK(bytes)`

- **Purpose:** per-function stack-frame budget.
- **Enforcement:** `scripts/checks/stack_usage_check.py` cross-checks
  against GCC `-fstack-usage` `.su` files.
- **Example:**

  ```c
  RA8_MAX_STACK(128)
  ra8_err_t ra8_uart_isr_drain_fifo(ra8_uart_handle_t* h);
  ```

### 10. `RA8_ISR_SAFE`

- **Purpose:** the function is safe to call from interrupt context.
- **Enforcement:** marker only -- see the Rule 10 note under
  "Rule-by-rule notes". The transitive ISR-chain walk this used to feed
  was keyed on `ra8_isr_handler`, an annotation no macro emits, so it
  never ran; it was deleted rather than left looking enforced.
- **Example:**

  ```c
  RA8_ISR_SAFE
  void ra8_ringbuf_push_byte(ra8_ringbuf_t* rb, uint8_t b);
  ```

  `ra8_ringbuf_push_byte` is safe to call from `ra8_uart0_rxi_handler`.

### 11. `RA8_EXPECTS_LOCK(name)`

- **Purpose:** function expects the named lock to be held on entry.
- **Enforcement:** libclang verifies every caller either acquires the
  lock for its own body with `RA8_OWNS_RESOURCE(name)` -- which the
  acquire/release rule separately pairs with `RA8_RELEASES_RESOURCE(name)`
  -- or is itself tagged `RA8_EXPECTS_LOCK(name)`, propagating the
  contract upward. It used to look for a preceding `RA8_TAKE_LOCK` call;
  nothing of that name has ever existed in this tree, which made the
  annotation unsatisfiable and therefore unused.
- **Example:**

  ```c
  RA8_EXPECTS_LOCK("i2c0_bus")
  ra8_err_t ra8_i2c0_write_locked(const uint8_t* buf, uint32_t len);
  ```

### 12. `RA8_HOST_FRIENDLY`

- **Purpose:** the function works under `RA8_OFF_TARGET` on the host.
- **Enforcement:** libclang AST walk: no unmocked `volatile` MMIO inside
  the call subtree.
- **Example:**

  ```c
  RA8_HOST_FRIENDLY
  ra8_err_t ra8_pid_step(ra8_pid_state_t* s, float setpoint, float measured);
  ```

  `ra8_pid_step` is pure math and runs identically on hardware and host.

### 13. `RA8_LATENCY_BUDGET_NS(n)`

- **Purpose:** real-time deadline; function must complete within `n` ns.
- **Enforcement:** future WCET analysis pass cross-checks the budget.
- **Example:**

  ```c
  RA8_LATENCY_BUDGET_NS(2000)
  void ra8_servo_pwm_update(uint16_t duty_q8);
  ```

### 14. `RA8_NO_RECURSION`

- **Purpose:** NASA P10 Rule 1; no direct or indirect self-call.
- **Enforcement:** libclang call-graph cycle detection.
- **Example:**

  ```c
  RA8_NO_RECURSION
  ra8_err_t ra8_fs_walk_directory(const char* path, ra8_fs_visitor_fn visit);
  ```

### 15. `RA8_BOUNDED_LOOP(symbol)`

- **Purpose:** NASA P10 Rule 2, FUNCTION-level; every loop in the
  function has a constant upper bound named by `symbol`.
- **Enforcement:** libclang loop analyzer verifies each loop's
  termination condition references a constant or the named symbol.
- **Position:** This is an `[[clang::annotate]]` attribute, so it may
  appear ONLY before a DECLARATION. It is NOT valid in statement
  position: `RA8_BOUNDED_LOOP(x);` inside a body is a hard clang error
  and a silent GCC no-op that binds to nothing. To bind a bound to ONE
  specific loop, use `RA8_LOOP_BOUND` / `RA8_LOOP_BOUND_RUNTIME` below.
- **Example:**

  ```c
  RA8_BOUNDED_LOOP(k_max_retries)
  ra8_err_t ra8_i2c_send_with_retry(const uint8_t* buf, uint32_t len);
  ```

### 15b. `RA8_LOOP_BOUND(ceiling)`

- **Purpose:** NASA P10 Rule 2, per-LOOP; bind ONE loop to a positive
  compile-time-constant ceiling. The statement-position counterpart to
  `RA8_BOUNDED_LOOP`.
- **Mechanism:** Not an annotation. Lowers to
  `static_assert((uint32_t)(ceiling) > 0U, ...)`, real C valid in
  statement position under every toolchain, so it cannot degrade to a
  comment no-op. If `ceiling` is not a positive compile-time constant
  the build fails under both arm-none-eabi-gcc and clang.
- **Enforcement:** two ways at once. (1) The `static_assert` fails the
  compile on a non-constant ceiling. (2) `check_annotations.py`
  (loop-bound scan, `annot_loopbound`) fails the gate if the marker is
  not immediately followed by a `for` / `while` / `do` loop.
- **Example:**

  ```c
  RA8_LOOP_BOUND(k_max_retries);
  for (uint32_t i = 0U; i < (uint32_t)k_max_retries; i++) { ... }
  ```

### 15c. `RA8_LOOP_BOUND_RUNTIME(ceiling_ref)`

- **Purpose:** NASA P10 Rule 2, per-LOOP; the honest form for a loop
  whose bound is real but NOT a compile-time constant -- e.g. a
  `.data` / `.bss` copy loop bounded by a linker end symbol
  (`while (dst < &g_ra8_ls_cpu1_data_end)`). Faking a `static_assert`
  on a link-time address would be dishonest, so this form is separate.
- **Mechanism:** Lowers to `((void)sizeof(&(ceiling_ref)))`: the operand
  is unevaluated (no codegen, safe in a reset handler that runs before
  `.data` is copied) yet still requires `ceiling_ref` to be a declared,
  addressable object, so a typo fails to compile.
- **Enforcement:** the symbol reference fails the compile on an
  undeclared ceiling; `check_annotations.py` (loop-bound scan) fails the
  gate if the marker is not immediately followed by a loop.
- **Example:**

  ```c
  RA8_LOOP_BOUND_RUNTIME(g_ra8_ls_cpu1_data_end);
  while (dst < &g_ra8_ls_cpu1_data_end) { *dst = *src; dst++; src++; }
  ```

### 16. `RA8_VALIDATES(n)`

- **Purpose:** NASA P10 Rule 5; function body has at least `n`
  `RA8_CHECK_*` / `RA8_VALIDATE_*` / `RA8_ASSERT` calls.
- **Enforcement:** libclang AST walk counts validation invocations.
- **Example:**

  ```c
  RA8_VALIDATES(3)
  ra8_err_t ra8_gpio_output_init(ra8_port_t port, uint8_t pin, ra8_level_t lvl);
  ```

### 17. `RA8_OWNS_RESOURCE(kind)`

- **Purpose:** RAII-style resource-ownership contract.
- **Enforcement:** libclang control-flow walk verifies every return path
  releases the resource via a matching `RA8_RELEASES_RESOURCE(kind)` call.
- **Example:**

  ```c
  RA8_OWNS_RESOURCE("dma_channel")
  ra8_err_t ra8_dma_acquire_channel(ra8_dma_channel_t* out_ch);
  ```

  Every caller of `ra8_dma_acquire_channel` must, on success, call
  `ra8_dma_release_channel` before any `return`.

### 17b. `RA8_RELEASES_RESOURCE(kind)`

- **Purpose:** the release half of the `RA8_OWNS_RESOURCE(kind)` pair.
- **Enforcement:** the acquire-side rule looks for a call to a function
  carrying this annotation with a byte-identical `kind`.
- **Example:**

  ```c
  RA8_RELEASES_RESOURCE("dma_channel")
  ra8_err_t ra8_dma_release_channel(ra8_dma_channel_t ch);
  ```

  This macro was documented by rule 17 but never defined, so the acquire
  side had no counterpart to find and could only ever report that nothing
  releases anything.

### 18. `RA8_REVIEWED_BY(name)`

- **Purpose:** safety-critical reviewer sign-off.
- **Enforcement:** the qualification toolchain rolls annotations up into
  `docs/qualification/SVR.md`.
- **Example:**

  ```c
  RA8_REVIEWED_BY("bsikar")
  ra8_err_t ra8_secure_storage_commit(const uint8_t* key_blob, uint32_t len);
  ```

### 19. `RA8_REGISTER_BANK(peripheral)`

- **Purpose:** group MMIO accessors by parent peripheral.
- **Enforcement:** the documentation generator groups accessors by
  `peripheral` name when emitting per-peripheral reference pages.
- **Example:**

  ```c
  RA8_REGISTER_BANK("sci0")
  RA8_HW_REGISTER_ACCESS
  static inline volatile ra8_sci_regs_t* ra8_sci0_regs(void);
  ```

## Backend notes

The macros expand under clang to `[[clang::annotate("ra8_<tag>")]]`
or `[[clang::annotate("ra8_<tag>:<arg>")]]`. Clang preserves the
annotation in the AST and exposes it through libclang
(`clang_Cursor_getAnnotations`). GCC parses the same syntax silently and
emits no warning, but does not surface it through a public API. The
header therefore gates on `__clang__` and falls back to a literal
comment placeholder under other toolchains so portable builds compile
cleanly.

## Enforcement script

The static enforcement framework lives at
[`scripts/checks/check_annotations.py`](../scripts/checks/check_annotations.py).
It walks the AST of every C/C++ TU under `libs/`, `examples/`,
`tests/`, `port/`, `tools/` and `apps/` via the Python `libclang` bindings and applies
the rules documented above, plus the linkage rule below. Excluded
subtrees: `build/`, `_deps/`, `third_party/`, and the various `build-*/`
host-test directories.

### The linkage rule (`ra8_linkage`)

Every non-`static` function definition in first-party code has to say why
it has external linkage. It passes when any of these holds:

- it carries a linkage annotation whose own contract passes (`RA8_PRIV` is
  non-`static` module-private, `RA8_INTERNAL` is `static` file-local, and
  `RA8_TEST_HELPER` is externally linked but test-only);
- a header publishes it -- a library's public `inc/` header, an
  application's local header, a mock's header, or the vendored SOUP
  header whose interface it implements. An `*_internal.h` prototype does
  **not** count: that header exists to say "library-private", which is
  exactly what `RA8_PRIV` marks;
- it is a hardware vector-table entry. The CPU reaches these through VTOR
  with no C caller, so nothing can publish them. The category is derived
  structurally -- a file-scope array of `const` function pointers, or a
  `const` array the linker places in a `*vectors` section -- so a handler
  that is unwired from its table stops being exempt the moment it is;
- it is `main`, the ISO C entry point.

### Two self-checks that run before any rule

Both of these failure modes look exactly like success, so each one fails
the gate on its own terms:

- **Rule keys.** Every key the script dispatches on is cross-checked
  against the annotation strings `ra8_attributes.h` actually emits. A
  rule keyed on a spelling no macro produces matches nothing and reports
  zero violations forever -- four rules were in that state at once.
- **Parse integrity.** An unresolved include or a drop in the fraction of
  resolved call sites below `MIN_CALL_RESOLUTION` is fatal. When the
  parse comes apart the call-graph rules stop policing anything and the
  gate reports *fewer* violations, which reads as an improvement.

### The loop-bound scan (`ra8_loop_bound`)

`RA8_LOOP_BOUND` / `RA8_LOOP_BOUND_RUNTIME` are not annotations, so they
never reach the AST as such -- they have already expanded to a
`static_assert` / a symbol reference. Their contract is a source-text
adjacency: the marker sits on the line above the loop it bounds. So
[`annot_loopbound.py`](../scripts/checks/annot_loopbound.py) scans the
source (comments, strings and `#define` lines stripped) and fails on two
directions, both of which are the exact defect these markers replaced --
a bound annotation that binds to nothing:

- a `RA8_LOOP_BOUND` / `RA8_LOOP_BOUND_RUNTIME` marker whose next code
  line is not a `for` / `while` / `do` loop (mis-attached); and
- a legacy `RA8_BOUNDED_LOOP(x);` in statement position, immediately
  above a loop (the clang-error / GCC-no-op form that #382 removed).

`--selftest` asserts both directions and both clean shapes (a correctly
attached marker, and a function-level `RA8_BOUNDED_LOOP` above a
declaration), so the rule cannot be defanged without the test noticing.

### The checker's own regression test

`--selftest` runs the rules over a synthetic multi-module tree held in the
script, and CI runs it before it trusts the gate's verdict on the real
tree. It guards the two defects this gate has actually shipped:

- **Namesake merging.** A name-keyed symbol table merged distinct
  same-named `static` helpers into one entry, and a module calling its own
  file-local copy was reported for calling another module's `RA8_PRIV`
  symbol. The fixture checks both directions, so the rule cannot be
  "fixed" by defanging it.
- **A rule that cannot fire.** The fixture holds one definition of every
  shape the linkage rule accepts and one of every shape it rejects, so
  deleting the rule fails the test rather than reading as a clean tree.

The vector-table exemption gets both halves deliberately: two identical
handlers, one named by the table and one not. Keyed on table membership
the first passes and the second is reported. Keyed on anything about the
function itself -- a name prefix, a signature -- both would pass, and the
test fails on exactly that mutation.

### Running it

`just quality::local::gate annotations` is the convenience recipe;
`python3 scripts/checks/check_annotations.py` runs the same rules and
exits non-zero on any violation. Its `--help` lists the rest: a quiet
CI mode, a dump of every annotated symbol, the checker's own selftest
against synthetic TUs, and a full prefix/storage audit that can be
emitted as JSON for tooling. None of the audit modes is a suppressing
baseline -- every finding stays visible and fatal.

### Dependency

The script depends on the `libclang` Python wheel, which ships its own
`libclang.{so,dylib}` so no system-wide LLVM install is required:

```sh
just setup-python
```

That recipe creates the repository `.venv` and installs the project-pinned
Python tools without modifying the operating system interpreter.

### Mode

There is no warn-only mode. Every rule is fatal, and the three
informational entries (`ra8_latency_budget_ns`, `ra8_reviewed_by`,
`ra8_register_bank`) record a value rather than assert a property, so
they print but never fail. A gate that reports a known gap without
failing is a gate that hides the gap.

### Pre-commit wiring

The hook at [`scripts/git/pre-commit`](../scripts/git/pre-commit)
invokes the script after the existing static gates (`cite_check`,
`check_world_tags`, etc.) and before the stack-usage aggregator.

### Rule-by-rule notes

| #  | Rule                            | Implementation kind                     |
|----|---------------------------------|-----------------------------------------|
| 1  | `ra8_test_helper`               | caller path must contain `/tests/`      |
| 2  | `ra8_internal`                  | linkage check on definition             |
| 3  | `ra8_priv`                      | `libs/<module>/` path comparison        |
| 4  | `ra8_di_slot:<role>`            | address-of vs direct-call heuristic     |
| 5  | `ra8_nsc_veneer`                | path + range-check scan + section attr  |
| 6  | `ra8_hw_register_access`        | inline + return-type volatile check     |
| 7  | `ra8_nasa_rule_3_ok`            | global malloc/free sweep                |
| 8  | `ra8_mcdc_deactivated:<reason>` | reason-string regex                     |
| 9  | `ra8_max_stack:<bytes>`         | reads `examples/**/build*/**/*.su`      |
| 10 | `ra8_isr_safe`                  | marker; no static check yet (see below) |
| 11 | `ra8_expects_lock:<name>`       | caller owns or propagates the lock      |
| 12 | `ra8_host_friendly`             | rejects calls into MMIO accessors       |
| 13 | `ra8_latency_budget_ns:<n>`     | informational until a WCET pass exists  |
| 14 | `ra8_no_recursion`              | transitive call-closure walk            |
| 15 | `ra8_bounded_loop:<symbol>`     | textual condition-expression check      |
| 16 | `ra8_validates:<n>`             | counts `RA8_CHECK_*` calls in body      |
| 17 | `ra8_owns_resource:<kind>`      | matching `ra8_releases_resource:<kind>` |
| 18 | `ra8_reviewed_by:<name>`        | informational rollup only               |
| 19 | `ra8_register_bank:<periph>`    | informational only (doc-gen feed)       |
| 20 | `ra8_linkage`                   | non-static definition must be justified |

Rules 6, 7, 9 and 13 above are spelled as the macros actually emit them.
Each was once keyed on a different string (`ra8_hw_mmio`,
`ra8_p10_rule3_exception`, `ra8_stack_max`, `ra8_latency_max_ns`), so
each matched nothing while reporting success. `check_rule_keys()` now
proves the table and the header agree on every run.

Rule 10 is a marker only. The transitive ISR-chain walk it used to feed
was keyed on `ra8_isr_handler`, an annotation no macro has ever emitted,
so the walk never ran on a single handler. The dead walk is gone rather
than left looking enforced; deriving the handler set from the vector
tables and requiring `RA8_ISR_SAFE` across each closure is a campaign of
its own, tracked separately.
