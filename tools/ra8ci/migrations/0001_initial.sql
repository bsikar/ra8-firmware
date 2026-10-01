-- Initial control-plane schema. All cross-host timestamps are UTC timestamptz.
CREATE TABLE runs (
    id uuid PRIMARY KEY,
    trigger text NOT NULL CHECK (length(trigger) BETWEEN 1 AND 64),
    actor_id text NOT NULL CHECK (length(actor_id) BETWEEN 1 AND 256),
    repository text NOT NULL CHECK (length(repository) BETWEEN 1 AND 512),
    branch text NOT NULL DEFAULT '',
    commit_sha text NOT NULL CHECK (commit_sha ~ '^[0-9a-f]{40}$'),
    snapshot_sha256 text NOT NULL CHECK (snapshot_sha256 ~ '^[0-9a-f]{64}$'),
    catalog_sha256 text NOT NULL CHECK (catalog_sha256 ~ '^[0-9a-f]{64}$'),
    parent_run_id uuid REFERENCES runs(id),
    state text NOT NULL CHECK (state IN ('queued', 'running', 'terminal')),
    execution_result text CHECK (execution_result IN ('succeeded', 'failed', 'cancelled', 'timed_out', 'incomplete_evidence')),
    cleanup_result text NOT NULL DEFAULT 'not_required' CHECK (cleanup_result IN ('not_required', 'pending', 'succeeded', 'failed')),
    evidence_state text NOT NULL DEFAULT 'pending' CHECK (evidence_state IN ('pending', 'complete', 'incomplete')),
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    started_at timestamptz,
    ended_at timestamptz,
    version bigint NOT NULL DEFAULT 1 CHECK (version > 0),
    CHECK ((state = 'terminal') = (ended_at IS NOT NULL)),
    CHECK (state <> 'terminal' OR execution_result IS NOT NULL)
);
CREATE INDEX runs_repository_created_idx ON runs (repository, created_at DESC);
CREATE INDEX runs_actor_created_idx ON runs (actor_id, created_at DESC);
CREATE INDEX runs_state_created_idx ON runs (state, created_at);

CREATE TABLE tasks (
    id uuid PRIMARY KEY,
    run_id uuid NOT NULL REFERENCES runs(id) ON DELETE RESTRICT,
    task_key text NOT NULL CHECK (length(task_key) BETWEEN 1 AND 128),
    name text NOT NULL CHECK (length(name) BETWEEN 1 AND 128),
    arguments jsonb NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(arguments) = 'object'),
    tier text NOT NULL CHECK (tier IN ('required', 'optional', 'nightly')),
    scope text NOT NULL CHECK (scope IN ('safe-local-read-only', 'safe-local-write-working-tree', 'runner', 'linux-vm', 'windows-vm', 'hil')),
    host_class text NOT NULL DEFAULT '',
    state text NOT NULL CHECK (state IN ('scheduled', 'running', 'succeeded', 'failed', 'timed_out', 'cancelled', 'preempted', 'lost', 'skipped')),
    skip_reason text,
    deadline_seconds integer NOT NULL CHECK (deadline_seconds BETWEEN 1 AND 86400),
    enqueued_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    started_at timestamptz,
    ended_at timestamptz,
    version bigint NOT NULL DEFAULT 1 CHECK (version > 0),
    UNIQUE (run_id, task_key),
    UNIQUE (run_id, id),
    CHECK ((state IN ('succeeded', 'failed', 'timed_out', 'cancelled', 'preempted', 'lost', 'skipped')) = (ended_at IS NOT NULL))
);
CREATE INDEX tasks_queue_idx ON tasks (tier, enqueued_at) WHERE state = 'scheduled';
CREATE INDEX tasks_run_state_idx ON tasks (run_id, state);

