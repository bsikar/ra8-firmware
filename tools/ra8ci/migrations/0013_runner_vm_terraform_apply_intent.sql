-- Applying a saved Terraform plan is a separate external side effect. Persist
-- this one-way intent before invoking Terraform; a replay must reconcile.
ALTER TABLE runner_vm_operations
    ADD COLUMN terraform_apply_started_at timestamptz;

ALTER TABLE runner_vm_operations
    ADD CONSTRAINT runner_vm_terraform_apply_intent_provider_check
    CHECK (provider_kind = 'terraform' OR terraform_apply_started_at IS NULL);
