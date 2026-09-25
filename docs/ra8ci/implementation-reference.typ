#pagebreak()
= Implementation reference

This section closes the gap between the architectural intent and an executable engineering handoff. It is normative for the implementation lane unless it conflicts with `CLAUDE.md`, the machine-readable inventory, or a later owner decision. It does not authorize a live infrastructure or board mutation.

== How to read this reference

Every component is assigned exactly one maturity label:

#table(
  columns: (0.9fr, 3.7fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Label*], [*Meaning*]),
  [`CURRENT`], [Present in the implementation branch and covered by at least focused tests. It is not necessarily production-ready.],
  [`TARGET`], [Approved behavior that still has to be implemented or wired.],
  [`BLOCKED`], [Implementation must fail closed until a named operator input, physical proof, or external credential exists.],
  [`REFERENCE`], [Existing repository behavior to preserve during migration; it is not automatically the final implementation.],
)

The repository is the executable source of truth. This PDF supplies boundaries and contracts; these machine-readable files supply exhaustive item-level input:

- `tools/ra8ci-script-inventory.json`: every in-scope helper, its references, and verdict.
- `tools/ra8ci-work-units.json`: planner input and dependency information.
- `tools/ra8ci/catalog/tasks.json`: exact task declarations embedded in the current binary.
- `tools/ra8ci/migrations/*.sql`: exact database DDL and constraints.
- `tools/ra8ci/internal/protocol/protocol.go`: current agent wire structs and validation.

An engineer or AI must read those files before changing the corresponding subsystem. It must not infer a script mapping, database column, or protocol field from prose when a machine-readable definition exists.

== As-built snapshot

This snapshot describes the `ci/ra8ci-implementation` code at `125f026ce4b596e7530c2743444f7ad07a78eaf9`, rebased onto `origin/dev` at `71feca26c2583545df7e06ee7c1e653e0a9d8025` on 23 September 2026. All 83 ra8ci commits replayed without conflict; the rebased feature branch was pushed. This document refresh is layered on that code snapshot. The branch includes the integrated `ci/orchestrator` work; the source branch is closed.

#table(
  columns: (1.25fr, 3.35fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Item*], [*State at this snapshot*]),
  [Go surface], [215 tracked Go files under `tools/ra8ci`, including tests and the server, agent, executor, store, board, scaler, GitHub, provisioning, and analytics packages.],
  [Database], [Forward-only PostgreSQL migrations `0001` through `0019`; typed run, task, step, lease, runner, event, audit, HIL observation, and operation evidence exists in code. This is not evidence that a production database is deployed.],
  [Task catalog], [84 reviewed task definitions. 18 task definitions contain a native `ra8ci:` step; 66 remain transitional wrappers or external-command tasks. The native count is a direct catalog count, not a weighted completion percentage.],
  [Tracked helpers], [532 `.sh`/`.py` files under `scripts/` and 28 `just/*.just` files remain. The Phase 0 JSON inventory still reflects its earlier baseline and must be reconciled before planner batching.],
  [Latest dev infrastructure baseline], [Dev commits `b7c0f7eb0` and `71feca26c` harden disposable Linux/Windows Proxmox CI, Windows Server Core/WSL2 provisioning, log/status handling, and cleanup. The Linux full run was stopped before completion; Windows has no verified full run.],
  [Completed checker ports in this lane], [Five checker responsibilities now have native Go task implementations and have their old scripts removed in the same change: driver-assembly guard, goto/setjmp guard, GNU attribute guard, assert-casts, and C23 nullptr-only guard.],
  [Server and agent], [CLI, local executor, offline receipt spool/sync, authenticated HTTP server, PostgreSQL store, agent claim/log/result flow, deadlines, cancellation, and resource facts are implemented and tested.],
  [Analytics], [PostgreSQL timings and resource facts support report queries. Further report/API coverage and real production data are still required before scheduling tiers are tuned from evidence.],
  [GitHub control], [Scale-set client, durable inbox, policy, controller, and scaler state-machine packages exist. Critical gap: `serve()` does not compose the production controller, scaler handler, concrete guest bootstrapper, runner observer, or Terraform runtime.],
  [Provisioning], [Terraform control/runner definitions, a Terraform runner provisioner package, and the service Ansible role exist. The new dev lab path is a separate legacy baseline: `just infra::lab::ci` launches the detached server runner; `just infra::lab::ci-terraform` uses the local Terraform/Ansible driver. Production ra8ci Linux/Windows readiness and one-use JIT bootstrap are incomplete; no control-VM plan was applied.],
  [Board and HIL], [Durable lease/session/checkpoint/recovery primitives, API/client packages, neutral receipt primitives, and HIL timing policy exist. Production board-agent enrollment/device observation and `bench.sh` cutover are not complete.],
  [Deployment], [No control VM or production PostgreSQL has been created. Proxmox/GitHub credentials, approved storage/network values, and an off-VM backup destination remain operator inputs.],
  [Acceptance], [After the rebase, `GOWORK=off go test ./...`, `go test -race ./...`, `go vet ./...`, and static Linux-amd64/Windows-amd64 builds all exited 0. Logs and build artifacts are under `/home/bsikar/ra8-verify/ra8ci-implementation/pdf-handoff-20260923/`. The current dev full Proxmox CI runs are not verified: Linux has no final exit code and Windows has not completed. No live ra8ci GitHub dispatch, control-VM deployment, restore drill, Windows runtime, or hardware acceptance is claimed.],
)

There is no defensible single project-completion percentage: one native checker port and a production dispatch controller are not comparable units. Use the status table below, the per-file JSON inventory, task catalog, and actual exit-code evidence instead. The earlier conversational estimate of roughly 85 percent remaining was deliberately approximate, not a release metric.

== Implementation status and remaining work

The target architecture remains approved. The rows below separate code that exists from acceptance that is still required. `SUBSTANTIAL` means useful implementation exists, not production-ready; `INCOMPLETE` and `NOT DEPLOYED` are explicit non-completion states.

#table(
  columns: (1.05fr, 1.05fr, 2.95fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Workstream*], [*Status*], [*Remaining work and proof required*]),
  [CLI and task execution], [SUBSTANTIAL], [Finish semantic task coverage, strict task arguments, output/artifact contracts, and parity for every migrated behavior. Keep safe local tasks usable offline; prove sync receipts are idempotent and never become CI attestations.],
  [PostgreSQL and analytics], [SUBSTANTIAL], [Complete API/report surfaces and query tests for queue, step, resource, board, and runner timings. Prove append-only audit privileges, concurrency, retention, schema upgrade, and performance with representative disposable PostgreSQL data.],
  [GitHub scale-set control], [INCOMPLETE / FAIL-CLOSED], [Compose the production GitHub session/controller in `serve()`; add fixed reviewed config, singleton-controller fencing, durable reconciliation loops, concrete metadata resolution, guest bootstrap, runner observation, cancellation, and graceful shutdown. Until then the server must advertise zero managed capacity.],
  [Disposable VM dispatch], [INCOMPLETE], [Resolve Terraform protection/identity/state contracts and implement Linux and Windows Ansible readiness. Deliver one-use JIT data through a protected channel only after reservation-bound guest proof. Test lost acknowledgments, restart, cancellation, busy drain, runner deregistration, and identity-checked cleanup on disposable VMs.],
  [Board and HIL], [INCOMPLETE / PRODUCTION-DISABLED], [Implement and enroll the persistent board agent with key rotation, real device adapters, and independently verified neutral/restore proof. Integrate checkpoints, human > CI > AI ordering, yield/requeue, expiry, and recovery. Shadow then cut over from `bench.sh` without two live authorities; run emulator first and only one matching hardware case under lease.],
  [Scripts and recipes], [EARLY], [Rebuild `tools/ra8ci-script-inventory.json` and `tools/ra8ci-work-units.json` from the current tree. Delete only evidenced dead files in planner-owned units. Port each used helper's behavior to a native task/package or explicitly retain it as provisioning/out-of-scope; repoint callers in the same change. 532 script files and 28 Justfiles remain as raw counts, not a promise to port every file.],
  [Just, hooks, and workflows], [EARLY], [Repoint the remaining recipes and hook entries to semantic ra8ci task names. Keep GitHub's official Actions runner for workflow and `uses:` semantics; switch `runs-on` demand to ra8ci-managed scale sets only after end-to-end capacity acceptance. Remove old dispatch entry points only after parity.],
  [Control VM and operations], [NOT DEPLOYED], [Obtain approved least-privilege Proxmox and GitHub credentials, node/pool/template/storage/bridge/network inputs, and encrypted off-VM backup target. Plan the VM in the 9000+ range, review before apply, configure TLS/firewall/monitoring, and complete a restore drill before starting dispatch.],
  [Security and deployment acceptance], [INCOMPLETE], [Test workflow admission, RCE boundaries, secret/JIT non-disclosure, Windows service ACLs, Linux process isolation, network denies, agent identity/fencing, database outage behavior, disk/backup/certificate alarms, recovery and rollback. Record operator approval; tests alone do not authorize live infrastructure changes.],
  [Verification and integration], [INCOMPLETE], [Complete the required coverage threshold and all supported platform/runtime tests without skips. Run Terraform/Ansible validation, isolated Linux and Windows scale-set lifecycle acceptance, and the board emulator-then-single-hardware lane. The planner defines the MR stack; do not merge to `dev` without Brighton's approval.],
)

=== Remaining-work ledger and completion evidence

These stable keys are for status discussions and implementation reports; they do not replace planner-owned issues or the machine-readable per-file work-unit list.

#table(
  columns: (0.55fr, 1.35fr, 3.1fr),
  inset: 4pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Key*], [*Work unit*], [*Done only when*]),
  [R0], [Inventory reconciliation], [Current script, tool, infra, helper, Just, workflow, hook, Terraform, and Ansible rows are scanned and classified; absorbed files and caller links no longer point to deleted behavior; planner receives current JSON.],
  [R1], [Executor and evidence], [Every required task has a reviewed schema, deadline, ordered steps, host facts, durable result/log evidence, offline behavior where allowed, and parity tests; no duplicate old implementation remains.],
  [R2], [Server production composition], [A reviewed config builds exactly one fenced scale-set controller, handler, concrete provisioning/bootstrap dependencies, and reconcile/shutdown loops. Missing credential, bootstrapper, observer, audit, or database fails closed and advertises zero capacity.],
  [R3], [Linux/Windows guest lifecycle], [First complete the legacy baseline below, then prove each ra8ci OS path binds VM identity to reservation and Ansible readiness before one-use JIT delivery. Each real isolated job drains, deregisters, and cleans up safely across cancellation, restart, and lost acknowledgments.],
  [R4], [Board agent and cutover], [The board agent proves physical neutral state and recovery context; lease, yield, priority, and human wait semantics pass concurrency/failure tests. Emulator passes before a single matching leased hardware run, then old file/flock authority is retired.],
  [R5], [Long-tail helper absorption], [All used in-scope helpers are ported or have an explicit retained-owner rationale; dead items are deleted only from evidence. Caller changes and deletion land together, with no old/new overlap.],
  [R6], [Repository entry points], [Just recipes, hooks, and workflow jobs invoke the same semantic task names. Third-party Actions remain on the official runner; ra8ci-managed labels are enabled only after R2/R3 acceptance.],
  [R7], [Control VM and recovery], [Operator-approved infrastructure values exist; the persistent VM is applied only after review; PostgreSQL, encrypted off-VM backups, TLS, monitoring, restore, and rollback drills pass.],
  [R8], [Release verification], [Required static/unit/race/coverage/database/platform/security/CI gates and non-skipped end-to-end acceptance pass with logs and exit codes. Production readiness and owner approval are recorded before external cutover.],
)

