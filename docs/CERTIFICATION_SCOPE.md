# Certification Scope

This document is the authoritative statement of what `ra8-firmware`
**does** and **does not** pursue with respect to third-party
certification. It supersedes any earlier "future-assessor" wording in
`docs/QUALIFICATION_ROADMAP.md`, `docs/qualification/PSAC.md`, or
related artefacts.

## Decision (2026-05-02): no independent assessor, ever

`ra8-firmware` will achieve **technical compliance** with the
substantive software-engineering requirements of:

- IEC 61508-3:2010 SIL 3 (industrial functional safety)
- DO-178C:2011 Level B (avionics)
- ISO 26262-6:2018 ASIL C / ASIL D (automotive)

This means MC/DC structural coverage, MISRA-C 2012 conformance (with
the residual-risk envelope recorded in
`docs/qualification/MISRA_DEVIATIONS.md`), the planning + verification
+ accomplishment document family under `docs/qualification/`, the
SOUP register under `docs/SOUP/`, the architectural ring + TrustZone
world tagging in `docs/RING_AND_WORLD.md`, and the configuration-
management / quality-assurance discipline enforced by the CI gates.

The project will **NOT** pursue, fund, or schedule:

- An **independent assessor** engagement under IEC 61508-1 cl. 8.2
  (e.g. TUV SUD, exida, SGS-TUV Saar). Typical cost: **USD
  $30,000-$150,000** per campaign, plus repeat fees per major version.
- A **DO-178C Stage of Involvement (SOI) review** by an FAA / EASA
  Designated Engineering Representative.
- An **ISO 26262 Confirmation Review** by an external ASIL-qualified
  auditor.
- A **Bluetooth-SIG QDID** for the BLE radio path.
- Any **paid certification body** engagement of any kind.

## Why this is final

`ra8-firmware` is an MIT-licensed personal/research project
maintained by a single contributor (Brighton Sikarskie). It will not
ship as a regulated commercial product. The MIT licence and the
non-commercial intent together make a paid assessor engagement
economically irrational:

- The assessor cost dwarfs any plausible value the project could
  return to its single maintainer.
- An assessment is per-version: each significant release would
  require a re-engagement, multiplying the cost.
- The MIT licence offers no warranty, and a downstream party cannot
  "inherit" an assessor's signoff -- they must commission their own.

## What downstream adopters get

Any party who adopts this codebase as the basis for a regulated
product is welcome to do so under the MIT licence. They are
**responsible for**:

- Engaging their own independent assessor / DER / Confirmation
  Reviewer for their target standard.
- Procuring their own commercial MISRA checker (LDRA, Helix QAC,
  Polyspace, PVS-Studio) per their assessor's tooling-qualification
  requirements -- see `docs/qualification/MISRA_DEVIATIONS.md`
  Section "Tooling policy".
- Re-running the verification evidence under their own
  configuration-management baseline.
- Providing their own warranty, indemnity, and field-issue support.

The artefacts in `docs/qualification/` (PSAC, SDP, SVP, SCMP, SQAP,
SVCP, SVR, SAS) are intended to make this re-use as low-friction as
possible: a downstream assessor can read the existing planning and
verification record and decide which parts to accept as-is and which
to re-derive under their own oversight.

## Cross-references

- `docs/qualification/PSAC.md` Section "Certification scope" -- the
  short-form restatement of this decision inside the PSAC.
- `docs/qualification/MISRA_DEVIATIONS.md` Section "Tooling policy"
  -- the consistent "no commercial MISRA tool, ever" decision.
- `docs/HIL_DEVELOPER_WORKFLOW.md` -- the self-hosted Pi 5 HIL
  runner that closed the CI-hardware question (see
  `docs/QUALIFICATION_ROADMAP.md` Section 6 item 4).
- `docs/VENDOR_BLOBS.md` -- the consistent "obtain Renesas blobs from
  public FSP as SOUP, never NDA" decision.
- `docs/QUALIFICATION_ROADMAP.md` Section 6 -- the open-questions
  register, with all four 2026-05-02 decisions marked CLOSED.
- `LICENSE.txt` -- MIT licence text.
