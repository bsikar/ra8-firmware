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
  [`CURRENT`], [Present in the uncommitted implementation worktree and covered by at least focused tests. It is not necessarily production-ready.],
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

The snapshot below describes the worktree at the time this document was regenerated on 22 September 2026.

#table(
  columns: (1.25fr, 3.35fr),
  inset: 5pt,
  stroke: rgb("#d6e1e8"),
  table.header([*Item*], [*State*]),
  [Branch], [`ci/ra8ci-implementation`, based on `ci/orchestrator` commit `7ff60fee72cbc0d8e2754de9cff247fa5c24ee46`.],
  [Commit state], [The implementation is untracked and uncommitted. No implementation commit or push has occurred.],
  [Go surface], [107 Go files, including 44 test files, under `tools/ra8ci`.],
  [Packages], [`agent`, `board`, `boardclient`, `catalog`, `executor`, `github`, `hilpolicy`, `hilspec`, `neutral`, `protocol`, `proxmox`, `runclient`, `scaler`, `server`, `source`, `spool`, `store`, and `syncclient`.],
  [Database], [Forward-only PostgreSQL migrations `0001` through `0013`; the runtime expects schema version 13. Migration 0012 stores durable run-cancellation intent; 0013 records the one-way Terraform apply intent before an external apply.],
  [Task catalog], [Four transitional Linux tasks: `format`, `format-check`, `lint-go`, and `test-go`. They still delegate to existing commands and are not completed ports.],
  [Inventory], [31 `dead` and 557 `used` helpers. There is no `unclear` residue in the accepted inventory. No Phase 1 deletion has been made on this branch.],
  [Deployment], [Static control-VM Terraform and Ansible definitions exist. No plan has been applied and no VM has been created.],
  [Board], [Durable reducer, store, HTTP API, client, and signed neutral receipt primitives exist. Production mutation remains disabled without a physical observer and approved fixture profile.],
  [GitHub], [Durable inbox, admission policy, official scale-set client wrapper, controller, and a provider-injected scaler state machine exist. Production GitHub App bootstrap and runner provisioning are incomplete.],
)

Verification is evidence, not status by assertion. Detached full Go unit tests, full PostgreSQL-backed integration tests, race tests, `go vet`, gofmt check, and Windows-amd64 cross-build exited zero after run cancellation was added. The integration suite used disposable PostgreSQL 17 with no persistent volume; it covers authenticated/idempotent HTTP cancellation, cancellation of queued runs, refusal to execute issued work after cancellation, active cancellation delivery on heartbeat, and terminal cancellation evidence. Coverage is not at the repository's 85 percent gate. The `test-go` catalog route was exercised earlier and exited 1 because the existing root-owned managed Python environment receipt does not match this checkout's `pyproject.toml`; that gate is not counted as passed. Windows runtime, live GitHub, live Proxmox, emulator-to-board, restore-drill, and hardware acceptance have not passed. Logs and exit files are under `/home/bsikar/ra8-verify/ra8ci-implementation/run-cancel/`; rerun affected gates after implementation changes stop.

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
  [`ra8ci tasks`], [`CURRENT`], [Lists embedded task names.],
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
  [Board release/extend/checkpoint CLI], [`BLOCKED`], [Not exposed as general-purpose human board control. Server mutation still requires verifier-backed neutral evidence, an enrolled/fenced board agent, and an approved fixture profile.],
)

Current process exit meanings are 0 for command/task success, the child exit for a completed local task, 124 for a task deadline, 130 for cancellation, 2 for CLI usage, and 1 for control-plane or internal failure. The target CLI must replace the generic 1 with stable documented categories without changing an underlying task's meaningful child result.

=== Target commands

Add only after their server contracts and tests exist:

- `ra8ci run retry|explain <run-id>`.
- `ra8ci board checkpoint|extend|release|queue|doctor`.
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

Board routes currently include status, take, waiter cancel, checkpoint, release, extend, neutral challenge, agent acknowledgment/observation/unavailable, recovery start, and quarantine under `/v1/boards/{board-id}`. A route existing does not make it production-enabled: absence of a neutral verifier, board-agent identity, or approved fixture profile must return unavailable or denied and must be audited.

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

=== Exhaustive migration rule

The accepted inventory contains 588 helpers: 31 dead and 557 used. The 31 dead files are deleted only in planner-defined Phase 1 units and only after re-running reference scans. The 557 used files are not necessarily 557 public tasks: entry points become catalog tasks; shared parsing/model code becomes internal Go packages; Terraform and Ansible remain provisioning; substantial firmware/deliverable programs remain out of scope.

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

`TARGET`: publish one separate ra8ci lifecycle check per GitHub run/attempt. It remains pending through guest creation, runner registration, execution, evidence, deregistration, and cleanup. Native job success cannot override failed or incomplete lifecycle evidence. An ambiguous Checks API write is reconciled by SHA, check name, App ID, and external ID.

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
