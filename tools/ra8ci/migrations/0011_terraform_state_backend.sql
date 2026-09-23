ALTER TABLE api_grants DROP CONSTRAINT api_grants_role_check;
ALTER TABLE api_grants ADD CONSTRAINT api_grants_role_check
    CHECK (role IN ('observer', 'submitter', 'board_human', 'operator', 'board_agent', 'agent_executor', 'terraform_state'));

CREATE TABLE runner_vm_terraform_states (
    runner_vm_id uuid PRIMARY KEY REFERENCES runner_vms(id) ON DELETE RESTRICT,
    state_ciphertext bytea,
    state_sha256 text CHECK (state_sha256 ~ '^[0-9a-f]{64}$'),
    lineage text,
    serial bigint CHECK (serial >= 0),
    lock_id uuid,
    lock_info jsonb CHECK (lock_info IS NULL OR jsonb_typeof(lock_info) = 'object'),
    locked_at timestamptz,
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    CHECK (
        (state_ciphertext IS NULL AND state_sha256 IS NULL AND lineage IS NULL AND serial IS NULL)
        OR
        (state_ciphertext IS NOT NULL AND state_sha256 IS NOT NULL AND lineage IS NOT NULL AND serial IS NOT NULL)
    ),
    CHECK (
        (lock_id IS NULL AND lock_info IS NULL AND locked_at IS NULL)
        OR
        (lock_id IS NOT NULL AND lock_info IS NOT NULL AND locked_at IS NOT NULL)
    )
);
CREATE INDEX runner_vm_terraform_states_locked_idx ON runner_vm_terraform_states (locked_at)
    WHERE lock_id IS NOT NULL;
