#pagebreak()
= Implementation appendix

== Internal Go boundaries

Keep one executable but explicit packages: catalog defines task schemas and validation; cli handles human input and machine-readable output; server owns authentication, API, transitions, scheduling, and reporting; github adapts the Actions Runner Scale Set Client behind a narrow capacity interface; agent owns bounded process execution and resource sampling; provision defines a narrow Terraform/Ansible interface; board owns lease policy; store owns SQL transactions and migrations. Dependencies point inward toward task and state contracts. Provisioning code must not call board hardware directly, GitHub job payloads must not become provisioner commands, and agents must not import server database packages.

A task implementation has prepare, execute, checkpoint, and finalize phases. Prepare is side-effect-free where possible and validates tool availability, repository state, arguments, and output paths. Execute emits typed step boundaries. Checkpoint is the only place a yield can be honored before more board I/O. Finalize writes artifacts and releases resources even after task failure. Every phase has an idempotency key derived from run, task, attempt, and phase.

== API contract sketch

- POST /v1/runs accepts trigger, immutable source identity, requested task set, and a client idempotency key; returns a run ID.
- GET /v1/runs/{id} returns state and attribution timestamps. GET /v1/runs/{id}/events streams ordered, resumable progress by event sequence.
- POST /v1/runs/{id}/cancel records intent; it does not falsely report an immediate process stop.
- POST /v1/agents/enroll exchanges a short-lived bootstrap credential for a capacity-reservation-bound guest identity; only a later, verified job grant is run-bound. The persistent board agent has a separate operator-enrolled host identity.
- GET /v1/agents/{id}/assignments is an authenticated long poll. The agent acknowledges an assignment with its attempt ID before execution.
- POST /v1/attempts/{id}/chunks accepts numbered log or resource batches and is idempotent on attempt plus sequence.
- POST /v1/attempts/{id}/result accepts a terminal receipt with exit code, deadline outcome, artifact digests, and final sequence.
- POST /v1/boards/{id}/requests creates a waiter. GET /v1/boards/{id} returns holder and queue. Release, extend, and yield endpoints require the appropriate lease generation and role.
- GET /v1/reports/slow returns a window, filters, sample counts, quantiles, and exclusion reasons.

All mutating endpoints accept an idempotency key and return a correlation ID. API versions are explicit. A protocol mismatch is a visible failed enrollment or drained agent, not an undefined task result.

== Transaction and lease sketch

A board has a single current generation. Granting a lease locks its board row and requires recovery_complete for the current generation, regardless of whether its former lease expired. It then chooses the next eligible waiter, increments the generation, inserts a grant_pending lease, changes waiter state, and appends audit events in one transaction. Only after the board agent durably installs and acknowledges the generation does the lease become active and usable. A uniqueness constraint on pending or active board ID is defense in depth. Heartbeat and extension compare generation and expiry; a stale holder cannot extend. Release compares the same token and records the end reason. If neutral state is not confirmed, recovery_required remains true and the next grant is blocked.

The board-side agent must validate the token at a checkpoint and immediately before a board-touching command. A token check is not sufficient on its own for a long electrical operation, so each such operation has a declared maximum safe segment and a local deadline. The server does not claim a lease is safely reusable merely because PostgreSQL time has passed.

Queue claims likewise use a transaction and a stable attempt ID. If the server crashes after dispatch but before receiving acknowledgment, the recovered server asks the agent for that attempt ID; it does not issue the same non-idempotent task twice. If the agent is gone, the attempt becomes lost and a policy-controlled fresh attempt may be created.

== GitHub scale-set handoff contract

The GitHub-facing adapter is a capacity controller, not a job executor. It persists each scale-set demand/decision with an idempotency key and a requested OS/capability profile. The provisioner creates one approved guest identity and returns a verifiable VM marker. After guest health and agent enrollment, the controller requests a short-lived JIT runner configuration and delivers it to that guest only. The official runner registers, accepts at most one GitHub job, executes its steps, and reports native job status. ra8ci separately correlates the GitHub workflow run/attempt/job IDs with its own run and task attempts. The order of GitHub assignment and ra8ci telemetry receipt is not assumed; missing correlation remains visible.

