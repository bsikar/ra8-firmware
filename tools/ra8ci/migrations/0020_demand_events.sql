-- One row per unit of demand (a job and its run attempt), however many
-- deliveries describe it, plus the deliveries themselves so a replayed
-- webhook is recognized as the copy it is instead of moving the row twice.
CREATE TABLE demand_events (
    demand_key text PRIMARY KEY CHECK (demand_key ~ '^[0-9]{1,19}/[0-9]{1,4}$'),
    job_id bigint NOT NULL CHECK (job_id > 0),
    run_id bigint NOT NULL CHECK (run_id > 0),
    run_attempt integer NOT NULL CHECK (run_attempt BETWEEN 1 AND 1000),
    phase text NOT NULL CHECK (phase IN ('queued', 'in_progress', 'completed')),
    adapter text NOT NULL CHECK (length(adapter) BETWEEN 1 AND 64),
    delivery_id text NOT NULL CHECK (length(delivery_id) BETWEEN 1 AND 128),
    owner text NOT NULL CHECK (length(owner) BETWEEN 1 AND 39),
    repository text NOT NULL CHECK (length(repository) BETWEEN 1 AND 100),
    workflow text NOT NULL CHECK (length(workflow) BETWEEN 1 AND 255),
    job_name text NOT NULL CHECK (length(job_name) BETWEEN 1 AND 255),
    commit_sha text NOT NULL CHECK (commit_sha ~ '^[0-9a-f]{40}$'),
    labels jsonb NOT NULL CHECK (jsonb_typeof(labels) = 'array'
        AND jsonb_array_length(labels) BETWEEN 1 AND 32),
    runner_name text,
    conclusion text,
    queued_at timestamptz NOT NULL,
    started_at timestamptz,
    completed_at timestamptz,
    observed_at timestamptz NOT NULL,
    first_seen_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    version bigint NOT NULL DEFAULT 1,
    -- A conclusion exists exactly when the job is over, and a phase past
    -- queued has a start: the same rule the adapter contract states, kept
    -- here so a future writer cannot store a shape placement cannot read.
    CHECK ((phase = 'completed') = (conclusion IS NOT NULL)),
    CHECK ((phase = 'queued') = (started_at IS NULL)),
    CHECK ((phase = 'completed') = (completed_at IS NOT NULL)),
    CHECK (completed_at IS NULL OR completed_at >= queued_at)
);
-- The reconciliation pass reads demand that has not finished, oldest first.
CREATE INDEX demand_events_open_idx ON demand_events (queued_at)
    WHERE phase <> 'completed';
CREATE TABLE demand_deliveries (
    adapter text NOT NULL CHECK (length(adapter) BETWEEN 1 AND 64),
    delivery_id text NOT NULL CHECK (length(delivery_id) BETWEEN 1 AND 128),
    demand_key text NOT NULL REFERENCES demand_events(demand_key) ON DELETE RESTRICT,
    phase text NOT NULL CHECK (phase IN ('queued', 'in_progress', 'completed')),
    observed_at timestamptz NOT NULL,
    received_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (adapter, delivery_id)
);
CREATE INDEX demand_deliveries_key_idx ON demand_deliveries (demand_key, received_at);