CREATE TABLE task_edges (
    run_id uuid NOT NULL REFERENCES runs(id) ON DELETE RESTRICT,
    task_id uuid NOT NULL,
    depends_on_task_id uuid NOT NULL,
    PRIMARY KEY (run_id, task_id, depends_on_task_id),
    FOREIGN KEY (run_id, task_id) REFERENCES tasks(run_id, id) ON DELETE RESTRICT,
    FOREIGN KEY (run_id, depends_on_task_id) REFERENCES tasks(run_id, id) ON DELETE RESTRICT,
    CHECK (task_id <> depends_on_task_id)
);
CREATE INDEX task_edges_parent_idx ON task_edges (run_id, depends_on_task_id);

CREATE TABLE agents (
    id uuid PRIMARY KEY,
    principal_id text NOT NULL UNIQUE,
    host_class text NOT NULL,
    version text NOT NULL,
    capabilities jsonb NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(capabilities) = 'object'),
    enrollment_reservation_id uuid UNIQUE,
    vm_marker text UNIQUE,
    capacity integer NOT NULL CHECK (capacity >= 0),
    enrolled_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    last_heartbeat_at timestamptz,
    revoked_at timestamptz,
    state text NOT NULL CHECK (state IN ('enrolled', 'healthy', 'draining', 'offline', 'revoked')),
    version_counter bigint NOT NULL DEFAULT 1
);
CREATE INDEX agents_state_heartbeat_idx ON agents (state, last_heartbeat_at);

CREATE TABLE task_attempts (
    id uuid PRIMARY KEY,
    task_id uuid NOT NULL REFERENCES tasks(id) ON DELETE RESTRICT,
    attempt_no integer NOT NULL CHECK (attempt_no > 0),
    agent_id uuid REFERENCES agents(id),
    state text NOT NULL CHECK (state IN ('issued', 'acknowledged', 'running', 'succeeded', 'failed', 'timed_out', 'cancelled', 'preempted', 'lost')),
    engine text NOT NULL CHECK (length(engine) BETWEEN 1 AND 64),
    host text NOT NULL DEFAULT '',
    host_cores integer CHECK (host_cores > 0),
    host_ram_bytes bigint CHECK (host_ram_bytes > 0),
    host_load double precision CHECK (host_load >= 0),
    host_facts jsonb NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(host_facts) = 'object'),
    issued_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    started_at timestamptz,
    ended_at timestamptz,
    deadline_at timestamptz NOT NULL,
    child_exit_code integer,
    hit_deadline boolean NOT NULL DEFAULT false,
    evidence_complete boolean NOT NULL DEFAULT false,
    result_reason text,
    version bigint NOT NULL DEFAULT 1,
    UNIQUE (task_id, attempt_no),
    CHECK (ended_at IS NULL OR started_at IS NOT NULL),
    CHECK (ended_at IS NULL OR ended_at >= started_at)
);
CREATE INDEX task_attempts_state_deadline_idx ON task_attempts (state, deadline_at);
CREATE INDEX task_attempts_agent_state_idx ON task_attempts (agent_id, state);

CREATE TABLE task_steps (
    attempt_id uuid NOT NULL REFERENCES task_attempts(id) ON DELETE RESTRICT,
    step_key text NOT NULL CHECK (length(step_key) BETWEEN 1 AND 128),
    ordinal integer NOT NULL CHECK (ordinal >= 0),
    phase text NOT NULL CHECK (length(phase) BETWEEN 1 AND 64),
    started_at timestamptz NOT NULL,
    ended_at timestamptz NOT NULL,
    duration_ns bigint NOT NULL CHECK (duration_ns >= 0),
    state text NOT NULL CHECK (state IN ('succeeded', 'failed', 'timed_out', 'cancelled', 'skipped')),
    child_exit_code integer,
    PRIMARY KEY (attempt_id, step_key),
    UNIQUE (attempt_id, ordinal),
    CHECK (ended_at >= started_at)
);

