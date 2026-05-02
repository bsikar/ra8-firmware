# Vendor Blobs (Renesas-Only, Not Shipped)

This document tracks Renesas-distributed binary artifacts that the
`ra8d2-firmware` tree intentionally does NOT include. They are
license-restricted (Renesas NDA / OEM agreement / BLE-evaluation
license) and therefore cannot be redistributed in this open-source
repository. Each entry below describes:

1. What the blob is.
2. What functionality it enables.
3. Where in this tree it would be dropped if a developer obtains it.
4. What runtime behavior degrades / fails without it.
5. Which source files / functions are directly affected.

If you need any of these, consult your Renesas FAE or the RA SDK
distribution channel you have a license for. This document does not
list URLs or NDA terms because we have no authoritative public source
for them.

---

## 1. RSIP-E50D firmware blobs

### What it is

Signed firmware blobs for the Renesas Secure IP block (RSIP-E50D)
that ships inside the RA8D2 silicon (Hardware User's Manual Ch 52,
"Renesas Secure IP (RSIP-E50D)", pp 3302-3307). In Renesas' FSP tree
these are the `hw_sce_*.c` files; they implement the OEM-provisioned
key-handling, key-wrap and key-unwrap state machines that the bare
RSIP register interface alone does not expose.

### What it enables

- OEM-style **wrapped / protected key install** (the
  `*_install_plain` and `key_wrap` / `key_unwrap` flows backed by
  Renesas-managed key infrastructure rather than a software MAC).
- Signed key import as offered by the secure-side `key_import`
  veneers (`src/secure_app/key_import.{c,h}`).
- Renesas-managed attestation flows.
- Production-grade RSIP-E50D state machine (raw AES-GCM / SHA / TRNG
  calls do NOT need the blobs and are achievable from datasheet-only
  material).

### Where it would go

Suggested drop-in path (gitignored, never committed):

```
libs/third_party/renesas-rsip-blobs/
```

This directory does not exist in the tree. Create it locally if you
have the blobs; do not check it in.

### What fails without them

The current `libs/ra_hal/src/ra_rsip.c` provides a software backend
(see the `RA_RSIP_SOFTWARE_BACKEND` compile guard) that emulates the
RSIP-E50D primitive surface for host unit tests and bring-up. It is
NOT a hardware-equivalent RSIP and produces results that are
verifiable but not cryptographically Renesas-signed. Without the
genuine RSIP-E50D firmware blobs:

- OEM-managed key wrap / unwrap and signed key import paths cannot
  be exercised on real silicon with Renesas-signed wrapping. The
  in-tree wrapped-key MAC layout in `ra_rsip_key_injection.c` is
  explicitly documented as not cryptographically authenticated.
- Renesas-managed attestation cannot run.
- The RSIP-E50D register layer (HUM Ch 52) is the open path forward
  and is tracked in `docs/MISSING.md` section 1.1 and
  `docs/ROADMAP_RA8D2.md` section 1.1.

### Affected source files / functions

Public RSIP key-install / wrap surface (declarations in
`libs/ra_hal/inc/ra_rsip.h`, implementations in
`libs/ra_hal/src/ra_rsip.c`):

- `ra_rsip_aes128_install_plain`
- `ra_rsip_aes192_install_plain`
- `ra_rsip_aes256_install_plain`
- `ra_rsip_chacha20_install_plain`
- `ra_rsip_hmac_install_plain`
- `ra_rsip_key_wrap`
- `ra_rsip_key_unwrap`

Wrapper / consumer layers:

- `libs/ra_hal/src/ra_rsip_protected.c` (drives the
  `*_install_plain` functions for AES, RSA, ECDSA flows)
- `libs/ra_hal/src/ra_rsip_key_injection.c` (wrapped-key blob
  pack / validate; the wrapping is explicitly not cryptographic)
- `src/secure_app/key_import.c` and `src/secure_app/key_import.h`
  (Ring-5 secure-side veneers that would call the RSIP wrap path
  in a production build)

A grep for `k_ra_err_not_supported` / `k_ra_err_unsupported` in
`libs/ra_hal/src/ra_rsip*.c` returns no hits today: the software
backend silently substitutes for the missing blobs. There is no
hard-fail return code at the RSIP layer that signals "blob missing";
the failure mode is "not Renesas-signed".

### Cross-references