== Integration takeover handoff

This is the start-here section for the next engineer or AI taking over integration. It separates checked-in code from observed acceptance, gives the exact existing entry points, and names the prerequisites that must not be guessed. The handoff does not authorize a secret change, a Proxmox apply, a GitHub App change, a board operation, or a `dev` merge.

=== Repository and branch identity

#table(
  columns: (1.25fr, 3.35fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Item*], [*Known state*]),
  [Repository], [`bsikar/ra8-firmware`. Work only in `/home/bsikar/worktrees/ra8ci-implementation` on the dev box over `ssh dev`; never touch `/Users/bsikar/Documents/github/ra8-firmware`.],
  [Feature branch], [`ci/ra8ci-implementation`. The code snapshot before this doc refresh is `125f026ce4b596e7530c2743444f7ad07a78eaf9`, pushed after rebasing.],
  [Base], [`origin/dev` at `71feca26c2583545df7e06ee7c1e653e0a9d8025`; 83 ra8ci commits are replayed above it. `dev` is not this feature's merge target; Brighton controls the final integration.],
  [Commit identity], [`Brighton Sikarskie <bsikar@tuta.io>`. Keep commits free of AI attribution.],
  [Build/test evidence], [Post-rebase Go tests, race tests, vet, and Linux/Windows static builds exited 0. Current-runner acceptance is still incomplete; a Go package pass is not a Proxmox VM pass.],
  [Output root], [Put every build and test output under `/home/bsikar/ra8-verify/ra8ci-implementation/<unique-unit>/`; do not use a shared or fixed `/tmp` build directory.],
  [Hardware rule], [The board is single-holder. For any later HIL change, run the matching emulator case first, then at most one matching hardware case under a lease. Do not run a full hardware sweep.],
)

The detailed machine contracts remain in the repository: `tools/ra8ci/catalog/tasks.json`, `tools/ra8ci/migrations/`, `tools/ra8ci/internal/protocol/protocol.go`, `tools/ra8ci-script-inventory.json`, and `tools/ra8ci-work-units.json`. The inventory is stale relative to the current tree and must be regenerated before using it as an exhaustive deletion or migration plan.

=== What dev added and what it proves

The two relevant dev commits are `b7c0f7eb0` (`infra: harden disposable Proxmox CI runners`) and `71feca26c` (`WIP: continue Proxmox Linux and Windows ephemeral CI`). They change the existing disposable lab path: Linux guest setup and cleanup, a substantially expanded Windows Server Core/WSL2 playbook, Proxmox server-runner lifecycle and streamed logs, status/list/stop behavior, Windows-safe trusted Git/environment parsing, and a password variable named `RA8_LAB_WINDOWS_PASSWORD`.

The WIP commit reports that Linux reached the CI gate phase after fixing source-snapshot and rootless-container temporary-filesystem issues, but the run was intentionally stopped before completion. It has no final Linux exit status. Windows has not completed a verified full run. Infrastructure syntax/format checks passed, but the detached launch with the Windows password configured has not been verified. Do not translate any of these statements into an end-to-end pass.

=== The two lab commands are different implementations

#table(
  columns: (1.35fr, 3.65fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Recipe*], [*Actual path and what it validates*]),
  [`just infra::lab::ci PROFILE`], [`just/infra_lab.just` -> `scripts/dev/proxmox_lab_manage.py` -> SSH upload of a source archive and runner -> detached `sudo -n nohup` launch of `scripts/dev/proxmox_lab_server_runner.sh` on Proxmox. The server runner uses direct `qm` lifecycle calls plus Ansible and applies the `terraform` tag. This is the detached server-runner baseline required first; it is not the local Terraform driver.],
  [`just infra::lab::ci-terraform PROFILE`], [`just/infra_lab.just` -> local `scripts/dev/proxmox_lab_ci.sh`; Terraform/Ansible lifecycle runs from the invoking control node. This is a separate implementation. A pass here does not validate the detached server-runner path, and neither command validates ra8ci production dispatch.],
)

For the requested baseline use `just infra::lab::check` first, then `just infra::lab::ci linux` to completion. Follow or reattach to output with `just infra::lab::logs linux` and query `just infra::lab::status`. The server log is `/var/log/ra8-lab/linux.log`; the Windows log is `/var/log/ra8-lab/windows.log`. Copy complete logs and terminal evidence to the unique verification directory on dev. The profile VM IDs are Linux `9000` from template `9001`, and Windows `9010` from template `9011`. Each guest should carry `terraform`, `ra8-lab`, and a unique run tag. Do not run both profiles concurrently.

=== Windows password is a hard gate

The credential no longer travels in the environment, so the question of whether a value survives SSH, `sudo`, and the detached launch no longer arises. `scripts/dev/proxmox_lab_server_runner.sh` takes it from a root-owned file on the Proxmox host, `/etc/ra8-lab/windows-password`, mode `0600`. The runner checks presence as a boolean before the profile starts, refuses to run at all if `RA8_LAB_WINDOWS_PASSWORD` is still set in its environment, and reads the file only at the point of use: the bytes go straight from the file into the run's Ansible inventory without passing through a shell variable, an exported name, or a command-line argument. The inventory is created `0600` and removed by `cleanup()`. `scripts/dev/proxmox_lab_manage.py` gates a Windows start on the same presence probe over SSH, which answers with an exit status and prints nothing about the value.

The operator still owns creating that file on the host; nothing in the repository writes it. Do not put the password in Git, a command-line argument, shell history, Terraform state, a retained Ansible artifact, CI output, or logs. Never dump the environment or process arguments during verification; report only a boolean presence result and the run ID.

None of this has run against a real Proxmox host from an agent box. The presence gate and its refusals are covered by `scripts/dev/proxmox_lab_credential_selftest.sh`; a full Windows provisioning run remains unverified.

=== Required disposable-run sequence and evidence

First run the read-only preflight `just infra::lab::check`. Then run Linux to completion with `just infra::lab::ci linux`; follow or reattach to output with `just infra::lab::logs linux`, and query `just infra::lab::status`. Preserve the complete log and the final runner exit code under a unique verification directory on dev. An interrupted/killed run is unknown, not pass or fail; do not report it as a completed measurement.

