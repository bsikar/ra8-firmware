#set page(
  paper: "us-letter",
  margin: (top: 0.86in, bottom: 0.78in, left: 0.9in, right: 0.9in),
  footer: context [#align(center)[#text(size: 8pt, fill: rgb("#687487"))[ra8ci architecture - #counter(page).display()]]],
)
#set text(font: "DejaVu Sans", size: 9.5pt, fill: rgb("#172333"))
#set par(justify: true, leading: 0.62em)
#show heading.where(level: 1): set text(size: 17pt, weight: "bold", fill: rgb("#123c63"))
#show heading.where(level: 2): set text(size: 12pt, weight: "bold", fill: rgb("#176785"))
#show heading.where(level: 3): set text(size: 10pt, weight: "bold", fill: rgb("#263f5a"))
#show link: set text(fill: rgb("#176785"))

#v(0.8in)
#align(center)[
  #text(size: 36pt, weight: "bold", fill: rgb("#123c63"))[ra8ci]
  #v(0.22in)
  #text(size: 17pt)[One control plane for CI, the lab, and the board]
  #v(0.24in)
  #text(size: 10pt, fill: rgb("#687487"))[Approved architecture and implementation contract | 22 September 2026]
]
#v(0.7in)
#block(fill: rgb("#edf5f8"), stroke: rgb("#b9d2dc"), radius: 7pt, inset: 17pt)[
  *The proposal in one sentence.* A single Go product provides a command-line interface, a server in a protected Proxmox VM, and outbound-connecting agents. It controls disposable GitHub runner capacity, owns durable task and board lifecycles, records fine-grained measurements in colocated PostgreSQL, invokes Terraform and Ansible for provisioning, and runs named Zig build and quality tasks behind familiar just recipes.
]
#v(0.45in)
*Status:* Brighton approved the architecture and Go choice. Implementation is underway on a separate branch. This revision contains an as-built snapshot, the approved target, explicit production blockers, and an executable completion contract. It does not claim that deployment or script migration is complete. The planner still owns issue and MR boundaries. The active Zig build-graph work remains out of scope.

*Audience:* Brighton, the planner AI, implementers, lab operators, and future CI maintainers.

#pagebreak()
#outline(title: [Contents], depth: 1)
#pagebreak()

= Executive brief

== What changes for a person using the repository

The familiar command remains a just recipe. A developer can use just format, just test, or just hil. Those recipes call the corresponding ra8ci task name. The same task can be called directly by a person, a git hook, a CI trigger, or an AI agent. There is one definition of what the task does, what it needs, how long it may run, and which evidence it must produce.

Simple, safe local tasks can run on the machine that invoked them. For GitHub Actions, GitHub remains the workflow scheduler: ra8ci receives demand for runner capacity, creates eligible disposable guests, and tracks the tasks those jobs invoke. Hardware work enters the ra8ci board queue. A person can see not only that a run is slow but whether it spent its time waiting for a runner, creating a VM, downloading dependencies, compiling, waiting for the board, or executing a particular step.

The board remains one-at-a-time. A human can request it with a reason and duration. An active CI or AI task receives a yield request and stops at its next declared safe checkpoint. The lease is not forcibly stolen during a flash, reset, or measurement. The person sees the holder, remaining time, and queue. Every transition is audited.

== Chosen technology and deployment boundaries

- *Implementation:* Go for one source tree and one binary name with server, agent, and CLI modes. Build separate static binaries for Linux amd64 and Windows amd64; do not claim that one executable file runs on both operating systems.
- *Persistent state:* PostgreSQL, written by the server. Agents never receive database credentials. The schema has typed lifecycle tables and an append-only audit stream.
- *GitHub integration:* ra8ci uses GitHub's Go Actions Runner Scale Set Client as the controller for an ephemeral runner scale set. Each disposable Linux or Windows guest runs the official GitHub Actions runner for one job plus ra8ci agent for our task, telemetry, and board contracts. ra8ci does not reimplement the GitHub job worker.
- *Transport:* Authenticated HTTPS between CLI, server, and outbound-connecting agents. Use server-sent events or chunked HTTP for live logs at first; a versioned bidirectional stream can be added only if measured needs justify it.
- *Provisioning:* ra8ci owns the run state and authorizes every create or destroy. Terraform remains the declarative guest mechanism; Ansible remains guest and fleet provisioning. The direct qm lifecycle is retired when parity is proven.
- *Build path:* Semantic ra8ci task names invoke the current build implementation. As epic #857 completes, those implementations move to zig build steps. just remains the convenient wrapper; ra8ci does not implement another build graph.
- *Deployment:* One protected, persistent control VM on Proxmox (VM ID in the 9000+ range) runs ra8ci server and PostgreSQL together; use a Unix socket for their local database connection. Agents run in disposable Linux and Windows guests and at the board boundary. Repository jobs never run on the control VM or Proxmox host. No ra8ci agent listens for inbound jobs.

== Why this shape

