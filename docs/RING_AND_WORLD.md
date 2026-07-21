# Ring and World tagging

Every source file in `libs/ra8_hal/`, `libs/ra8_*_pal/`, `libs/ra8_nsc/`,
and `src/` carries a two-part tag in its file-level Doxygen header:

```c
/**
 * @file ra8_acmphs.c
 * @brief High-Speed Analog Comparator driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 * ...
 */
```

The pair is enforced by `scripts/checks/check_world_tags.py`, which runs
in the pre-commit hook and refuses commits whose Ring-3+ files are
missing either tag (or carry a tag that's inconsistent with where the
file lives).

## `[Ring N / LAYER]` -- architectural ring

The project organises code into numbered rings, similar in spirit to
OS privilege rings but used here for **layered architecture**. Lower
ring = closer to the metal; higher ring = consumer of lower-ring
services.

| Ring | Layer | Where it lives | What it does |
|---:|---|---|---|
| **0** | BSP | `examples/<app>/{vector_table,system_init,secure_exception,trustzone_init}.c` + `examples/<app>/linker_script.ld` | Vector table, SystemInit, linker script. CPU-state setup before C runtime is live. Each app carries its own copy so two apps may diverge (different vector tables, different memory layouts). |
| **1** | Core fundamentals | `libs/ra8_core/` | Pure-C utilities with no hardware dependencies (err codes, log, time, pin validator, register-protection helpers). Compiles identically on host and target. |
| **2** | Register layer | `libs/ra8_hal/inc/ra8d2_*_regs.h` | Hand-written register layouts derived from the HUM. No code paths -- just typed enums + accessor inline functions. |
| **3** | HAL drivers | `libs/ra8_hal/src/ra8_*.c` | Hardware Abstraction Layer. Programmes peripherals via Ring-2 register headers. The vast majority of driver code lives here. |
| **4** | NSC veneers | `libs/ra8_nsc/` | TrustZone Non-Secure-Callable veneers. Bridges between `{World: S}` and `{World: NS}` -- the only place where `__attribute__((cmse_nonsecure_entry))` is allowed. |
| **5** | Secure app | `src/secure_app/` | Secure-side application code (key vault, secure-boot trust anchor). Sits above the HAL but below the NS-callable veneer surface. |
| **6** | Application | `examples/<tier>/.../<app>/main.c` (e.g. `.../smoke/blink/`, `.../smoke/blink_hal/`), test mocks | The firmware "user code" -- whatever drives the HAL to do something useful. The blink demo, board-bringup smoke tests, and unit-test harnesses all live at Ring 6. |

The numbering doesn't have to be contiguous; it's a coordinate system,
not a rule book. A Ring 3 driver can include Ring 1 / Ring 2 headers
freely. A Ring 6 application uses Ring 3 drivers via their public
headers. Crossing **down** is fine; crossing **up** -- a Ring 3 driver
calling Ring 6 application code -- is a layering violation.

## `{World: X}` -- TrustZone world

The RA8D2 implements the Armv8-M Security Extension (TrustZone-M),
which partitions execution into two **worlds**: Secure (S) and
Non-Secure (NS). The SAU (Security Attribution Unit) plus the
peripheral `xxxSAR` registers determine which world owns each address.

The tag declares the world a file *expects to run in*:

| Tag | Meaning | Where allowed |
|---|---|---|
| `{World: S}` | Runs in the Secure world. Has full access to all peripherals and memory. Cannot be called directly from NS code -- only via NSC veneers. | `libs/ra8_hal/`, `libs/ra8_*_pal/` (when serving the secure side), `src/secure_app/`, per-app boot files (`examples/<app>/{vector_table,system_init,...}.c`), secure-side apps. |
| `{World: NS}` | Runs in the Non-Secure world. Reaches into Secure code only through `__cmse_nonsecure_entry` veneers in `libs/ra8_nsc/`. | `libs/ra8_hal/` driver TUs that the SAU partition keeps NS, NS-side apps. |
| `{World: NSC}` | Non-Secure-Callable veneer code. The bridge between worlds. The `.gnu.sgstubs` section lands here at link time. | **Only** under `libs/ra8_nsc/`. |
| `{World: MIXED}` | A file that legitimately straddles both worlds (rare -- typically a header consumed by both sides). | Header files only, sparingly. |