After Linux reaches a terminal result, query both `just infra::lab::status` and `just infra::lab::list`. Confirm VM `9000`, its attached volumes, temporary bridge/firewall state, and its runner process are removed, unless `--keep` was deliberately requested for diagnosis. For a retained guest, record who requested preservation and destroy it with the approved destroy recipe after evidence collection. Confirm Terraform-created VMs carry the expected `terraform` tag before teardown.

Only after Linux cleanup and the password prerequisite are proven, run `just infra::lab::ci windows` to completion and follow with `just infra::lab::logs windows`, `just infra::lab::status`, and `just infra::lab::list`. Verify Server Core boots, WSL2 and the intended distro are usable, Ansible installs all declared tools including Go, the scratch `TMPDIR` resolves to the intended filesystem, logs remain visible through long tasks, and VM `9010` plus volumes/processes/network state are cleaned up. Save the full log and exact terminal exit code. For both profiles, test teardown separately for a completed run, a controlled provisioning/task failure, and a deliberate cancellation using `just infra::lab::stop PROFILE`. Do not cancel the required uninterrupted full run to test cleanup. If no safe failure-injection path exists, record that case as untested rather than improvising one. After each case, inspect `just infra::lab::status` and `just infra::lab::list`, then use approved read-only Proxmox inspection keyed to the exact run ID to verify VM, attached volumes, runner process, and temporary network/firewall state are gone. If a failed run remains active, record its run ID and status before requesting stop. If `--keep` was used, remove only that exact retained target with `just infra::lab::destroy TARGET` after evidence is saved.

For each profile record: commit under test, exact commands, start/end times, run ID, Proxmox VM ID and tags, log location, terminal exit code (or explicit unknown), gate failures grouped as infrastructure vs source/policy, cleanup checks, and any retained guest reason. A final `status` showing no active job is not by itself proof of no orphaned volume or process.

=== Failure triage and ra8ci integration order

#table(
  columns: (1.05fr, 3.95fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Classification*], [*Evidence and next action*]),
  [Infrastructure failure], [Missing package/tool, Ansible failure, guest boot/WSL2 issue, network, VM sizing, TMPDIR, absent/opaque logs, or leaked VM/volume/process. Fix the provisioning/runner path, rerun that profile from a clean state, and capture a new terminal result. A source-only finding may not mask an infrastructure failure.],
  [Source or policy failure], [Formatting, static analysis, repository policy, or firmware test findings after infrastructure gates execute correctly. Record the exact gate and output separately. The source issue may remain for this baseline handoff, but do not call the overall CI run green.],
  [UNKNOWN], [Signal, timeout outside a declared task deadline, or lost SSH/log session without server-side terminal evidence. Reattach through `logs` and `status`. If terminal result cannot be recovered, preserve evidence and rerun; interruption is never a pass.],
  [Safety incident], [Guest or cleanup identity differs from the recorded run/tags. Do not destroy by guessed VM ID or name. Stop dispatch, retain evidence, and use the exact run identity and approved operator recovery process.],
)

Only after the legacy baseline has reproducible Linux and Windows results should the ra8ci lifecycle be integrated against it. Keep the handoff phases distinct:

1. Compose the production GitHub controller into `ra8ci server`: trusted fixed config, exactly one fenced controller, durable reconciliation, real guest bootstrap/observer, cancellation, and shutdown. Today `serve()` starts the authenticated API and maintenance reaper but does not start that production controller. The server must continue to advertise zero managed capacity until those dependencies and credentials are present.
2. Integrate the reservation-bound Terraform/Ansible lifecycle for Linux and Windows, including guest identity/readiness proof before one-use runner bootstrap. Preserve audit on retries, lost acknowledgments, cancellation, controller restart, busy drain, deregistration, and identity-checked cleanup.
3. Accept ra8ci dispatch on one isolated Linux job, then one isolated Windows job. Preserve GitHub Actions as scheduler and the official Actions runner as worker; ra8ci manages capacity and our task evidence, not GitHub's job protocol.
4. Once ra8ci parity is proven for a responsibility, repoint its `just` recipe/workflow and delete the superseded dispatch behavior in that same planner-defined change. Never operate old and new authorities for the same responsibility concurrently.
5. Only then expand script/task absorption, production deployment, board-agent enrollment, database restore drills, and fleet cutover. Board work still requires the emulator-first, one-matching-hardware-case rule.

The human handoff is intentionally scoped: this document gives the successor exact baseline paths and acceptance evidence. It does not grant access to secrets, the Proxmox API, a VM apply, GitHub App installation, physical hardware, or authority to merge to `dev`.

== Full-stack ownership map

#table(
  columns: (1.1fr, 1.15fr, 2.65fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Layer*], [*Owner*], [*Contract*]),
  [Developer UX], [`just`], [Stable recipes remain the memorable front door and invoke semantic `ra8ci` tasks.],
  [Task semantics], [`ra8ci` catalog and Go packages], [Names, arguments, tiers, deadlines, host scope, steps, artifacts, retry class, and board policy.],
  [Build graph], [Zig], [Compilation dependencies and parallel build graph. `ra8ci` schedules named work; it does not reproduce the Zig graph.],
  [Workflow scheduling], [GitHub Actions], [Workflow/job graph, UI, checks, and the official runner job protocol.],
  [Capacity control], [`ra8ci server`], [Scale-set demand, durable reservation, guest lifecycle, job correlation, and cleanup evidence.],
  [Job execution], [Official ephemeral GitHub runner], [One job in one disposable Linux or Windows guest. `ra8ci` does not replace Runner.Listener or Runner.Worker.],
  [Task execution], [`ra8ci agent`], [Outbound mTLS claim, exact source/catalog validation, process control, logs, facts, steps, and terminal receipt.],
  [Guest readiness], [Ansible], [OS hardening, pinned official runner and agent install, services, and structured readiness proof.],
  [Control persistence], [PostgreSQL], [Only the server has runtime database credentials; migration and operator roles remain separate.],
  [Board execution], [Persistent board agent], [The sole software owner of J-Link, UART, reset, relay, and power operations after cutover.],
  [Analytics], [`ra8ci server` plus PostgreSQL], [Typed timings, resource samples, queue phases, failures, cleanup, board waits, and reports.],
)

The control VM contains `ra8ci server` and PostgreSQL. Repository-controlled job code never runs there or on the Proxmox host. Disposable guests have outbound access to the GitHub endpoints they need and the authenticated ra8ci relay, but no route or credential for PostgreSQL, the Proxmox management API, OpenBao, or board devices.

== Command-line contract

=== Current commands

