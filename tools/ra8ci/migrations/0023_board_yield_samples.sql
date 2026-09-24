-- Retain the request-to-neutral measurement a yield leaves behind, so the
-- dynamic yield budget has history to estimate over. Written by the same
-- transaction that ends the handoff.
CREATE TABLE board_yield_samples (
    lease_id uuid PRIMARY KEY REFERENCES board_leases(id) ON DELETE RESTRICT,
    board_id text NOT NULL REFERENCES boards(id) ON DELETE RESTRICT,
    waiter_id uuid REFERENCES board_waiters(id) ON DELETE RESTRICT,
    board_model text NOT NULL CHECK (length(board_model) BETWEEN 1 AND 256),
    fixture_revision text NOT NULL CHECK (length(fixture_revision) BETWEEN 1 AND 256),
    task_name text NOT NULL CHECK (length(task_name) BETWEEN 1 AND 256),
    catalog_digest text NOT NULL CHECK (length(catalog_digest) BETWEEN 1 AND 256),
    image_sha256 text NOT NULL DEFAULT '' CHECK (length(image_sha256) <= 256),
    requested_at timestamptz NOT NULL,
    neutral_at timestamptz,
    exclusion_reason text CHECK (length(exclusion_reason) BETWEEN 1 AND 256),
    safety_overrun boolean NOT NULL DEFAULT false,
    shown_target_ms bigint NOT NULL DEFAULT 0 CHECK (shown_target_ms >= 0),
    recorded_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    -- A sample is either a measurement or a censored row naming why it is not
    -- one. Both at once would be counted twice by the estimator, and neither
    -- would be a row describing nothing.
    CHECK ((neutral_at IS NULL) <> (exclusion_reason IS NULL)),
    CHECK (neutral_at IS NULL OR neutral_at >= requested_at),
    -- An overrun is a claim about a measured latency, so a censored row must
    -- not carry one.
    CHECK (NOT safety_overrun OR neutral_at IS NOT NULL)
);
-- The estimator reads one cohort, newest first, bounded by age.
CREATE INDEX board_yield_samples_cohort_idx ON board_yield_samples
    (board_id, board_model, fixture_revision, task_name, catalog_digest, image_sha256, requested_at DESC);
