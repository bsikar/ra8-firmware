# SOUP Justification: Renesas BLE Controller Patch Image

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting the Renesas RA8D2 BLE
controller patch image into this firmware as Software Of Unknown
Provenance (SOUP).

## Component identity

- **Name**: Renesas BLE controller encrypted firmware patch image.
- **Renesas reference name**: BLE patch / BLE firmware image, shipped
  inside `r_ble` under FSP.
- **Version**: tracked by FSP release tag (TBD on first vendoring).
- **Upstream URL**: <https://github.com/renesas/fsp>, subtree
  `ra/fsp/src/r_ble/`.
- **Local path (after vendoring)**: `libs/third_party/fsp_blobs/ble_patch/`.

## Provenance

- **Origin**: Renesas Electronics Corporation, distributed as part of
  the **Renesas Flexible Software Package (FSP)** under the FSP
  top-level `LICENSE.md`. The source-of-truth distribution channel
  for this project is the public `renesas/fsp` GitHub repository.
- **License**: per FSP `LICENSE.md`. The patch payload itself is an
  encrypted firmware image; the FSP-side loader is licensed under
  the same terms as the rest of the `r_ble` driver.
- **How it would enter our tree**: vendored snapshot copied directly
  from a tagged FSP release into
  `libs/third_party/fsp_blobs/ble_patch/`. The exact FSP release tag
  and the file SHA-256 hashes are recorded in this doc on first
  vendoring (currently TBD -- see `Procurement status` below).

## Use case in this firmware

- Loaded into the on-chip BLE radio at boot by
  `libs/ra_hal/src/ra_ble_patch.c`. Without it, the radio silicon
  does not transmit or receive on real hardware (see
  `docs/VENDOR_BLOBS.md` Section 2 for the full failure-mode
  description).
- Required by every example app under `examples/ek_ra8d2/threadx_*`
  that exercises `ra_ble` / `ra_ble_host` / NimBLE.
- Integrity-claim category: hardware-driver firmware. Failure of the
  blob would manifest as link-layer breakage (no advertisements, no
  scan results), detectable by the host-side BLE smoke tests.

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 (proven-in-use route)
and DO-178C Section 12.1.4 (previously developed software):

- **Service history**: the Renesas BLE patch flow has shipped in the
  RA4W1 / RA6E2 / RE01 generations and is re-used essentially
  unchanged on the RA8D2 BLE radio.
- **Black-box hardware-IP treatment**: per IEC 61508-3 sec. 7.4.2.12
  the blob is admitted as a pre-developed component. Source-level
  re-audit is impossible (the payload is encrypted to a
  Renesas-controlled key); compensating controls are (a) the
  host-side BLE host-stack tests under `tests/test_ra_ble_*.c` and
  (b) the Phase-6 hardware-in-the-loop smoke described in
  `docs/HIL_DEVELOPER_WORKFLOW.md`.
- **Bug tracker review**: Renesas FSP issue tracker
  <https://github.com/renesas/fsp/issues> is reviewed at the
  re-review cadence below. Bluetooth qualification status of the RA8D2
  BLE radio is owned by Renesas; this project does not file or claim
  Bluetooth-SIG QDIDs.

## Risk mitigation

- The blob is invoked **only** by `libs/ra_hal/src/ra_ble_patch.c` and
  is opaque to the rest of the firmware. The NimBLE host stack (SOUP
  entry `nimble.md`) sees only the post-load BLE controller surface.
- The stub-with-warning code path (`ra_ble_patch_load(NULL, 0)` ->
  `k_ra_err_not_supported`) remains in the tree so host unit tests
  continue to compile and run without the blob.
- Downstream consumers who require Bluetooth-SIG qualification or
  certified evidence for the radio firmware must obtain Renesas's
  own qualification artefacts under the appropriate evaluation
  agreement -- this project does not re-derive them.

## Deviations / patches

None. The blob is loaded verbatim. Any future workaround for a known
patch-image bug must live in `libs/ra_hal/src/ra_ble_patch.c` and
must not modify the vendored payload.

## Procurement status

- **Vendored**: NO -- `BLOCKED: not redistributed in public FSP`
  (as of 2026-05-02).
- **Investigation result** (renesas/fsp commit
  `40bbaa11b1a1b87e0ee0675e401aea6351f90d14`, master HEAD on
  2026-05-02): the public BSD-3-Clause FSP repository under
  `ra/fsp/src/r_ble/` contains exactly **one file**, `r_ble.c`
  (480 lines, BSD-3-Clause), which provides the host-side
  RTOS-glue API. It does NOT contain any controller patch image
  (`.dat`, `.bin`, `lc_pwr_pic.*`, or similar). The companion
  `rm_ble_abs_spp/r_ble_spp_api.c` defines
  `R_BLE_VS_UpdateModuleFirmware()` -- a function that *uploads* a
  patch image -- but the payload itself is not part of the
  redistributable source tree.
- **Why**: the BLE controller patch is an encrypted firmware image
  signed/keyed to Renesas-controlled material and ships **outside**
  the public BSD-3-Clause FSP repository. End users obtain it via
  Renesas's own distribution channels (e2 studio installer payloads
  or a Renesas Software License Agreement download), neither of
  which is admissible into this repo per the
  `libs/third_party/fsp_blobs/README.md` "Policy" section
  ("These blobs MUST be vendored from `renesas/fsp` only").
- **End-user fetch procedure** (for downstream consumers who need
  real BLE radio bring-up): obtain a Renesas FSP installer or e2
  studio image directly from Renesas, accept the accompanying
  Renesas Software License Agreement, locate the BLE patch payload
  in the installer's `r_ble_*` subtree, and stage it locally at
  `libs/third_party/fsp_blobs/ble_patch/` BEFORE building any
  example app that depends on the BLE radio. The build system does
  not fetch this for you.
- **Effect on the firmware**: `libs/ra_hal/src/ra_ble_patch.c`
  continues to return `k_ra_err_not_supported` at runtime when no
  blob is staged; host unit tests under `tests/test_ra_ble_*.c`
  continue to compile and run unaffected.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
