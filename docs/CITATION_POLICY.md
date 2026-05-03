# Citation Policy

This file is the authoritative reference for the project's source-code
citation rules. The short form lives in `CLAUDE.md` under
`Code Style / Comment citations`.

## Rule 1: in-tree line citations are FORBIDDEN

Comments must not reference other files in this repository by
`<file>:<line>` (e.g. `libs/ra_drw/src/ra_drw.c:776`). Line numbers
go stale the moment any agent reformats or edits the target file, and
they are not searchable -- a reader cannot grep for `:776` and find
anything useful.

Use the function or symbol name instead:

- `internal_rect_below_min`
- `ra_dmac::internal_mode_to_dmtmd`
- `see ra_pid_step in libs/ra_pid/src/ra_pid.c`

The pre-commit gate `scripts/utils/check_line_citations.py` enforces
this rule. It runs in WAVE-0 warn-only mode until the pre-existing
violations have been cleaned up; the flag `WAVE_0_WARN_ONLY` at the
top of that script flips to `False` once the cleanup wave lands.

## Rule 2: external / vendor citations are MANDATORY

Every HAL register access, ISR, and driver path must cite the source
of truth so a future reader can verify the implementation against the
spec without guessing. Acceptable forms:

- `/* HUM section 11.2.7 PWPR write-protect register */`
- `/* FSP r_sci_b/r_sci_b.c @ commit 8b3f2c1 */`
- `/* RFC 791 section 3.2 IPv4 header layout */`
- `/* Datasheet R01DS0493EJ table 6.4 */`

References to anything under `docs/reference/` (the committed PDFs)
or `libs/third_party/` (SOUP) are exempt from rule 1 -- those line
numbers belong to artifacts we do not edit.

## Per-line opt-out: `CITES-OK: <reason>`

In rare cases an in-tree line citation is the right call (e.g. a
historical migration note describing exactly which old line moved
where). Add `// CITES-OK: <reason>` to that line; the gate skips any
line containing a non-empty reason after the marker.

```c
/* moved from libs/old_module/foo.c:412 to libs/new_module/bar.c::priv_foo
 * CITES-OK: historical migration note, target file deleted */
```

CHANGELOG-style "moved from <file>:NNN to ..." snippets are also
exempt automatically.

## Tooling

- Gate: `scripts/utils/check_line_citations.py` -- pre-commit
  enforcement. Pass `--all` to scan the whole tree.
- Extractor: `scripts/utils/extract_line_citations.py` -- emits CSV
  of every violation with the enclosing function and a suggested
  `<file>::<func>` replacement. Used by cleanup agents.
