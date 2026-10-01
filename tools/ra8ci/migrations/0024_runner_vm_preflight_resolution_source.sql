-- A Terraform preflight that fails before any apply is authorized resolves its
-- operation as failed with resolution_source 'terraform_preflight' and no
-- reconciliation digest (internal/store/runner_vms.go validateVMResolution,
-- internal/scaler/lifecycle.go). 0010 left two checks that refuse that row:
-- the source list never admitted 'terraform_preflight', and a resolved
-- Terraform operation always had to carry a reconciliation digest, which a
-- preflight that never applied has no state snapshot to produce. Every such
-- resolution was refused and the reservation stayed in an unknown outcome.
-- Admit exactly the row the store already validates: a failed preflight
-- resolution, with no digest, on an operation whose apply never started.
ALTER TABLE runner_vm_operations
    DROP CONSTRAINT runner_vm_operation_resolution_source_check,
    ADD CONSTRAINT runner_vm_operation_resolution_source_check CHECK (
        resolution_source IS NULL OR resolution_source IN
            ('upid', 'clone_marker', 'operator', 'terraform_state', 'terraform_preflight')
    ),
    DROP CONSTRAINT runner_vm_operation_reconciliation_evidence_check,
    ADD CONSTRAINT runner_vm_operation_reconciliation_evidence_check CHECK (
        (provider_kind = 'proxmox' AND reconciliation_sha256 IS NULL)
        OR
        (provider_kind = 'terraform' AND
            ((status = 'unresolved' AND reconciliation_sha256 IS NULL)
            OR (status IN ('succeeded', 'failed') AND reconciliation_sha256 IS NOT NULL)
            OR (status = 'failed' AND resolution_source = 'terraform_preflight'
                AND reconciliation_sha256 IS NULL AND terraform_apply_started_at IS NULL)))
    );