The hardest failure today is split ownership: one disposable lab path runs Terraform then Ansible, while another server-side path directly clones guests with qm. A durable controller gives each run one identity and one recovery path. GitHub's scale-set client lets that controller supply native GitHub runner capacity without copying the runner's job protocol. The controller also provides the timestamps needed to see where CI time actually goes. Keeping Zig responsible for build dependencies avoids duplicating its graph. Keeping Terraform and Ansible responsible for infrastructure convergence retains the established safety boundaries.

== What is not promised yet

The first version should not claim automatic optimal scheduling, arbitrary interruption of HIL tests, a web dashboard, or complete removal of every helper script. Nor does the scale-set client make ra8ci itself a replacement for the official Actions worker. Those are different responsibilities. This document proposes a simple API and CLI first; a visual console is optional.

= Goals, boundaries, and current system

== Goals

1. A named task behaves the same whether invoked through just, the CLI, a hook, or CI.
2. Every execution has an owner, deadline, host, result, log, steps, and resource samples.
3. The server can reconcile an interrupted run after its own restart without inventing success or silently destroying a nonmatching guest.
4. One board lease is active at a time; humans, CI, and AI agents can cooperate through a visible queue and safe yields.
5. Analytics identify the critical path and the resource profile of slow steps before anyone changes scheduling tiers.
6. The CMake-to-Zig transition can change implementation commands without changing task identities or erasing historical comparison.

== Non-goals

- Implementing the Zig build graph, porting firmware libraries, or changing epic #857.
- Replacing Terraform state, Ansible roles, OpenBao, or the persistent runner fleet declaration before a proven cutover.
- Reimplementing the official GitHub Actions worker or executing arbitrary `uses:` actions inside the control VM.
- Giving an agent general-purpose shell access to the Proxmox host.
- Making a database outage look like a successful HIL run.
- Removing a script before its behavior and callers move in the same MR.
- Defining issues, epics, or stacked MR topology; the separate planner owns those.

== Observed repository baseline

The published dev branch inspected for this draft was a7666d637. Its disposable lab driver uses run-local Terraform state, template checks, an isolated bridge, Ansible playbooks, and strict cleanup identity checks. A separate server-side runner uses direct qm commands and host networking changes. The Linux playbook stages a source archive and runs just ci in the pinned container. The Windows playbook prepares a disposable Windows guest and runs just ci there. The persistent fleet is declared in infra/fleet.yml: ARC on k3s, Docker runners on NAS and Windows/WSL, and a dedicated HIL listener on dev. Workflows select these with `runs-on` labels. Ansible installs the official runner, whose listener starts a worker per job. The dedicated HIL listener and bench are separate from ordinary scalable runners.

The ra8ci inventory on ci/orchestrator has 588 in-scope shell/Python helpers: 557 used, 31 dead, and zero unclear. Twenty dead files have been removed in the current lane. Eleven still require reference or dependency-policy cleanup. The inventory is a migration ledger, not proof that every used script is a desirable permanent task.

= User journeys

== Fast local check

A developer runs just format. The recipe invokes ra8ci format. The CLI resolves the repository root and requires the embedded task-catalog digest to match the checkout's reviewed manifest. The first release uses pinned binary and catalog digests, not an in-app release-signing protocol or compatibility adapters. A mismatch fails visibly rather than silently using the wrong task. The CLI creates a local run identity, executes bounded steps, prints logs directly, and records the child's exit code; its own stable exit category is documented in the implementation contract. Safe local tasks such as format and lint run even when the server or PostgreSQL is unavailable. Their results are durably spooled on the invoking host as explicitly unsynced receipts, then uploaded idempotently when service returns. A synced receipt remains labeled local/offline and is never promoted into a CI gate attestation. Board and dispatched tasks fail closed.

== Disposable CI run

A GitHub workflow selects an ra8ci-managed runner scale set with `runs-on`. GitHub owns the workflow queue and offers job demand to the ra8ci controller. The server records its capacity decision before provisioning, creates a disposable guest through the restricted Terraform/Ansible path, and passes a short-lived just-in-time runner configuration to the official runner in that guest. The runner accepts one GitHub job and reports its native logs and status to GitHub; ra8ci agent records our named task steps, resources, and board use. The guest is then drained and destroyed. A failed cleanup remains a visible incident requiring reconciliation; it is never reported as clean success.

== Human asks for the board

A human runs ra8ci board take --why "debugging power sequencing" --duration 30m. The server records a waiter with human priority. If CI holds the board, the server sends a yield request. The holder reaches a safe checkpoint, records its completed phase, puts the board in a neutral state, releases the lease, and requeues its continuation. The human sees the wait position and then the granted lease. If the holder never reaches a checkpoint, it keeps the board until its deadline or safety recovery; priority is not permission to interrupt electrical work.

== AI agent works beside a human

The AI can run emulator and analysis tasks without a board. Its HIL task waits behind the human. It cannot bypass the server or proceed because a local lock file appears stale. When the human releases, the queued task is revalidated before acquiring a lease, because the checkout, image, or board state may have changed while it waited.

