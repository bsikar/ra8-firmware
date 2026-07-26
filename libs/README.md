# libs/

The project's standard library: hand-written, first-party C (plus a small
amount of C++ where a vendored dependency requires it) organised into one
directory per library under `libs/`.

**Do not hardcode a count of libraries here.** The number of `ra8_*`
directories grows as the firmware grows, and a stale count ("Five
subdirectories", "N libraries") is exactly the kind of claim that rots the
moment the next library is added. To see the current, authoritative set, list
the tree:

```sh
ls libs/
```

Every entry is an `ra8_*` library (first-party, built under the full
style/safety rules -- see below) except the one vendored tree,
`third_party/`. One `ra8_*` entry, `ra8_fonts/`, carries the prefix but
holds font *data* rather than hand-written code, so it is exempt from the
coding-style rules; both exemptions are set out below.

## The `ra8_` naming convention

`ra8_` is the C symbol namespace for this firmware's own code -- not just a
directory-naming habit. Every public function, type, and enum value a
first-party library exports carries it, per
[`docs/STYLE_GUIDE.md`](../docs/STYLE_GUIDE.md):

| Identifier kind | Convention | Example |
|---|---|---|
| Functions | `ra8_<snake_case>` | `ra8_gpio_output_init` |
| Public/private types | `ra8_<snake_case>_t` | `ra8_err_t`, `ra8_port_pin_t` |
| Enum values | `k_ra8_<scope>_<name>` | `k_ra8_ok`, `k_ra8_pin_led1` |

A directory under `libs/` named `ra8_foo` is the library that owns the
`ra8_foo_*` symbol family (functions, types, and `k_ra8_foo_*` enum values).
This is also why `third_party/` below is an exception rather than just a
differently-named library: it does not participate in this project's own C
namespace at all.

## The two style-rule exemptions

| Dir | What it is | Why it is exempt |
|---|---|---|
| [`third_party/`](third_party/) | Vendored third-party dependencies (SOUP -- Software of Unknown Provenance): TLS, filesystem, RTOS, codec, and ML libraries such as `mbedtls`, `litehtml`, `libwebp`, `threadx`, `netxduo`, `tflite-micro`. Each subdirectory is upstream source, not hand-authored here. | `CLAUDE.md` and the style guide both carve this path out explicitly: the C23/Doxygen/HUM-citation/naming rules apply to first-party code, and vendored code keeps its own upstream conventions. MC/DC re-test is likewise waived here (component justifications live under `docs/SOUP/`). |
| [`ra8_fonts/`](ra8_fonts/) | Generated/curated font assets for the rendering stack (`libs/ra8_reflow`, `libs/ra8_gfx`) -- committed `.ttf` files plus a generated `extern` header for a baked-in glyph subset. It owns the `g_ra8_font_*` blob symbols, hence the `ra8_` prefix. | Not hand-authored: it is font data (and a generator-produced header over that data), so the coding-style rules that apply to hand-written C do not apply here either. |

Everything else under `libs/` is a hand-written `ra8_*` library and is held to
the full rule set in the top-level `CLAUDE.md` and `docs/STYLE_GUIDE.md`
(C23 typed enums, Doxygen coverage, HUM citations for register access, NASA
Power of 10, etc).

## Layering: the ring model

The project organises library code into numbered architectural rings --
lower ring = closer to the metal, higher ring = consumer of lower-ring
services. The full model, including the `{World: S/NS/NSC}` TrustZone tag
that rides alongside the ring tag on every Ring-3+ file, is documented in
[`docs/RING_AND_WORLD.md`](../docs/RING_AND_WORLD.md); read that file for the
authoritative per-file tag format and the enforcement tooling
(`scripts/checks/check_world_tags.py`). The short version, from the bottom
up:

- **`ra8_core`** is the foundation. It is pure C with no hardware
  dependencies, compiles identically on the unit-test host and the ARM
  target, and is the only layer every other library is allowed to depend on.
  This direction is enforced mechanically: `scripts/checks/check_core_layering.py`
  fails the build if anything under `libs/ra8_core` includes a header owned
  by another `libs/*` module.
- **`ra8_hal`** sits directly above it: hand-written register layouts derived
  from the Hardware User's Manual (no code paths, just typed enums and
  inline accessors), plus the peripheral drivers built on top of them. This
  is the only layer allowed to touch MMIO directly.
- **`ra8_nsc`** holds the TrustZone Non-Secure-Callable veneers -- the only
  place in the tree where `__attribute__((cmse_nonsecure_entry))` is
  permitted.
