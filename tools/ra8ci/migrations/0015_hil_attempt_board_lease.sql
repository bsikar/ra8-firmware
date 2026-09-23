-- Tie each HIL execution attempt to the exact durable board lease it holds.
ALTER TABLE task_attempts ADD COLUMN board_lease_id uuid
    REFERENCES board_leases(id) ON DELETE RESTRICT;
CREATE INDEX task_attempts_board_lease_idx ON task_attempts (board_lease_id)
    WHERE board_lease_id IS NOT NULL;
