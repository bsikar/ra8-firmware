-- The scaler writes an exact reservation and operation intent before any
-- Proxmox mutation. An unresolved intent is never silently retried.
CREATE TABLE runner_vms (
    id uuid PRIMARY KEY,
    scale_set_id bigint NOT NULL CHECK (scale_set_id > 0),
    job_id text NOT NULL CHECK (length(job_id) BETWEEN 1 AND 128),
    runner_request_id bigint NOT NULL CHECK (runner_request_id > 0),
    workflow_run_id bigint NOT NULL CHECK (workflow_run_id > 0),
    workflow_attempt integer NOT NULL CHECK (workflow_attempt > 0),
    repository text NOT NULL CHECK (length(repository) BETWEEN 1 AND 512),
    workflow_ref text NOT NULL CHECK (length(workflow_ref) BETWEEN 1 AND 1024),
    commit_sha text NOT NULL CHECK (commit_sha ~ '^[0-9a-f]{40}$'),
    vmid integer NOT NULL CHECK (vmid >= 9000),
    node text NOT NULL CHECK (length(node) BETWEEN 1 AND 128),
    pool text NOT NULL CHECK (length(pool) BETWEEN 1 AND 128),
    storage text NOT NULL CHECK (length(storage) BETWEEN 1 AND 128),
    vm_name text NOT NULL CHECK (vm_name ~ '^ra8-lab-[a-z0-9][a-z0-9-]{0,54}$'),
    template_vmid integer NOT NULL CHECK (template_vmid >= 100 AND template_vmid <> vmid),
    template_name text NOT NULL CHECK (length(template_name) BETWEEN 1 AND 128),
    creation_operation_id uuid NOT NULL UNIQUE,
    state text NOT NULL CHECK (state IN
        ('reserved','cloning','stopped','starting','running','registered',
         'draining','stopping','deleting','released')),
    generation bigint NOT NULL DEFAULT 1 CHECK (generation > 0),
    unknown_outcome boolean NOT NULL DEFAULT false,
    current_operation_id uuid,
    external_runner_id bigint CHECK (external_runner_id > 0),
    external_runner_name text,
    created_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    updated_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    ended_at timestamptz,
    UNIQUE (scale_set_id, workflow_run_id, workflow_attempt, job_id),
    CHECK ((current_operation_id IS NULL) = (NOT unknown_outcome)),
    CHECK ((state='released') = (ended_at IS NOT NULL))
);
CREATE UNIQUE INDEX runner_vms_one_active_vmid_idx ON runner_vms (node, vmid)
    WHERE state <> 'released';
CREATE UNIQUE INDEX runner_vms_one_active_runner_idx ON runner_vms (scale_set_id, external_runner_id)
    WHERE external_runner_id IS NOT NULL AND state <> 'released';
CREATE INDEX runner_vms_unresolved_idx ON runner_vms (updated_at)
    WHERE unknown_outcome;

CREATE TABLE runner_vm_operations (
    id uuid PRIMARY KEY,
    runner_vm_id uuid NOT NULL REFERENCES runner_vms(id) ON DELETE RESTRICT,
    kind text NOT NULL CHECK (kind IN ('clone','start','stop','destroy')),
    from_state text NOT NULL,
    pending_state text NOT NULL,
    generation bigint NOT NULL CHECK (generation > 0),
    status text NOT NULL CHECK (status IN ('unresolved','succeeded','failed')),
    upid text CHECK (length(upid) BETWEEN 6 AND 512),
    safety_evidence_id uuid,
    safety_observed_at timestamptz,
    config_sha256 text CHECK (config_sha256 ~ '^[0-9a-f]{64}$'),
    approval_id uuid,
    runner_deregistered boolean NOT NULL DEFAULT false,
    resolution_evidence_id uuid,
    resolution_source text CHECK (resolution_source IN ('upid','clone_marker','operator')),
    resolution_observed_at timestamptz,
    intended_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    resolved_at timestamptz,
    CHECK ((status='unresolved') = (resolved_at IS NULL))
);
CREATE UNIQUE INDEX runner_vm_one_unresolved_operation_idx ON runner_vm_operations (runner_vm_id)
    WHERE status='unresolved';
CREATE INDEX runner_vm_operations_reservation_idx ON runner_vm_operations (runner_vm_id, intended_at DESC);
