# MC/DC Coverage (DO-178C Level B)

This document describes how `ra8d2-firmware` measures **Modified
Condition/Decision Coverage** (MC/DC) and how to add MC/DC test
vectors for new code.

MC/DC is the structural coverage criterion mandated by **DO-178C Level
B** (Hazardous failure condition) and **DO-178C Table A-7 objective
5**. The infrastructure described here is the foundation for
qualifying portions of this codebase under DO-178C; per-module MC/DC
test vectors are tracked separately.

## What is MC/DC?

For a compound boolean decision with `N` conditions, MC/DC requires:

1. **Decision coverage** -- the decision has evaluated to both `true`
   and `false`.
2. **Condition coverage** -- every condition has evaluated to both
   `true` and `false`.
3. **Independence** -- for every condition `Ci`, there is a pair of
   test cases where `Ci` flips and the *decision outcome* also flips,
   while every other condition is held constant.

The third requirement is what distinguishes MC/DC from plain
"condition coverage". For an `N`-condition decision, MC/DC typically
requires `N + 1` test cases (vs. `2^N` for full multi-condition
coverage).

Statement coverage and branch coverage are necessary but not
sufficient: they cannot detect that a condition was masked by
short-circuit evaluation, and they cannot prove that each condition
*independently* drives the outcome.

## Toolchain

| Tool          | Version             | Role                                |
|---------------|---------------------|-------------------------------------|
| `clang`       | >= 18 (we use 22)   | Source-based MC/DC instrumentation  |
| `llvm-profdata` | matching $CC      | Merge `.profraw` per-test files     |
| `llvm-cov`    | matching $CC        | Render MC/DC report                 |

The flag combination is:

```
-fprofile-instr-generate -fcoverage-mapping -fcoverage-mcdc
```

`-fcoverage-mcdc` is what enables MC/DC bookkeeping. The first two
flags are the standard clang source-based coverage flags that MC/DC
piggy-backs on. See LLVM's
"[Source-based Code Coverage](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html#mc-dc-instrumentation)"
documentation.

If clang >= 18 is not on `$PATH`, the build falls back to gcc 14's
`-fcondition-coverage` (condition coverage, **NOT** MC/DC) and prints
a loud warning. This fallback exists so the script still runs
end-to-end on machines without modern clang; **it is not
DO-178C-compliant** and the report gate is skipped.

## How to run

```sh
make mcdc
```

This wraps `scripts/utils/mcdc_report.sh`, which:

1. Configures `tests/` with `cmake -DRA_MCDC=ON`.
2. Builds every host test with the MC/DC flag trio.
3. Runs each test binary with `LLVM_PROFILE_FILE` set so each emits
   its own `.profraw`.
4. Merges them via `llvm-profdata merge -sparse`.
5. Renders both a verbose per-file dump
   (`build/mcdc-report/mcdc.txt`) and a numeric summary
   (`build/mcdc-report/summary.txt`).
6. Exits non-zero if first-party MC/DC < 100% (override via
   `RA_MCDC_THRESHOLD=NN`).

The existing `make test` and `make coverage` flows are untouched --
MC/DC instrumentation is opt-in.

## How to read the report

`build/mcdc-report/summary.txt` is a `llvm-cov report` table with an
extra **MC/DC Coverage** column (added by `--show-mcdc-summary`). A
typical row looks like:

```
Filename                                  Regions   Missed Regions   Cover     ...   MC/DC Conditions   Missed   Cover
libs/ra_core/src/ra_log.c                 142       8                94.37%    ...   12                 4        66.67%
```

A line is fully MC/DC-covered when **Missed = 0** in the MC/DC
column. The verbose `build/mcdc-report/mcdc.txt` shows, per decision,
the truth-table rows that have and have not been observed.

## Adding MC/DC test vectors -- worked example

`libs/ra_core/src/ra_log.c` contains:

```c
while (value != 0U && i < k_ra_u32_max_digits) {
  buf[i++] = (char)('0' + (char)(value % (uint32_t)k_ra_decimal_base));
  value /= (uint32_t)k_ra_decimal_base;
}
```

This decision has two conditions:

- `C1: value != 0U`
- `C2: i < k_ra_u32_max_digits`

The decision short-circuits on `C1`, so the MC/DC test set must
exercise:

| Test | C1   | C2   | Decision | Notes                                                 |
|------|------|------|----------|-------------------------------------------------------|
| T1   | F    | -    | F        | C1 false; C2 not evaluated (short-circuit)            |
| T2   | T    | F    | F        | Independence pair for C2: with C1=T, C2 flips outcome |
| T3   | T    | T    | T        | Independence pair for C1: with C2=T, C1 flips outcome |

Three tests cover MC/DC for the loop guard:

