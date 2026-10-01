ALTER TABLE runs
    ADD COLUMN cancel_requested_at timestamptz,
    ADD COLUMN cancel_requested_by text,
    ADD CONSTRAINT runs_cancel_request_pair CHECK
        ((cancel_requested_at IS NULL) = (cancel_requested_by IS NULL));