#table(
  columns: (1.35fr, 0.7fr, 2.85fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Command*], [*State*], [*Behavior*]),
  [`ra8ci tasks`], [`CURRENT`], [Lists embedded task names, one per line, in manifest order.],
  [`ra8ci tasks --digest`], [`CURRENT`], [Prints the digest of the catalog embedded in this binary, alone and unlabelled, so it can be compared against the digest recorded on an attempt, a HIL claim, or a local outbox record rather than edited out of a sentence.],
  [`ra8ci tasks --json`], [`CURRENT`], [Prints one document carrying the schema version, the digest, and every reviewed definition in manifest order, including the exact argv of every step. It answers what the binary carries; reading `tools/ra8ci/catalog/tasks.json` out of the source tree answers what review changed.],
  [`ra8ci <task>`], [`CURRENT`], [Runs only an embedded safe-local task after checkout/catalog validation. Creates an append-only local outbox record.],
  [`ra8ci sync`], [`CURRENT`], [Uploads schema-v2 finished local evidence over mTLS. Legacy or unverified evidence never becomes a CI pass.],
  [`ra8ci server`], [`CURRENT`], [Runs the authenticated control-plane HTTP server and expired-assignment reaper.],
  [`ra8ci agent`], [`CURRENT`], [Claims one fenced task at a time from the server and streams logs/results.],
  [`ra8ci db migrate`], [`CURRENT`], [Applies forward-only migrations using the separate migration DSN.],
  [`ra8ci report slow`], [`CURRENT`], [Bounded server-side median/p95/max plus clearly unit-labeled Linux load-per-core or Windows CPU-busy context and RAM use; host context is not task-attributed CPU.],
  [`ra8ci run submit --idempotency-key KEY TASK...`], [`CURRENT`], [Submits up to 100 catalog-matched read-only tasks from a clean pinned source snapshot. Retries with the same key return the same run.],
  [`ra8ci run status <id>`], [`CURRENT`], [Fetches an authorized run status over mTLS and verifies the returned run ID, cancellation request, and actor.],
  [`ra8ci run cancel <id>`], [`CURRENT`], [Idempotently records a durable, audited cooperative cancellation request. Queued tasks cancel immediately; assigned tasks stop at the next fenced heartbeat and must return terminal evidence.],
  [`ra8ci run logs <run> <attempt>`], [`CURRENT`], [Pages through currently available attempt logs over mTLS, checking sequence and SHA-256 before writing stdout/stderr.],
  [`ra8ci board status <id>`], [`CURRENT`], [Read-only snapshot through the authenticated board API.],
  [`ra8ci board take <id>`], [`CURRENT`], [Queues a human-priority request with reason and duration, prints request and lease IDs before waiting, and cancels the waiter on interrupted wait where safe.],
  [`ra8ci board cancel <id> <request> <lease>`], [`CURRENT`], [Withdraws only a still-queued waiter using its board/request/lease IDs and authenticated owner identity. If the lease was already granted, cancellation refuses and does not release hardware.],
  [`ra8ci board extend <id> --why WHY --duration DURATION`], [`CURRENT`], [Extends the current user's lease using a private, owner-only token saved after grant; server-side class ceilings and contention rules remain authoritative.],
  [`ra8ci board heartbeat <id>`], [`CURRENT`], [Reports the current holder alive using the owner-only token saved after grant, and prints the server's judgement of that holder's silence. It carries no reason, duration, or claim about the hardware; a returned deadline later than the saved one is refused rather than written down, so a beat can never buy lease time that `board extend` would have had to justify.],
  [`ra8ci board liveness <id>`], [`CURRENT`], [Reads how long a board's holder has been silent without recording a beat. Separate from `board heartbeat` because a beat is a claim about who is alive and only the holder may make it; this verb needs no token and is how an operator tells a crashed holder from a person at a bench.],
  [`ra8ci board recover <id> --plan PLAN --why WHY`], [`CURRENT`], [Hands a board that is waiting in recovery-required or quarantine the identifier of a reviewed recovery plan. The plan is required and never defaulted, and nothing in the tree starts a recovery automatically: what puts a board back in service is a human approving a reviewed hardware sequence.],
  [Board release/checkpoint CLI], [`BLOCKED`], [Release still requires verifier-backed neutral evidence from an enrolled/fenced board agent; checkpoint requires an implemented agent operation and an approved fixture profile.],
)

Current process exit meanings are 0 for command/task success, the child exit for a completed local task, 124 for a task deadline, 130 for cancellation, 2 for CLI usage, and 1 for control-plane or internal failure. The target CLI must replace the generic 1 with stable documented categories without changing an underlying task's meaningful child result.

=== GitHub check-run commands

`ra8ci github` carries fourteen subcommands built for #1481. Thirteen of them change nothing: `publish-check-run` is the only one that writes to GitHub, and it says so in its own documentation. Each reads one JSON document from standard input, writes one to standard output, and takes its mode from the environment rather than from an argument, so moving a deployment onto the merge gate is a deployment change rather than a flag somebody passes.

#table(
  columns: (1.35fr, 0.7fr, 2.85fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Command*], [*State*], [*Behavior*]),
  [`ra8ci github check`], [`CURRENT`], [Establishes and cleanly closes an official scale-set message session with the configured credentials, consuming no jobs. Speaks to GitHub and changes nothing there.],
  [`ra8ci github shadow`], [`CURRENT`], [Prints the check-run configuration this process would publish with: the mode and the declared task-to-job correspondence, read from the environment and the catalog. Speaks to nobody, because the command that shows what is configured must not itself publish a run.],
  [`ra8ci github pull-request`], [`CURRENT`], [Reads `{"number":N}` and reports where one pull request is (head commit, base, state, merged, whether the head is on a fork) and every workflow run Actions recorded on that head, each marked with whether `actions-run` will grade it. It picks nothing: which run is the evidence, and whether the pull request is representative at all, are the operator's judgement. A head carrying no completed run exits clean with `gradable` 0, because "CI has not finished here yet" is the answer somebody choosing pull requests needs.],
  [`ra8ci github evidence-run`], [`CURRENT`], [Reads `{"number":N,"workflow":"Checks"}` and answers which single run on that pull request's head is the evidence, with enough of the head beside it to judge whether the pull request is representative. The workflow is named by the caller and matched exactly: "the only completed run" would grade a commit against its docs workflow on the morning the checks workflow failed to start. A run still executing, a run that concluded without deciding anything, and two decided runs are each their own refusal rather than a guess.],
  [`ra8ci github pull-request-survey`], [`CURRENT`], [Reads `{"workflow":"Checks","pull_requests":[N,...]}` and answers, for the whole set at once, which candidates can carry the evidence: per pull request where it is and either the run `pull-request-evidence` would select there or the refusal in words, with the selectable and unselectable counts beside them. A candidate no run can be selected on is that pull request's answer and exits clean; a pull request GitHub could not be read at all refuses the whole survey, because a failed read says nothing about the candidate. It names the run `pull-request-evidence` would gather, never a second opinion about it.],
  [`ra8ci github actions-run`], [`CURRENT`], [Reads `{"run_id":N,"plane":[...]}`, reads that workflow run's current attempt and its job conclusions through the App, and writes exactly the document `shadow-compare` reads. The plane half passes through verbatim and is never fetched. The attempt travels with the outcomes, because a re-run answers the same run number with different conclusions. A run still executing is refused rather than banked as indeterminate.],
  [`ra8ci github shadow-compare`], [`CURRENT`], [Grades one commit's plane outcomes against the Actions outcomes through the declared correspondence and renders the page the required-check decision is read from. It fetches neither side: which job covers which task is a claim somebody makes, and a command that collected both sides would be making it silently.],
  [`ra8ci github shadow-evidence`], [`CURRENT`], [Grades several pull requests' comparisons and reports, per task, how many commits were observed and graded and which of them agreed, diverged, or conflicted. #1481 holds the gate move until conclusions have been compared over representative pull requests, which `shadow-compare` cannot answer because it grades one commit.],
  [`ra8ci github pull-request-evidence`], [`CURRENT`], [Reads `{"workflow":...,"threshold":N,"pull_requests":[{"number":N,"plane":[...]}]}` and gathers the whole evidence document from pull request numbers: each head, the evidence run on it, and that run's job conclusions, written as exactly the document `shadow-evidence` reads. The plane half is the caller's statement and passes through verbatim, never rewritten to the head it was gathered at; a plane half about another commit is refused, naming the pull request. Every pull request is gathered before anything is written, and one named twice is refused rather than counted twice against the threshold.],
  [`ra8ci github gate`], [`CURRENT`], [Reads `{"branch":"main"}` and reports the status check contexts branch protection requires on that branch today, as exactly the document `required-checks` reads. An unreadable protection is a refusal, never an empty gate.],
  [`ra8ci github required-checks`], [`CURRENT`], [Plans the required-context change from the mode and the currently required list: what to add, remove, keep, and what is foreign to this plane. Speaks to nobody, so an operator can read the plan before the gate is touched.],
  [`ra8ci github evidence-gate`], [`CURRENT`], [Plans the same change from the shadow evidence at a stated threshold rather than from the mode alone, proposing only the tasks the evidence backs and naming every task it withholds with its reason. It never takes an existing gate off. A withheld task is a non-zero exit, and the plan is written first.],
  [`ra8ci github reconcile`], [`CURRENT`], [Reports what one commit already carries for a document of task outcomes: per task, the publish decision and every published run with its identifier, whether this plane posted it, and what it said, meaning its title and an excerpt of its output summary with `summary_truncated` when the excerpt is not the whole body. The summary is reported and never matched on: the decision beside it has no intended summary to compare against, so a run whose title agrees while its summary describes other work is settled here and visible only in this report. It builds only the `checks:read` reader, so the read that decides a write cannot perform one. A conflict is reported rather than refused, and is a non-zero exit.],
  [`ra8ci github publish-check-run`], [`CURRENT`], [Posts one check run per task outcome for one commit, and is the only ra8ci command that writes to GitHub. Nothing is posted before the commit's existing runs are read: a run this plane already published is left alone, a write still in flight is waited for rather than repeated, and a run that disagrees stops the whole document. A write still in flight is a non-zero exit, not an input error. An output GitHub will not accept is refused before an installation token is minted, not posted and rejected: a check run is published with a summary, a summary is held to 65535 characters and a title to 255, and both are counted in characters rather than bytes because that is the unit GitHub states them in. An over-long body is refused and never shortened, because a published summary carries nothing beside it to say it was cut.],
)

Each command writes the next one's input, which is how the evidence is gathered without any command claiming both sides of a comparison:

```
ra8ci github actions-run           < observations.json | ra8ci github shadow-compare
ra8ci github pull-request-evidence < pulls.json        | ra8ci github shadow-evidence
ra8ci github gate <<< '{"branch":"main"}'              | ra8ci github required-checks
```

`pull-request-evidence` is the whole of the first two columns of that work done from pull request numbers. The other three pull-request commands serve the step before it, deciding which pull requests are representative: `pull-request-survey` answers that for a whole candidate set at once, and `pull-request` and `evidence-run` answer the same questions one pull request at a time in more detail. All four select through one function, so a survey cannot name a run the gathering would not use.

Configuration is `RA8CI_GITHUB_SHADOW_CORRESPONDENCE_FILE` (the reviewed task-to-job declaration, a file because it covers an eighty-odd-task catalog and belongs in a diff), `RA8CI_GITHUB_CHECK_RUN_MODE` (absent means shadow), and `RA8CI_GITHUB_CHECK_RUN_REPOSITORY`, whose presence is what turns publishing on. The App credentials are the scale-set ones under the names `env_config.go` already defines, because there is one App and one installation; a second spelling would be a second place for one deployment to disagree with itself.

Each reader mints its own installation token holding one permission: `actions:read` for the metadata and workflow-run readers, `administration:read` for the gate reader, `checks:read` for the reconciler, `pull_requests:read` for the pull-request head reader, and `checks:write` for the publisher alone. No reader can reach the write permission, and the publisher's token is never widened to cover a read. Listing a commit's workflow runs is a method on the existing `actions:read` reader rather than a seventh token, and the pull-request reader does not list runs itself: neither token grows a second permission to save a hop.

=== Target commands

Add only after their server contracts and tests exist:

- `ra8ci run retry|explain <run-id>`.
- `ra8ci board checkpoint|release|queue|doctor`.
- `ra8ci board flash|probe|reset|serial|debug` through allowlisted board-agent operations; never raw remote shell text.
- `ra8ci report critical-path|resource-fit|board`.
- `--json` for stable machine output and `--local` or `--dispatch` only where both routes have identical task semantics.

== HTTP and agent protocol contract

=== Implemented HTTP routes

#table(
  columns: (0.65fr, 2.25fr, 2.1fr),
  inset: 4pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Verb*], [*Path*], [*Purpose*]),
  [`GET`], [`/health/live`], [Process liveness only.],
  [`GET`], [`/health/ready`], [Database, audit write, and schema readiness.],
  [`POST`], [`/v1/runs`], [Admit a catalog-matched run with source identity and idempotency key.],
  [`GET`], [`/v1/runs/{id}`], [Authorized run state.],
  [`GET`], [`/v1/runs/{id}/events`], [Authorized append-only event stream with bounded contiguous cursor pages.],
  [`GET`], [`/v1/runs/{id}/logs`], [Authorized attempt-scoped, bounded, digest-checked log pages.],
  [`POST`], [`/v1/local-runs/sync`], [Durable local evidence ingest, separate from trusted CI runs.],
  [`GET`], [`/v1/reports/slow`], [Bounded repository-scoped performance report.],
  [`POST`], [`/v1/agents/me/claim`], [Authenticated fenced task claim.],
  [`POST`], [`/v1/assignments/{id}/ack`], [Exact assignment/source/catalog acknowledgment.],
  [`POST`], [`/v1/attempts/{id}/logs`], [Gap-free, digest-checked log chunks.],
  [`POST`], [`/v1/attempts/{id}/result`], [Terminal receipt and step evidence.],
  [`POST`], [`/v1/agents/me/heartbeat`], [Host facts, phase, cancel, and yield intent.],
)

Board routes currently include status, holder liveness read, take, waiter cancel, yield, checkpoint, release, extend, holder heartbeat, neutral challenge, agent acknowledgment/observation/unavailable, recovery start, recovery completion, quarantine, HIL attempt claim/completion, HIL observation history, and indivisible segment begin/finish under `/v1/boards/{board-id}`. The holder heartbeat is `POST /v1/boards/{board-id}/leases/{lease-id}/heartbeat` and is authorized to the holder of that exact lease; the operator read is `GET /v1/boards/{board-id}/liveness` and records nothing. A route existing does not make it production-enabled: absence of a neutral verifier, board-agent identity, or approved fixture profile must return unavailable or denied and must be audited.

=== Required API completion

Before stable v1 is declared, generate an OpenAPI document from the actual structs and add contract tests for it. Implement log streaming, enrollment, artifact upload/download, resource batches, operator recovery, drain, and the remaining reports. Run events are implemented with bounded pages, repository authorization, contiguous sequence validation, and JSONL CLI output. Every mutation must define maximum body size, strict unknown-field rejection, authenticated role, idempotency scope, correlation ID, retryability, and reconciliation after an ambiguous response.

HTTP status policy is: malformed 400, unauthenticated 401 at the TLS/API boundary, authenticated-but-forbidden 403 where disclosure is safe, concealed foreign resources 404, stale/conflict 409, expired authority 410, too large 413, unsupported media 415, rate limit 429, and unavailable dependency 503. Error bodies use versioned `application/problem+json`; internal SQL, paths, credentials, and foreign resource existence are never exposed.

=== Agent protocol invariants

Protocol version 1 carries host facts, a task reference, exact catalog digest, recursively verified source identity, assignment ID/version, fencing token, and a relative remaining deadline. Server wall-clock deadlines are never trusted as a way to gain time on another host. The agent validates its local clean snapshot immediately before execution, seeds a local timeout, acknowledges exact evidence, uploads ordered digest-checked chunks, and reports one terminal receipt. A complete success requires every declared step, successful step exits, final sequence, byte counts, and stream digests. A stale fence, missing log, mismatched source, expired certificate, or incomplete step makes the attempt non-successful.

Remote script dispatch remains closed unless `RA8CI_AGENT_TRUSTED_COMMIT` is an exact reviewed commit. This is transitional. The final design executes absorbed task implementations in disposable guests and does not give pull-request code access to the agent's mTLS private key or management credentials.

== Identity and authorization matrix

#table(
  columns: (1fr, 1.25fr, 2.7fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Principal*], [*Credential*], [*Maximum authority*]),
  [Observer], [Short-lived client certificate], [Read authorized runs, reports, and board status.],
  [Submitter], [Short-lived client certificate], [Observer plus admit reviewed non-board tasks for an authorized repository.],
  [Human board user], [Short-lived client certificate], [Request human-priority lease and use allowlisted operations while holding the exact active generation.],
  [CI/AI actor], [Server-correlated job or agent certificate], [Only its configured class; it cannot claim human priority.],
  [Execution agent], [Reservation-bound mTLS certificate], [Claim catalog tasks for its host class and return evidence. No database or Proxmox credential.],
  [Board agent], [Pinned host identity plus receipt signing key], [Acknowledge generations and perform approved physical operations.],
  [Provisioner], [Dedicated control-VM identity], [Only approved Terraform/Ansible lifecycle for reserved pools, templates, storage, and bridges.],
  [Operator], [Separate interactive identity], [Migrations, fixture approval, recovery, exceptional cleanup, and audited credential rotation.],
)

Certificate fingerprints map to `api_principals`; repository-scoped roles live in `api_grants`. The runtime database role may insert audit/event rows but cannot update or delete append-only history, alter identity/grants/fixture profiles/migrations, or execute DDL. Migration and operator credentials never enter the server environment used for normal requests.

#v(0.08in)
== PostgreSQL schema and state machines

=== Migration ledger

#table(
  columns: (0.55fr, 1.55fr, 2.85fr),
  inset: 4pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Version*], [*Migration*], [*Responsibility*]),
  [1], [`initial`], [Runs, task graph, attempts, steps, resource samples, artifacts/logs, boards, GitHub inbox/jobs, lifecycle/external operations, idempotency, events, audit, and API identity.],
  [2], [`board_state`], [Serialized board snapshot and append-only board events.],
  [3], [`board_neutral_challenge`], [Approved fixture profiles, recovery context, and one-use neutral challenges.],
  [4], [`agent_dispatch`], [Durable assignment, fencing, log receipt, and attempt lifecycle support.],
  [5], [`offline_sync`], [Separate append-only local runs and local step evidence.],
  [6], [`runner_vms`], [Immutable runner reservation and external operation intents.],
  [7], [`runner_vm_cleanup_fence`], [Monotonic cleanup-requested fence so replay cannot restart a completed guest.],
  [8], [`runner_vm_exact_digests`], [Exact template/live config digests, permanent scale-set/job uniqueness, runner safety identity, and approved reconciliation.],
)

