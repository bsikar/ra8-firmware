# Certification Scope

This document is the authoritative statement of what `ra8-firmware`
**does** and **does not** pursue with respect to third-party
certification. Where another artefact still reads as though an assessor
were expected, this file wins.

## Decision: no independent assessor, ever

`ra8-firmware` will achieve **technical compliance** with the
substantive software-engineering requirements of:

- IEC 61508-3:2010 SIL 3 (industrial functional safety)
- DO-178C:2011 Level B (avionics)
- ISO 26262-6:2018 ASIL C / ASIL D (automotive)

This means MC/DC structural coverage, MISRA-C 2012 conformance (with
the residual-risk envelope documented in the qualification records),
the planning + verification + accomplishment document family in the
`RA8FW` YouTrack knowledge base (`RA8FW-A-2`), the SOUP register under
`docs/SOUP/`, the architectural ring + TrustZone world tagging in
`docs/RING_AND_WORLD.md`, and the configuration-management /
quality-assurance discipline enforced by the CI gates in `bsikar/ra8ci`.

The project will **NOT** pursue, fund, or schedule:

- An **independent assessor** engagement under IEC 61508-1 cl. 8.2
  (e.g. TUV SUD, exida, SGS-TUV Saar).
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
  requirements -- see the MISRA policy in `RA8FW-A-4` (PSAC) and
  `RA8FW-A-14` (TOOL_QUALIFICATION) in the `RA8FW` YouTrack KB.
- Choosing their **MISRA edition**. The verification evidence here
  is against MISRA C:2012 (Third Edition, March 2013); the current
  edition is MISRA C:2025 (March 2025), with MISRA C:2023 between
  them. The open-source cppcheck add-on implements MISRA C:2012
  only, so no checker version bump changes this -- see `RA8FW-A-14`
  (TOOL_QUALIFICATION) in the `RA8FW` YouTrack KB.
- Re-running the verification evidence under their own
  configuration-management baseline.
- Providing their own warranty, indemnity, and field-issue support.

The qualification artefacts in the `RA8FW` YouTrack knowledge base under
`RA8FW-A-2` (`RA8FW-A-4` PSAC, `RA8FW-A-5` SDP, `RA8FW-A-6` SVP, `RA8FW-A-7`
SCMP, `RA8FW-A-8` SQAP, `RA8FW-A-11` SVCP, `RA8FW-A-12` SVR, `RA8FW-A-13` SAS)
are intended to make this re-use as low-friction as possible: a downstream
assessor can read the existing planning and verification record and decide
which parts to accept as-is and which to re-derive under their own oversight.

## Cross-references

- `RA8FW-A-4` (PSAC) Section "Certification scope" in the `RA8FW` YouTrack
  KB -- the short-form restatement of this decision inside the PSAC.
- `RA8FW-A-14` (TOOL_QUALIFICATION) in the `RA8FW` YouTrack KB -- the
  consistent "no commercial MISRA tool, ever" decision.
- `bsikar/ra8ci` HIL workflow docs -- the managed dev-box HIL listener
  and remote Pi 5 instrument host that close the CI-hardware question.
- `docs/VENDOR_BLOBS.md` -- the consistent "obtain Renesas blobs from
  public FSP as SOUP, never NDA" decision.
- `RA8FW-A-3` (QUALIFICATION_ROADMAP) Section 6 in the `RA8FW` YouTrack KB --
  the open-questions register, where this decision is recorded as closed.
- `LICENSE.txt` -- MIT licence text.
