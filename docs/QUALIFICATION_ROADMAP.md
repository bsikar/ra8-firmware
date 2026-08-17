# Qualification Basis -- IEC 61508 SIL 3 / DO-178C Level B

What the two target safety standards require of this codebase, which evidence
artifacts in this tree answer those requirements, and where each one lives.

This is a reference map, not a plan. Schedules, phase breakdowns and per-module
status belong in GitHub issues per repo policy, and the individual artifacts
below own their own current state. The project pursues technical conformance
with the standards named here; it does not pursue certification itself, and the
standing decisions that follow from that are recorded at the end.

---

## Standards target

The primary anchor is **IEC 61508 SIL 3** -- industrial functional safety,
hardware-platform agnostic. Two adjacent standards are mapped in parallel
because the bulk of the supporting evidence (planning documents, MC/DC coverage,
the MISRA-C subset, the SOUP register) is shared:

- **IEC 61508-3 SIL 3** -- general-purpose industrial functional safety, the
  standard most likely to consume RA8-class hardware in the field: motor
  control, programmable logic, safety I/O.
- **DO-178C Level B** -- the "hazardous failure condition" tier used in
  avionics. It drives the MC/DC structural-coverage requirement (Table A-7
  objective 5) and the document set below.
- **ISO 26262 ASIL C / ASIL D** -- the automotive equivalent. The evidence
  needed to reach ASIL C with this codebase is a subset of the same superset as
  SIL 3 / Level B.

### Why IEC 61508 is the anchor

IEC 61508 is sector-neutral. The more specialised standards -- DO-178C,
ISO 26262, IEC 62304, EN 50128 -- all derive their software clauses from the
same body of practice: V-model lifecycle, language subset, structural coverage,
configuration management, SOUP/COTS register, tool qualification. Anchoring on
IEC 61508 lets every artifact double as evidence for whichever sector this
firmware ships into first.

### Side-by-side objective map

| Lifecycle area              | IEC 61508-3 (SIL 3)           | DO-178C (Level B)                     | ISO 26262-6 (ASIL C/D)              | This repo's artifact                                  |
|-----------------------------|-------------------------------|----------------------------------------|--------------------------------------|--------------------------------------------------------|
| Planning                    | Annex B, FSM plan             | Section 4 (PSAC, SDP, SVP, SCMP, SQAP) | Part 6 cl. 5, project planning       | `docs/qualification/PSAC.md` + plan family             |
| Requirements                | 7.2 software safety reqs      | Section 5.1 high-level reqs            | Part 6 cl. 6, sw safety reqs         | `docs/ARCHITECTURE.md`, per-module `@brief` corpus     |
| Architecture / design       | 7.4.3 software architecture   | Section 5.2 low-level design           | Part 6 cl. 7, sw architectural design| `docs/RING_AND_WORLD.md`, `docs/MEMORY_MAP.md`         |
| Coding standard             | Annex A.4 language subset     | Section 11.8 / 6.3 coding standard     | Part 6 cl. 5.4.7 language subset     | `docs/MISRA.md`, `docs/STYLE_GUIDE.md`                 |
| Code verification (review)  | 7.9.2.7 module review         | Section 6.3.4 code reviews             | Part 6 cl. 9 verification            | PR review history + `clang-tidy` / `cppcheck` gates    |
| Structural coverage         | Annex C statement+branch; MC/DC strongly recommended at SIL 3 | Section 6.4.4.2 MC/DC at Level B | Part 6 cl. 9.4.5 MC/DC at ASIL C/D | `docs/MCDC.md`, `make mcdc`                            |
| Test cases (req-based)      | 7.4.7 / 7.7 testing           | Section 6.4.2 requirements-based test  | Part 6 cl. 9 test specification      | `tests/test_*.c` (requirements-traced)                 |
| Integration / HW-SW         | 7.5 integration               | Section 6.4.3 integration test         | Part 6 cl. 10 sw integration         | `docs/HARDWARE_BRINGUP.md`, `docs/HIL_SUITE.md`        |
| Configuration management    | 6.2.3 / Annex B.2             | Section 7 SCM process                  | Part 8 cl. 7 sw CM                   | git + signed tags + `docs/qualification/SCMP.md`       |
| Quality assurance           | 6.2.5                         | Section 8 SQA process                  | Part 2 cl. 5 / Part 8 cl. 5          | CI gates + `docs/qualification/SQAP.md`                |
| Tool qualification          | 7.4.4 / Annex D               | Section 12.2 + DO-330                  | Part 8 cl. 11                        | "Tool qualification" below                             |
| Pre-existing software       | 7.4.2.12 (SOUP)               | Section 12.1.4 + DO-278A               | Part 8 cl. 12 SEooC                  | `docs/SOUP/`                                           |
| Certification liaison       | independent assessor (8.2)    | Section 9 SOI 1-4 + SAS                | Part 2 cl. 6 confirmation reviews    | `docs/qualification/SAS.md`                            |

