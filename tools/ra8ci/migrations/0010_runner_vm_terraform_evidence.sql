ALTER TABLE runner_vm_operations
    ADD COLUMN provider_kind text NOT NULL DEFAULT 'proxmox'
        CHECK (provider_kind IN ('proxmox', 'terraform')),
    ADD COLUMN terraform_version text CHECK (terraform_version ~ '^[0-9]+[.][0-9]+[.][0-9]+$'),
    ADD COLUMN plan_sha256 text CHECK (plan_sha256 ~ '^[0-9a-f]{64}$'),
    ADD COLUMN module_sha256 text CHECK (module_sha256 ~ '^[0-9a-f]{64}$'),
    ADD COLUMN input_sha256 text CHECK (input_sha256 ~ '^[0-9a-f]{64}$'),
    ADD COLUMN provider_lock_sha256 text CHECK (provider_lock_sha256 ~ '^[0-9a-f]{64}$'),
    ADD COLUMN state_identity_sha256 text CHECK (state_identity_sha256 ~ '^[0-9a-f]{64}$');

ALTER TABLE runner_vm_operations
    ADD CONSTRAINT runner_vm_operation_provider_evidence_check CHECK (
        (provider_kind = 'proxmox' AND terraform_version IS NULL AND plan_sha256 IS NULL
            AND module_sha256 IS NULL AND input_sha256 IS NULL
            AND provider_lock_sha256 IS NULL AND state_identity_sha256 IS NULL)
        OR
        (provider_kind = 'terraform' AND terraform_version IS NOT NULL AND plan_sha256 IS NOT NULL
            AND module_sha256 IS NOT NULL AND input_sha256 IS NOT NULL
            AND provider_lock_sha256 IS NOT NULL AND state_identity_sha256 IS NOT NULL)
    );

CREATE INDEX runner_vm_terraform_plan_idx ON runner_vm_operations (runner_vm_id, plan_sha256)
    WHERE provider_kind = 'terraform';

ALTER TABLE runner_vm_operations
    ADD COLUMN reconciliation_sha256 text CHECK (reconciliation_sha256 ~ '^[0-9a-f]{64}$');

ALTER TABLE runner_vm_operations
    DROP CONSTRAINT runner_vm_operations_resolution_source_check,
    ADD CONSTRAINT runner_vm_operation_resolution_source_check CHECK (
        resolution_source IS NULL OR resolution_source IN ('upid', 'clone_marker', 'operator', 'terraform_state')
    ),
    ADD CONSTRAINT runner_vm_operation_reconciliation_evidence_check CHECK (
        (provider_kind = 'proxmox' AND reconciliation_sha256 IS NULL)
        OR
        (provider_kind = 'terraform' AND
            ((status = 'unresolved' AND reconciliation_sha256 IS NULL)
            OR (status IN ('succeeded', 'failed') AND reconciliation_sha256 IS NOT NULL)))
    );