Migrations are forward-only, sequential, embedded in the binary, protected by a PostgreSQL advisory lock, and recorded in `schema_migrations`. Runtime startup rejects an older or newer schema and rejects a role with forbidden privileges. Never edit an applied migration; add a new numbered migration.

=== Core lifecycle rules

- Run: `queued -> running -> terminal`; a terminal run has an end time and explicit execution, cleanup, and evidence outcomes.
- Task: `scheduled -> running -> succeeded|failed|timed_out|cancelled|preempted|lost|skipped`.
- Assignment: issued, acknowledged, running, then one terminal state. Expired assignments are fenced before retry and cannot leave a run permanently open.
- Board: ready, grant pending, active, yield requested or draining, recovery required, recovering, or quarantined. Exactly one pending/active authority per board.
- Runner VM: reserved, provisioning, ready/registered, draining, stopped, destroyed, or failed/unknown according to the DDL. Cleanup request is monotonic. An unresolved external operation prevents blind replay.
- Audit, `run_events`, `board_events`, local evidence, and operation history are append-only.

Every state transition and external side effect uses intent-before-action: commit the operation identity first, invoke the external system second, then record observed evidence. After connection loss, reconcile by immutable provider identity; never repeat clone, start, flash, stop, destroy, or check publication merely because the client did not receive a response.

== Task catalog and script migration

=== Transitional catalog

