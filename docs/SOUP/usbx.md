# SOUP Justification: Eclipse USBX

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Eclipse USBX into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Eclipse USBX (formerly Azure RTOS USBX)
- **Version**: 6.5.0 (per `common/core/inc/ux_api.h` USBX_MAJOR / MINOR /
  PATCH macros).
- **Upstream URL**: https://github.com/eclipse-threadx/usbx
- **Local path**: none. Pinned in `build.zig.zon` as package `usbx` (upstream
  tarball at commit `6dc0cf2`, url + Zig content hash) and fetched once into
  the Zig cache.

## Provenance

- **Origin**: Eclipse Foundation, Eclipse ThreadX top-level project
  (donated by Microsoft from Azure RTOS in 2024).
- **License**: MIT (`LICENSE.txt`, "Copyright (c) 2024 - present Microsoft
  Corporation").
- **How it enters the build**: the unmodified upstream release tag
  `v6.5.0.202601_rel`, commit `6dc0cf233d5b7ee6e1a7434581964975f8d8d37b`,
  fetched as a build.zig.zon package. It used to be vendored; every file the
  build used was byte-identical to that commit (the only differences were a
  local `.gitattributes` edit and the line endings of four unused Windows
  `.inf` templates), so the vendored copy was dropped for upstream as is.

## Use case in this firmware

- USB device stack used by
  `examples/ek_ra8d2/hw_validated/manual/threadx_usbx_cdc_demo`,
  `usb_cdc_echo`, `usb_hid_device`, `usb_msc_device`, `usb_host_keyboard`,
  `usb_host_msc_browse`, and the `usb_selftest_*` self-loop suite. The two
  `usb_host_*` apps drive the first-party polled host driver
  (`ra8_usb_host_*` / `ra8_usb_hmsc`), not USBX: no USBX host class is
  compiled, so USBX serves the device side of every app above.
- Class drivers used, all device-side: CDC-ACM, HID, MSC (storage) and
  DFU. `cmake/usbx.cmake` globs these four out of
  `common/usbx_device_classes/src/` and nothing out of
  `common/usbx_host_classes/` (305 vendored files, zero compiled). The
  vendored MSC `INQUIRY` handler is the one TU replaced by a first-party
  override in `port/usbx/`; PIMA still-image files sharing the
  `storage_*` prefix are filtered back out.

<!-- usbx-class-claims: device=cdc_acm,dfu,hid,storage host=none -->

  `scripts/checks/check_usbx_class_claims.py` re-derives that list from
  the globs in `cmake/usbx.cmake` and fails when this record and the
  recipe disagree, so the bullet cannot drift away from the build again.
- Integrity claim category: data-handling (USB transfer payloads,
  enumeration descriptors).

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 and DO-178C Section
12.1.4:

- **Service history**: Express Logic USBX has shipped in industrial USB
  hosts and devices since the mid-2000s.
- **Open-source community process**: Eclipse Foundation governance,
  active issue tracker.
- **Bug tracker review**: Issues at
  https://github.com/eclipse-threadx/usbx/issues reviewed; no open
  advisories affect the CDC/HID/MSC class drivers in use here.
- **Vendor qualification data**: Pre-Eclipse, USBX carried SGS-TUV
  Saar pre-certifications for IEC 61508, IEC 62304, ISO 26262, and EN
  50128; cited for context only.

## Risk mitigation

- The USBX porting layer is `port/usbx/`, a single first-party surface:
  the DCD bridge (`ux_dcd_ra8_usb*.c`) and the HCD bridge
  (`ux_hcd_ra8_usb.c`) both sit on `ra8_usb.h` / `ra8_usb_regs.h`
  directly. `libs/ra8_usb_pal/` is a separate stack-agnostic PAL with no
  USBX consumer; it is not part of this SOUP surface.
- Class-driver use is demo-only except for firmware update:
  `dfu_bootloader` programs code-MRAM through the vendored DFU device
  class under RoT enforcement, so that path is integrity-critical, not a
  demo. The CDC-ACM, HID and MSC paths carry no safety-critical I/O.

## Deviations / patches

None. The build uses the upstream `v6.5.0.202601_rel` tarball exactly as
published; `build.zig.zon` pins it by Zig content hash, so a changed byte fails
the fetch rather than reaching the compile.

While it was vendored, the tree carried one hygiene edit (`.gitattributes`
with its `[attr]` macro blocks dropped, commit `368072a1a`) and, before that,
CRLF copies of the four unused `support/windows_host_files/*.inf` templates.
Neither touched a compiled source, and neither exists now that the copy is
gone.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
