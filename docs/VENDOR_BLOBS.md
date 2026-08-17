# Vendor Blobs (Renesas-Only, Not Shipped)

This document tracks Renesas-controlled artifacts that the
`ra8-firmware` tree intentionally does NOT include. Each entry below
describes:

1. What the blob is.
2. What functionality it enables.
3. Where in this tree it would be dropped if a developer obtains it.
4. What runtime behavior degrades / fails without it.
5. Which source files / functions are directly affected.

"Not shipped" is not the same as "not obtainable", and the distinction
matters: the one entry below is **publicly downloadable under
BSD-3-Clause**. It is absent because a partial copy is worse than none
(see "Where it would go"), not because a licence forbids it.

---

## 1. RSIP-E50D firmware blobs

### What it is

The vendor-controlled implementation of the Renesas Secure IP block
(RSIP-E50D) that ships inside the RA8D2 silicon (Hardware User's
Manual Ch 52, "Renesas Secure IP (RSIP-E50D)", pp 3302-3307). In
current FSP these are the obfuscated `r_rsip_*.c` procedure sources
under `ra/fsp/src/r_rsip_protected/` (in older FSP releases, the
`hw_sce_*.c` files); they implement the OEM-provisioned key-handling,
key-wrap and key-unwrap state machines that the bare RSIP register
interface alone does not expose. "Blob" here means "opaque,
vendor-controlled implementation", not literally a binary file.

### What it enables

- OEM-style **wrapped / protected key install** (the
  `*_install_plain` and `key_wrap` / `key_unwrap` flows backed by
  Renesas-managed key infrastructure rather than a software MAC).
- Signed key import as offered by the secure-side `key_import`
  veneers (`libs/ra8_secure_app/src/key_import.c`).
- Renesas-managed attestation flows.
- Production-grade RSIP-E50D state machine (raw AES-GCM / SHA / TRNG
  calls do NOT need the blobs and are achievable from datasheet-only
  material).

### Where it would go

Nowhere yet, and that is deliberate. A snapshot of the FSP RSIP-E50D
primitives *was* vendored under `libs/third_party/fsp_blobs/r_sce_AMC/`
from 2026-05-02 until 2026-08, and it was deleted (#614) having never
been built: no `cmake/` recipe referenced it, no first-party call site
named a symbol in it, and 284 of its 287 translation units included
`r_rsip_reg.h` / `r_rsip_util.h`, headers that were never copied into
the tree. It could not have compiled if something had tried.

The lesson is that a *partial* vendoring of this component is dead
weight, not a head start. Whoever brings hardware RSIP up should
vendor a complete, **tag-pinned** snapshot -- the `ra_rsip_e50d`
primitives *plus* the `r_rsip_protected` driver and the util/reg layer
they include -- add a `cmake/rsip_blob.cmake` behind an option that is
OFF by default, and register the component in
`scripts/gen/sbom_registry.py` so the sbom / soup-upstream / osv gates
cover it. Anything less will not compile, and an in-tree copy that
compiles for nobody is what this section already cost the project
once.

### What fails without them

The current `libs/ra8_hal/src/ra8_rsip.c` provides a software backend
(see the `RA8_RSIP_SOFTWARE_BACKEND` compile guard) that emulates the
RSIP-E50D primitive surface for host unit tests and bring-up. It is
NOT a hardware-equivalent RSIP and produces results that are
verifiable but not cryptographically Renesas-signed. Without the
genuine RSIP-E50D firmware blobs:

- OEM-managed key wrap / unwrap and signed key import paths cannot
  be exercised on real silicon with Renesas-signed wrapping. The
  in-tree wrapped-key MAC layout in `ra8_rsip_key_injection.c` is
  explicitly documented as not cryptographically authenticated.
- Renesas-managed attestation cannot run.
- The RSIP-E50D register layer (HUM Ch 52) is the open path forward.

### Affected source files / functions

Public RSIP key-install / wrap surface (declarations in
`libs/ra8_hal/inc/ra8_rsip.h`, implementations in
`libs/ra8_hal/src/ra8_rsip.c`):

- `ra8_rsip_aes128_install_plain`
- `ra8_rsip_aes192_install_plain`
- `ra8_rsip_aes256_install_plain`
- `ra8_rsip_chacha20_install_plain`
- `ra8_rsip_hmac_install_plain`
- `ra8_rsip_key_wrap`
- `ra8_rsip_key_unwrap`

Wrapper / consumer layers:

- `libs/ra8_hal/src/ra8_rsip_protected.c` (drives the
  `*_install_plain` functions for AES, RSA, ECDSA flows)
- `libs/ra8_hal/src/ra8_rsip_key_injection.c` (wrapped-key blob
  pack / validate; the wrapping is explicitly not cryptographic)
- `libs/ra8_secure_app/src/key_import.c` and its
  `key_import_internal.h`
  (Ring-5 secure-side veneers that would call the RSIP wrap path
  in a production build)

A grep for `k_ra8_err_not_supported` / `k_ra8_err_unsupported` in
`libs/ra8_hal/src/ra8_rsip*.c` returns no hits today: the software
backend silently substitutes for the missing blobs. There is no
hard-fail return code at the RSIP layer that signals "blob missing";
the failure mode is "not Renesas-signed".

### Cross-references

- `docs/reference/CHAPTER_MAP.md` -- HUM Ch 52 "Renesas Secure IP
  (RSIP-E50D)", pp 3302-3307
- `docs/reference/ra8d2-hardware-user-manual.pdf`, Ch 52

---

## 2. How to obtain

From the public FSP repository, <https://github.com/renesas/fsp>: the
RSIP-E50D procedure sources live under
`ra/fsp/src/r_rsip_protected/` and carry per-file SPDX
`BSD-3-Clause` notices. No FAE, no NDA, no SDK licence is involved.
Check out a **release tag** rather than a branch head, so the pin is a
thing that cannot move underneath the provenance record.

---

## 3. Policy

- Vendor this component from `renesas/fsp` only -- never from e2 studio
  installer payloads, leaked OEM packages, or third-party
  redistributors.
- Vendor it **whole or not at all**, at a release tag, with a build
  option that compiles it and an `sbom_registry.py` row that gates it.
  A snapshot that nothing builds is accretion; the tree carried one for
  three months and deleted it (#614).
- Every vendored file must appear verbatim. If a patch is required,
  write a separate integration shim and declare the deviation in the
  component's `docs/SOUP/*.md`.
- The software-backend code paths are the open-source baseline and are
  tested on the host. They are intentionally NOT a substitute for the
  real vendor implementation on production silicon.
