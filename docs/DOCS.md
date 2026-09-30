# Documentation in this repository

There is no generated HTML site. Doxygen, MkDocs, the vendored theme and the
publish pipeline were removed deliberately in `a67c04f3e`, which deleted the
top-level Doxygen configuration, the theme tree, the MkDocs configuration, the
four builder scripts and the three generator scripts behind them. This page
used to describe how to build and publish that site; every command and path in
it had been wrong since 2026-09-18.

What is left is the part that was always doing the work: the documentation
lives in the tree as Markdown and as Doxygen comment blocks on the source, and
gates hold both to a standard. Nothing renders them into a site, and nothing
needs to.

## Where the documentation is

- **Doxygen comment blocks** on every first-party function, file, enum value,
  struct member and macro. The required tag set is in `docs/STYLE_GUIDE.md`
  and the rules are restated in `CLAUDE.md`. They are read in the source, not
  in a browser.
- **Markdown under `docs/`**: the architecture and reference pages, the SOUP
  records under `docs/SOUP/`, the dated decisions under `docs/adr/`, and the
  qualification material under `docs/qualification/`.
- **Per-app `README.md`** beside each example and product.

## What enforces it

Three gates, all runnable locally, none of which needs a doxygen binary:

| Gate | What it asks |
|---|---|
| `scripts/checks/doxy_audit.py` | Every function carries the required tag set; every enum value, struct member and macro carries a doc comment; the file-header block and `@param` direction brackets follow the style guide |
| `scripts/checks/check_doc_attachment.py` | A block actually describes the symbol it is attached to, which the tag audit cannot see |
| `scripts/checks/check_markdown_references.py` | Every link, anchor and repository path named in first-party Markdown resolves |

The first runs in the `pre-commit-checks` gate in `scripts/ci/gates/checks.sh`,
the second in the `doc-attachment` gate in
`scripts/ci/gates/checks_standalone.sh`, and the third in
`scripts/ci/gates/hygiene.sh`.
Each takes `--selftest`, which proves the detector fires and stays quiet in
both directions before any tree scan is trusted.

```sh
python3 scripts/checks/doxy_audit.py --check
python3 scripts/checks/doxy_audit.py --members --check
python3 scripts/checks/doxy_audit.py --style
python3 scripts/checks/check_doc_attachment.py --check
python3 scripts/checks/check_markdown_references.py
```

Run with no arguments, `doxy_audit.py` writes a tag-coverage report to
`build/reports/doxygen/` instead of gating.

## The `just docs` recipes

`just/docs.just` no longer builds anything. What it carries now is reporting:

```sh
just docs::clean          # remove build/docs
just docs::record_stats   # refresh the historical HAL completion summary
just docs::sizes          # summarise app .text/.data/.bss footprints
just docs::audit_init     # audit peripheral init order across apps
```

Those run `scripts/report/roadmap_stats.py`, `scripts/report/app_sizes.py` and
`scripts/checks/audit_init_order.py` respectively.

## Workflow when you add a symbol

1. Write the full Doxygen block per `docs/STYLE_GUIDE.md`.
2. Run `doxy_audit.py --check` and `--members --check` over the tree, and
   `check_doc_attachment.py --check` if the block is doing anything subtle.
3. If you named a file or a path in Markdown, run
   `check_markdown_references.py`, which resolves every one of them.

There is no rendered page to open and no warnings log to tail. If a reviewer
needs to read the contract for a function, they read the block above it.

## Residue

A DoxygenLayout.xml under docs/ was left behind by the dismantle and was
referenced by nothing; it is deleted alongside this rewrite.