= Product topology and deployment

== One product, three modes

- ra8ci server: persistent HTTP API, scheduler, durable state machine, board authority, audit writer, deadline enforcer, and report query service.
- ra8ci agent: outbound authenticated connection, capability/host-fact report, local bounded process supervisor, log/resource sampler, step result producer, and safe-yield client.
- ra8ci task: human/automation CLI that executes approved local tasks or submits a dispatched run. Board and report subcommands are clients of server authority.

A single Go module and shared task schema avoid inconsistent behavior. The control VM starts as one server process and one colocated PostgreSQL service; separate server workers are a future split only if measured load or isolation requirements demand them. There is no need to split the ra8ci binary merely because modes run on different hosts. The official GitHub runner remains a separate upstream program inside each disposable job guest.

== GitHub Actions control and execution

GitHub Actions has two distinct planes. GitHub evaluates workflow YAML, queues jobs, matches `runs-on` labels, and reports workflow results. A self-hosted runner registers with GitHub, listens for a job, starts a worker, executes shell and `uses:` steps, and reports native logs and status. A process that merely posts a check run or exposes an HTTP API is not a GitHub Actions runner.

Use GitHub's Actions Runner Scale Set Client, a Go module extracted from Actions Runner Controller, in ra8ci server. It receives scale-set demand and manages scale-set registration and just-in-time runner configuration. ra8ci maps demand to a capacity reservation, provisions a guest through the existing Terraform/Ansible layer, enrolls ra8ci agent, and starts an official ephemeral runner in that guest with the just-in-time configuration. The runner accepts at most one job. GitHub continues to schedule its workflow steps; ra8ci schedules capacity and its own named tasks. These are separate queues with correlated IDs, not competing owners of one job.

The controller must account for capacity requested, provisioning, registered/idle, busy, draining, deregistered, and destroyed. It never advertises capacity as usable before the guest is healthy, and never treats an assignment, registration, or delete request as proof that a job ran or cleanup completed. Reconcile GitHub runner state, Proxmox guest identity, Terraform state, and ra8ci agent receipts after any lost acknowledgment. A stopped runner cannot be killed just to scale down while its worker is busy. Linux and Windows guests require separate images and capability labels; HIL stays in its dedicated lane until the board lease migration is proven.

The scale-set client is public preview. Before depending on it, a read-only API review and an isolated prototype must verify repository-scoped authorization, Windows guest registration, label matching, one-job teardown, cancellation, client/server versioning, quota behavior, and how pending jobs recover from failed provisioning. Its interfaces may change; pin a reviewed version and keep the GitHub adapter behind a small internal interface. No direct GitHub runner wire-protocol implementation is authorized by this design.

== Trust boundaries

The control VM has authority to request lab lifecycle actions, not carte-blanche root access to the Proxmox host. A dedicated, least-privilege Proxmox API credential or narrowly scoped provisioner invokes approved Terraform and Ansible workflows. Template, bridge, pool, datastore, run marker, and stopped-state checks remain enforced at the provisioning boundary. Credential delivery through the existing secret system must be verified before deployment; neither an API credential nor OpenBao access is assumed to exist on dev. No Proxmox credential is copied to a guest.

GitHub App credentials belong only to the control VM and are scoped to the repository or organization and permissions actually required by the scale set. Short-lived registration and just-in-time configuration material is treated as a secret and passed only to the intended guest. GitHub job tokens and repository secrets remain confined to the disposable worker. Workflow YAML and checked-out repository code are untrusted inputs: neither may select a Proxmox API operation, inject provisioner arguments, reach the control VM's database socket, or execute on the Proxmox host. Named task arguments are validated against a reviewed schema; no shell-concatenated command reaches a privileged boundary. Audit authorization failures, token issuance, VM lifecycle actions, and runner cleanup without logging secret values.

Agents receive task-scoped credentials and a capability allowlist. A disposable guest agent first enrolls against a capacity reservation and verified VM identity with health-only authority; it has no run ID, source SHA, task, or board permission until the server correlates a GitHub job and issues a separate job grant. A Windows guest never receives a token that can create Proxmox VMs. The board-side agent cannot mint its own lease. Unlike disposable guests, the persistent board agent has an operator-enrolled host identity with rotation and revocation; it is not bound to a run ID.

PostgreSQL lives on persistent storage in the protected control VM, not inside a disposable CI guest. ra8ci connects through a local Unix socket; no database port is exposed to runners. The app role can insert audit records but not update or delete them. This is one failure domain, so off-VM encrypted backups, retention, schema migration, restore drills, and a named backup destination are deployment requirements, not optional analytics polish. A database outage stops dispatch and board grants but does not stop safe local tasks.

== Host and resource model

Each agent reports OS/architecture, host class, CPU cores, RAM, available disk, current load, supported tools, board reachability, and an agent version. The server additionally stores the declared budget from infra/fleet.yml. Scheduling is bounded by the declared budget; instantaneous host facts refine placement but never authorize consuming more than the declaration. Runner capacity and quiet-hours drains remain the fleet layer until explicitly migrated.

