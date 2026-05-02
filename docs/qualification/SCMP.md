# Software Configuration Management Plan (SCMP)

**Status**: stub. Populated during Phase 7 of
`docs/QUALIFICATION_ROADMAP.md`.

**DO-178C reference**: Section 11.4.
**IEC 61508-3 reference**: Clause 6.2.3.

## Scope

Defines configuration identification, baseline control, change
control, problem reporting, and archive / retrieval procedures.

## Sections to populate

1. Configuration items (source under `libs/`, `src/`, `examples/`,
   build artifacts under `build/`, documents under `docs/`).
2. Baselines (git tags per release).
3. Change control (PR workflow, sign-off requirements).
4. Problem reporting (GitHub issues + linked PRs).
5. Archive and retrieval (git remote + signed tags).
6. Build environment configuration (cmake toolchain file
   `cmake/toolchain-ra8d2.cmake`).
7. SOUP configuration (versions pinned in `docs/SOUP/`).
