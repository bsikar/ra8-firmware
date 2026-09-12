# Plan: Local workflow harness prototype

Everything under a heading is freeform until the first metadata bullet.
This paragraph is body text and is carried into the emitted issue body.

## Config
- statuses: Needs you, Ready, In flight, Bench-blocked, Landed
- tracks: C6 wireless, Bench + infra, CI health, Codebase, Product

## Epic: harness-core -- Opt-in local workflow harness
The umbrella for the prototype. Composes existing workspace tooling; it
replaces nothing and owns no validation.
- labels: priority:P2, epic:harness-core, area:scripts
- priority: P2
- track: Codebase
- status: Ready

### Issue: harness-parser -- Parse and validate the notes schema
Collect every violation in one pass rather than stopping at the first.
- labels: priority:P2, epic:harness-core, area:scripts
- priority: P2
- track: Codebase
- status: Ready
- estimate: 1d

### Issue: harness-start -- Derive and preview a task worktree
Dry run by default; creation needs an explicit flag.
- labels: priority:P1, epic:harness-core, area:scripts
- priority: P1
- track: Codebase
- status: Ready
- depends-on: harness-parser
- estimate: 2d

### Issue: harness-doc -- Write the contributor document
- labels: priority:P3, epic:harness-core, area:docs
- priority: P3
- track: Codebase
- status: Needs you
- depends-on: harness-parser, harness-start