- Everything else -- platform abstraction layers, domain/content libraries,
  UI, board support, system services -- builds on `ra8_core` and `ra8_hal`
  (directly or through a PAL) and never the reverse.

Concretely: code may depend on anything **lower** in this list; never on
anything higher. `ra8_core` depends on nothing else in `libs/`.

## The libraries, grouped by purpose

This grouping is descriptive, not a second registry -- if it drifts from the
tree, `ls libs/` and each library's own file-header `@par Tag` (per
`docs/RING_AND_WORLD.md`) are the source of truth, not this table.

**Foundation and hardware access**

| Dir | What it is |
|---|---|
| [`ra8_core/`](ra8_core/) | Pure-C utilities with no hardware dependencies: `ra8_err`, `ra8_log`, `ra8_check`, `ra8_time`, pin validator, register-protection guards, error/exception handlers. The one library every other library may depend on. |
| [`ra8_hal/`](ra8_hal/) | The Hardware Abstraction Layer: HUM-derived register headers (`inc/ra8d2_*_regs.h`) plus the peripheral drivers built on them (`src/ra8_*.c`). Every documented RA8D2 peripheral driver lives here. |
| [`ra8_mpu/`](ra8_mpu/) | Cortex-M85 Memory Protection Unit configuration helper. |
| [`ra8_nsc/`](ra8_nsc/) | TrustZone Non-Secure-Callable veneer scaffold -- the only place `cmse_nonsecure_entry` is allowed. Bridges `{World: S}` HAL code and `{World: NS}` application code. |
| [`ra8_tz_secure_boot/`](ra8_tz_secure_boot/) | FSP-style TrustZone secure-boot bring-up for the Cortex-M85 (CPU0). |

**Board support**

| Dir | What it is |
|---|---|
| [`ra8_board_ek_ra8d2/`](ra8_board_ek_ra8d2/) | Board-support layer for the Renesas EK-RA8D2 v1 evaluation kit. |
| [`ra8_board_ra8p1/`](ra8_board_ra8p1/) | Board-support layer for the Renesas RA8P1 (R7KA8P1KFLCAC) target board. |

**Platform abstraction layers (PAL)**

Sit above `ra8_hal` and below an external stack or application code, so the
higher-level consumer does not need to know the RA8D2-specific details.

| Dir | What it is |
|---|---|
| [`ra8_net_pal/`](ra8_net_pal/) | Ethernet PAL over `ra8_hal`'s ESWM / ETHA / RMAC drivers, below an external IP stack (NetX Duo today). |
| [`ra8_usb_pal/`](ra8_usb_pal/) | USB device-mode PAL over `ra8_hal`'s `ra8_usb.c`, below an external USB stack. |
| [`ra8_display_pal/`](ra8_display_pal/) | Display PAL spanning both LCD (GLCDC) and e-paper panel backends. |
| [`ra8_io/`](ra8_io/) | Peripheral-agnostic I/O fabric: block-device vtable (SD/SPI, SDHI, OSPI NOR, MRAM, SDRAM, RAM), SPI/I2C controller-bus vtables, streams, a VFS + file operations, pluggable filesystem formats, page/block cache, compression. See [`ra8_io/README.md`](ra8_io/README.md). |
| [`ra8_fs/`](ra8_fs/) | Minimal FAT12/FAT16/FAT32 filesystem adapter (read + write). |
| [`ra8_ftl/`](ra8_ftl/) | Flash Translation Layer -- free overwrite over erase-before-write media. |
| [`ra8_modem_at/`](ra8_modem_at/) | Cellular modem AT command/response driver layered on UART. |
| [`ra8_tls/`](ra8_tls/) | Facade over the vendored Mbed TLS stack. |
| [`ra8_psa_crypto/`](ra8_psa_crypto/) | Application-level PSA Crypto facade over tf-psa-crypto. |

**Memory primitives**

| Dir | What it is |
|---|---|
| [`ra8_mem/`](ra8_mem/) | Zero-heap memory primitives: bump arenas, slabs, glyph atlas, key cache, virtual-memory/tile-cache helpers, `vsource` read-only storage views. |
| [`ra8_cache_store/`](ra8_cache_store/) | Persistent key(CRC32)->blob cache for compiled `.rabook` containers. |

**Content / e-reader domain pipeline**

