-- Serialize a bounded hardware operation against board lease transitions.
CREATE TABLE board_segments (
    id uuid PRIMARY KEY,
    board_id text NOT NULL REFERENCES boards(id) ON DELETE RESTRICT,
    lease_id uuid NOT NULL REFERENCES board_leases(id) ON DELETE RESTRICT,
    generation bigint NOT NULL CHECK (generation > 0),
    actor_id text NOT NULL REFERENCES api_principals(principal_id) ON DELETE RESTRICT,
    segment_key text NOT NULL CHECK (length(segment_key) BETWEEN 1 AND 128),
    started_at timestamptz NOT NULL,
    deadline_at timestamptz NOT NULL,
    recovery_margin_ms bigint NOT NULL CHECK (recovery_margin_ms >= 0),
    ended_at timestamptz,
    outcome text CHECK (outcome IN ('completed','failed','yielded')),
    CHECK (deadline_at > started_at),
    CHECK ((ended_at IS NULL) = (outcome IS NULL))
);
CREATE UNIQUE INDEX board_segments_one_open_per_board
    ON board_segments (board_id) WHERE ended_at IS NULL;
CREATE INDEX board_segments_lease_idx ON board_segments (lease_id, started_at DESC);
