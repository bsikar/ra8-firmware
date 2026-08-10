# SOUP Justification: Eclipse ThreadX

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Eclipse ThreadX into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Eclipse ThreadX (formerly Azure RTOS ThreadX, originally
  Express Logic ThreadX)
- **Version**: 6.5.0 (per `common/inc/tx_api.h` THREADX_MAJOR / MINOR /
  PATCH macros). The `CHANGELOG.md` head also references release line 6.4.x;
  the in-tree headers are 6.5.0.
- **Upstream URL**: https://github.com/eclipse-threadx/threadx
- **Local path**: `libs/third_party/threadx/`

## Provenance

- **Origin**: Eclipse Foundation, Eclipse ThreadX top-level project
  (donated by Microsoft from the former Azure RTOS family in 2024).
- **License**: MIT (`LICENSE.txt`, "Copyright (c) 2024 - present Microsoft
  Corporation").
- **How it entered our tree**: Vendored snapshot of the upstream Eclipse
  ThreadX repository copied into `libs/third_party/threadx/`. Resolved
  (#548) to release tag `v6.5.0.202601_rel`, commit
  `3726d7906b4808bfec7855fc088e073199df9120`: 4757 of the 4758 vendored
  files are byte-identical to it, the exception being the `.gitattributes`
  edit recorded under "Deviations / patches" below. The vendored subset
  drops 3611 upstream files (ports we do not build, test suites, docs).

## Use case in this firmware

- Cooperative + preemptive RTOS kernel. This is the kernel substrate for the
  vendored-middleware world, not a demo-corner component: **45 example
  applications** declare `USES ... threadx`, **39 of them under
  `examples/ek_ra8d2/hw_validated/`** (measured at `e0ac93111`). They include
  all 26 applications that link USBX, all five ESP32-C6 Wi-Fi applications and
  the DFU bootloader family. Beyond the examples, the non-secure e-reader
  product image enters the kernel directly (`src/app/ns_main.c`,
  `tx_kernel_enter()`), and the first-party `libs/ra8_wdt_supervisor/` creates
  a ThreadX thread.
- The applications actually NAMED `threadx_*` are ten in the supported tiers:
  `threadx_blink`, `threadx_canfd_demo`, `threadx_fs_demo`,
  `threadx_fs_levelx_demo`, `threadx_ipc_demo`, `threadx_levelx_demo`,
  `threadx_mpu_partition_demo`, `threadx_netx_tcp_echo` and
  `threadx_systick_retune` under `hw_validated/hil/`, plus
  `threadx_usbx_cdc_demo` under `hw_validated/manual/` (three more sit under
  `examples/_unsupported/`). `threadx_ota_demo` was deleted in `d38587e80` and
  no longer exists anywhere in the tree.
- Provides scheduler, synchronization primitives, and timing for
  NetX Duo and USBX. LevelX is NOT in that set unconditionally:
  `cmake/levelx_standalone.cmake` exists precisely to build it with
  `LX_STANDALONE_ENABLE` and no ThreadX dependency, which is the mode
  `libs/ra8_cache_store/` uses.
- Integrity claim category: control-flow (scheduler decisions sequence
  every other task in the images that use it).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 (proven-in-use route) and
DO-178C Section 12.1.4 (previously developed software):

- **Service history**: Express Logic ThreadX has shipped in billions of
  embedded devices since the late 1990s across automotive, industrial,
  and medical domains.
- **Open-source community process**: Now governed by the Eclipse
  Foundation development process, including Eclipse Quality Assurance
  reviews and the project's `SECURITY.md` disclosure policy.
- **Vendor qualification data**: Pre-Eclipse, ThreadX held formal
  pre-certifications for IEC 61508 EIL 4, IEC 62304 Class C, ISO 26262
  ASIL D, and EN 50128 SW-SIL 4 from SGS-TUV Saar; that evidence is
  referenced for context only and is not re-asserted by Eclipse.
- **Bug tracker review**: Issues at
  https://github.com/eclipse-threadx/threadx/issues reviewed; no open
  advisories at vendor-in date affect Cortex-M85 ports used here.

## Risk mitigation

- Direct ThreadX API calls are **not** confined to the `threadx_*` examples.
  The first-party callers are `libs/ra8_wdt_supervisor/` (`tx_thread_create`),
  `src/app/ns_main.c` (two `tx_thread_create` plus `tx_kernel_enter`, the
  e-reader NS product image), `libs/ra8_core/src/ra8_time.c` (a weak-linked
  `_tx_timer_interrupt` tick hook that resolves to nothing when the kernel is
  absent), and the ports `port/threadx/`, `port/usbx/`, `port/netxduo/`,
  `port/nimble/` and `port/esp-hosted/`. That is the audited blast radius.
- `libs/ra8_hal/` does not depend on the kernel (zero `tx_*` API references),
  so the HAL is built and unit-tested off-target without ThreadX present.
- The kernel path is exercised by the emulator gates, not by a `make smoke`
  target -- no such target exists. `scripts/emu/smoke.sh` behind the
  `emulator-smoke` gate boots the example applications in `ra8_emulator`, the
  `emulator-matrix` gate does the same across the full breadth ratcheted
  downward, and `scripts/hil/all.sh` then runs the demos on real silicon.

## Deviations / patches

One file, `.gitattributes`, and it is a repository-hygiene edit rather than a
change to any shipped source. Commit `368072a1a` dropped its two `[attr]`
attribute-macro blocks (`our-c-style`, `generated`) from all five vendored
Eclipse ThreadX trees: git honours `[attr]` definitions only in the top-level
`.gitattributes` and printed a "not allowed" warning for each on EVERY git
operation. The macro *uses* left behind reference undefined attributes, which
git ignores silently, so no vendored file's checkout behaviour changes.

Declared in `scripts/gen/sbom_registry.py` as `patched_files` and pinned by
content in `docs/sbom/upstream/threadx.manifest`; every other file in this
component is verified byte-identical to the upstream pin on each CI run
(#548).

The edit is from 2026-07-13 and went unrecorded here until #548 found it two
weeks later, which is the point: "the vendored tree is unmodified" was prose,
and prose does not notice a tree-wide sweep reaching into `libs/third_party/`.

## Last review date

- Reviewed: 2026-05-02
- Use case + risk mitigation re-verified against the tree and corrected
  (#624): 2026-08-04. The kernel's footprint was understated by roughly 4x,
  a deleted application was still cited, and the named verification hook
  (`make smoke`) never existed.
- Expected re-review by: 2027-05-02