= Task contract and Zig integration

== Task definition

A task has a stable semantic name, schema version, tier (required, optional, nightly), execution scope (local, runner, Linux VM, Windows VM, HIL), supported OS/architecture, required capabilities, deadline, board requirement, safe checkpoints, step list, output contracts, retry policy, and resource hints. Command arguments are structured, not shell-concatenated strings. The catalog is embedded in the binary; the repository carries a reviewed manifest of its digest and compatible schema range. A run records the binary, catalog, manifest, and source digests. A mismatch refuses execution unless an explicitly approved compatibility adapter exists.

A task name is not a source file name. For example, format, unit-test, cross-build, lint, emulator, HIL, and report are stable concepts. The current command adapter may invoke an existing script only during the executor stage. Each later migration replaces that adapter with a real implementation and deletes the old script and repoints the just recipe in the same MR.

== Zig owns build dependencies

ra8ci invokes selected zig build steps with a pinned Zig toolchain, explicit target/configuration, and an out-of-tree cache/prefix. It does not parse build.zig to recreate the dependency graph. Zig is responsible for parallel build steps and caching within a build. ra8ci is responsible for cross-task placement, deadlines, board access, and longitudinal evidence.

The task identity and schema survive the CMake retirement. Historical records include engine=cmake or engine=zig, Zig/CMake version, target, configuration, and build-graph digest. Reports compare like with like; a change of engine is a segmentation boundary rather than an unexplained speedup. The #857 parity gates remain authoritative until the Zig route preserves analysis databases, coverage, stack-usage, and other required artifacts.

== just remains the front door

Just recipes call ra8ci by task name and pass explicit arguments. A recipe does not duplicate task policy or decide which VM to use. Hooks and workflows use the same names. A direct ra8ci invocation is equivalent; it is not required to have just installed on an agent if the task does not itself need just.

= Durable execution and scheduling

== Run state machine

A run moves through accepted, queued, provisioning, ready, running, draining, cleaning, and a terminal state: succeeded, failed, timed_out, cancelled, or cleanup_failed. Each transition is transactional and emits an audit event. A task attempt is distinct from the logical task so a retry does not overwrite the original timing or result. There is no "unknown equals success" state.

Provisioning actions have operation IDs. Before executing an external action, the server commits intent and expected identity. Terraform state is run-scoped but stored in a persistent, access-controlled directory keyed by run ID, with encrypted backup and an operation-to-state-path mapping in PostgreSQL; it is never a throwaway process temp directory. After an interruption the server reconciles against that state, Proxmox markers, and agent facts, including the case where a provisioner died before publishing state. Replaying a create/destroy call without checking identity is forbidden. Acknowledgment loss may yield an unknown result; reconciliation, not blind retry, resolves it.

== Queue and placement

Initially use explicit rules, not a learned scheduler. For ra8ci-owned tasks, enqueue by tier and priority, with age-based fairness and per-host/per-profile concurrency limits. For GitHub workflows, GitHub retains job ordering and matching; ra8ci reports available scale-set capacity and chooses which eligible guests to provision. It must not promise to reprioritize a job already assigned by GitHub. Choose guests/agents with matching OS, capability, declared budget, free memory, and quiet-hours state. Preserve a lane for quick required checks so large nightly work cannot monopolize every worker. Board-dependent tasks acquire a board lease only immediately before their first board step; emulation, compilation, and packaging happen before the lease.

Top-level run intervals form a disjoint wall-time partition: queued, provisioning, setup, executing, and cleanup. Board wait and board hold are labeled subintervals inside executing, not extra time added to it. Overlapping work on different agents is shown as a timeline and dependency graph, not summed into run wall time. This is essential: CI taking an hour is not a useful optimization target until the hour is attributed.

== Deadlines, cancellation, and logs

The server holds an absolute ra8ci task deadline and the agent enforces a local monotonic timeout. GitHub may also cancel its workflow job; the GitHub runner owns that outer cancellation, and ra8ci must correlate it to a task cancellation receipt rather than report a second, contradictory success. For non-board tasks, timeout or cancellation first requests process-tree termination, waits a bounded grace period, and then kills the remaining tree; board tasks instead obey the phase-aware cancellation and recovery rules below. This behavior needs separate Linux and Windows tests. A skipped test is never a pass; it is a distinct result with an explicit reason and policy decision.

Logs are streamed in numbered chunks with run/task/attempt/step IDs. A reconnect resumes from the last acknowledged sequence. The server stores an indexed excerpt and durable artifact pointer; giant stdout blobs do not live in PostgreSQL rows. Every result records truncation and artifact integrity. The task row is terminal only after exit code and log finalization are durably recorded, or after a clear lost-agent reconciliation outcome.

= Board commons: human, CI, and AI coordination

== Lease invariants

