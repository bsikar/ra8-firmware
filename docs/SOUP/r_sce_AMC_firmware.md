# SOUP Justification: RSIP-E50D Firmware (`r_sce_AMC`)

Per IEC 61508-3 Section 7.4.2.12 and DO-178C Section 12.1.4, this document
records the qualification basis for accepting the Renesas RSIP-E50D
firmware blobs (the `r_sce` "AMC" / Async-MailboxCoprocessor procedures)
into this firmware as Software Of Unknown Provenance (SOUP).

## Component identity

- **Name**: RSIP-E50D firmware procedures (Renesas Secure IP, the
  protected-mode crypto coprocessor inside the RA8D2 silicon).
- **Renesas reference name**: `r_sce` SCE9 protected procedures /
  `crypto_procedures_protected/src/sce9/`.
- **Version**: tracked by FSP release tag (TBD on first vendoring).
- **Upstream URL**: <https://github.com/renesas/fsp>, subtree
  `ra/fsp/src/r_sce/crypto_procedures_protected/src/sce9/`.
- **Local path (after vendoring)**: `libs/third_party/fsp_blobs/r_sce_AMC/`.

## Provenance

- **Origin**: Renesas Electronics Corporation, distributed as part of
  the **Renesas Flexible Software Package (FSP)** under the FSP
  top-level `LICENSE.md`. The source-of-truth distribution channel
  for this project is the public `renesas/fsp` GitHub repository.
- **License**: per FSP `LICENSE.md` -- the per-file notice in each
  `.c` / `.h` / blob file governs. The protected procedures are
  shipped as Renesas-proprietary text-with-binary tables and are
  reproduced verbatim under the FSP redistribution clause.
- **How it would enter our tree**: vendored snapshot copied directly
  from a tagged FSP release into
  `libs/third_party/fsp_blobs/r_sce_AMC/`. The exact FSP release tag
  and the copied-files SHA-256 hashes are recorded in this doc on
  first vendoring (currently TBD -- see `Procurement status` below).

## Use case in this firmware

- Drives the RSIP-E50D protected-mode key-install / key-wrap /
  key-unwrap flows inside `libs/ra_hal/src/ra_rsip*.c` and
  `src/secure_app/key_import.c`. These are the OEM-grade crypto
  primitives that the bare RSIP register interface (Hardware User's
  Manual Ch 52, pp 3302-3307) does not expose by itself.
- Required by the secure-side key vault (`Ring 5`, `World: S`) for
  any production deployment that claims hardware-backed key
  protection.
- Integrity-claim category: cryptographic. Failure of the blob would
  manifest as wrong-ciphertext / wrong-key-handle output, detectable
  at the call site by PSA-Crypto self-tests.

## Qualification basis

Accepted as-is per IEC 61508-3 Section 7.4.2.12 (proven-in-use route)
and DO-178C Section 12.1.4 (previously developed software):

- **Service history**: the RSIP / SCE9 protected procedures have
  shipped in millions of Renesas RA family devices since the SCE9
  block was introduced in the RA6M4 / RA6T2 generation, including
  multiple RA8 derivatives that re-use the same firmware payload.
- **Vendor maintenance**: Renesas is the sole authoritative source
  for both the blob and its corresponding hardware. Bug-fixes ship
  as new FSP minor releases; the project tracks FSP release notes at
  the cadence given under `Re-review` below.
- **Treatment as a black-box hardware-IP block**: per IEC 61508-3
  sec. 7.4.2.12 the blob is admitted as a pre-developed component
  whose internal structure cannot be re-verified (no source-level
  MC/DC, no MISRA-C audit, no Doxygen audit). The compensating
  controls are the host-side PSA-Crypto self-tests under
  `tests/test_ra_psa_crypto.c` and the Phase-6 hardware-in-the-loop
  smoke (per `docs/HIL_DEVELOPER_WORKFLOW.md`).
- **Bug tracker review**: Renesas FSP issue tracker
  <https://github.com/renesas/fsp/issues> is reviewed at the
  re-review cadence below. No RSIP-E50D advisories at vendor-in date
  are open against the RA8D2 protected procedures.

## Risk mitigation

- The blob is invoked **only** from `libs/ra_hal/src/ra_rsip*.c` and
  the `src/secure_app/key_import*` veneers; first-party application
  code never calls into `r_sce` directly.
- The software-only fallback (`RA_RSIP_SOFTWARE_BACKEND`) remains in
  the tree and provides host-side coverage of the surrounding
  state-machine logic without exercising the blob itself.
- Downstream consumers who require certified evidence for RSIP
  primitives must obtain Renesas's own qualification artefacts under
  NDA -- this project does not re-derive them.

## Deviations / patches

None planned. Any future patch must live in a separate first-party
shim (`libs/ra_hal/src/ra_rsip_patch.c`) and must not modify the
vendored tree.

## Procurement status

- **Vendored**: NO (as of 2026-05-02). The `libs/third_party/fsp_blobs/r_sce_AMC/`
  subdirectory is intentionally absent from the tree; only the
  parent `libs/third_party/fsp_blobs/README.md` exists.
- **Vendoring procedure**: see
  `libs/third_party/fsp_blobs/README.md` Section "Procurement plan".
  The next contributor with network access and a tagged FSP release
  performs the copy + SHA-256 + commit.

## Last review date

- Reviewed: 2026-05-02
- Expected re-review by: 2027-05-02
