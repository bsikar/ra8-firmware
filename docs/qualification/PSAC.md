# Plan for Software Aspects of Certification (PSAC)

**Last refreshed**: 2026-08-22 (test inventory and execution evidence refresh;
see Section 8).

**Status**: First draft, 2026-05-02. Authored against the Phase 7 schedule
in [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md) Section 3.
**DO-178C reference**: Section 11.1.
**IEC 61508-3 reference**: Clause 7.1, Annex B (planning of safety
lifecycle activities).
**ISO 26262-6 reference**: Clause 5 (initiation of product development at
the software level).
**Project**: `ra8-firmware`.
**Maintainer**: Brighton Sikarskie (single developer; no separate quality
or certification organisation).

This document is the gateway artefact for any future third-party
qualification activity against the assurance levels named in Section 3.
It does not commit `ra8-firmware` to a specific market deployment; it
records the planning baseline against which all subsequent verification
evidence is collected.

---

## 1. System overview

### 1.1 Target hardware

The system under consideration is the Renesas EK-RA8D2 evaluation kit
(part number 968-K7EKA8D2S01001BE) populated with the R7KA8D2KFLCAC MCU
from the RA8D2 group. The configuration is summarised in
[`../../CLAUDE.md`](../../CLAUDE.md) Section "Target Hardware":

- Arm Cortex-M85 primary core at 1 GHz with the Helium / MVE extension.
- Arm Cortex-M33 secondary core at 250 MHz; M33/CPU1 and dual-core example
  images exercise it.
- 1 MB MRAM code store, 1664 KiB (1.625 MiB) SRAM with ECC.
- 64 MB Octo-SPI NOR flash, 64 MB SDRAM, 7.0-inch parallel TFT,
  OV5640 5 MP camera, on-board J-Link OB debugger.
- Package R7KA8D2KFLCAC, 289-pin BGA, 12 mm x 12 mm, 0.65 mm pitch.

The chip-level reference is the Renesas Hardware User's Manual
R01UH1065EJ, committed under [`../reference/`](../reference/). All
register-level code in this tree cites a section of that manual via
`@cite` doxygen tags audited by `scripts/checks/cite_check.py`.

### 1.2 Intended use

`ra8-firmware` is presently a personal in-house exploration codebase.
Its qualification posture is staged so that **if** the firmware (or a
subset of it) is later carried into a deployed product, the planning
record, the traceability, and the verification evidence already exist.
There is no current production deployment, and no end-user sale path
is in scope for this draft.

Because no specific deployment is committed, the **operating
environment** is taken to be the worst case the chip itself supports
under Renesas R01AN8060EJ (high-temperature operation app note,
committed under [`../reference/`](../reference/)): industrial
temperature range up to 105 C ambient, electrically noisy environment,
with no human-rated safety claim.

### 1.3 Regulatory context

No regulatory authority has been engaged. The standards in Section 3
are pursued **on a best-evidence basis** rather than under an active
certification submission. Any future authority engagement (FAA / EASA
DER for DO-178C, TUV / Exida for IEC 61508, automotive supplier audit
for ISO 26262) would consume the artefacts in this document set;
nothing in the lifecycle assumes an authority is already in the loop.

---

## 2. Software overview

### 2.1 Architecture rings

The codebase is partitioned into seven architectural rings documented
in [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md). Each translation
unit declares its ring with a doxygen tag (`@ring` 0..6) which is
audited in strict mode by `scripts/checks/check_world_tags.py`. The rings are:

- Ring 0 -- silicon register definitions (`libs/ra8_hal/inc/ra8_*_regs.h`).
- Ring 1 -- thin register accessors (`libs/ra8_hal/src/ra8_*.c`).
- Ring 2 -- driver state machines (HAL on top of Ring 1).
- Ring 3 -- platform abstraction layers (`libs/ra8_*_pal/`).
- Ring 4 -- middleware (TLS, OTA, filesystem wrappers, USB classes).
- Ring 5 -- secure-side substrate (`libs/ra8_secure_app/`, key vault).
- Ring 6 -- application binaries under `examples/`.

Higher rings may only call lower rings; sibling-ring calls are flagged
by `check_world_tags.py`. This is the architectural-design evidence
required by IEC 61508-3 Clause 7.4.3 and DO-178C Section 5.2.

### 2.2 TrustZone partitioning