The controller records busy/idle state before any drain or destroy. A GitHub cancellation requests a graceful job stop; a timeout follows the runner's cancellation semantics and ra8ci's local task safety policy. A board operation never inherits permission to hard-kill an indivisible flash merely because the outer GitHub job ended. After the worker exits, ra8ci reconciles GitHub registration, agent result, VM marker, and Terraform state, then tears down the guest with identity checks. Lost acknowledgment or failed destroy becomes cleanup_failed, not success. A controller restart reconstructs this state before requesting new capacity.

The first acceptance matrix covers Linux and Windows JIT registration; `runs-on` matching; one job per guest; ordinary `run:` and representative `uses:` steps; log and conclusion parity in GitHub; cancellation while idle and busy; token expiry; failed guest boot; controller/database outage; cleanup failure; and no Proxmox/database credentials in the worker. The public-preview client is pinned and tested before any old runner lane is removed.

== Measurement specification

Use monotonic clocks for durations within one process and UTC timestamps for cross-host correlation. Record both, plus clock-skew diagnostics. Top-level run phases are mutually exclusive intervals on the server clock; nested task and board spans are linked to their parent phase and never summed into the parent wall time. A step's duration uses its agent's monotonic clock. Step timing cannot be computed by subtracting clocks from different hosts without a synchronization assumption. Host facts at task start satisfy provenance; periodic process-tree samples support resource analysis. Sampling interval and dropped samples are recorded, so low observed CPU does not masquerade as measured low CPU.

The first performance hypothesis should be tested as follows: select tasks with at least a minimum sample count on comparable host and engine profiles; calculate median and p90 execution and queue time; inspect CPU-seconds divided by wall seconds, peak RSS, I/O and cache state; simulate an additional concurrency slot against declared memory/CPU budgets; then trial a bounded change and compare critical-path time and failure rate. Revert if p90, memory pressure, or flakiness worsens. Do not move a required gate to optional as a shortcut.

== Yield estimator and replay contract

At each human request, record the target holder's task definition/version, image digest, board/fixture revision, current phase, request time, first safe checkpoint time, neutral/available time, and whether cancellation, recovery, or an overrun intervened. Human ETA uses request-to-neutral/available, not only request-to-checkpoint. Estimate only from comparable, completed samples. The first policy uses a 60-day rolling cohort, at least 30 samples, and nearest-rank p95 plus max(5 seconds, 20 percent); it is floored at the declared remaining safe segment plus restore/probe bounds. Report sample count, censored/failed count, and overrun rate. A maximum observed task runtime plus standard deviation is not a sound yield estimator: it measures different work, is sensitive to one outlier, and hides censored or failed attempts. If data are sparse or stale, use the declared segment plus restore/probe bounds; if a board task has no bound, reject automatic dispatch and show ETA unknown. Thirty seconds is a handoff target, never a safety deadline. Never let learning override a physical safety limit or an existing HIL validity deadline.

Each board task declares cancellation class (checkpointed, safe process cancellation, or noninterruptible), safe-step bound, retry class (idempotent, restart from baseline, or manual), and restore action. Persist the command/input snapshot and content-addressed firmware image, but do not pretend JSONB can snapshot running RAM or peripherals. A replay after preemption uses a new attempt ID and reacquires a lease; it verifies the board and flashes its recorded image before restarting from the beginning, unless a tested task-specific checkpoint explicitly permits more. Repeated preemptions and failed restores are separate outcomes in analytics, not successful short yield samples.

== Deployment and recovery drill

Package one versioned ra8ci binary per target OS/architecture: Linux amd64 for the control VM and Linux guest, Windows amd64 for the Windows guest, and Linux arm64 for the Raspberry Pi 5 board agent. A reviewed systemd unit starts the server in the protected control VM with an unprivileged identity; PostgreSQL is colocated and reached through a local Unix socket. Only a restricted provisioner identity reaches approved Proxmox operations. Disposable guests receive both an official ephemeral GitHub runner and a ra8ci agent whose initial enrollment and lifetime are bound to a capacity reservation, not an as-yet-unknown GitHub run. On Windows the agent runs as a managed service with a deliberately narrow account. The Proxmox host itself runs neither repository code nor the GitHub job worker.

Before production use, perform restore drills for PostgreSQL and artifact storage; server restart during provisioning; agent disconnect during Zig build; server and database outage during a board flash; expired lease with an unresponsive board agent; lost cleanup acknowledgment; and Windows process-tree cancellation. Each drill has a recorded expected state and an operator recovery command. The system is ready only when it reports incomplete work truthfully and refuses unsafe automatic cleanup or board reassignment.
