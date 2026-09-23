-- Durable, append-only HIL timing observations keyed by reviewed workload identity.
CREATE TABLE hil_observations (
    id uuid PRIMARY KEY,
    attempt_id uuid NOT NULL REFERENCES task_attempts(id) ON DELETE RESTRICT,
    manifest_path text NOT NULL CHECK (manifest_path LIKE 'examples/%' AND length(manifest_path) <= 512),
    board_model text NOT NULL CHECK (length(board_model) BETWEEN 1 AND 128),
    fixture_revision text NOT NULL CHECK (length(fixture_revision) BETWEEN 1 AND 128),
    profile_sha256 text NOT NULL CHECK (profile_sha256 ~ '^[0-9a-f]{64}$'),
    program_family text NOT NULL CHECK (length(program_family) BETWEEN 1 AND 128),
    mode text NOT NULL CHECK (mode IN ('alive','uart_scrape','rtt_scrape','jlink_memprobe','hil_eth_tcp','c6_camera_livestream')),
    step_key text NOT NULL CHECK (length(step_key) BETWEEN 1 AND 128),
    duration_ns bigint NOT NULL CHECK (duration_ns > 0 AND duration_ns <= 3600000000000),
    succeeded boolean NOT NULL,
    evidence_complete boolean NOT NULL,
    timed_out boolean NOT NULL,
    observed_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (attempt_id, manifest_path, board_model, fixture_revision, profile_sha256, program_family, mode)
);
CREATE INDEX hil_observations_workload_time_idx ON hil_observations
    (manifest_path, board_model, fixture_revision, profile_sha256, program_family, mode, observed_at DESC);
CREATE TRIGGER hil_observations_append_only BEFORE UPDATE OR DELETE ON hil_observations
    FOR EACH ROW EXECUTE FUNCTION reject_audit_mutation();
REVOKE UPDATE, DELETE, TRUNCATE ON hil_observations FROM PUBLIC;
