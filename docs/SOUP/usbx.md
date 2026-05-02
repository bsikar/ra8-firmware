# SOUP Justification: Eclipse USBX

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting Eclipse USBX into this
firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: Eclipse USBX (formerly Azure RTOS USBX)
- **Version**: 6.5.0 (per `common/core/inc/ux_api.h` USBX_MAJOR / MINOR /
  PATCH macros).
- **Upstream URL**: https://github.com/eclipse-threadx/usbx
- **Local path**: `libs/third_party/usbx/`

## Provenance

- **Origin**: Eclipse Foundation, Eclipse ThreadX top-level project
  (donated by Microsoft from Azure RTOS in 2024).
- **License**: MIT (`LICENSE.txt`, "Copyright (c) 2024 - present Microsoft
  Corporation").
- **How it entered our tree**: Vendored snapshot of the upstream Eclipse
  USBX repository. Upstream commit hash unknown.

## Use case in this firmware

- USB host and device stack used by `examples/ek_ra8d2/threadx_usbx_cdc_demo`,
  `usb_cdc_echo`, `usb_hid_device`, `usb_msc_device`, `usb_host_cdc_echo`,
  `usb_host_keyboard`, and `usb_host_msc_browse`.
- Class drivers used: CDC-ACM (device + host), HID (device + host), MSC
  (device + host).
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

- USB controller bring-up is wrapped in `libs/ra_usb_pal/` so the SOUP
  surface is a single porting layer.
- Class-driver use is limited to demos; no safety-critical I/O traverses
  USB in the current firmware.

## Deviations / patches

None. The vendored tree is unmodified.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
