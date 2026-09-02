# Workflow automation: `work`

`work` connects a strict notes format to the repository's existing project
board, workspace, and CI policies. It is a supported, gate-tested client, not
another lifecycle authority.

It is strictly opt-in. `just setup`, the devcontainer, and the default CI/
development workflow do not invoke it; the devcontainer remains the supported
toolchain authority for contributors who never install or use this client.
The planner performs deterministic dependency ordering only. It does not
estimate capacity, assign iterations, plan sprints, merge branches, or perform
autonomous remote work.

## Authority boundaries

| Concern | Authority | `work` behavior |
|---|---|---|
| Workspace creation, locking, metadata, release, reaping | `scripts/dev/agent_workspace.sh` | Delegates creation; reads its metadata |
| Gate definitions and verdicts | `scripts/ci.sh` and shared CI monitor | `ready --run-ci` runs exact local CI; `landed` accepts only cached remote PASS |
| GitHub issues and project board | GitHub plus operator review | Emits a script; never runs it |
| Work tracking target/schema | `tools/work/tracker.json` | Pins github.com, repository, project number, Status, and Track names |

No `<git-common-dir>/ra8-work` store exists. Canonical schema-2 records live at
`$RA8_WS_ROOT/.meta/<workspace-name>`. `work` reserves names of the form
`work-<identifier>`, branches of the form `work/<identifier>`, and records
`owner=work`. Status ignores agent-owned and legacy metadata.

The canonical lifecycle is Linux-only. A standard-library guard rejects broad,
symlinked, or foreign roots and opens the lock with `O_NOFOLLOW`, without
truncation. It validates the opened inode, takes a blocking `flock`, and keeps
that descriptor across collision checks, exact ref resolution, worktree
creation, and atomic metadata publication.

Canonical creation refreshes `origin`, runs the safe stale-workspace reaper,
and refreshes its optional timer helper. A failed refresh makes every deletion
candidate retain; cached refs never authorize removal. `work start` previews
all names locally, then repeats decisive checks under the lock.

The reaper removes only registered, real, direct children of its validated
root whose Git common directory matches the upstream. It never falls back to
raw recursive deletion, scans HOME, follows a symlink, or globally prunes a
container runtime. Any uncertainty retains every byte.

## Commands

### `work doctor`

Checks Python, Git, jq, repository discovery, the canonical workspace script,
workspace-root writability, `gh`, and GitHub authentication scope. It changes
nothing. The `gh auth status` check may make a read-only API request, so this
is not an offline command.

### `work plan NOTES`

Parses notes and prints a summary by default. Optional outputs are:

```sh
work plan NOTES --json PATH
work plan NOTES --json -
work plan NOTES --summary
work plan NOTES --emit-commands
```

`--summary`, `--emit-commands`, and `--json -` are mutually exclusive stdout
artifacts. `--json PATH` may stand alone or accompany one stdout artifact; its
completion notice goes to stderr, so stdout remains machine-consumable. File
output is atomic and refuses every existing destination that is not a regular
file, without opening that destination. JSON is deterministic: it contains no
time, host, or source path.

### `work start IDENTIFIER`

Derives `work-IDENTIFIER`, `work/IDENTIFIER`, the canonical metadata path, and
the exact local base commit. The default is a dry run. `--execute` calls the
canonical lifecycle with that commit, branch, and `owner=work`; it does not
create a worktree itself.

### `work status`

Reports work-owned canonical records as `READY`, `STALE`, `FOREIGN`, or
`FORGED`. `READY` requires the exact metadata path, derived branch, derived
worktree path, and Git worktree binding to agree. Recovery lines are shell
quoted and point back to canonical `release` or `forget`; they are advice for
human review, not commands the client executes.

### `work ready IDENTIFIER --run-ci`

Requires a `READY` binding and clean committed tree, then runs exact `just ci`.
It is the pre-push phase and can pass before a remote SHA exists.

### `work landed IDENTIFIER`

Requires a clean `READY` claim whose tree is content-equivalent to cached
`origin/dev`, then asks the shared monitor for that pushed SHA. Only cached
PASS succeeds; UNKNOWN remains UNKNOWN. Branch deletion is a separate explicit
action that first proves content equivalence and never runs automatically.

## Notes schema

The input is untrusted Markdown with one plan and at least one node:

