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
  records under `docs/SOUP/`, and the dated decisions under `docs/adr/` (see
  [`docs/README.md`](README.md)). Qualification planning, verification, and
  accomplishment documents live in the `RA8FW` YouTrack knowledge base under
  `RA8FW-A-2`, and CI gate manuals live under `docs/` in `bsikar/ra8ci`.
- **Per-app `README.md`** beside each example and product.

## What enforces it

Three documentation gates live in `bsikar/ra8ci`, all runnable locally without a
doxygen binary:

| Gate (in `bsikar/ra8ci`) | What it asks |
|---|---|
| `scripts/checks/doxy_audit.py` | Every function carries the required tag set; every enum value, struct member and macro carries a doc comment; the file-header block and `@param` direction brackets follow the style guide |
| `scripts/checks/check_doc_attachment.py` | A block actually describes the symbol it is attached to, which the tag audit cannot see |
| `scripts/checks/check_markdown_references.py` | Every link, anchor and repository path named in first-party Markdown resolves |

Those Markdown and Doxygen gate scripts (`doxy_audit.py`,
`check_doc_attachment.py`, `check_markdown_references.py`) moved to
`bsikar/ra8ci` alongside `scripts/ci/gates/checks.sh`,
`scripts/ci/gates/checks_standalone.sh`, and `scripts/ci/gates/hygiene.sh`. Each
takes `--selftest`, which proves the detector fires and stays quiet in both
directions before any scan is trusted:

```sh
# In bsikar/ra8ci:
python3 scripts/checks/doxy_audit.py --check
python3 scripts/checks/doxy_audit.py --members --check
python3 scripts/checks/doxy_audit.py --style
python3 scripts/checks/check_doc_attachment.py --check
python3 scripts/checks/check_markdown_references.py
```

Run with no arguments in `bsikar/ra8ci`, `doxy_audit.py` writes a tag-coverage
report to `build/reports/doxygen/` instead of gating.

## The `just docs` recipes

`just/docs.just` in `ra8-firmware` no longer builds anything. What it carries
now is reporting:

```sh
just docs::clean          # remove build/docs
just docs::record_stats   # refresh the historical HAL completion summary
just docs::sizes          # summarise app .text/.data/.bss footprints
just docs::audit_init     # audit peripheral init order across apps
```

Those run `scripts/builders/roadmap_stats.sh`, `scripts/report/app_sizes.py`,
and `scripts/checks/audit_init_order.py` respectively.

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
