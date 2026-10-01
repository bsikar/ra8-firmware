-- The original 0006 reservation predates the reviewed Proxmox digest
-- contract. Never invent a digest for an existing reservation: a null value
-- prevents clone until independently reviewed recovery. No automated
-- reservation abandonment is available without verified Proxmox absence.
ALTER TABLE runner_vms ADD COLUMN template_digest text
    CHECK (template_digest ~ '^[0-9a-f]{40}$');
CREATE UNIQUE INDEX runner_vms_one_job_idx ON runner_vms (scale_set_id, job_id);

ALTER TABLE runner_vm_operations ADD COLUMN expected_config_digest text
    CHECK (expected_config_digest ~ '^[0-9a-f]{40}$');
ALTER TABLE runner_vm_operations ADD COLUMN safety_runner_id bigint
    CHECK (safety_runner_id > 0);
ALTER TABLE runner_vm_operations ADD COLUMN resolution_approval_id uuid;
