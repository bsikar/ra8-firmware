# Tool Qualification Dossier

**Status**: stub. Populated during Phase 7 of
`docs/QUALIFICATION_ROADMAP.md`.

**DO-178C reference**: Section 12.2 + DO-330.
**IEC 61508-3 reference**: Clause 7.4.4 + Annex D.

## Scope

Per-tool qualification record. The summary table lives in
`docs/QUALIFICATION_ROADMAP.md` Section 5; this document holds
the per-tool detail.

## Tool entries to populate

1. arm-none-eabi-gcc (production cross-compiler).
2. clang-18 (host MC/DC instrumentation).
3. cppcheck + misra addon.
4. clang-tidy.
5. clang-format.
6. llvm-profdata + llvm-cov.
7. cmake + make.
8. JLinkExe (SEGGER).
9. arm-none-eabi-addr2line.
10. python3 + audit scripts under `scripts/utils/`.

Each entry records: tool version, vendor, intended use, TQL
classification, qualification basis, compensating verification,
and re-qualification trigger (e.g. major-version bump).