Cortex-M85 runs both a Secure (S) and Non-Secure (NS) world. The
partitioning rules and SAU configuration are in
[`../RING_AND_WORLD.md`](../RING_AND_WORLD.md) Section "World
tagging". Every translation unit declares a `@world` tag (S, NS, or
NSC). NSC veneers live in [`../../libs/ra8_nsc/`](../../libs/ra8_nsc/)
and are the only entry points from NS to S. The default SAU and TrustZone
bring-up sequence comes from the selected board layer, with an app-local file
used only when the app overrides it (for example,
[`ra8_board_ek_ra8d2/src/boot/trustzone_init.c`](../../libs/ra8_board_ek_ra8d2/src/boot/trustzone_init.c)).

### 2.3 RTOS choice

The default substrate is bare-metal (`while(1)` main loops with the
`ra8_time` SysTick driver). Examples that need cooperative scheduling
link against ThreadX 6.5.0 from the SOUP catalogue
([`../SOUP/threadx.md`](../SOUP/threadx.md)). No first-party RTOS is
shipped in this tree; a first-party kernel remains a reserved but unscheduled
long-term option.

### 2.4 Vendor SOUP inventory

Pre-existing software is catalogued in [`../SOUP/`](../SOUP/) per
IEC 61508-3 Clause 7.4.2.12 and DO-178C Section 12.1.4. The generated SBOM at
[`../sbom/ra8-firmware.cdx.json`](../sbom/ra8-firmware.cdx.json) and the SOUP
index are the current component inventory; this plan does not duplicate their
mutable count or names. Each entry carries a written qualification basis and a
12-month re-review cadence.

No vendor binary blob is shipped in this tree. The RSIP-E50D protected
firmware -- the one blob a hardware-backed key-wrap path would need --
is obtainable from public FSP under BSD-3-Clause but is not vendored
here; see [`../VENDOR_BLOBS.md`](../VENDOR_BLOBS.md) for what degrades
without it. The RA8D2 carries no BLE radio, so no controller patch
image is a dependency of anything.

---

## 3. Certification considerations

### 3.1 Target assurance levels

Per [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md)
Section 1, the anchor standard is **IEC 61508-3:2010 SIL 3**
(industrial functional safety). Two adjacent standards are mapped
in parallel because the bulk of the supporting evidence is shared:

- **DO-178C:2011 Level B (DAL B)** -- "Hazardous failure condition"
  tier, used for avionics. Drives the MC/DC structural-coverage
  obligation (Table A-7 objective 5, Section 6.4.4.2).
- **ISO 26262-6:2018 ASIL C / ASIL D** -- automotive equivalent.

