-- Bind each durable hardware segment to the live HIL task attempt that authorized it.
ALTER TABLE board_segments ADD COLUMN attempt_id uuid
    REFERENCES task_attempts(id) ON DELETE RESTRICT;
CREATE INDEX board_segments_attempt_idx ON board_segments (attempt_id)
    WHERE attempt_id IS NOT NULL;