- `docs/MISSING.md` section 1.1 (Crypto driver gap)
- `docs/MISSING.md` section 7 (Genuinely impossible without Renesas IP)
- `docs/ROADMAP_RA8D2.md` section 1.1 (Drive the RSIP-E50D registers
  natively)
- `docs/reference/CHAPTER_MAP.md` -- HUM Ch 52 "Renesas Secure IP
  (RSIP-E50D)", pp 3302-3307
- `docs/reference/ra8d2-hardware-user-manual.pdf`, Ch 52

---

## 2. Renesas BLE encrypted patch image

### What it is

An encrypted firmware patch image that the BLE host driver must
download into the BLE radio at boot. Renesas distributes it as a
closed-source binary; in their FSP tree the `r_ble` driver loads it
from a license-restricted blob. In our tree the integration point is
documented but the image itself is absent.

### What it enables

- BLE radio TX and RX. Without the patch loaded, the radio silicon
  does not transmit or receive on real hardware.
- End-to-end Bluetooth functionality of any example app that
  exercises `ra_ble` / `ra_ble_host`.

### Where it would go

Suggested drop-in path (gitignored, never committed):

```
libs/third_party/renesas-ble-patch/
```

This directory does not exist in the tree. The integration shim
already exists and is wired to be a no-op when no image is provided.

### What fails without it

`libs/ra_hal/src/ra_ble_patch.c` is a stub-with-warning. At init time
`ra_ble_patch_load(NULL, 0)` logs a warning via `ra_log_warn` with
the literal text:

> "no BLE patch image configured -- radio TX/RX will not work ..."

`ra_ble_patch_load(image, len)` with a non-NULL pointer also logs a
warning and returns `k_ra_err_not_supported` because there is no
production loader to push the image into the radio. The host-side
unit tests in `tests/test_ra_ble_patch.c` confirm both behaviors:

- `ra_ble_patch_load(NULL, 0)` returns `k_ra_err_not_supported`.
- `ra_ble_patch_load(buf, sizeof(buf))` returns
  `k_ra_err_not_supported` (after passing length / null-pointer
  validation against the `k_ra_ble_patch_min_bytes` /
  `k_ra_ble_patch_max_bytes` enum bounds).

`libs/ra_hal/src/ra_ble.c` documents at line 128 that "Real silicon
receives an encrypted patch blob from Renesas ..." and proceeds in a
no-radio mode.

### Affected source files / functions

- `libs/ra_hal/inc/ra_ble_patch.h` -- public API
  (`ra_ble_patch_load`, `ra_ble_patch_is_loaded`,
  `k_ra_ble_patch_min_bytes`, `k_ra_ble_patch_max_bytes`)
- `libs/ra_hal/src/ra_ble_patch.c` -- stub-with-warning
  implementation
- `libs/ra_hal/src/ra_ble.c` -- BLE host driver consumer
- `tests/test_ra_ble_patch.c` -- host-side tests asserting the
  unsupported return path

### Cross-references

- `docs/MISSING.md` lines 49-55 (BLE driver requires Renesas-supplied
  encrypted patch)
- `docs/MISSING.md` section 7 (Genuinely impossible -- BLE patch
  image)
- The RA8D2 datasheet and HUM in `docs/reference/` describe the BLE
  controller interface but do not document the patch-image format;
  consult the Renesas RA SDK distribution / FAE.

---

## 3. How to obtain (general guidance)

We deliberately do NOT publish URLs, contact emails, or NDA terms
here -- we have no authoritative public source for any of them and
they change. If you need either blob:

- Contact your Renesas Field Application Engineer (FAE).
- Check the Renesas RA SDK distribution channel for which you hold a
  license.
- For the BLE patch specifically, the Bluetooth qualification /
  evaluation package associated with the RA8D2 BLE radio is the
  starting point.

If unsure which package applies: **consult Renesas FAE / RA SDK
distribution.**

---

## 4. Policy

- These blobs MUST NEVER be committed to this repository.
- The suggested `libs/third_party/renesas-rsip-blobs/` and
  `libs/third_party/renesas-ble-patch/` paths exist as local
  drop-in locations only; add them to your local `.gitignore` if
  you populate them.
- The software-backend / stub-with-warning code paths are the
  open-source baseline and are tested on the host. They are
  intentionally NOT a substitute for the real blobs on production
  silicon.