1. At most one active board lease exists. The database enforces this invariant transactionally; a local lock is not authority. The grant is unusable until the board-side agent durably acknowledges its generation.
2. Priority is human above CI above AI agent, but active work is cooperatively yielded, never yanked in the middle of an unsafe phase.
3. A waiter does not touch hardware. An emulator task runs first and does not hold the board.
4. Every lease has a requested duration, absolute expiry, heartbeat, holder identity, reason, and generation token.
5. Expiry ends the holder's authority and moves the board into recovery-required state. No new lease generation is granted until the board-side agent confirms a neutral state; an offline or unresponsive agent keeps the board unavailable.
6. A stale holder cannot act after its generation is superseded. The board-side agent durably records its highest seen generation, validates the current lease token before each board-touching step, and fails closed when it cannot validate it; an already-started bounded safe segment completes before recovery. A database restore behind the agent's high-water mark forces quarantine.
7. Take, grant, extend, yield request, checkpoint, release, expiry, recovery, and denied action are audited. All software-mediated flash, reset, probe, UART, relay, and power operations cross the board agent's serialized local gate. Direct physical, root, or locally attached-probe access cannot be cryptographically fenced and needs an operator physical-control procedure.

== Queue and handoff

A human request joins ahead of CI and AI waiters. A yield request is visible to the holder and the human. Each task declares checkpoint boundaries and a maximum noninterruptible segment. At a checkpoint it stops board I/O, flushes results, restores the safe board state, releases, and requeues a continuation behind the human. A task that cannot safely resume declares itself nonresumable; yielding then ends that attempt as interrupted and a later attempt starts from a clean image. No generic pause/resume illusion is presented.

Fairness needs more than strict priority. While a human is waiting, new lower-priority work cannot jump ahead. Within one priority class use FIFO with bounded aging. A human who keeps extending indefinitely is visible, and extensions require a reason and policy limit. A second human cannot silently preempt the first.

== Dynamic yield budget and cancellation

Three clocks remain distinct: a HIL test's validity deadline, the time from a human yield request to a safe checkpoint, and the safety limit of an indivisible flash or recovery operation. The current per-app hil.conf files declare observation/probe windows, sometimes longer than 30 seconds; they do not bound total setup and flash or authorize interruption. ra8ci imports those values as test deadlines and fallback evidence only where the declared step matches. A board task must also declare a maximum safe-step and restore/probe duration. If comparable request-to-neutral history has sufficient samples, a conservative high quantile plus margin supplies a visible dynamic handoff estimate; otherwise the declared bounds are used. If a board task has no such bounds, its ETA is unknown and automatic dispatch is rejected. Thirty seconds remains a handoff target and a default process-stop grace only for a separately declared cancel-safe experiment; it is never a substitute safety bound. The estimator never shortens a declared safety bound, and its cohort, sample count, and age are shown to the requester.

Thirty seconds is the default handoff target, not a guarantee that permits a half-written flash. Once a human waits, no new lower-priority board step starts. If an active flash, erase, verify, or required recovery exceeds the target, the holder finishes that indivisible phase, reports the overrun and revised ETA, then yields. If the phase hangs or board state cannot be proved neutral, the board enters recovery-required or quarantined state rather than being handed to the human. The same rule applies to task validity expiry: invalid result does not imply safe board release.

For checkpointed HIL, the interrupted attempt ends with a distinct preempted result and requeues for a fresh run from a verified baseline, unless that specific task declares a tested resumable checkpoint. For an open-ended AI experiment, a yield first requests graceful stop, then terminates its process tree after a bounded drain if its current phase is cancel-safe. Stopping the host program is not enough: the board-side agent fences its access, resets or reflashes the board according to a declared restore policy, probes the result, and only then releases it. An open-ended firmware image cannot be assumed to recreate RAM, peripheral, or interaction state when reflashed; its owner must restart the experiment. Human-held work is notified of later requests but is never force-preempted by CI or AI.

Board session metadata records the immutable firmware artifact digest, flash arguments, board/fixture revision, owner, current phase, last safe checkpoint, restore image/action, and retry class. Queryable identity and state belong in typed columns; JSONB holds versioned, variable metadata, not firmware bytes. Artifacts live in durable content-addressed storage. A failed restore quarantines the board and requires operator recovery.

== Failure and recovery

On holder crash, the server waits for expiry and asks the board-side agent to perform a reviewed recovery sequence. On server or PostgreSQL outage, the board-side agent stops at the next safe point and does not grant a new lease locally. If a hardware operation is already in progress, it completes only the bounded safe segment. After recovery, the server reconciles board state before granting a new token. These rules deliberately favor safety over throughput.

The first board version should preserve bench.sh hold, free, extend, status, and journal behavior. Cutover keeps the old flock as the sole authority while the new lease path runs in shadow, then drains old holders, disables direct access, and enables the new agent gate; the two authorities are never independently active. Rollback requires all new leases drained. Acceptance testing includes two concurrent claimants, human yield during flash, lost agent, server restart, database outage, expiry, stale token, direct-bypass attempt, and safe release failure.

= Database and audit design

== Core relational tables

