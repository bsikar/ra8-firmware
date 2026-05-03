# Renesas FSP Vendor Blobs

This directory is the drop-in location for the two binary artefacts
that the `ra8d2-firmware` tree intentionally cannot ship under MIT
licensing: the **RSIP-E50D firmware image** and the **Renesas BLE
controller patch image**.

Per the decision recorded on 2026-05-02, the project pulls these
blobs **directly from `renesas/fsp`** as Software Of Unknown
Provenance (SOUP) per IEC 61508-3 sec. 7.4.2.12 and DO-178C
sec. 12.1.4. Each blob is qualified individually under
[`docs/SOUP/`](../../../docs/SOUP/) (see the index).

## What lives here

| Blob                              | Source path in `renesas/fsp`                                | Local path (after vendoring)                       | SOUP doc                                             |
| --------------------------------- | ----------------------------------------------------------- | -------------------------------------------------- | ---------------------------------------------------- |
| RSIP-E50D firmware (`r_sce_AMC`)  | `ra/fsp/src/r_sce/crypto_procedures_protected/src/sce9/`    | `libs/third_party/fsp_blobs/r_sce_AMC/`            | [`docs/SOUP/r_sce_AMC_firmware.md`](../../../docs/SOUP/r_sce_AMC_firmware.md) |
| BLE controller patch image        | `ra/fsp/src/r_ble/r_ble_api/`                               | `libs/third_party/fsp_blobs/ble_patch/`            | [`docs/SOUP/ble_patch_image.md`](../../../docs/SOUP/ble_patch_image.md) |

The two SOUP entries above carry the full provenance, license,
qualification basis, integration notes, and last-review stamp. This
README only points at them.

## Why "as SOUP" and not "rewrite from datasheet"

- The RSIP-E50D firmware is **not documented** at the cleartext register
  level in the Hardware User's Manual chapter 52. It is a Renesas-managed
  state machine accessed through opaque mailbox writes; only the
  pre-built blob from `renesas/fsp` knows the protocol. A from-scratch
  rewrite is not feasible without an NDA-grade specification.
- The BLE controller patch is an **encrypted firmware image** uploaded to
  the radio at boot. It is opaque by design (encrypted to a key Renesas
  controls), and there is no clean-room path.

Both blobs are therefore admitted under the SOUP exemption recorded in
[`CLAUDE.md`](../../../CLAUDE.md) "IEC 61508 SIL 3 / DO-178C Level B
Qualification (This Project)" -- "Exempt Code" subsection -- with the
per-component justification linked above.

## Upstream identity

- **Upstream repository**: <https://github.com/renesas/fsp>.
- **Upstream license**: per the FSP top-level `LICENSE.md` (per-file
  notices govern; the binary blobs ship under the standard FSP terms).
- **FSP commit pinned for this project**: TBD -- to be recorded in each
  SOUP doc on first vendoring. Until then both `r_sce_AMC/` and
  `ble_patch/` subdirectories are intentionally absent from the tree.

When a contributor first vendors either blob:

1. Copy the relevant directory verbatim from a tagged FSP release into
   the local path listed above.
2. Update the matching SOUP doc with the FSP release tag, the upstream
   commit SHA, and the file SHA-256 of every blob copied in.
3. Bump the "Last review date" in the SOUP doc.
4. Commit the binary tree along with the doc update in a single change.

## Procurement plan (deferred -- no network in current commit)

The actual binary copy is **not** performed in the same commit as this
README. The procurement plan is:

- Step 1 (this commit): land directory + SOUP entries + procurement
  procedure. No network access required.
- Step 2 (a later commit, when network and a tagged FSP release are
  available): clone `renesas/fsp` at a pinned tag, copy the two blob
  trees into this directory, fill in the SHA-256 hashes and FSP commit
  SHA in the SOUP docs.

Until step 2 lands, every consumer of the affected functionality
(`libs/ra_hal/src/ra_rsip*.c`, `libs/ra_hal/src/ra_ble_patch.c`)
continues to use its existing software-only stub and reports
`k_ra_err_not_supported` at runtime. The behaviour matches what
[`docs/VENDOR_BLOBS.md`](../../../docs/VENDOR_BLOBS.md) already
documents.

## Policy

- These blobs **must be vendored from `renesas/fsp` only** -- not from
  e2 studio installer payloads, not from leaked OEM packages, not from
  any third-party redistributor.
- Every vendored file must appear verbatim with no in-tree
  modifications. If a patch is required, write a separate
  `libs/ra_hal/src/<name>_patch.c` integration shim and document it in
  the matching SOUP doc.
- This project does **not** redistribute these blobs in tagged releases
  unless the FSP license terms explicitly permit it. Consult the FSP
  `LICENSE.md` before publishing any release artefact that includes
  this directory.

## Cross-references

- [`docs/SOUP/README.md`](../../../docs/SOUP/README.md) -- SOUP
  catalogue index.
- [`docs/SOUP/r_sce_AMC_firmware.md`](../../../docs/SOUP/r_sce_AMC_firmware.md) -- RSIP-E50D blob qualification.
- [`docs/SOUP/ble_patch_image.md`](../../../docs/SOUP/ble_patch_image.md) -- BLE controller patch qualification.
- [`docs/VENDOR_BLOBS.md`](../../../docs/VENDOR_BLOBS.md) -- the
  developer-facing description of what each blob does and what fails
  without it.
- [`docs/QUALIFICATION_ROADMAP.md`](../../../docs/QUALIFICATION_ROADMAP.md)
  Section 6 -- the "vendor-blob blockers" entry, marked CLOSED with
  reference to this directory.