References for this table: IEC 61508-3:2010 Annex A/B/C; RTCA DO-178C:2011
Sections 4-12 + Annex A; ISO 26262-6:2018 Clauses 5-11.

---

## The audit document set

All planning and verification documents live under `docs/qualification/`. The
structure mirrors DO-178C Section 11, with the IEC 61508-3 cross-reference in
each document's preamble.

| Document                                          | Path                                       | DO-178C ref   | IEC 61508-3 ref     |
|---------------------------------------------------|--------------------------------------------|---------------|---------------------|
| Plan for Software Aspects of Certification (PSAC) | `docs/qualification/PSAC.md`               | 11.1          | 7.1, Annex B        |
| Software Development Plan (SDP)                   | `docs/qualification/SDP.md`                | 11.2          | 7.1.2               |
| Software Verification Plan (SVP)                  | `docs/qualification/SVP.md`                | 11.3          | 7.9                 |
| Software Configuration Management Plan (SCMP)     | `docs/qualification/SCMP.md`               | 11.4          | 6.2.3               |
| Software Quality Assurance Plan (SQAP)            | `docs/qualification/SQAP.md`               | 11.5          | 6.2.5               |
| Software Verification Cases & Procedures (SVCP)   | `docs/qualification/SVCP.md`               | 11.13         | 7.9.2               |
| Software Verification Results (SVR)               | `docs/qualification/SVR.md`                | 11.14         | 7.9.6               |
| Software Accomplishment Summary (SAS)             | `docs/qualification/SAS.md`                | 11.20         | 6.2.6 (assessment)  |
| MISRA-C 2012 deviation register                   | `docs/qualification/MISRA_DEVIATIONS.md`   | 11.8          | Annex A.4           |
| Tool qualification dossier                        | `docs/qualification/TOOL_QUALIFICATION.md` | 12.2 + DO-330 | 7.4.4 + Annex D     |

The live measurement artifacts those documents draw on are
[`MCDC.md`](MCDC.md) and [`MCDC_GAPS.md`](MCDC_GAPS.md) for structural
coverage, [`MISRA.md`](MISRA.md) for the language subset,
[`DOXYGEN_GAPS.md`](DOXYGEN_GAPS.md) for documentation conformance, and
`docs/SOUP/` for the pre-existing-software register. Each is regenerated from
the tree rather than transcribed, which is why none of their numbers are copied
into this file.

---

## Tool qualification

DO-178C 12.2 and DO-330 classify a development tool by Tool Qualification Level.
**TQL-5** is a tool whose output is verified by another process; **TQL-1** is a
tool whose output is not independently verified and whose failure could inject
an undetected error into the certified software.

