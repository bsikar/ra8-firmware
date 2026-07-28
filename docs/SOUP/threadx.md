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
  ThreadX repository copied into `libs/third_party/threadx/`. Upstream
  commit hash unknown (no `.git` directory retained).

## Use case in this firmware

- Cooperative + preemptive RTOS kernel underneath every `examples/ek_ra8d2/threadx_*`
  application, including `threadx_blink`, `threadx_canfd_demo`, `threadx_filex_demo`,
  `threadx_filex_levelx_demo`, `threadx_ipc_demo`,
  `threadx_levelx_demo`, `threadx_mpu_partition_demo`,
  `threadx_netx_tcp_echo`, `threadx_ota_demo`, and `threadx_usbx_cdc_demo`.
- Provides scheduler, synchronization primitives, and timing for FileX,
  NetX Duo, USBX and LevelX (all of which depend on it).
- Integrity claim category: control-flow (scheduler decisions sequence
  every other task in the demos that use it).

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

- Direct ThreadX API calls are confined to the `threadx_*` example
  applications; first-party HAL code under `libs/ra8_hal/` does not depend
  on the kernel.
- Per-app integration tests (`make smoke`) exercise the kernel path for
  every shipped demo.

## Deviations / patches

None. The vendored tree is unmodified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