#table(
  columns: (1fr, 0.75fr, 0.95fr, 0.65fr, 2fr),
  inset: 4pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Task*], [*Tier*], [*Scope*], [*Deadline*], [*Current step*]),
  [`format`], [required], [local write], [900 s], [`bash scripts/checks/format_tree.sh`],
  [`format-check`], [required], [local read], [900 s], [`bash scripts/checks/format_tree.sh --check`],
  [`lint-go`], [required], [local read], [1200 s], [`just quality::local::gate lint-go`],
  [`test-go`], [required], [local read], [1800 s], [`just quality::local::gate test-go`],
)

These are executor-stage adapters, not finished ports. A completed task implementation no longer shells out to the helper whose behavior it replaces.

=== Catalog schema

Each task declaration has: stable name and version; tier `required`, `optional`, or `nightly`; scope; OS allowlist; capability list; strict arguments schema; total deadline; board policy; ordered typed steps; declared artifacts; bounded retry policy; and resource hints. Board tasks additionally declare cancellation class, maximum safe segment, restore/probe bound, retry-from-baseline rule, fixture profile, and checkpoint positions. Unknown fields, executable paths, environment additions, or arguments fail catalog validation.

The catalog is identified by the SHA-256 of its canonical JSON, and that digest is the identifier the plane records rather than a convenience: it is written onto a HIL claim, checked again when the completion arrives, refused when the recorded catalog differs from the one in hand, and stamped onto every local outbox record. Two definitions of one task name are therefore distinguishable after the fact only through the digest they came from, which is why `ra8ci tasks --digest` and `ra8ci tasks --json` report it from the binary and why the document carries it beside the definitions rather than apart from them.

=== Exhaustive migration rule

The Phase 0 inventory captured 588 helper rows at its historical `ci/orchestrator` snapshot: 31 dead and 557 used. It is no longer an exact picture of the implementation branch: dead files were removed and five used checker responsibilities were absorbed. Recompute the inventory before further planner batching. A used file is not necessarily a public task: entry points become catalog tasks; shared parsing/model code becomes internal Go packages; Terraform and Ansible remain the provisioning layer; substantial firmware/deliverable programs remain out of scope.

For every used file, the migration ledger must record:

`old_path`, all callers, responsibility, new task or package, task version, host/OS, old success/failure/timeout/artifact/selftest fixtures, new parity proof, caller changes, deletion change, rollback boundary, and state.

The safe order for each responsibility is: capture golden behavior; implement the Go replacement; run both against non-hardware fixtures; repoint every caller; delete the old helper in the same change; run the registered gate; and never enable old and new implementations concurrently. HIL parity runs emulator first and exactly one matching hardware case under a lease.

== GitHub Actions integration

=== Approved model

GitHub remains workflow scheduler and check UI. The official ephemeral runner executes `run:`, JavaScript, composite, container, and third-party `uses:` actions. `ra8ci` uses the pinned `github.com/actions/scaleset` v0.4.0 client to listen for scale-set demand, persist messages before acknowledgment, apply trusted admission policy, reserve capacity, and reconcile guests. It does not implement the private Actions worker protocol.

=== Required GitHub App configuration

`TARGET`: accept explicit App ID, installation ID, private-key file, repository identity, runner scale-set ID, maximum capacity, approved workflow paths and refs, events, job names, and labels. Use `https://api.github.com` and validate dynamically returned Actions service/queue HTTPS hosts against GitHub's documented domains. Disable proxy inheritance for management credentials. Reject GHES or alternate API origins unless a separate reviewed configuration names them.

The App receives only permissions needed to read workflow jobs, manage the repository runner scale set, and publish the dedicated lifecycle check. Private key and tokens live only on the control VM. The official SDK may renew its admin/queue tokens; `ra8ci` still bounds session lifetime, validates returned scale-set identity and URL, and stops advertising capacity on authentication or inbox failure.

=== Durable message and job policy

Persist scale-set ID, session ID, message ID, and normalized nonsecret payload before acknowledging the message. A divergent replay is a conflict. Admission compares repository ID, workflow path and pinned ref, event class, job name, and labels against server-owned policy. Fork PRs and untrusted refs cannot obtain board or provisioner authority. Job text, labels, environment variables, and repository files never become Terraform, Ansible, Proxmox, or host shell arguments.

Runner teardown is permitted only from the durably persisted terminal `JobCompleted` event whose job/request/run/repository/ref and runner ID/name exactly match the reservation, and which contains a nonempty result and finish time. That GitHub terminal event is the no-active-job proof; independently query the configured scale-set administration API, verify the exact runner identity, remove it, then query again and require absence. An already-absent exact runner is an idempotent success. API uncertainty, a mismatched runner, missing completion facts, or continued registration fails closed. VM stop and Terraform cleanup remain separately fenced by fresh drain evidence, distinct cleanup approval, stopped-state identity, and exact preserved Terraform state.

`TARGET`: publish one separate ra8ci lifecycle check per GitHub run/attempt. It remains pending through guest creation, runner registration, execution, evidence, deregistration, and cleanup. Native job success cannot override failed or incomplete lifecycle evidence. An ambiguous Checks API write is reconciled by SHA, check name, App ID, and external ID.

The reconciliation half of that clause is built and in use: `publish-check-run` lists the commit's check runs and decides each intended run before posting any of them, and every run this plane posts carries a derived external identifier so a second reader can tell our run from a run somebody else left under the same name. It diverges from the clause in one respect, deliberately. Ownership is settled by that identifier rather than by App ID: the list endpoint filters on the numeric app ID, this deployment holds the App client ID, and adding a lookup to translate one into the other would put a second identity in the path of a decision the identifier already answers. A run under one of our names carrying no identifier, or somebody else's, is reported as a conflict for an operator to settle rather than published over. The per-run/attempt lifecycle check itself remains `TARGET`: what is published today is one check run per catalog task.

=== JIT and guest correlation

Issue one-use JIT configuration only after the declared guest identity and Ansible readiness proof are verified. Bind it to reservation, scale set, OS, job request, and expiration. The guest agent accepts a task only after server-side correlation of repository, exact head SHA, workflow policy, and official runner identity. Completion revokes the grant and drains the agent. A compromised disposable guest can falsify guest-origin measurements, so those measurements remain labeled; it cannot gain management credentials or authority for another job.

== Terraform and Ansible runner provisioning

=== Mandatory production path

Production guest mutation is `Terraform -> Ansible -> official runner/agent`, behind the scaler's injected `Provisioner` interface. The direct Proxmox Go client is not the production mutation backend. It may provide pinned-CA read-only identity and task reconciliation, and its fake-backed lifecycle tests exercise ambiguity handling. Enabling its clone/start/stop/destroy methods in production requires a separate explicit architecture decision and safety review.

=== Discovered incompatibilities to resolve

#table(
  columns: (1.45fr, 3.25fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Conflict*], [*Required resolution*]),
  [Destroy protection], [Existing lab VM modules use `protection=true` and `lifecycle.prevent_destroy=true`; the old script bypasses Terraform with direct `qm destroy` and `terraform state rm`. Do not reproduce that bypass. Design a reviewed disposable-resource module and tested cleanup/import contract.],
  [Identity marker], [Existing description uses `RA8_LAB_RUN=<16hex>` while the new lifecycle expects reservation and operation UUIDs. Define one immutable marker schema emitted by Terraform and verified everywhere.],
  [Names], [Existing Windows names require `ra8-lab-win-*`; the prototype scaler assumed `ra8-lab-ci-<vmid>`. Define OS-specific naming in one provider-owned function.],
  [VMID and bridges], [Current disposable lab reserves 9000-9099 and exact `vmbr8`/`vmbr9` paths. Persistent control VM is 9100 or greater and must use an independently approved management bridge.],
  [Operation ledger], [Runner VM operations currently name Proxmox UPID evidence. Generalize through a new forward migration to provider kind, operation identity, plan digest, state identity, and reconciliation proof before Terraform is the production backend.],
)

=== Provisioner contract

Inputs are typed values, never a command string: reservation/job identity, OS profile, template ID and digest, node, pool, storage, bridge, VMID/name, immutable marker, source version manifest, and state root. Before invocation, validate every value against an operator-approved allowlist. Use fixed argv, an empty/minimal environment, pinned Terraform/provider versions, `TF_DATA_DIR` and state under a private per-reservation directory, and no repository-controlled extra variables.

Persist operation intent and expected identity; run `terraform init -backend=false`, `validate`, and a saved plan; hash plan, lock file, module tree, variables-without-secrets, and provider versions; then apply that saved plan. Preserve state and logs after any uncertainty. Reconcile actual identity and state before retry. Cleanup requires completed/failed job intent, independently verified runner deregistration and idle state, exact marker/template/pool/storage/bridge/non-template identity, a saved cleanup plan, and explicit outcome. Foreign or ambiguous resources are left intact and raised for operator recovery.

=== Ansible readiness contract

Create `infra/ansible/playbooks/ra8ci-lab-linux.yml` and `ra8ci-lab-windows.yml`. They reuse reviewed connectivity/hardening roles but install pinned, digest-verified official runner and ra8ci binaries as narrow service identities. They accept an exact generated inventory, version manifest, one-use enrollment token, and later JIT config through a protected secret channel. They return structured JSON proof containing reservation marker, OS/arch, binary digests, service accounts, time/skew, disk, network tests, agent enrollment, and runner readiness. A nonzero, warning, changed-when-forbidden, skipped, malformed, or identity-mismatched result blocks JIT and cleanup.

