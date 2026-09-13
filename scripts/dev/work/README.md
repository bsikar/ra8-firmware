<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# scripts/dev/work

`work` is the repository workflow client. It validates notes, emits a
reviewable GitHub issue-and-board script, and provides identifier-bound views
of the canonical agent workspace lifecycle.

This is an opt-in local client. It is not part of `just setup`, does not
replace the devcontainer/toolchain path, and is not a sprint, merge, or remote
mutation bot. Contributors who never invoke `just work` are unaffected.

It does not own a second worktree implementation. `work start --execute`
delegates to `scripts/dev/agent_workspace.sh`, whose lock, metadata, reaper,
and release behavior remain authoritative. `work status`, `work ready`, and
`work landed`
read only schema-2 records under `$RA8_WS_ROOT/.meta` whose `owner` is `work`.
They never adopt an unrelated workspace.

The client performs no GitHub mutation. Its board commands are strict read-only
views: they allow only the exact `gh project item-list` and `gh issue view`
argument vectors documented below, pin `GH_HOST=github.com`, remove `GH_REPO`,
and use the repository/project target from `tracker.json`. `work plan
--emit-commands` prints a POSIX shell script for an operator to review and run.
That script proves the repository, project, fields, options, and labels before
its first mutation. Permissions and all field/option/label discovery complete
before mutation; the emitted script records partial work without auto-deleting
anything.

Plan notes reject C0 controls other than structural line feed, plus DEL and C1
controls, Unicode line/paragraph separators, and Unicode bidi controls before
any artifact is rendered. Ordinary multilingual text remains valid.
`--summary`, `--emit-commands`, and `--json -` are mutually exclusive stdout
artifacts. `--json PATH` can also write the deterministic file while one of
the other two modes owns stdout; an existing nonregular destination is never
opened or replaced.

Local commands can make read-only network calls: `work doctor` runs `gh auth
status`, and canonical workspace creation refreshes `origin` before reaping.
Fetch failure retains every candidate instead of trusting cached refs. Those are
properties of the shared lifecycle, not hidden behavior of a second client.

```sh
just work::doctor
just work::board_focus
just work::board_quick_wins
just work::board_track "Codebase"
just work::board_epic "epic:Harness"
just work::board_issue 742
just work::plan_summary scripts/dev/work/tests/fixtures/valid_notes.md
just work::start 742
just work::start_execute 742
just work::status
just work::ready 742
just work::landed 742
just work::test
```

The board recipes query only open Issue items in the configured repository.
`board_focus` highlights
the project statuses that deserve attention, `board_quick_wins` shows only
explicit `Ready` plus `effort:S` candidates, and the Track/Epic commands filter
without changing the board. `board_issue` combines the open-project snapshot
with native parent, sub-issue, blocked-by, and blocking relationships. These
are evidence views, not scheduling or closure decisions.

`work start` is a mutation-free preview. `work ready` runs exact local CI on a
clean committed claim before the sole push. `work landed` is the separate
post-push phase: content-equivalent `origin/dev` plus cached remote PASS. It
never polls GitHub.

The command reference, notes schema, trust boundaries, and recovery model are
in [docs/WORKFLOW_AUTOMATION.md](../../../docs/WORKFLOW_AUTOMATION.md).

The offline tests intentionally use stdlib `unittest`. The narrow Ruff PT009
and PT027 exemptions preserve `TestCase` assertions and `assertRaises` without
requiring pytest in this portable gate.