| Tool                         | Role                                | TQL   | Compensating verification |
|------------------------------|-------------------------------------|-------|---------------------------|
| `arm-none-eabi-gcc`          | Cross-compiler -> production object | TQL-5 | Object code re-verified against requirements by the hardware-in-the-loop suite (`scripts/hil/all.sh`). |
| host `clang`                 | MC/DC instrumentation + host tests  | TQL-5 | Output is test-only; no production code path. Instrumentation re-verified by the host tests passing. |
| `cppcheck` (misra addon)     | MISRA-C 2012 checker                | TQL-5 | Findings reviewed manually and dispositioned in the deviation register. Sole MISRA tool by policy. |
| `clang-tidy`                 | Naming + complexity gate            | TQL-5 | Advisory; no autofix in CI. The line-threshold gate is cross-checked against NASA P10 Rule 4. |
| `clang-format`               | Style enforcement                   | TQL-5 | Idempotent, and reviewed by a human on every PR. |
| `llvm-profdata` / `llvm-cov` | MC/DC measurement                   | TQL-5 | Coverage results spot-checked against hand-traced decisions during review. |
| `cmake` + `make`             | Build orchestration                 | TQL-5 | Produces the same object as a manual invocation; the build log is archived per CI run. |
| `JLinkExe`                   | Flash + register dump for smoke     | TQL-5 | Read-only with respect to certified bits; any write step is verified by post-flash readback. |
| `arm-none-eabi-addr2line`    | Smoke-test PC resolution            | TQL-5 | Cross-checked against the ELF symbol table when a classification is ambiguous. |
| `python3` audit scripts      | Coverage / MISRA / doc gap reports  | TQL-5 | Output reviewed; the scripts under `scripts/checks/` carry their own host tests and selftests. |

No tool in the chain requires TQL-1, because none emits certified production
code without a downstream verification step. The compiler is the closest call;
the mitigation is the hardware-in-the-loop smoke plus the host integration
tests.

---

## Standing decisions

These are settled, not pending. Each removes a question that otherwise gets
re-litigated.

**No commercial MISRA checker, ever.** cppcheck remains the sole MISRA
enforcement tool. LDRA, Helix QAC, Polyspace and PVS-Studio are out of scope:
this is an MIT-licensed, zero-budget personal research project that will not
seek certification. The MISRA rules cppcheck does not cover are accepted as
residual risk per IEC 61508-7 Annex D.7. Full rationale in the deviation
register's "Tooling policy" section.

**No independent assessor, ever** (IEC 61508-1 cl. 8.2). The project aims at
technical conformance with the substantive software requirements of IEC 61508
SIL 3 / DO-178C Level B / ISO 26262 ASIL C-D, without certification-body
sign-off. A downstream adopter who needs an assessor engages their own. Full
rationale in [`CERTIFICATION_SCOPE.md`](CERTIFICATION_SCOPE.md).

**Vendor crypto blobs are a procurement route, not a dependency.** The RSIP-E50D
protected procedures are published in the public `renesas/fsp` repository under
BSD-3-Clause, so they can be pulled as SOUP under IEC 61508-3 sec. 7.4.2.12 and
DO-178C sec. 12.1.4 whenever a hardware build needs them -- no NDA, no
clean-room rewrite. Nothing is vendored today: the tree builds the software
backend (`RA8_RSIP_SOFTWARE_BACKEND` in `libs/ra8_hal/src/`). An earlier
in-tree snapshot of the FSP primitives was deleted (#614) because no target
compiled it and it could not have compiled anyway -- nearly all of its
translation units included FSP headers that were never copied alongside them.
That is the standing lesson: a real port re-vendors a complete, tag-pinned
snapshot together with the `r_rsip_protected` driver layer *and* a build option
that actually compiles it. Procurement guidance is in
[`VENDOR_BLOBS.md`](VENDOR_BLOBS.md).

**Hardware-in-the-loop is a gate, not an aspiration.** A self-hosted runner with
an EK-RA8D2 wired to it runs the HIL suite on every change that touches
HIL-relevant paths. The contract is documented in
[`HIL_SUITE.md`](HIL_SUITE.md); the developer-side workflow in
[`HIL_DEVELOPER_WORKFLOW.md`](HIL_DEVELOPER_WORKFLOW.md).

---

## Cross-references

- `CLAUDE.md` -- the coding rules and the NASA P10 + SOLID-for-C policy this
  qualification basis layers onto.
- [`RING_AND_WORLD.md`](RING_AND_WORLD.md) -- the architectural baseline the
  PSAC system overview builds on.
- [`CERTIFICATION_SCOPE.md`](CERTIFICATION_SCOPE.md) -- what this project does
  and does not claim.
