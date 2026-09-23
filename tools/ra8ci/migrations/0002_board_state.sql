-- The reducer snapshot is the authoritative board state. Typed board tables
-- are a queryable projection updated by the same transaction.
CREATE TABLE board_snapshots (
    board_id text PRIMARY KEY REFERENCES boards(id) ON DELETE RESTRICT,
    version bigint NOT NULL CHECK (version >= 0),
    state jsonb NOT NULL CHECK (jsonb_typeof(state) = 'object'),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE TABLE board_events (
    board_id text NOT NULL REFERENCES board_snapshots(board_id) ON DELETE RESTRICT,
    event_seq bigint NOT NULL CHECK (event_seq > 0),
    id uuid NOT NULL UNIQUE,
    snapshot_version bigint NOT NULL CHECK (snapshot_version > 0),
    kind text NOT NULL,
    happened_at timestamptz NOT NULL,
    actor_id text NOT NULL,
    waiter_id text,
    lease_id text,
    generation bigint NOT NULL CHECK (generation >= 0),
    reason text NOT NULL DEFAULT '',
    PRIMARY KEY (board_id, event_seq)
);
CREATE INDEX board_events_time_idx ON board_events (board_id, happened_at DESC);
CREATE TRIGGER board_events_append_only BEFORE UPDATE OR DELETE ON board_events
    FOR EACH ROW EXECUTE FUNCTION reject_audit_mutation();
REVOKE UPDATE, DELETE, TRUNCATE ON board_events FROM PUBLIC;

ALTER TABLE api_grants DROP CONSTRAINT api_grants_role_check;
ALTER TABLE api_grants ADD CONSTRAINT api_grants_role_check
    CHECK (role IN ('observer', 'submitter', 'board_human', 'board_agent', 'operator'));
ALTER TABLE api_grants ADD CONSTRAINT board_agent_requires_board_scope
    CHECK (role <> 'board_agent' OR board_id <> '');