The side-by-side objective map is in
[`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md)
Section 1 ("Side-by-side objective map") and is not duplicated here.

### 3.2 Independence of verification

For SIL 3, IEC 61508-1:2010 Clause 8.2 requires an **independent
person** for the functional safety assessment. Per the 2026-05-02
decision recorded in [`../CERTIFICATION_SCOPE.md`](../CERTIFICATION_SCOPE.md),
this project will **not** engage a paid third-party assessor at any
point: the developer is also the verifier, and this is the project's
permanent posture, not a transitional state. Downstream adopters who
require independence must engage their own assessor.

### 3.2.1 Certification scope (final policy)

Per [`../CERTIFICATION_SCOPE.md`](../CERTIFICATION_SCOPE.md), this
project achieves **technical compliance** with the substantive
software-engineering requirements of IEC 61508 SIL 3 / DO-178C
Level B / ISO 26262 ASIL C/D (MC/DC, MISRA-C subset, structural
coverage, full doc artefact set) but **does NOT pursue
independent-assessor signoff**. The MIT licence plus single-
maintainer non-commercial intent makes paid assessor engagement
(TUV SUD / exida / Verocel / SGS-TUV Saar / etc., USD $30k-$150k
typical per campaign) **out of scope, permanently**. Any downstream
party who adopts this codebase as the basis for a regulated product
is responsible for engaging their own assessor; the artefacts in
this directory exist to make that re-use as low-friction as
possible.

### 3.3 Deactivated and dead code

DO-178C Section 6.4.4.3 distinguishes deactivated code (present but
not exercised in the target operational configuration) from dead
code (unreachable). Phase 2 of the roadmap requires every uncovered
MC/DC decision region to be classified as one of:

- A genuine missing test vector, scheduled to be added.
- A deactivated decision region with a written rationale (to be appended
  to [`../MCDC_GAPS.md`](../MCDC_GAPS.md)).
- Dead code, to be deleted under the project's zero-backward-
  compatibility policy ([`../../CLAUDE.md`](../../CLAUDE.md) Section
  "Backward Compatibility Policy").

The current MC/DC instrumentation is described in
[`../MCDC.md`](../MCDC.md). Its archived 2026-05 measurement history is not a
current structural-coverage claim and must be restamped for the release
evidence pack. Deactivated decision regions are catalogued under
DO-178C 6.4.4.3 in [`../MCDC_DEACTIVATIONS.md`](../MCDC_DEACTIVATIONS.md).

### 3.4 Parameter data items

DO-178C Section 11.21 defines parameter data items (PDIs) as
configuration constants that affect behaviour. In `ra8-firmware`
all PDIs are C23 typed enums per the rule in
[`../../CLAUDE.md`](../../CLAUDE.md) "Constants and Macros". This
gives every PDI a name, a fixed underlying type, and a single
declaration site. No runtime-loadable PDI mechanism exists in the
current build (no EEPROM-backed parameter file).

### 3.5 Tool qualification

Tool qualification is governed by DO-178C Section 12.2 + DO-330 and
IEC 61508-3 Clause 7.4.4 + Annex D. The full TQL classification of
the development chain is in
[`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md)
Section 5 and the dossier stub is at
[`./TOOL_QUALIFICATION.md`](./TOOL_QUALIFICATION.md). Every tool in
the chain is proposed at TQL-5 because its output has a downstream verification
method. Retained evidence is bounded to the historical 118/118 physical build,
the 2026-08-22 two-OS 689-case registration snapshot, and that snapshot's
Linux/devcontainer 689/689 pass in 8.66 s. The selected-app HIL and remote-GDB lifecycle
results are historical. Final tool qualification remains open for
current-candidate hardware runs, release-specific logs, coverage, trace, and
other evidence-pack inputs.

---

## 4. Software life cycle

### 4.1 Model

The development model is a **waterfall + agile hybrid**:

- **Waterfall scaffolding** -- the planning documents (PSAC, SDP,
  SVP, SCMP, SQAP) front-load the lifecycle definition before the
  bulk of verification evidence is collected. The verification
  results document set (SVCP, SVR, SAS) closes the lifecycle.
