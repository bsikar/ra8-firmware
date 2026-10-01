-- An operator approval is the human escape hatch for an operation whose
-- outcome no machine source can settle. 0010 required every resolved
-- Terraform operation to carry a reconciliation digest, which only the
-- terraform_state source produces, so an operator could not resolve a
-- Terraform operation at all: the row was refused by this check and the
-- reservation stayed in an unknown outcome. Admit an operator resolution of
-- a Terraform operation with no digest and a named approval, keeping the
-- 0024 preflight arm and the digest requirement for every machine source.
ALTER TABLE runner_vm_operations
    DROP CONSTRAINT runner_vm_operation_reconciliation_evidence_check,
    ADD CONSTRAINT runner_vm_operation_reconciliation_evidence_check CHECK (
        (provider_kind = 'proxmox' AND reconciliation_sha256 IS NULL)
        OR
        (provider_kind = 'terraform' AND
            ((status = 'unresolved' AND reconciliation_sha256 IS NULL)
            OR (status IN ('succeeded', 'failed') AND reconciliation_sha256 IS NOT NULL)
            OR (status = 'failed' AND resolution_source = 'terraform_preflight'
                AND reconciliation_sha256 IS NULL AND terraform_apply_started_at IS NULL)
            OR (status IN ('succeeded', 'failed') AND resolution_source = 'operator'
                AND resolution_approval_id IS NOT NULL AND reconciliation_sha256 IS NULL)))
    );