- runs: run ID, trigger, actor, repository, branch, commit, source snapshot digest, task catalog digest, creation/start/end, status, and parent run.
- tasks: logical task ID, run ID, stable task name, tier, host class, placement, enqueue/start/end, deadline, terminal result, and board requirement.
- task_attempts: attempt ID, task ID, agent, start/end, exit code, timeout/cancel/lost-agent reason, host facts at start, and implementation engine.
- task_steps: attempt ID, stable step key, ordinal, start/end, exit/result, exclusive phase label, and artifact links.
- task_edges: run ID, predecessor task/step, successor task/step, and dependency reason; these edges form the recorded execution DAG needed for critical-path analysis.
- resource_samples: attempt/step, monotonic offset, CPU time, process CPU utilization, process-tree RSS and peak RSS, host load, RAM available, disk and network I/O. Keep a documented sampling interval and record gaps.
- board_waiters: request ID, class, actor, reason, enqueue time, requested duration, state, and queue order.
- board_leases: lease ID, board ID, holder, generation token, grant/expiry/release times, reason, requested duration, yield state, and end reason.
- board_sessions: session ID, owner, board/fixture revision, lease generation, phase, cancellation and retry class, restore policy, current and baseline image digests, last safe checkpoint, and versioned JSONB metadata.
- board_yield_samples: session/step ID, request/checkpoint times, observed latency, task/catalog/image/board cohort, safety overrun, and exclusion reason; failed or censored requests are retained but not passed off as completed latency samples.
- agents: identity, host class, enrollment, capabilities, version, last heartbeat, and declared capacity.
- github_jobs: GitHub installation/repository, workflow run and attempt IDs, job ID, requested scale set and labels, runner identity, immutable commit, GitHub conclusion, and correlation to the ra8ci run. Store references and status, not GitHub tokens or unredacted payloads.
- runner_lifecycles: capacity request, guest and Terraform identity, JIT issuance, registration, first job, last worker state, deregistration, destruction, and any reconciliation/cleanup failure.
- artifacts and log_chunks: integrity digests, content location, byte size, sequence, retention, and redaction status.
- audit: immutable event ID, wall time, actor, action, target, correlation/run ID, request ID, outcome, previous/new state, and structured reason.

The board uniqueness constraint and state transitions must be tested against concurrent transactions. PostgreSQL row locks or queue-safe selection may coordinate waiters, but the safety invariant lives in a constraint and transactional state change, not in timing luck. The app's normal database role cannot mutate audit history.

== What the first reports answer

- ra8ci report slow ranks median, p90, and total wall time for comparable tasks over a chosen window, with sample count and failure rate.
- ra8ci report critical-path computes the longest dependency path from recorded task_edges and step durations, with missing edges or clock uncertainty called out rather than guessing from the longest individual task.
- ra8ci report idle-time separates queue wait, provisioning wait, board wait, and host idle capacity.
- ra8ci report runners separates GitHub queue delay, scale-set demand-to-provision, guest boot/setup, runner idle, job execution, and cleanup. GitHub-owned timestamps and ra8ci-observed timestamps carry provenance; missing GitHub events are shown as gaps, not invented durations.
- ra8ci report resource-fit identifies long low-CPU steps, but also reports RSS and I/O so parallelization does not create memory or disk contention.
- ra8ci report board shows utilization, wait by priority class, yield latency, expired leases, and time spent in recovery.
- ra8ci report regress compares equivalent commit ranges, profile, engine, task schema, and cache state.

No task changes from required to optional on a hunch. A tier change is reviewed with sample size, failure consequence, dependency impact, and a recorded rationale. Analytics is evidence for a decision, not an automatic gate weakening mechanism.

== Retention and privacy

Audit and run metadata persist longer than raw logs and high-frequency samples. Define retention by class and estimate storage before deployment. Secret values, OpenBao tokens, command environment, and private board data are redacted before persistence. The audit stream records that a secret was accessed or a provisioner was invoked, never the secret value. Restore drills must prove both database and artifact references survive.

= API, CLI, and operations

== Initial CLI surface

- ra8ci format, lint, test, build, emulator, and HIL task names; --local or --dispatch when both modes are meaningful.
- ra8ci run status, logs, cancel, retry, and explain for run visibility.
- ra8ci board take, release, extend, status, and queue.
- ra8ci report slow, critical-path, resource-fit, and board.
- ra8ci agent and ra8ci server for deployment modes.

Human output is concise by default; --json gives stable machine-readable schemas. Exit codes distinguish task failure, unavailable server, denied policy, timeout, cancelled, and incomplete evidence. The API and agent protocol are versioned. A new server must tolerate an older agent for a bounded transition window, or explicitly drain it before upgrading.

== API and access control

Use authenticated HTTPS, short-lived agent identity, and role-based permissions: observer, task submitter, board human, operator, and provisioner. Humans may view their authorized logs and request board time; agents cannot request human priority; only operators can approve exceptional keep-guest or cleanup recovery. Every authorization failure is audited without leaking secrets. The server binds to a protected management interface and is not exposed directly to the lab guest subnet.

