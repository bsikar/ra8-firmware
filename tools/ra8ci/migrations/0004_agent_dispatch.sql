-- Agent grants and assignments are fenced independently of task row versions.
ALTER TABLE api_grants DROP CONSTRAINT api_grants_role_check;
ALTER TABLE api_grants ADD CONSTRAINT api_grants_role_check
    CHECK (role IN ('observer', 'submitter', 'board_human', 'operator', 'board_agent', 'agent_executor'));

ALTER TABLE task_attempts ADD COLUMN assignment_id uuid UNIQUE;
ALTER TABLE task_attempts ADD COLUMN assignment_version bigint;
ALTER TABLE task_attempts ADD COLUMN fencing_token bigint;
ALTER TABLE task_attempts ADD COLUMN agent_last_heartbeat_at timestamptz;
ALTER TABLE task_attempts ADD CONSTRAINT agent_grant_identity CHECK (
    (agent_id IS NULL AND assignment_id IS NULL AND assignment_version IS NULL AND fencing_token IS NULL)
    OR (agent_id IS NOT NULL AND assignment_id IS NOT NULL AND assignment_version > 0 AND fencing_token > 0)
);
CREATE UNIQUE INDEX log_chunks_global_sequence_idx ON log_chunks (attempt_id, seq);