- **Agile execution** -- inside the scaffolding, work proceeds in
  small commits gated by the pre-commit hook (Section 7 of the SDP)
  and CI workflow ([`../../.github/workflows/firmware.yml`](../../.github/workflows/firmware.yml)).
  Breaking changes are encouraged
  ([`../../CLAUDE.md`](../../CLAUDE.md) "Backward Compatibility
  Policy"); the standing integration requirement is that `dev` builds and
  passes its required CI gates before release promotion to `main`.

### 4.2 Plan family

| Plan        | Path                                  | Status                |
|-------------|---------------------------------------|-----------------------|
| PSAC        | this document                         | First draft           |
| SDP         | [`./SDP.md`](./SDP.md)                | First draft           |
| SVP         | [`./SVP.md`](./SVP.md)                | Stub, Phase 7 sister  |
| SCMP        | [`./SCMP.md`](./SCMP.md)              | Stub, Phase 7 sister  |
| SQAP        | [`./SQAP.md`](./SQAP.md)              | Stub, Phase 7 sister  |
| SVCP        | [`./SVCP.md`](./SVCP.md)              | Stub, Phase 7 sister  |
| SVR         | [`./SVR.md`](./SVR.md)                | Stub, Phase 7 sister  |
| SAS         | [`./SAS.md`](./SAS.md)                | Stub, Phase 7 sister  |

The mapping from each plan to its DO-178C / IEC 61508 section is in
[`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md)
Section 4.

---

## 5. Software life cycle data

The artefact inventory below tracks every data item required by
DO-178C Section 11 and IEC 61508-3 Annex A. Each row points at the
authoritative location in the live tree.

| Data item                          | Location                                                   | DO-178C Section |
|------------------------------------|------------------------------------------------------------|-----------------|
| Plan for software aspects of cert. | [`./PSAC.md`](./PSAC.md)                                   | 11.1            |
| Software development plan          | [`./SDP.md`](./SDP.md)                                     | 11.2            |
| Software verification plan         | [`./SVP.md`](./SVP.md)                                     | 11.3            |
| Software config management plan    | [`./SCMP.md`](./SCMP.md)                                   | 11.4            |
| Software quality assurance plan    | [`./SQAP.md`](./SQAP.md)                                   | 11.5            |
| Requirements standards             | per-feature `*.md` under `docs/`, `@brief` corpus in code  | 11.6            |
| Design standards                   | [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md)             | 11.7            |
| Software coding standards          | [`../../CLAUDE.md`](../../CLAUDE.md), [`../STYLE_GUIDE.md`](../STYLE_GUIDE.md), [`../MISRA.md`](../MISRA.md) | 11.8 |
| Software requirements data         | per-module `@brief`/`@details` blocks; `docs/ARCHITECTURE.md` | 11.9         |
| Design description                 | [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md), [`../MEMORY_MAP.md`](../MEMORY_MAP.md) | 11.10 |
| Source code                        | `libs/`, `examples/`, `port/`, `apps/`                     | 11.11           |
| Executable object code             | per-app `build/<app>.elf` / `.hex`                         | 11.12           |
| Software verif. cases & procedures | [`./SVCP.md`](./SVCP.md), distributed test inventory       | 11.13           |
| Software verification results      | [`./SVR.md`](./SVR.md), `build/mcdc-report/`, HIL logs, `build/misra/` | 11.14 |
| Software life cycle env. config    | [`../../cmake/toolchain-ra8d2.cmake`](../../cmake/toolchain-ra8d2.cmake), checked-in [`.devcontainer/`](../../.devcontainer/) | 11.15 |
| Software config management records | git history, signed tags (planned), [`./SCMP.md`](./SCMP.md) | 11.16, 11.18  |
| Problem reports                    | git issues + commit messages                               | 11.17           |
| Software quality assurance records | CI logs in GitHub Actions; [`./SQAP.md`](./SQAP.md) appendix | 11.19         |
| Software accomplishment summary    | [`./SAS.md`](./SAS.md)                                     | 11.20           |
| Tool qualification data            | [`./TOOL_QUALIFICATION.md`](./TOOL_QUALIFICATION.md)       | 11.20 + DO-330  |

---

## 6. Schedule

The 22-week schedule is owned by
[`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md)
Section 3. This PSAC does not duplicate the per-phase task list; it
simply records the calendar commitment.

| Phase | Weeks | Original deliverable summary                      |
|------:|------:|---------------------------------------------------|
| 1     |  1-2  | 100 % MC/DC on critical-path modules              |
| 2     |  3-6  | 95 %+ first-party MC/DC, deactivated-code register |
| 3     |  7-10 | Doxygen audit driven to zero gaps                 |
| 4     | 11-14 | MISRA-C deviation register signed off             |
| 5     | 15-16 | Per-app integration test layer                    |
| 6     | 17-18 | Hardware-in-the-loop CI on a self-hosted runner   |
| 7     | 19-22 | Plan + verification + accomplishment document set |

This is the original planning schedule, retained as history rather than a
current completion claim. Current bounded evidence and pending restamps are
recorded in Sections 8 and 9.

---

## 7. Additional considerations

### 7.1 Software of unknown provenance

SOUP is admitted under IEC 61508-3 Clause 7.4.2.12 and DO-178C
Section 12.1.4. The catalogue is at [`../SOUP/`](../SOUP/) (20 component
qualification pages excluding README). Re-review cadence: 12 months
maximum from each entry's "Last review" stamp.

### 7.2 Vendor blobs and open blockers

One vendor-controlled component is missing from a certified build, and
it is documented in [`../VENDOR_BLOBS.md`](../VENDOR_BLOBS.md):

- **RSIP-E50D firmware** -- required for production-grade key
  install / wrap. Today's mitigation is a host-only emulator
  (`RA8_RSIP_SOFTWARE_BACKEND`) that is **not** hardware-equivalent
  and cannot ship in a certified build. See also the
  `threadx_https_client` root-cause section in
  [`../HARDWARE_BRINGUP.md`](../HARDWARE_BRINGUP.md). Note this is a
  *vendoring* gap, not a licensing one: the sources are public FSP
  under BSD-3-Clause.

A second item, a "Renesas BLE controller patch image", was carried here
until 2026-08 and was never real: commit `6f6209a95` established that
the RA8D2 has no on-chip BLE radio, so no patch image exists to acquire
and the five BLE example apps were driving a phantom controller. BLE
now runs on the ESP32-C6 companion across the HCI transport seam.

The RSIP item must be either vendored properly or scoped out of a
specific certification campaign before that campaign closes. The
decision is currently deferred and is recorded as a known blocker
in [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md)
Section 6.

### 7.3 Deactivated code

The classification register will be appended to
[`../MCDC_GAPS.md`](../MCDC_GAPS.md) during Phase 2. Today the
register is empty.

### 7.4 Parameter data items

See Section 3.4 above. PDIs are C23 typed enums; no runtime
parameter file mechanism exists.

### 7.5 User-modifiable software

None. The build artefact is a single `.hex` per application;
end-user reconfiguration is not a feature.

### 7.6 Field-loadable software

OTA orchestration exists ([`../../libs/ra8_ota/`](../../libs/ra8_ota/),
Phase-5 work referenced in commit 10b9eedfc) but is currently
unsigned beyond the existing PSA Crypto signature stub. Treat as
out of scope until the RSIP blocker (Section 7.2) is resolved.

---

## 8. Bounded evidence metrics (2026-08-21/22 snapshots)

This table combines live derived authorities with explicitly dated retained
measurements. It reproduces the bounded evidence in
[`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md)
Section 2. They are quoted here so an assessor reading the PSAC in
isolation has the gap picture without chasing references.

| Metric                                                      | Value             | Source                                  |
|-------------------------------------------------------------|-------------------|-----------------------------------------|
| Test source files                                           | **693** (689 C, 4 C++) | retained 2026-08-22 snapshot |
| Registered CTest cases, clean standalone macOS configure    | **689**           | retained 2026-08-22 snapshot |
| Registered CTest cases, clean standalone Linux configure    | **689**           | retained 2026-08-22 snapshot |
| Linux/devcontainer host execution                           | **689/689 passed in 8.66 s** | retained unit-gate result, 2026-08-22 |
| macOS host execution                                        | **Not claimed**   | low-address tests require Linux/container; registration only |
| EIL application inventory                                   | Derived by `scripts/dev/ra8_apps.py` | live app authority |
| RA8D2 physical applications built                           | **118/118**       | retained historical snapshot; current matrix pending |
| Real target HIL execution                                   | **Pending**       | run `hil-all` on the current candidate |
| Remote GDB lifecycle                                        | **Historical pass** | restamp attach/continue/detach/stop for release evidence |

Coverage, documentation, MISRA, SOUP, and vendor-blob populations must be
refreshed from their live artifacts before the next evidence pack; the table
does not turn an archived population into current evidence.

The metrics that close before the next PSAC revision are recorded
as the Phase 1 / Phase 2 / Phase 3 acceptance gates in
[`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md).

---

## 9. Cross-references

- [`../../CLAUDE.md`](../../CLAUDE.md) -- coding rules, NASA Power-of-10 mapping, backward-compatibility policy.
- [`../STYLE_GUIDE.md`](../STYLE_GUIDE.md) -- human-facing style guide.
- [`../RING_AND_WORLD.md`](../RING_AND_WORLD.md) -- architectural rings + TrustZone world tagging.
- [`../MEMORY_MAP.md`](../MEMORY_MAP.md) -- RA8D2 memory map and partition assignments.
- [`../MCDC.md`](../MCDC.md) -- MC/DC instrumentation and measurement.
- [`../MISRA.md`](../MISRA.md) -- MISRA-C 2012 audit baseline.
- [`../HARDWARE_BRINGUP.md`](../HARDWARE_BRINGUP.md) -- archived bring-up sweep history; current bounded evidence is recorded in the SVR.
- [`../SOUP/`](../SOUP/) -- pre-existing software register.
- [`../VENDOR_BLOBS.md`](../VENDOR_BLOBS.md) -- blocker register.
- [`../QUALIFICATION_ROADMAP.md`](../QUALIFICATION_ROADMAP.md) -- 22-week schedule and tooling-chain TQL classification.
- [`../CERTIFICATION_SCOPE.md`](../CERTIFICATION_SCOPE.md) -- final policy: technical compliance pursued, third-party assessor engagement out of scope.
