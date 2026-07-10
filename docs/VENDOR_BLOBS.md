# Vendor Blobs (Renesas-Only, Not Shipped)

This document tracks Renesas-distributed binary artifacts that the
`ra8-firmware` tree intentionally does NOT include. They are
license-restricted (Renesas NDA / OEM agreement) and therefore
cannot be redistributed in this open-source
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
- The RSIP-E50D register layer (HUM Ch 52) is the open path forward.

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

- `docs/reference/CHAPTER_MAP.md` -- HUM Ch 52 "Renesas Secure IP
  (RSIP-E50D)", pp 3302-3307
- `docs/reference/ra8d2-hardware-user-manual.pdf`, Ch 52

---

## 2. How to obtain (general guidance)

We deliberately do NOT publish URLs, contact emails, or NDA terms
here -- we have no authoritative public source for any of them and
they change. If you need this blob:

- Contact your Renesas Field Application Engineer (FAE).
- Check the Renesas RA SDK distribution channel for which you hold a
  license.

If unsure which package applies: **consult Renesas FAE / RA SDK
distribution.**

---

## 3. Policy

- This blob MUST NEVER be committed to this repository.
- The suggested `libs/third_party/renesas-rsip-blobs/` path exists as
  a local drop-in location only; add it to your local `.gitignore` if
  you populate it.
- The software-backend / stub-with-warning code paths are the
  open-source baseline and are tested on the host. They are
  intentionally NOT a substitute for the real blob on production
  silicon.