== Control VM deployment

`CURRENT`: `infra/terraform/environments/ra8ci-service` declares a separate persistent VM with an explicit enable gate, VMID at least 9100, reviewed template, dedicated pool, boot/data storage, private management bridge, static network, and separate data disk. The first plan remains stopped unless `start_after_review` is explicitly set. `infra/ansible/ra8ci-service` declares PostgreSQL, separate migration/runtime/operator roles, local socket access, TLS server service, and encrypted pgBackRest scheduling.

`BLOCKED`: no apply may occur until all inputs in the operator registry below are supplied and reviewed. A successful static Terraform validate is not permission to create a VM. No personal Tailscale discovery, Mac keychain credential, guessed Proxmox node, default bridge, or placeholder backup target may be used.

Deployment order is:

1. Record approved inputs and credential owners without secrets in Git.
2. Run Terraform init/fmt/validate and produce a saved plan from the control environment.
3. Review exact VM identity, network, disks, pool, and no repository job execution on the host/control VM.
4. Apply with a dedicated least-privilege Proxmox/OpenBao identity; capture plan and task evidence.
5. Run Ansible in check mode, review changes, then configure PostgreSQL/backups and the unprivileged service.
6. Apply migrations with migration role; start server with runtime role and mTLS.
7. Prove live/ready separation, authorization denial audit, backup freshness, and restore into an isolated VM.
8. Enroll no runner or board agent until its own acceptance gate is complete.

== Board agent, leases, and HIL

=== Authority and physical boundary

The persistent board agent is the only software allowed to touch J-Link, UART, reset, relay, or board power after cutover. Disposable guests, hooks, workflows, and legacy scripts lose direct device/SSH authority. Physical/root access remains outside software fencing and must be an audited operator procedure.

The lease service provides human > CI > AI priority, FIFO within class, one holder, bounded duration, cooperative yield, and crash durability. Priority requests never yank an indivisible operation. An active holder checks yield at declared checkpoints, neutralizes, releases with signed proof, and requeues if work remains. Expiry forbids starting another segment but does not claim that an already-running electrical operation became safe.

=== Holder liveness

`CURRENT`: a lease carries a heartbeat as well as a duration and an expiry, and the two answer different questions. The expiry says when authority ends. The heartbeat says whether the holder is still there, and the server reports that judgement without ever acting on it.

A holder beats with `POST /v1/boards/{board-id}/leases/{lease-id}/heartbeat`, authorized to the holder of that exact lease and refused for a superseded generation before anything is written. The board agent beats on a timer for the length of a HIL attempt and joins that loop before the attempt returns, so an attempt never outlives its own reporting. A human or CI holder beats with `ra8ci board heartbeat`. Anyone authorized to read the board reads the same judgement with `GET /v1/boards/{board-id}/liveness` or `ra8ci board liveness`, which records no beat.

The reporting interval is the server's to set: `defaultHolderHeartbeatInterval` is one minute, a deployment may configure any positive interval up to `board.MaxHeartbeatInterval` of ten minutes, and the server hands the interval it used back with every report so a holder does not have to guess a cadence. `board.HeartbeatGraceBeats` is three, so a holder is reported overdue after three consecutive intervals of silence rather than on the first miss, which would make a dropped packet look like a crash. At the ceiling that is thirty minutes of silence, still inside the shortest class lifetime, so a holder can be reported overdue before its authority ends rather than only after expiry has already answered the question.

*Overdue is reported and never enforced.* A missed beat does not shorten a lease, end a segment, change a phase, or release hardware, and no caller treats it as though it did: an overdue holder still owns its lease with its expiry intact. A beat cannot lengthen a lease either. That is `board extend`, which demands a reason and is held to the class ceiling and the contended ten-minute limit, and a liveness call that quietly bought time would route around both. Silence is evidence for an operator, not authority to withdraw.

What does act on time is expiry, and only expiry. A maintenance pass runs on the server's existing fifteen-second tick, finds leases whose absolute deadline has passed, and asks the board for the reclaim the reducer would have performed anyway on the next command; a lease that turns out not to be expired under the transaction clock is a loud conflict naming the board, never a quietly reclaimed live lease. The pass prints a line only when it found something, and a board reclaimed that way lands in recovery-required, where it waits for a reviewed plan exactly as any other recovery does.

=== Signed neutral proof

`CURRENT`: the neutral receipt format is canonical JSON with domain separation and an allowlisted Ed25519 key ID. It binds challenge ID/nonce, board, lease, generation, snapshot version, agent high-water mark, approved fixture revision/profile/restore policy, recovery plan, issue/expiry, observation time, and fixture evidence digest. The store consumes each challenge once inside the board transition transaction.

`BLOCKED`: production needs a privileged `NeutralObserver` adapter that actually proves process/fd quiescence, no unexpected device owner, correct probe/board serial, approved reset/relay/power state, VTref/sensors where available, and baseline firmware health. No default observer, shell fallback, or connectivity-only probe is acceptable. Key generation, storage, rotation, revocation, and board-agent enrollment must be operator-approved.

=== HIL specification and timing

`CURRENT`: the literal-only Go HIL parser accepts the current allowlisted `HIL_*` assignments without sourcing shell or evaluating interpolation, and its tests parse all 208 current `examples/**/hil.conf` files. It separates observation timeout from flash/restore safety bound.

The timeout decision filters exact comparable successful, evidence-complete, uncensored history. With sufficient samples it uses the conservative historical estimator; with sparse data it uses declared `HIL_TIMEOUT_S`, otherwise 30 seconds. An optional approved maximum clamps learning. Flash/restore bound and recovery margin are independent mandatory inputs for board dispatch. A human yield ETA uses request-to-neutral history, not the firmware observation timeout. Unknown bounds reject automatic board dispatch.

The first live adapter must support allowlisted flash, probe, reset, serial, and debug lifecycle operations with fixed arguments and supervised process groups; record immutable image digest and full arguments; verify unstripped firmware before transformation; restore the recorded baseline after preemption; and quarantine on identity, clock, neutral, recovery, or high-water disagreement.

== RCE and trust-boundary threat model

#table(
  columns: (1.35fr, 1.45fr, 2.3fr),
  inset: 4pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Threat*], [*Boundary*], [*Required control*]),
  [Untrusted PR command], [GitHub message/workflow to control VM], [Server-owned policy maps verified metadata to task IDs. Never pass workflow strings to shell, Terraform, Ansible, or Proxmox.],
  [Agent key theft], [Job process to ra8ci agent], [Different OS identities and ACLs; job accesses only a narrow local task relay. Long-term agent key is unreadable. Disposable identity expires after one job.],
  [Workspace swap], [Runner checkout to agent], [Canonical workspace under dedicated root, no symlink escape, clean recursive snapshot, exact verified head SHA immediately before task.],
  [Credential exfiltration], [Guest to management plane], [No DB/Proxmox/OpenBao credentials in guest; management firewall denies routes; egress allowlist; redact logs and audit access, not values.],
  [Replay after crash], [Server to external systems], [Intent-before-action, permanent idempotency identity, fencing generation, explicit reconciliation, no blind retries.],
  [Foreign VM deletion], [Provisioner cleanup], [Exact marker/name/VMID/node/pool/storage/bridge/template/config digest and independent stopped/idle/deregistered proof. Ambiguity preserves the VM.],
  [Board takeover], [Client/server/board agent], [mTLS role, class fixed by server, pending grant until agent high-water acknowledgment, local monotonic deadline, signed neutral release.],
  [Database privilege escalation], [Runtime process to PostgreSQL], [Separate roles; no DDL; no identity/config mutation; no update/delete of append-only history; startup privilege audit.],
)

The security review must additionally cover denial of service, certificate clock skew, log-volume caps, JSON depth/number handling, archive traversal, artifact digest confusion, submodule replacement, Windows service ACLs, Linux cgroups/namespaces, proxy environment inheritance, DNS rebinding, and dependency provenance. A passing unit test is not proof of host isolation; acceptance includes firewall and service-account tests in disposable guests.

== Configuration and operator input registry

=== Application environment

Current configuration names are: `RA8CI_DATABASE_URL`, `RA8CI_MIGRATION_DATABASE_URL`, `RA8CI_LISTEN_ADDR`, `RA8CI_TLS_CERT`, `RA8CI_TLS_KEY`, `RA8CI_CLIENT_CA`, `RA8CI_SERVER_URL`, `RA8CI_SERVER_CA`, `RA8CI_CLIENT_CERT`, `RA8CI_CLIENT_KEY`, `RA8CI_AGENT_CERT`, `RA8CI_AGENT_KEY`, `RA8CI_AGENT_ROOT`, `RA8CI_AGENT_POLL_WAIT`, `RA8CI_AGENT_TRUSTED_COMMIT`, `RA8CI_REPOSITORY`, and `RA8CI_STATE_DIR`. Proxmox test/prototype code also recognizes `RA8CI_PROXMOX_API_TOKEN`, `RA8CI_RESERVATION`, and `RA8CI_OPERATION`; these do not authorize production direct mutation.