== Operational readiness

A health endpoint distinguishes "process alive" from "database writable, scheduler able to commit, and audit durable." Startup performs schema compatibility checks and refuses unsafe downgrade. Backups, disk pressure, expired certificates, stalled agents, stuck cleanup, lease recovery, and log-ingest lag have explicit alerts. An operator can put the server in drain mode, stop new dispatch, and let current safe segments finish.

GitHub integration health separately reports scale-set session health, authentication expiry, runner registration failures, assigned-versus-available capacity, and orphaned guests. A GitHub outage does not revoke a human's existing safe local task, but no new GitHub job is presumed assigned; board authorization still requires ra8ci's own healthy server and database. A control VM outage leaves GitHub jobs queued or pending according to GitHub's own limits, not silently passed. Recovery replays durable capacity decisions and guest identities before starting new provisioners.

= Alternatives and trade-offs

== Go, Rust, or Zig for ra8ci

*Decision: Go.* It provides a straightforward portable server, process supervision, HTTP service, SQL client, and cross-compiled Linux/Windows binaries from one codebase. The Zig migration does not require the orchestrator itself to be Zig: Zig remains the build system and firmware/library language, while ra8ci coordinates tasks and records evidence. Rust can offer tighter memory control but raises implementation complexity for a rapidly changing scheduler. Zig would unify languages but couples the control plane to the actively changing compiler/build stack. A language change now requires a new architecture decision and migration plan.

== PostgreSQL versus a local file database

*Decision: PostgreSQL.* Durable concurrent state transitions, queue claims, relational analytics, and access controls fit the server/agent architecture. Centralize writes through the server; "many agents" is a reason for durable shared state, not for distributing database credentials. A local database is simpler for one process but complicates failover and multi-host audit. The cost is a persistent service and backup obligation.

== One server versus microservices

*Decision: one server process first.* It gives one lifecycle authority and fewer failure seams. Worker-like components can be internal packages with narrow interfaces. Split processes only when measured reliability or resource isolation requires it. Independent microservices from day one would multiply protocol, deployment, and reconciliation burdens before the workload is understood.

== Direct Proxmox API versus Terraform/Ansible

*Decision: Terraform/Ansible remain the production mechanisms.* ra8ci owns when, why, and which run, and verifies results. The current direct qm runner is an alternate provisioning path and should retire only after a verified replacement exists. Keeping the existing guardrails avoids two sources of truth for VM and network state. The Go Proxmox client may reconcile exact identity but is not authorized as the production mutation backend. Direct API mutation requires a separate safety review and architecture decision, not an incidental optimization.

== GitHub runner replacement versus scale-set control

*Decision: scale-set control with the official runner in disposable guests.* Replacing the runner binary itself would require ra8ci to implement registration/authentication, encrypted job receipt, listener/worker behavior, arbitrary shell/JavaScript/container/composite action semantics, workflow commands, secret masking, logs, artifacts, cancellation, and compatibility with GitHub updates on Linux and Windows. The published Go scale-set client provides controller integration, not a substitute job worker. A simple workflow bridge that invokes ra8ci from an existing runner is a useful migration step, but does not let ra8ci own runner capacity or lifecycle. The selected design gives native GitHub UI/job behavior and lets ra8ci own Proxmox capacity, task semantics, analytics, and hardware safety without maintaining a private Actions protocol implementation.

= Migration with proof at every boundary

The planner AI decides issues, MR batches, and ordering. This is the approved technical dependency spine, not a planner-defined stack.

1. *Executor and evidence.* Define task contracts, local CLI, server run ingestion, deadlines, logs, and the run/task/step schema. Initially delegate to existing commands so comparable measurements start early. No old/new executor may run the same responsibility concurrently.
2. *Board lease.* Move bench.sh semantics into the server, add durable queue and audit, then delete the script and repoint its just recipes in the same MR. Prove cooperative checkpoint and recovery behavior before hardware use.
3. *Dispatch and GitHub capacity.* First prototype the Go scale-set client against a disposable Linux guest, then Windows. Enroll agents in the guests, start an official ephemeral runner with JIT configuration, and replace both Proxmox CI dispatch scripts with one ra8ci-owned lifecycle that invokes Terraform and Ansible. Prove label matching, one-job teardown, identity-checked cleanup, result propagation, cancellation, failed provisioning, GitHub and server reconnect, and restart reconciliation. Keep the current ARC/Docker/HIL fleet until matching gates are green; do not route the same job class to two competing dispatch paths during cutover.
4. *Absorb checks.* Replace each used script's behavior, repoint every caller, delete the old script in that MR, and require a non-skipped gate. Prioritize frequent/slow checks using real task-step data.
5. *Entry points.* Hooks, workflows, and just recipes call stable ra8ci task names. GitHub workflows may remain thin native Actions entry points: `runs-on` selects the ra8ci scale set, while required setup or third-party `uses:` actions still execute in the official runner. Keep required Zig build-parity and analysis coverage; only retire CMake-facing paths when #857's acceptance evidence is complete.

