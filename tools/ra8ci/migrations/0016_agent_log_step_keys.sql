-- Attribute remotely streamed output to the reviewed task step before terminal receipt.
ALTER TABLE log_chunks ADD COLUMN agent_step_key text CHECK (agent_step_key IS NULL OR length(agent_step_key) BETWEEN 1 AND 128);