CREATE TABLE resource_samples (
    attempt_id uuid NOT NULL REFERENCES task_attempts(id) ON DELETE RESTRICT,
    sample_no bigint NOT NULL CHECK (sample_no >= 0),
    step_key text,
    monotonic_offset_ns bigint NOT NULL CHECK (monotonic_offset_ns >= 0),
    interval_ns bigint NOT NULL CHECK (interval_ns > 0),
    dropped_count bigint NOT NULL DEFAULT 0 CHECK (dropped_count >= 0),
    cpu_time_ns bigint CHECK (cpu_time_ns >= 0),
    process_cpu_percent double precision CHECK (process_cpu_percent >= 0),
    process_rss_bytes bigint CHECK (process_rss_bytes >= 0),
    process_peak_rss_bytes bigint CHECK (process_peak_rss_bytes >= 0),
    host_load double precision CHECK (host_load >= 0),
    host_ram_available_bytes bigint CHECK (host_ram_available_bytes >= 0),
    disk_read_bytes bigint CHECK (disk_read_bytes >= 0),
    disk_write_bytes bigint CHECK (disk_write_bytes >= 0),
    network_rx_bytes bigint CHECK (network_rx_bytes >= 0),
    network_tx_bytes bigint CHECK (network_tx_bytes >= 0),
    clock_skew_ns bigint,
    server_received_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (attempt_id, sample_no),
    FOREIGN KEY (attempt_id, step_key) REFERENCES task_steps(attempt_id, step_key)
);

CREATE TABLE artifacts (
    sha256 text PRIMARY KEY CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    size_bytes bigint NOT NULL CHECK (size_bytes >= 0),
    storage_key text NOT NULL UNIQUE,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    retained_until timestamptz
);
CREATE TABLE attempt_artifacts (
    attempt_id uuid NOT NULL REFERENCES task_attempts(id) ON DELETE RESTRICT,
    artifact_sha256 text NOT NULL REFERENCES artifacts(sha256) ON DELETE RESTRICT,
    kind text NOT NULL,
    PRIMARY KEY (attempt_id, artifact_sha256, kind)
);
CREATE TABLE log_chunks (
    attempt_id uuid NOT NULL REFERENCES task_attempts(id) ON DELETE RESTRICT,
    stream text NOT NULL CHECK (stream IN ('stdout', 'stderr', 'system')),
    seq bigint NOT NULL CHECK (seq >= 0),
    step_key text,
    monotonic_offset_ns bigint NOT NULL CHECK (monotonic_offset_ns >= 0),
    sha256 text NOT NULL CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    bytes bytea NOT NULL CHECK (octet_length(bytes) <= 65536),
    received_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (attempt_id, stream, seq),
    FOREIGN KEY (attempt_id, step_key) REFERENCES task_steps(attempt_id, step_key)
);

