-- A step's declared outputs stream back the way logs do: ordered chunks under
-- the attempt's grant, then one manifest that closes the artifact. The
-- manifest is evidence about bytes already uploaded, so it is checked against
-- what is actually on file here and can never claim an upload that did not
-- happen.
CREATE TABLE agent_artifacts (
    attempt_id uuid NOT NULL REFERENCES task_attempts(id) ON DELETE RESTRICT,
    path text NOT NULL CHECK (length(path) BETWEEN 1 AND 256),
    step_key text NOT NULL CHECK (length(step_key) BETWEEN 1 AND 128),
    total_bytes bigint NOT NULL DEFAULT 0 CHECK (total_bytes BETWEEN 0 AND 67108864),
    chunk_count bigint NOT NULL DEFAULT 0 CHECK (chunk_count BETWEEN 0 AND 1024),
    sha256 text CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    truncated boolean NOT NULL DEFAULT false,
    captured_at timestamptz,
    closed_at timestamptz,
    started_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (attempt_id, path),
    -- A closed artifact carries its digest and its capture time; an open one
    -- carries neither, so a reader never mistakes a partial upload for a file.
    CHECK ((closed_at IS NULL) = (sha256 IS NULL)),
    CHECK ((closed_at IS NULL) = (captured_at IS NULL)),
    CHECK (closed_at IS NULL OR (total_bytes > 0 AND chunk_count > 0))
);

CREATE TABLE agent_artifact_chunks (
    attempt_id uuid NOT NULL,
    path text NOT NULL,
    seq bigint NOT NULL CHECK (seq BETWEEN 1 AND 1024),
    byte_offset bigint NOT NULL CHECK (byte_offset >= 0),
    sha256 text NOT NULL CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    bytes bytea NOT NULL CHECK (octet_length(bytes) BETWEEN 1 AND 262144),
    received_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (attempt_id, path, seq),
    FOREIGN KEY (attempt_id, path) REFERENCES agent_artifacts (attempt_id, path) ON DELETE RESTRICT,
    -- Reassembly is concatenation in sequence order, so two chunks claiming
    -- one position in the file is a contradiction the schema refuses.
    UNIQUE (attempt_id, path, byte_offset)
);

CREATE INDEX agent_artifacts_open_idx ON agent_artifacts (attempt_id) WHERE closed_at IS NULL;