Three concrete rules the linter enforces:

1. **NSC veneers stay in `libs/ra8_nsc/`.** Any file outside that
   directory that declares a function with
   `__attribute__((cmse_nonsecure_entry))` is rejected.
2. **Ring 1 / Ring 2 files never carry a World tag.** They have no
   peripheral access and run identically in either world; tagging
   them would lie about where the security boundary sits.
3. **Ring 3+ files require *both* tags.** A driver without a Ring tag
   cannot be placed in the build; a driver without a World tag cannot
   be linked into the secure / non-secure partition cleanly.

## Examples

```c
/* libs/ra8_hal/src/ra8_glcdc.c */
/**
 * @file ra8_glcdc.c
 * @brief Graphics LCD Controller driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 * ...
 */
```
GLCDC programming runs in the NS partition once SAU is up because the
display layer is NS-attributed by default. The HAL TU itself is built
to land in the NS image.

```c
/* src/secure_app/key_vault.h */
/**
 * @file key_vault.h
 * @brief Secure-only symmetric key store
 *
 * @par Tag
 * [Ring 5 / SECAPP] {World: S}
 * ...
 */
```
Holds 256-bit symmetric keys in a static array that's unreachable from
NS after the SAU partition is enabled. Strictly Secure.

```c
/* libs/ra8_nsc/src/ra8_nsc_key_vault.c */
/**
 * @file ra8_nsc_key_vault.c
 * @brief NSC veneer for ra8_key_vault_sha256_xor_challenge
 *
 * @par Tag
 * [Ring 4 / NSC] {World: NSC}
 * ...
 */
```
Carries `cmse_nonsecure_entry` attributes; lives under `libs/ra8_nsc/`
so the linker can place it in `.gnu.sgstubs`.

```c
/* examples/ek_ra8d2/hw_validated/hil/blink/main.c */
/**
 * @file main.c
 * @brief Blink-LED smoke test
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 * ...
 */
```
The Cortex-M85 boots in Secure mode on RA8D2; the blink demo never
transitions to NS so it stays Secure-side.

## Why bother

Without these tags, a driver author can accidentally:

- Reference a Secure peripheral from an NS-attributed file (and watch
  the SAU fault at runtime).
- Forget to put their NSC veneer under `libs/ra8_nsc/` (and have it
  silently land in the NS image with no SG instruction guarding the
  entry point).
- Write a Ring-1 utility that pulls in a HAL header, dragging
  peripheral dependencies into code that's supposed to be host-clean.

The pre-commit hook catches all three cases at the file-header level
before the diff has a chance to hide the mistake.

## Adding a new file

When you add a `.c` or `.h` under `libs/ra8_hal/`, `libs/ra8_*_pal/`,
`libs/ra8_nsc/`, `src/secure_app/`, or a per-app dir (`examples/<app>/`):

1. Pick the ring it belongs to using the table above.
2. Pick the world it runs in (almost always `S` for Ring 3 drivers
   today; `NS` only after the SAU partition lands; `NSC` only inside
   `libs/ra8_nsc/`).
3. Drop the tag into the file header, immediately after `@brief` and
   before `@details`:
   ```c
   /**
    * @file my_driver.c
    * @brief One-line summary
    *
    * @par Tag
    * [Ring 3 / HAL] {World: S}
    *
    * @details
    * ...
    */
   ```
4. Run `python3 scripts/checks/check_world_tags.py --strict` to verify.

That's the whole system.