Secrets are read from protected files or a reviewed secret provider, not committed environment templates, command arguments, Terraform state, logs, or audit metadata. Service startup validates file ownership/mode and rejects symlinks where a credential path is expected.

=== Required operator inputs

#table(
  columns: (1.25fr, 2.35fr, 1.55fr),
  inset: 4pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Domain*], [*Required values*], [*Absent behavior*]),
  [Control VM], [Proxmox HTTPS endpoint/CA, node, VMID >= 9100, template ID/digest, name, pool, boot/data storage, management bridge, IP/gateway/DNS, admin keys.], [No Terraform apply.],
  [Credentials], [Least-privilege Proxmox API token or approved OpenBao AppRole path, owner, rotation, and revocation.], [No provisioner.],
  [Backups], [Encrypted off-VM target, credentials, retention, RPO/RTO, restore destination, and drill owner.], [Server readiness fails for production.],
  [PKI], [CA owners, server/client profiles, principal roles, lifetimes, renewal, revocation, and emergency rotation.], [Only local offline tasks work.],
  [GitHub], [App ID, installation ID, private key, repository ID, scale-set IDs/labels, workflow/ref/event/job policy, lifecycle check name.], [Advertise zero capacity.],
  [Disposable guests], [Linux/Windows template IDs/digests, VMID/name pools, node/pool/storage/bridge, runner/agent versions/digests, firewall policy.], [No JIT and no guest creation.],
  [Board], [Board ID/serials, signing key, fixture revision/profile, baseline digest, device topology, neutral predicates, recovery plans, physical-clear owner.], [Mutating board commands unavailable.],
)

These are deployment data, not architecture ambiguity. The implementation must expose a validation report listing each absent item without printing secret values.

== Operations and recovery runbook contract

=== Normal operation

The server starts only after schema and privilege checks. `/live` means process alive; `/ready` means database/audit writable, expected schema, scheduler/reaper healthy, disk below critical threshold, and production backup freshness when production mode is enabled. Drain stops new run assignments, capacity reservations, and board grants while allowing bounded safe work and cleanup to finish.

Metrics and alerts cover run/task/step duration, queue wait, provision/registration/cleanup phases, CPU/RAM/load/I/O with provenance, log lag, dropped samples, agent heartbeat, database connections/WAL/backup age, disk 80/90 percent thresholds, certificate 30-day expiry, stuck cleanup, orphan guests, board recovery, yield overruns, and quarantines. Metrics never contain secrets, source text, or private board data.

=== Backup and restore

Back up PostgreSQL with encrypted pgBackRest full/differential/WAL retention to the approved off-VM destination. Back up only immutable artifact objects and manifests required to resolve database references; never claim a database restore is complete if artifacts or Terraform state are missing. Store per-run Terraform state in protected durable storage with its plan/module/provider/identity proof.

A restore drill creates an isolated control VM/database, verifies checksums and point-in-time recovery, runs migrations only if explicitly part of the drill, starts a non-provisioning server, reconciles agent/runner/board high-water state, and proves no live lease or external operation is silently resumed. Record timestamps, RPO, RTO, missing objects, and operator sign-off.

=== Upgrade and rollback

Drain, back up, verify restore, deploy the new binary, apply forward migration with the migration role, run readiness and protocol compatibility tests, then resume. Database migrations have no automatic down path. Binary rollback is allowed only when it accepts the current schema; otherwise restore to a new isolated database and reconcile. Agents outside the supported protocol window are drained, not assigned undefined work.

=== Incident rules

- Database unavailable: safe local tasks run and remain unsynced; dispatch and board mutation fail closed.
- GitHub unavailable: advertise zero new capacity; retain durable inbox/reservations and reconcile before resuming.
- Proxmox/Terraform uncertainty: preserve state and guest; do not replay or delete until exact identity is observed.
- Agent lost: fence assignment, preserve evidence, retry only according to catalog and idempotency policy.
- Board agent lost or server restored behind agent high-water: recovery required or quarantine; never grant next holder.
- Backup stale, disk critical, certificate expired, or audit unwritable: readiness fails and new external work stops.

== Implementation sequence and definitions of done

The planner still decides issue and MR boundaries. The engineering dependency order is:

1. *Baseline closure.* Commit current code on its dedicated branch only after complete race, integration, coverage, format, static analysis, schema-from-empty, Linux build, Windows cross-build, and PDF checks. Done means no warning, skip, or below-floor coverage is reported as pass.
2. *API and schema stabilization.* Generate OpenAPI, finish events/cancel/enrollment/artifacts/resources, generalize provider operation evidence, and add migration/privilege/concurrency tests. Done means protocol and DDL are versioned and crash-replay tested.
3. *Control VM readiness.* Obtain inputs, plan/apply the persistent VM, configure PostgreSQL/backups/PKI, and pass an isolated restore drill. Done means no repository job can execute there and production readiness observes backup freshness.
4. *GitHub bootstrap.* Configure the App and official scale-set session, durable inbox, admission, JIT delivery, and lifecycle check. Done means lost acknowledgments and controller restart do not lose or duplicate a job.
5. *Terraform/Ansible disposable runners.* Resolve module protection/marker/name issues, implement provider adapter and readiness playbooks, then prove one Linux and one Windows ephemeral job, cancellation, crash reconciliation, deregistration, and identity-checked cleanup.
6. *Board agent.* Implement device adapters and neutral observer, seed approved fixture/recovery profiles, enroll keys, and pass simulator/emulator then one matching leased hardware case. Done means no lower-priority work starts while a human waits and no release occurs without neutral proof.
7. *Absorb helpers.* Follow the planner's units: dead deletion first, then one responsibility at a time with parity/caller repoint/deletion. Done means the old helper is gone and the semantic task passes its registered gate.
8. *Entry-point cutover.* Repoint `just`, hooks, and workflows after each task exists. Done means one responsibility has one active implementation and the same task semantics run locally and in CI.
9. *Production acceptance.* Execute security, restore, firewall, performance, orphan cleanup, board recovery, and operator runbooks; record evidence and owner sign-off before making checks required or removing fallback fleet paths.

== Executable acceptance matrix

#table(
  columns: (1.2fr, 1.45fr, 2.45fr),
  inset: 4pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Gate*], [*Environment*], [*Pass condition*]),
  [Go format/vet/staticcheck], [dev box], [All packages; warnings are failures; no changed generated catalog digest.],
  [Race unit suite], [Linux dev box], [`go test -race -count=1 ./...` exits 0 with no skip used as success.],
  [PostgreSQL integration], [Disposable loopback test DB], [Fresh schema 1-8 plus concurrent store/server tests exit 0; production DSN is refused.],
  [Coverage], [Linux dev box], [One merged profile across packages and integration tests is at least 85 percent; the gate must not take the maximum of per-package module-wide percentages.],
  [Static binaries], [Linux build host], [Linux amd64, Linux arm64, and Windows amd64 cross-builds succeed without in-tree caches; Windows process behavior also receives a real Windows runtime test.],
  [Terraform/Ansible static], [dev box], [Pinned init without backend, fmt, validate, lint, syntax, and negative approval gates pass.],
  [Control restore], [Isolated VM], [PostgreSQL, artifacts, and state restore meet approved RPO/RTO and resume no stale authority.],
  [GitHub prototype], [Isolated scale set], [One Linux and one Windows job, representative `run:` and `uses:`, JIT expiry, cancellation, restart, same-SHA rerun, failure, and cleanup.],
  [Network/RCE], [Disposable guests], [GitHub and ra8ci relay reachable; Proxmox, database, secret store, management network, and another job's identity unreachable.],
  [Board], [Simulator, emulator, then one board], [Priority/yield/requeue, timeout, signed neutral release, baseline restore, agent loss, high-water rollback, and quarantine pass.],
  [Repository gates], [dev box and CI], [All applicable registered gates run; warning, skip, timeout, or incomplete evidence is not green.],
)

Build and test output goes under the per-worktree verification root, never in-tree and never a shared `/tmp` build directory. Every remote build is detached to a log with a separately captured exit code, and the log is read before recording a pass.

== Handoff checklist for an engineer or AI

Before writing code, confirm the isolated worktree and base, read `CLAUDE.md`, inspect the accepted inventory/work units, and identify the planner-owned unit. Do not inspect or modify Brighton's active checkout. State the exact files owned, preserve concurrent edits, and avoid live external mutations unless the unit explicitly includes an approved acceptance operation.

Before declaring a unit complete, provide: branch and head SHA; one commit for that MR unit; changed/deleted paths; old behavior and exact new owner; migration/API/catalog version changes; security assumptions; detached verification commands/logs/exit codes; skipped or unavailable environments; coverage; and remaining operator inputs. Do not merge to `dev`, create issues, invent the MR stack, bypass CI, or call an untested fallback a pass.
