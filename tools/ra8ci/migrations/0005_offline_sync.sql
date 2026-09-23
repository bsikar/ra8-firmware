-- Offline results are claims made by an authenticated local client. They are
-- never queued as server-executable CI tasks or counted as trusted CI passes.
-- The principal/local ID key is permanent: a retry cannot rebind old history.
CREATE TABLE local_runs (
    id uuid PRIMARY KEY,
    principal_id text NOT NULL CHECK (length(principal_id) BETWEEN 1 AND 256),
    local_id text NOT NULL CHECK (local_id ~ '^[0-9a-f]{32}$'),
    payload_sha256 text NOT NULL CHECK (payload_sha256 ~ '^[0-9a-f]{64}$'),
    source_verification text NOT NULL CHECK (source_verification IN ('verified', 'unverified')),
    repository text NOT NULL CHECK (length(repository) BETWEEN 1 AND 512),
    branch text NOT NULL DEFAULT '',
    commit_sha text CHECK (commit_sha ~ '^[0-9a-f]{40}$'),
    snapshot_sha256 text CHECK (snapshot_sha256 ~ '^[0-9a-f]{64}$'),
    catalog_sha256 text NOT NULL CHECK (catalog_sha256 ~ '^[0-9a-f]{64}$'),
    task_name text NOT NULL CHECK (length(task_name) BETWEEN 1 AND 128),
    tier text NOT NULL CHECK (tier IN ('required', 'optional', 'nightly')),
    scope text NOT NULL CHECK (scope IN ('safe-local-read-only', 'safe-local-write-working-tree')),
    deadline_seconds integer NOT NULL CHECK (deadline_seconds BETWEEN 1 AND 86400),
    arguments jsonb NOT NULL DEFAULT '[]'::jsonb CHECK (jsonb_typeof(arguments)='array'),
    started_at timestamptz NOT NULL,
    finished_at timestamptz NOT NULL,
    duration_ns bigint NOT NULL CHECK (duration_ns >= 0),
    result text NOT NULL CHECK (result IN ('succeeded','failed','timed_out','cancelled','incomplete_evidence')),
    child_exit_code integer,
    executor_error text NOT NULL DEFAULT '',
    received_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (principal_id, local_id),
    CHECK (finished_at >= started_at),
    CHECK (source_verification<>'verified' OR (commit_sha IS NOT NULL AND snapshot_sha256 IS NOT NULL)),
    CHECK (result <> 'succeeded' OR (child_exit_code=0 AND executor_error=''))
);
CREATE INDEX local_runs_principal_time_idx ON local_runs (principal_id, received_at DESC);
CREATE INDEX local_runs_task_time_idx ON local_runs (task_name, received_at DESC);
CREATE TRIGGER local_runs_append_only BEFORE UPDATE OR DELETE ON local_runs
    FOR EACH ROW EXECUTE FUNCTION reject_audit_mutation();
REVOKE UPDATE, DELETE, TRUNCATE ON local_runs FROM PUBLIC;

CREATE TABLE local_run_steps (
    local_run_id uuid NOT NULL REFERENCES local_runs(id) ON DELETE RESTRICT,
    ordinal integer NOT NULL CHECK (ordinal >= 0),
    step_key text NOT NULL CHECK (length(step_key) BETWEEN 1 AND 128),
    started_at timestamptz NOT NULL,
    ended_at timestamptz NOT NULL,
    duration_ns bigint NOT NULL CHECK (duration_ns >= 0),
    child_exit_code integer NOT NULL,
    timed_out boolean NOT NULL,
    cancelled boolean NOT NULL,
    stdout_sha256 text NOT NULL CHECK (stdout_sha256 ~ '^[0-9a-f]{64}$'),
    stderr_sha256 text NOT NULL CHECK (stderr_sha256 ~ '^[0-9a-f]{64}$'),
    stdout_bytes bigint NOT NULL CHECK (stdout_bytes >= 0),
    stderr_bytes bigint NOT NULL CHECK (stderr_bytes >= 0),
    PRIMARY KEY (local_run_id, ordinal),
    UNIQUE (local_run_id, step_key),
    CHECK (ended_at >= started_at)
);
CREATE TRIGGER local_run_steps_append_only BEFORE UPDATE OR DELETE ON local_run_steps
    FOR EACH ROW EXECUTE FUNCTION reject_audit_mutation();
REVOKE UPDATE, DELETE, TRUNCATE ON local_run_steps FROM PUBLIC;