| Dir | What it is |
|---|---|
| [`ra8_gfx/`](ra8_gfx/) | Software 2D graphics primitives layered on a caller-owned framebuffer. |
| [`ra8_reflow/`](ra8_reflow/) | HTML / CSS reflow + pagination engine for the e-reader. |
| [`ra8_epub/`](ra8_epub/) | EPUB (`.epub`) reader and chapter iterator. |
| [`ra8_book/`](ra8_book/) | Flat, execute-in-place container for a build-time "compiled" e-book. |
| [`ra8_rabook_compile/`](ra8_rabook_compile/) | EPUB -> RABOOK1 compile pipeline (XHTML->DOM parsing, packaging). |
| [`ra8_rabook_import/`](ra8_rabook_import/) | Production adapter binding the on-device import seam to the RABOOK compiler. |
| [`ra8_comic/`](ra8_comic/) | Demand-paged reader for comic-book archives -- CBZ (ZIP) and CBR (RAR). |
| [`ra8_longstrip/`](ra8_longstrip/) | Continuous vertical-scroll (longstrip/manhwa) reading mode. |
| [`ra8_jof/`](ra8_jof/) | JOF band-tile atlas: the display-native normalized image format used by the reader pipeline. |
| [`ra8_webp/`](ra8_webp/) | Heap-free scratch-allocator wrapper around the vendored libwebp decoder. |
| [`ra8_unarch/`](ra8_unarch/) | Bounded, fail-closed archive decoding: XZ/LZMA2, gzip, tar. |
| [`ra8_sdfont/`](ra8_sdfont/) | Loads a TTF/OTF font off a Pmod SD card, self-provisioning if absent. |

**UI**

| Dir | What it is |
|---|---|
| [`ra8_ui/`](ra8_ui/) | Bounded UI interaction core: hit-testing, screen stack, paging. |
| [`ra8_widget/`](ra8_widget/) | Widget tree (toolbar, nav bar, progress bar, status bar, keyboard, book/reflow views) built on `ra8_ui`. |
| [`ra8_box/`](ra8_box/) | Bounded, allocation-free box-model layout for e-reader chrome. |
| [`ra8_keyboard/`](ra8_keyboard/) | On-screen keyboard widget -- iOS-style layers, shift, hit-test. |
| [`ra8_app/`](ra8_app/) | Zero-heap application framework: lifecycle, static registry, launcher. |

**Sensors and external device drivers**

| Dir | What it is |
|---|---|
| [`ra8_lsm6dso/`](ra8_lsm6dso/) | ST LSM6DSO 6-DoF IMU driver (accel + gyro + temperature). |
| [`ra8_sdmmc_spi/`](ra8_sdmmc_spi/) | SD card driver in SPI mode (Pmod-attached cards). |
| [`ra8_touch_cal/`](ra8_touch_cal/) | Resistive/capacitive touch-screen calibration utility. |
| [`ra8_epd_cal/`](ra8_epd_cal/) | Per-device e-paper panel calibration (VCOM) -- record and storage seam. |

**System services**

| Dir | What it is |
|---|---|
| [`ra8_dfu/`](ra8_dfu/) | Controller-agnostic USB-DFU MRAM bootloader core, plus host-side DFU and anti-rollback/root-of-trust helpers. |
| [`ra8_ota/`](ra8_ota/) | OTA firmware-update orchestration. |
| [`ra8_wdt_supervisor/`](ra8_wdt_supervisor/) | Watchdog supervisor with a per-thread check-in registry. |
| [`ra8_batt/`](ra8_batt/) | Bounded, allocation-free low-battery nag policy. |
| [`ra8_power_profile/`](ra8_power_profile/) | Power-profiling helper. |

## Adding a new library

If it has hardware dependencies but nothing new architecturally, it's
probably a HAL driver -- add files under `libs/ra8_hal/`, not a new
top-level directory. A new top-level `libs/ra8_foo/` is warranted when the
code is a genuinely new library boundary: its own `ra8_foo_*` symbol
namespace, its own ring/world tag, and a reason another library shouldn't
just absorb it. When you add one:

1. Pick its ring (and, for Ring 3+, its TrustZone world) per
   [`docs/RING_AND_WORLD.md`](../docs/RING_AND_WORLD.md) and tag every file's
   header accordingly.
2. Give every exported symbol the `ra8_foo_*` / `k_ra8_foo_*` naming this
   file describes above.
3. Add it to the category table above that best fits its purpose (or start a
   new category if none fits) -- but do not add a running total or count
   anywhere in this file.