- **T1**: log a `uint32_t` value of `0`. The function takes the
  early-return path before the loop (line 144), but the loop guard's
  `C1` is still observed `false`.
- **T2**: log a `uint32_t` value with more than
  `k_ra_u32_max_digits` significant decimal digits (impossible for
  `uint32_t` -- but reachable if the buffer were artificially shrunk
  by adjusting `k_ra_u32_max_digits` in a test fixture, OR by calling
  the loop in a wrapper that pre-loads `i`). On real `uint32_t` this
  branch is unreachable, which means the MC/DC obligation translates
  to a **deactivated code** justification under DO-178C Section 6.4.4.3
  -- annotate it in the per-module Software Verification Plan rather
  than chasing impossible coverage.
- **T3**: log any non-zero `uint32_t` (e.g. `123U`). The loop runs
  normally and exits when `value` reaches zero before the index cap.

To add the vectors, drop new assertions into
`tests/test_ra_log.c` and re-run `make mcdc`. The MC/DC column for
`ra_log.c` should advance as soon as the new tests execute the
required truth-table rows.

## Currently exempted code (SOUP)

DO-178C Section 12.1.4 ("Software of Unknown Pedigree") allows
unmodified third-party libraries to be used without source-level MC/DC
provided their behaviour is verified at the integration boundary.

The following directories are excluded from MC/DC instrumentation in
`tests/CMakeLists.txt` and from the `llvm-cov` report:

- `libs/third_party/litehtml/` -- HTML renderer, used by `ra_reflow` v2
- `libs/third_party/gumbo/`    -- HTML5 parser used by litehtml
- `libs/third_party/miniz/`    -- zlib-compatible inflate, used by `ra_epub`
- `libs/third_party/tinyxml2/` -- XML parser, used by `ra_epub`
- `libs/third_party/stb/`      -- TrueType rasteriser, used by `ra_epub`
- Mbed TLS (when integrated)   -- vendored crypto, used by `ra_tls` and `ra_psa_crypto`

These are SOUP under DO-178C and are tracked separately in
`docs/VENDOR_BLOBS.md`.

First-party HAL, PAL, application, and security code under
`libs/ra_*/` (excluding `libs/third_party/`) and `src/` is **in scope**
for MC/DC and the gate.

## Roadmap / known gaps

- The first run of `make mcdc` will show many MC/DC gaps. Closing
  them is tracked per-module; do not attempt a single mass fix.
- The pre-commit hook does **not** yet require MC/DC. It will be added
  once we cross 100% on a stable subset of `libs/ra_core/`.
- `ra_psa_crypto` constant-time paths intentionally evaluate every
  condition (no short-circuit) for side-channel reasons; they will be
  documented as "deactivated short-circuit" rather than gated on MC/DC.

## Measurement history

| Date       | First-party MC/DC % | Notes                                              |
|------------|---------------------|----------------------------------------------------|
| 2026-05-02 | 68.31               | clang-18 in devcontainer; 142/169 host tests pass. |
| 2026-05-02 evening | 70.40       | clang-18 in devcontainer; 149/178 host tests pass. |
| 2026-05-02 late evening | 69.5   | clang-18 in devcontainer; 149/181 host tests pass; --keep-going build with -j2 to avoid linker OOM. |
| 2026-05-02 night | 75.25       | clang-18 in devcontainer; 188/188 host tests pass; +5.68pp from new MC/DC tests added across ra_modem_at, ra_power_profile, ra_psa_crypto, secure_app/key_import, OTA commit veneers. |
| 2026-05-02 late night | 77.26 | clang-18 in devcontainer; 187/187 host tests pass; +2.01pp from new MC/DC vectors added in ra_etha (set_queue_depth, descriptor_ring_init, set_vlan_tag), ra_vin (set_uds_scale, set_framebuffers alignment, capture_start geometry), ra_rsip (rsa_sign / rsa_verify size selectors, hash_validate shake bypass). |
| 2026-05-02 closing | 79.77 | clang-18 in devcontainer; 187/187 host tests pass; +2.51pp from new MC/DC vectors across 9 modules (ra_etha get_queue_level / set_max_frame_size / configure_cut_through / configure_cbs / get_cbs_state, ra_vin set_uds_passband / set_data_mode / set_csi_input / set_window, ra_rsip hash msg-null and AEAD AAD pairs, ra_psa_crypto sim AEAD scratch overflow C2, ra_flash status OR pairs, ra_ble send_acl_data + inject_rx, ra_i3c write+read len/ptr pairs, ra_touch_cal apply+run screen_height==0, ra_ota priv_hex_nibble C1=F vectors). Also: regenerated `docs/MCDC_GAPS.{csv,md}` from live llvm-cov report via new `scripts/utils/regen_mcdc_gaps.py`. |
