-- Approved fixture context is installed by the migration/operator role, not
-- derived from a board command or an HTTP request.
CREATE TABLE board_fixture_profiles (
    board_id text PRIMARY KEY CHECK (length(board_id) BETWEEN 1 AND 128),
    fixture_revision text NOT NULL CHECK (length(fixture_revision) BETWEEN 1 AND 128),
    profile_sha256 text NOT NULL CHECK (profile_sha256 ~ '^[0-9a-f]{64}$'),
    restore_policy text NOT NULL CHECK (length(restore_policy) BETWEEN 1 AND 128),
    approved_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    version bigint NOT NULL DEFAULT 1 CHECK (version > 0)
);
REVOKE INSERT, UPDATE, DELETE, TRUNCATE ON board_fixture_profiles FROM PUBLIC;

ALTER TABLE board_sessions ADD COLUMN profile_sha256 text
    CHECK (profile_sha256 ~ '^[0-9a-f]{64}$');
CREATE UNIQUE INDEX board_one_active_session_idx ON board_sessions (board_id)
    WHERE ended_at IS NULL;

CREATE TABLE board_recovery_context (
    board_id text PRIMARY KEY REFERENCES board_snapshots(board_id) ON DELETE RESTRICT,
    plan_id text NOT NULL CHECK (length(plan_id) BETWEEN 1 AND 256),
    started_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    ended_at timestamptz,
    version bigint NOT NULL DEFAULT 1 CHECK (version > 0)
);

CREATE TABLE board_neutral_challenges (
    id uuid PRIMARY KEY,
    nonce text NOT NULL UNIQUE CHECK (nonce ~ '^[0-9a-f]{64}$'),
    board_id text NOT NULL REFERENCES board_snapshots(board_id) ON DELETE RESTRICT,
    purpose text NOT NULL CHECK (purpose IN ('release', 'recovery')),
    lease_id uuid REFERENCES board_leases(id) ON DELETE RESTRICT,
    generation bigint NOT NULL CHECK (generation >= 0),
    snapshot_version bigint NOT NULL CHECK (snapshot_version >= 0),
    agent_high_water bigint NOT NULL CHECK (agent_high_water >= 0),
    fixture_revision text NOT NULL,
    profile_sha256 text NOT NULL CHECK (profile_sha256 ~ '^[0-9a-f]{64}$'),
    restore_policy text NOT NULL,
    recovery_plan_id text,
    issued_at timestamptz NOT NULL,
    expires_at timestamptz NOT NULL,
    consumed_at timestamptz,
    receipt_sha256 text CHECK (receipt_sha256 ~ '^[0-9a-f]{64}$'),
    outcome text CHECK (outcome IN ('accepted', 'rejected')),
    CHECK (expires_at > issued_at),
    CHECK ((consumed_at IS NULL) = (outcome IS NULL)),
    CHECK ((purpose = 'release' AND lease_id IS NOT NULL AND recovery_plan_id IS NULL)
        OR (purpose = 'recovery' AND recovery_plan_id IS NOT NULL))
);
CREATE INDEX board_neutral_pending_idx ON board_neutral_challenges (board_id, expires_at)
    WHERE consumed_at IS NULL;
CREATE TRIGGER board_neutral_challenge_no_delete BEFORE DELETE ON board_neutral_challenges
    FOR EACH ROW EXECUTE FUNCTION reject_audit_mutation();
REVOKE DELETE, TRUNCATE ON board_neutral_challenges FROM PUBLIC;

-- Runtime identities are provisioned separately and never own these tables.
-- These revocations complement the runtime startup privilege check.
REVOKE UPDATE, DELETE, TRUNCATE ON audit, board_events FROM PUBLIC;
CREATE TRIGGER run_events_append_only BEFORE UPDATE OR DELETE ON run_events
    FOR EACH ROW EXECUTE FUNCTION reject_audit_mutation();
REVOKE UPDATE, DELETE, TRUNCATE ON run_events FROM PUBLIC;