CREATE TABLE boards (
    id text PRIMARY KEY CHECK (length(id) BETWEEN 1 AND 128),
    generation bigint NOT NULL DEFAULT 0 CHECK (generation >= 0),
    state text NOT NULL CHECK (state IN ('available', 'held', 'recovery_required', 'quarantined')),
    recovery_required boolean NOT NULL DEFAULT false,
    last_neutral_at timestamptz,
    version bigint NOT NULL DEFAULT 1,
    CHECK (state <> 'available' OR recovery_required = false)
);
CREATE TABLE board_waiters (
    id uuid PRIMARY KEY,
    board_id text NOT NULL REFERENCES boards(id) ON DELETE RESTRICT,
    actor_id text NOT NULL,
    priority text NOT NULL CHECK (priority IN ('human', 'ci', 'agent')),
    reason text NOT NULL CHECK (length(reason) BETWEEN 1 AND 1024),
    requested_duration_seconds integer NOT NULL CHECK (requested_duration_seconds > 0),
    state text NOT NULL CHECK (state IN ('waiting', 'granted', 'cancelled', 'expired')),
    requested_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    ended_at timestamptz,
    request_id text NOT NULL UNIQUE
);
CREATE INDEX board_waiters_queue_idx ON board_waiters (board_id, priority, requested_at) WHERE state = 'waiting';
CREATE TABLE board_leases (
    id uuid PRIMARY KEY,
    board_id text NOT NULL REFERENCES boards(id) ON DELETE RESTRICT,
    waiter_id uuid REFERENCES board_waiters(id) ON DELETE RESTRICT,
    generation bigint NOT NULL CHECK (generation > 0),
    holder_id text NOT NULL,
    priority text NOT NULL CHECK (priority IN ('human', 'ci', 'agent')),
    reason text NOT NULL,
    requested_duration_seconds integer NOT NULL CHECK (requested_duration_seconds > 0),
    granted_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    expires_at timestamptz NOT NULL,
    ended_at timestamptz,
    end_reason text,
    yield_requested_at timestamptz,
    state text NOT NULL CHECK (state IN ('pending', 'active', 'ended')),
    version bigint NOT NULL DEFAULT 1,
    UNIQUE (board_id, generation),
    CHECK (expires_at > granted_at),
    CHECK ((state = 'ended') = (ended_at IS NOT NULL))
);
CREATE UNIQUE INDEX board_one_live_lease_idx ON board_leases (board_id) WHERE state IN ('pending', 'active');
CREATE INDEX board_leases_holder_idx ON board_leases (holder_id, state);
CREATE TABLE board_sessions (
    id uuid PRIMARY KEY,
    board_id text NOT NULL REFERENCES boards(id) ON DELETE RESTRICT,
    lease_id uuid NOT NULL REFERENCES board_leases(id) ON DELETE RESTRICT,
    owner_id text NOT NULL,
    fixture_revision text NOT NULL,
    phase text NOT NULL,
    restore_policy text NOT NULL,
    baseline_image_sha256 text,
    current_image_sha256 text,
    safe_checkpoint text,
    metadata_version integer NOT NULL CHECK (metadata_version > 0),
    metadata jsonb NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(metadata) = 'object'),
    started_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    ended_at timestamptz
);

CREATE TABLE github_jobs (
    id uuid PRIMARY KEY,
    installation_id bigint NOT NULL,
    repository text NOT NULL,
    workflow_run_id bigint NOT NULL,
    workflow_attempt integer NOT NULL CHECK (workflow_attempt > 0),
    job_id bigint NOT NULL UNIQUE,
    scale_set text NOT NULL,
    labels jsonb NOT NULL DEFAULT '[]'::jsonb CHECK (jsonb_typeof(labels) = 'array'),
    runner_id bigint,
    runner_name text,
    head_sha text NOT NULL CHECK (head_sha ~ '^[0-9a-f]{40}$'),
    conclusion text,
    correlation_state text NOT NULL CHECK (correlation_state IN ('unverified', 'verified', 'contradictory')),
    run_id uuid REFERENCES runs(id) ON DELETE RESTRICT,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    version bigint NOT NULL DEFAULT 1
);
CREATE INDEX github_jobs_run_idx ON github_jobs (workflow_run_id, workflow_attempt);
CREATE TABLE github_scaleset_inbox (
    scale_set_id text NOT NULL CHECK (length(scale_set_id) BETWEEN 1 AND 128),
    session_id text NOT NULL CHECK (length(session_id) BETWEEN 1 AND 256),
    message_id text NOT NULL CHECK (length(message_id) BETWEEN 1 AND 256),
    payload jsonb NOT NULL CHECK (jsonb_typeof(payload) = 'object'),
    received_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    processed_at timestamptz,
    PRIMARY KEY (scale_set_id, session_id, message_id)
);
CREATE INDEX github_scaleset_inbox_pending_idx ON github_scaleset_inbox (received_at)
    WHERE processed_at IS NULL;
