# ADR-0011: One USBX CDC echo example, and the first-party CDC layer

## Status

Proposed -- 2026-09-17. The choice between the two options below is
Brighton's to confirm; this ADR records the options, the constraint
that decides between them, and what each one costs.

## Context

`examples/ek_ra8d2/hw_validated/manual/usb_cdc_echo` and
`examples/ek_ra8d2/hw_validated/manual/threadx_usbx_cdc_demo` are the
same application twice. Measured on `dev` at `013631d`:

* Both app `main.c` files
  (`examples/ek_ra8d2/hw_validated/manual/usb_cdc_echo/src/main.c`,
  `examples/ek_ra8d2/hw_validated/manual/threadx_usbx_cdc_demo/src/main.c`)
  are 786 lines and differ in exactly three cosmetic places: the
  `@file` path, the `ra8_log_info` tag (`"USBCDC"` vs `"USBXCDC"`),
  and the boot message.
* Both `linker_script.ld` files are byte-identical.
* Both `CMakeLists.txt` files declare `STACK_BYTES 2200` and
  `USES threadx usbx`; they differ only in the app name.
* `threadx_usbx_cdc_demo` names its ThreadX thread `"usb_cdc_echo"`,
  which is how the copy announces itself.
* The demo's own README already says "`usb_cdc_echo` is the same echo
  test through the same stack".

The duplication is not confined to the two app directories. Both apps
are listed in `usb_enum_apps` in `scripts/emu/smoke_apps.sh`, both
carry rows in `.github/tree-coverage-baseline.txt`,
`.github/tidy-baseline.txt`, `.github/agnostic-register-baseline.txt`
and `.github/suppression-review-ledger.tsv`, both are named in
`docs/SOUP/usbx.md` and `docs/SOUP/threadx.md`, and each has its own
host integration test (`tests/mocks/src/test_app_usb_cdc_echo.c`, 256
lines; `tests/mocks/src/test_app_threadx_usbx_cdc_demo.c`, 343 lines).
The two host tests are the one place the pair genuinely differs.

Two consequences follow, and the second is the reason this is not
simply a tidy-up:

1. A fix to one app does not reach the other. This is the
   parallel-implementation debt `docs/STYLE_GUIDE.md` calls out.
2. Both apps sit under `hw_validated/manual/`, so one bench session
   with one cable is being counted as two hardware-validated apps.
   That makes the duplication a gate-honesty problem as well: the
   validated-app inventory overstates what the bench actually proved.

A third copy of the same device framework exists at
`examples/ek_ra8d2/hw_validated/hil/usb_selftest_cdc`, whose `main.c`
documents its device as "byte-identical to the proven `usb_cdc_echo`
device, retagged PID". That one earns its place: it is the host half
of a self-loop test, not a second echo demo.

## The constraint that decides it

The project has a first-party CDC layer that nothing runs.
`libs/ra8_hal/inc/ra8_usb_cdc.h` (304 lines) and
`libs/ra8_hal/src/ra8_usb_cdc.c` (477 lines) are reachable only from
`tests/usb/src/test_ra8_usb_cdc.c`,
`tests/usb/src/test_ra8_usb_cdc_cov.c` and
`tests/usb/src/test_ra8_usb_cdc_data_stage_cov.c`. No app and no
example in the tree consumes it: every CDC device on the board goes
through USBX's class layer over the `ux_dcd_ra8_usb` bridge. So 781
lines of first-party CDC code are host-tested under mocked MMIO and
have never been exercised on hardware, while
`docs/qualification/SRS.md` REQ-EXT-001 names `ra8_usb_cdc.c` as
implementation evidence for the J11 USB-FS device port.

That is what separates the two options: one of them deletes a
duplicate, the other converts the duplicate into the only hardware
evidence the first-party CDC layer has.

## Options

### Option A -- delete `threadx_usbx_cdc_demo`