A migration ledger maps old path, callers, new task, behavior proof, deletion MR, and fallback policy. A task is not "ported" because ra8ci shells out to the old script. The final state must preserve exact exit semantics, artifacts, selftests, and safety checks.

== Verification gates

Unit tests cover state transitions, SQL constraints, CLI parsing, scheduling, auth, deadlines, and Windows/Linux process-tree termination. Property and concurrency tests cover board uniqueness, queue ordering, idempotent retries, and lost acknowledgments. Integration tests use fake agents, disposable databases, and a mocked GitHub scale-set adapter. An isolated GitHub/Proxmox acceptance run proves the actual scale-set client, JIT runner registration, one Linux job, one Windows job, cancellation, cleanup, and recovery from controller restart; it is not a full CI sweep. Emulator tests precede the single matching hardware test. Hardware work requires a live lease and stops at the next safe checkpoint on yield. Full CI must be green; warnings and skipped tests are not passes.

#include "appendix.typ"
#include "implementation-contract.typ"
#include "implementation-reference.typ"
= Decision register and discussion prompts

*Confirmed decisions:* Safe local tasks run offline when the server or PostgreSQL is unavailable. Their receipts remain visibly unsynced until uploaded; board and dispatched tasks fail closed. Thirty seconds is the default human handoff target, not a hard takeover guarantee: an indivisible flash or recovery can overrun it visibly, and no next lease is granted until the board is verified safe. A hung or unverifiable board is quarantined.

*Confirmed design direction:* Go; one product/three modes; protected persistent control VM on Proxmox with colocated PostgreSQL; agents connect out; ra8ci owns lifecycle state while Terraform/Ansible provision; Zig owns the build graph; semantic tasks behind just; cooperative board priority human > CI > AI; detailed per-step and per-resource measurements. For GitHub Actions, ra8ci controls an ephemeral runner scale set while the official runner executes jobs in disposable guests; it does not replace the worker protocol.

*Required operator inputs before production:*
- What approved off-VM destination holds encrypted PostgreSQL and artifact backups, and which dedicated Proxmox API/GitHub App credentials can be provisioned? No deployment proceeds by guessing either.
- The first useful release is CLI/API plus streamed logs; a web console is outside initial acceptance and may follow measured usage.
- Who may approve keep-guest and manual recovery actions? Recommendation: a narrow operator role, never ordinary CI or AI.
- Existing third-party `uses:` steps remain supported by the official runner. Project-specific checks migrate to named ra8ci tasks without rewriting unrelated Actions behavior.

= Evidence and sources

Repository sources inspected read-only: infra/fleet.yml; infra/terraform/README.md and lab modules; infra/ansible/playbooks/proxmox-lab-linux.yml and proxmox-lab-windows.yml; scripts/dev/proxmox_lab_ci.sh and proxmox_lab_server_runner.sh on published dev at a7666d637. HIL timing and recovery evidence on that dev tip: scripts/hil/all.sh:21-32, 375-462, 487-531, 543-596; scripts/hil/lib/hil_conf.sh:97-109; examples/ek_ra8d2/hw_validated/hil/usb_selftest_soak/hil.conf:9 (200-second observation); scripts/hil/reflash.sh:92-127 (indivisible destructive reset and flash). The current ra8ci inventory and work-unit JSON are on ci/orchestrator at 7ff60fee7. Unpushed work in other checkouts was not inspected.

Runner-specific repository evidence on that published dev tip: infra/fleet.yml declares ARC on k3s, NAS Docker, Windows/WSL Docker, and a dedicated HIL listener; infra/ansible/roles/ci_runner/tasks/main.yml installs the ARC scale set and official runner; infra/ansible/roles/ci_runner_docker/tasks/deploy.yml configures per-instance registration and documents Runner.Listener / Runner.Worker; infra/ansible/roles/dev_box/tasks/hil_runner_transaction.yml installs the dedicated listener. GitHub workflows select those runners with `runs-on` labels. These are current-state observations, not authority to alter Brighton's active dev work.

- Build and storage: #link("https://github.com/bsikar/ra8-firmware/issues/857")[Epic #857], #link("https://ziglang.org/learn/build-system/")[Zig build system], and #link("https://www.postgresql.org/docs/current/sql-select.html")[PostgreSQL row locking].
- Execution: #link("https://pkg.go.dev/os/exec")[Go os/exec] and #link("https://pkg.go.dev/context")[Go context].
- GitHub runner model: #link("https://docs.github.com/en/actions/reference/runners/self-hosted-runners")[self-hosted runners], #link("https://github.com/actions/scaleset/blob/main/README.md")[scale-set client], #link("https://github.com/actions/runner/blob/main/docs/design/auth.md")[runner authentication], and #link("https://docs.github.com/en/actions/concepts/runners/actions-runner-controller")[ARC concepts].
- Document format: #link("https://typst.app/docs/reference/pdf/")[Typst PDF documentation].