CREATE TABLE runner_lifecycle (
    id uuid PRIMARY KEY,
    github_job_id uuid REFERENCES github_jobs(id) ON DELETE RESTRICT,
    reservation_id uuid NOT NULL UNIQUE,
    vm_marker text NOT NULL UNIQUE,
    vm_id integer UNIQUE,
    host_class text NOT NULL,
    image_digest text NOT NULL,
    state text NOT NULL CHECK (state IN ('requested', 'provisioning', 'healthy', 'jit_issued', 'registered_idle', 'busy', 'draining', 'deregistered', 'destroyed')),
    reconcile_required boolean NOT NULL DEFAULT false,
    runner_id bigint UNIQUE,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    version bigint NOT NULL DEFAULT 1
);
CREATE INDEX runner_lifecycle_state_idx ON runner_lifecycle (state, reconcile_required);
CREATE TABLE external_operations (
    id uuid PRIMARY KEY,
    kind text NOT NULL,
    target text NOT NULL,
    expected_identity jsonb NOT NULL CHECK (jsonb_typeof(expected_identity) = 'object'),
    state text NOT NULL CHECK (state IN ('intent', 'in_flight', 'succeeded', 'failed', 'unknown_outcome', 'reconciled')),
    requested_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    completed_at timestamptz,
    version bigint NOT NULL DEFAULT 1
);
CREATE INDEX external_operations_reconcile_idx ON external_operations (state, requested_at) WHERE state IN ('intent', 'in_flight', 'unknown_outcome');

CREATE TABLE idempotency_keys (
    principal_id text NOT NULL,
    method text NOT NULL,
    path text NOT NULL,
    key text NOT NULL,
    request_sha256 text NOT NULL CHECK (request_sha256 ~ '^[0-9a-f]{64}$'),
    response_status integer NOT NULL,
    response_body jsonb NOT NULL,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    expires_at timestamptz NOT NULL,
    PRIMARY KEY (principal_id, method, path, key)
);
CREATE INDEX idempotency_expiry_idx ON idempotency_keys (expires_at);

CREATE TABLE run_events (
    run_id uuid NOT NULL REFERENCES runs(id) ON DELETE RESTRICT,
    event_seq bigint NOT NULL CHECK (event_seq > 0),
    id uuid NOT NULL UNIQUE,
    kind text NOT NULL,
    data jsonb NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(data) = 'object'),
    happened_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (run_id, event_seq)
);
CREATE INDEX run_events_time_idx ON run_events (happened_at);

CREATE TABLE audit (
    id uuid PRIMARY KEY,
    happened_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    actor_id text NOT NULL,
    action text NOT NULL,
    target_type text NOT NULL,
    target_id text NOT NULL,
    correlation_run_id uuid REFERENCES runs(id) ON DELETE RESTRICT,
    request_id text,
    outcome text NOT NULL,
    previous_state text,
    new_state text,
    reason jsonb NOT NULL DEFAULT '{}'::jsonb CHECK (jsonb_typeof(reason) = 'object')
);
CREATE INDEX audit_time_idx ON audit (happened_at DESC);
CREATE INDEX audit_run_idx ON audit (correlation_run_id, happened_at);
CREATE INDEX audit_target_idx ON audit (target_type, target_id, happened_at DESC);
CREATE FUNCTION reject_audit_mutation() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'audit is append-only';
END;
$$;
CREATE TRIGGER audit_append_only BEFORE UPDATE OR DELETE ON audit
    FOR EACH ROW EXECUTE FUNCTION reject_audit_mutation();
REVOKE UPDATE, DELETE, TRUNCATE ON audit FROM PUBLIC;

CREATE TABLE api_principals (
    cert_sha256 text PRIMARY KEY CHECK (cert_sha256 ~ '^[0-9a-f]{64}$'),
    principal_id text NOT NULL UNIQUE,
    kind text NOT NULL CHECK (kind IN ('human', 'github_app', 'agent', 'board_agent')),
    revoked_at timestamptz,
    expires_at timestamptz NOT NULL
);
CREATE TABLE api_grants (
    principal_id text NOT NULL REFERENCES api_principals(principal_id) ON DELETE RESTRICT,
    repository text NOT NULL,
    role text NOT NULL CHECK (role IN ('observer', 'submitter', 'board_human', 'operator')),
    board_id text NOT NULL DEFAULT '',
    PRIMARY KEY (principal_id, repository, role, board_id)
);