Keep `usb_cdc_echo`, which has the clearer name, is the app the other
USB examples cross-reference, and is the name the demo's own ThreadX
thread already carries.

* Removes 786 duplicated lines plus an app directory, a README, a
  linker script, a CMake stub and a 343-line host test.
* Touches `scripts/emu/smoke_apps.sh` (`usb_enum_apps`), four
  `.github` baselines and ledgers, `docs/SOUP/usbx.md`,
  `docs/SOUP/threadx.md` and the `usb_cdc_echo` README cross-link.
* Drops the validated-app count by one, which is the honest
  direction: the bench proved one echo device, not two.
* Leaves `ra8_usb_cdc` with zero consumers, so the SRS evidence gap
  above stays open and needs its own issue.

### Option B -- make the pair prove something

Keep both apps and route `usb_cdc_echo` through the first-party
`ra8_usb_cdc` layer, leaving `threadx_usbx_cdc_demo` on USBX's class
layer. The pair then demonstrates the same CDC contract over two
stacks, which is what the names have always implied.

* Removes the duplication by making the second app different rather
  than absent, and gives `ra8_usb_cdc` its first consumer and its
  first hardware exercise.
* Costs real work: `ra8_usb_cdc`'s chapter-9 and CDC control-transfer
  paths have never run on silicon, so this is a bring-up, not a
  refactor. Expect descriptor, SETUP and endpoint-state bugs.
* Invalidates the existing hardware-validated claim for
  `usb_cdc_echo` until it is re-run on the bench. The app must move
  out of `hw_validated/manual/` (to `hw_pending/`) until that
  session happens, per `docs/HARDWARE_BRINGUP.md`.
* `tests/mocks/src/test_app_usb_cdc_echo.c` has to be rewritten
  against the `ra8_usb_cdc` surface instead of `ra8_usb_pal`.

### Option C -- keep both, document the alias (rejected)

Leave both apps and add a line to each README saying they are the
same test. Rejected: it keeps the double-counted bench evidence,
which is the part that misleads, and a comment has never stopped a
fix landing in one copy only.

## Decision

Pending Brighton's confirmation. The recommendation is **Option B**,
on the strength of the constraint above: Option A spends an app and
still leaves the first-party CDC layer with no consumer, whereas
Option B pays the duplication down and closes the SRS evidence gap in
the same move.

Option B is the larger job, so if it is taken it should land as a
sequence: move `usb_cdc_echo` to `hw_pending/`, port it to
`ra8_usb_cdc`, rewrite its host test, then re-validate on the bench
and move it back. Option A is a single afternoon and is the right
call if the first-party CDC layer is destined for deletion rather
than adoption, which is itself a decision this ADR does not make.

## Consequences

Whichever option is taken:

* The validated-app inventory stops counting one bench result twice,
  and `docs/HIL_SUITE.md` plus the `hw_validated` tree tell the same
  story as the bench log.
* The reference list above is the work item either way: two apps are
  named in six places outside their own directories, so neither
  option is a directory-only change.
* Under Option A the first-party `ra8_usb_cdc` layer needs a separate
  decision (adopt or delete); under Option B that decision is made by
  adoption and REQ-EXT-001's evidence becomes real.

## References

* Issue #728 -- `usb_cdc_echo` and `threadx_usbx_cdc_demo` are the
  same app twice.
* `docs/STYLE_GUIDE.md` -- parallel-implementation debt.
* `docs/HARDWARE_BRINGUP.md` -- what a `hw_validated` claim requires.
* `docs/qualification/SRS.md` -- REQ-EXT-001, which names
  `ra8_usb_cdc.c` as implementation evidence.
* `docs/SOUP/usbx.md`, `docs/SOUP/threadx.md` -- both apps are named
  as USBX/ThreadX consumers.
* ADR-0001 -- the qualification target that makes double-counted
  bench evidence a gate-honesty problem rather than a cosmetic one.