```text
# Plan: <title>

## Config
- statuses: <comma-separated allowed values>
- tracks: <comma-separated allowed values>

## Epic: <key> -- <title>
<body>
- labels: priority:P0, epic:<key>, ...
- priority: P0
- track: Codebase
- status: Ready

### Issue: <key> -- <title>
<body>
- labels: priority:P1, epic:<enclosing-epic>, ...
- priority: P1
- track: Codebase
- status: Ready
- depends-on: <key>, ...
- estimate: <text>
```

Keys match `^[a-z0-9][a-z0-9-]{0,62}$`. Heading separators are exactly
` -- `. Every epic and issue requires Status, Track, Priority, exactly one
matching `priority:P0..P3` label, and exactly one matching `epic:` label.
Issues inherit an ordering edge from their enclosing epic. Explicit
dependencies must exist, may not refer to the node itself, and must be
acyclic. A zero-node document is invalid.

Line feed separates Markdown structure. Every other C0 control, DEL, and C1
control is rejected before parsing, including tab, carriage return, NUL, and
escape. Unicode line and paragraph separators and Unicode's bidi-control
property are rejected too; ordinary multilingual text remains valid. Atomic
fields such as titles, labels, and board values cannot contain line feed
either. Multi-line body prose retains only its structural line feeds. The same
validation runs at each renderer boundary so a directly constructed `Plan`
cannot put terminal controls in summary, JSON, or shell artifacts.

Ordering uses Kahn topological sort with a lexicographic tie break. This makes
the JSON and emitted issue order deterministic while guaranteeing that epics
and dependencies precede their consumers.

## Emitted GitHub script

The emitted script is not executed by `work`. Before the first `gh issue
create`, it:

1. pins `github.com`, the exact repository, owner, and project number from
   `tools/work/tracker.json`;
2. requires issues enabled, repository create permission, and project update
   permission before the first issue is created;
3. discovers every field, option, and label before mutation; and
4. stops on every permission, identity, missing, duplicate, or mismatch.

Operator-supplied and notes-supplied strings reach jq through `--arg` and the
shell through data quoting. Every mutation writes a BEGIN and result row to a
private deterministic recovery ledger. Partial failure prints that ledger and
manual resume/cleanup guidance and never auto-deletes. An existing ledger
refuses a blind rerun, preventing duplicate issue creation.

That ledger lives at `$HOME/.ra8-work-recovery/<plan-id>`. The anchor is the
operator rather than the working directory on purpose: a `$PWD`-relative ledger
refuses a second run from the same directory and silently creates a duplicate
set of issues from any other one, which is precisely the outcome the guard
exists to prevent. Because the anchor is load-bearing, the script refuses
rather than guessing whenever it cannot trust it, always with exit code 2:
`HOME` unset or empty, `HOME` relative (which would put the anchor back under
the working directory), `HOME` naming something that is not an existing
directory, or a ledger root that is a symlink or not a plain directory. It
creates the root mode 0700 and the per-plan directory mode 0700, and never
chmods a root it did not create -- the mode of an existing root is the
operator's own. The rerun signal is the per-plan directory alone, so an
already-present root is reused without complaint, and a rerun refusal exits 1.

Read `<plan-id>` precisely: it is a digest of the exact plan bytes, so it
identifies one rendering of one notes file and not "this piece of work". Edit
the notes -- even to fix a typo in a body -- and the plan id changes, the guard
sees no ledger, and running the new script creates a **second complete set of
issues** for the same work. The guard defends against rerunning the same
script; it cannot defend against re-emitting an edited one. Before running a
regenerated script, check `$HOME/.ra8-work-recovery/` for an earlier ledger of
the same plan and reconcile by hand.

## Testing and recovery

The registered `work-harness` gate runs the offline unittest suite, requires a
non-vacuous discovery floor, syntax-checks the canonical Bash lifecycle,
executes emitted commands against a fake `gh`, invokes the real Just facade
with hostile path data, runs the destructive lifecycle selftest, and
shellchecks the generated operator script. The suite uses normal
`unittest.TestCase` assertions; a
narrow Ruff PT009 and PT027 policy entry records that intentional
standard-library test style. PT009 would replace `TestCase` assertions, while
PT027 would replace `assertRaises`; both alternatives require pytest syntax
in the portable unittest-only gate.

Tests use temporary repositories, homes, hook paths, workspace roots, and fake
GitHub commands. They do not use a token or contact GitHub. Concurrency tests
prove callers block and re-evaluate under one lock. Forged reserved metadata
makes status non-zero; Git routing/helper/config attacks, hostile lock objects,
failed fetches, and symlink traversal all have must-refuse/retain coverage.
