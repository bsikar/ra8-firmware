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
- **Upstream license**: per the FSP top-level `LICENSE.md`. Per-file
  SPDX notices govern; the redistributable RSIP-E50D source-tree
  "blob" ships as `BSD-3-Clause`. The BLE controller patch image is
  NOT in the public BSD-3-Clause distribution -- see "Vendor-in
  status" below.
- **FSP commit pinned for this project**:
  `40bbaa11b1a1b87e0ee0675e401aea6351f90d14` (renesas/fsp `master`
  HEAD on 2026-05-02; shallow clone, no tag pinned -- next
  re-review will pin to the most recent tag).
- A copy of the upstream `LICENSE.md` is mirrored alongside each
  vendored tree as `UPSTREAM_LICENSE.md` so the license travels
  with the blob even if FSP becomes unreachable.

## Vendor-in status

| Blob              | Status              | Notes                                                                                                                                |
| ----------------- | ------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| `r_sce_AMC/`      | VENDORED            | 314 files, ~3.8 MB. Mirrors `ra/fsp/src/r_rsip_protected/.../{primitive,private}/ra_rsip_e50d/` from FSP.                              |
| `ble_patch/`      | BLOCKED: license    | Not present in public BSD-3-Clause FSP. End users must stage from a Renesas-installer payload locally; see SOUP doc below.            |

### `r_sce_AMC/` -- VENDORED

The FSP path
`ra/fsp/src/r_rsip_protected/crypto_procedures_protected/src/rsip/ra/{primitive,private}/ra_rsip_e50d`
(plus the small `private/ra_rsip_e5xx/` shim it depends on) is
mirrored verbatim under
`libs/third_party/fsp_blobs/r_sce_AMC/ra/fsp/src/...`. Every file is
SPDX-`BSD-3-Clause`. The aggregate SHA-256 of the tree (sorted
per-file SHA-256s, hashed) is in
[`docs/SOUP/r_sce_AMC_firmware.md`](../../../docs/SOUP/r_sce_AMC_firmware.md).

Note on terminology: the artefacts as they exist in upstream public
FSP are obfuscated `r_rsip_*.c` source files, NOT a separate binary
`.dat` payload. The term "blob" in this project means
"opaque, vendor-controlled implementation" rather than literally a
binary file.

### `ble_patch/` -- BLOCKED

The Renesas BLE controller encrypted patch image is not part of the
public BSD-3-Clause FSP repository. The public `r_ble/` subtree
contains only `r_ble.c` (host-side RTOS-glue API) and the
`rm_ble_abs_spp/r_ble_spp_api.c` `R_BLE_VS_UpdateModuleFirmware()`
loader -- the payload itself ships outside the public source tree
under a separate Renesas Software License Agreement.

End-user fetch procedure (for downstream consumers who need real BLE
radio bring-up, mirrored from
[`docs/SOUP/ble_patch_image.md`](../../../docs/SOUP/ble_patch_image.md)):

1. Obtain a Renesas FSP installer or e2 studio image directly from
   Renesas; accept the accompanying Renesas Software License
   Agreement.
2. Locate the BLE patch payload in the installer's `r_ble_*` subtree.
3. Stage it locally at `libs/third_party/fsp_blobs/ble_patch/`
   BEFORE building any example app that depends on the BLE radio.
4. Do NOT commit it; the file is gitignored implicitly via the
   `BLOCKED: license` status in this README.

The build system does NOT fetch this blob automatically. While the
blob is absent, `libs/ra_hal/src/ra_ble_patch.c` returns
`k_ra_err_not_supported` at runtime; host unit tests under
`tests/test_ra_ble_*.c` continue to compile and run unaffected.

## Re-vendoring procedure (when bumping to a new FSP commit)

1. `git clone --depth 1 https://github.com/renesas/fsp.git /tmp/fsp`
   (or check out a tagged release).
2. `rm -rf libs/third_party/fsp_blobs/r_sce_AMC/ra/`.
3. `cp -R /tmp/fsp/ra/fsp/src/r_rsip_protected/.../ra_rsip_e50d`
   into the matching local path; same for the `ra_rsip_e5xx/`
   private shim. Copy the new `LICENSE.md` over `UPSTREAM_LICENSE.md`.
4. Recompute the aggregate SHA-256 (see SOUP doc for the recipe).
5. Update the FSP commit SHA + aggregate hash + last-review date in
   both this README and `docs/SOUP/r_sce_AMC_firmware.md`.
6. Commit the binary tree and the doc update in a single change.

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
