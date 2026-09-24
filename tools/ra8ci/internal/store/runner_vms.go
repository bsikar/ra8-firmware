package store

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"errors"
	"fmt"
	"regexp"
	"time"

	"github.com/jackc/pgx/v5"
)

var runnerVMName = regexp.MustCompile(`^ra8-lab-[a-z0-9][a-z0-9-]{0,54}$`)
var runnerVMUPID = regexp.MustCompile(`^UPID:[A-Za-z0-9_.-]+:[A-Za-z0-9:@!_.-]+$`)
var runnerVMTerraformVersion = regexp.MustCompile(`^[0-9]+\.[0-9]+\.[0-9]+$`)
var runnerVMHexSHA256 = regexp.MustCompile(`^[0-9a-f]{64}$`)

// RunnerVMInput identifies one GitHub job attempt and an exact, pre-approved
// Proxmox target/template. It contains no runner token or API credential.
type RunnerVMInput struct {
	ScaleSetID      int64
	JobID           string
	RunnerRequestID int64
	WorkflowRunID   int64
	WorkflowAttempt int
	Repository      string
	WorkflowRef     string
	CommitSHA       string
	VMID            int
	Node            string
	Pool            string
	Storage         string
	Name            string
	TemplateVMID    int
	TemplateName    string
	TemplateDigest  string
}

type RunnerVM struct {
	ID string
	RunnerVMInput
	CreationOperationID string
	State               string
	Generation          int64
	UnknownOutcome      bool
	CleanupRequested    bool
	CurrentOperationID  string
	ExternalRunnerID    int64
	ExternalRunnerName  string
	UnclaimedDeadline   time.Time
	ClaimedAt           *time.Time
	CreatedAt           time.Time
	UpdatedAt           time.Time
	EndedAt             *time.Time
}

// RunnerVMOperation is a committed intent. PriorRequestIssued is false only
// for the process that created that intent; a replay must reconcile before
// asking Proxmox to mutate anything.
type RunnerVMOperation struct {
	ID                      string
	RunnerVMID              string
	Kind                    string
	FromState               string
	PendingState            string
	Generation              int64
	Status                  string
	UPID                    string
	ProviderKind            string
	TerraformVersion        string
	PlanSHA256              string
	ModuleSHA256            string
	InputSHA256             string
	ProviderLockSHA256      string
	ReconciliationSHA256    string
	StateIdentitySHA256     string
	TerraformApplyStartedAt *time.Time
	PriorRequestIssued      bool
}

type RunnerVMSafetyEvidence struct {
	EvidenceID           string
	ObservedAt           time.Time
	Drained              bool
	NoActiveJob          bool
	RunnerDeregistered   bool
	ExpectedConfigDigest string
	ApprovalID           string
	ExternalRunnerID     int64
}

type RunnerVMTerraformPlanEvidence struct {
	TerraformVersion    string
	PlanSHA256          string
	ModuleSHA256        string
	InputSHA256         string
	ProviderLockSHA256  string
	StateIdentitySHA256 string
	PreparedAt          time.Time
}

// RunnerVMBootstrapEvidence contains only validated, non-secret guest readiness
// and JIT identity metadata. The encoded JIT configuration is never accepted.
type RunnerVMBootstrapEvidence struct {
	ReservationID      string
	VMID               int
	CommitSHA          string
	GuestOS            string
	GuestArchitecture  string
	ServiceAccount     string
	RunnerBinarySHA256 string
	AgentBinarySHA256  string
	ReadinessSHA256    string
	JITConfigSHA256    string
	JITConfigExpiresAt time.Time
	EvidenceID         string
	StartedAt          time.Time
	CompletedAt        time.Time
	PreparedAt         time.Time
}

// RecordRunnerVMBootstrapEvidence durably audits validated readiness and JIT
// metadata against the exact, still-running reservation. It intentionally
// stores only digests and never receives the JIT credential bytes.
func (s *Store) RecordRunnerVMBootstrapEvidence(ctx context.Context, actor string, evidence RunnerVMBootstrapEvidence) error {
	now := time.Now()
	if s == nil || s.pool == nil || ctx == nil || actor == "" || len(actor) > 256 ||
		!ValidID(evidence.ReservationID) || evidence.VMID < 9000 ||
		!commitSHA.MatchString(evidence.CommitSHA) ||
		(evidence.GuestOS != "linux" && evidence.GuestOS != "windows") ||
		(evidence.GuestArchitecture != "amd64" && evidence.GuestArchitecture != "arm64") ||
		evidence.ServiceAccount != "ra8ci" ||
		!runnerVMHexSHA256.MatchString(evidence.RunnerBinarySHA256) ||
		!runnerVMHexSHA256.MatchString(evidence.AgentBinarySHA256) ||
		!runnerVMHexSHA256.MatchString(evidence.ReadinessSHA256) ||
		!runnerVMHexSHA256.MatchString(evidence.JITConfigSHA256) ||
		!ValidID(evidence.EvidenceID) || evidence.StartedAt.IsZero() || evidence.CompletedAt.IsZero() || evidence.PreparedAt.IsZero() ||
		evidence.StartedAt.After(evidence.CompletedAt) || evidence.CompletedAt.Sub(evidence.StartedAt) > 5*time.Minute ||
		evidence.CompletedAt.After(now.Add(time.Second)) || now.Sub(evidence.CompletedAt) > 5*time.Minute ||
		evidence.PreparedAt.After(now.Add(time.Second)) || now.Sub(evidence.PreparedAt) > 5*time.Minute ||
		!evidence.JITConfigExpiresAt.After(now) ||
		evidence.JITConfigExpiresAt.After(now.Add(time.Hour)) {
		return ErrInvalid
	}
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.ReadCommitted})
	if err != nil {
		return fmt.Errorf("%w: begin runner bootstrap audit: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var vmID int
	var commitSHAValue, state string
	var cleanupRequested, unknownOutcome bool
	err = tx.QueryRow(ctx, `SELECT vmid,commit_sha,state,cleanup_requested,unknown_outcome
		FROM runner_vms WHERE id=$1 FOR UPDATE`, evidence.ReservationID).Scan(
		&vmID, &commitSHAValue, &state, &cleanupRequested, &unknownOutcome)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("%w: read runner bootstrap reservation: %v", ErrUnavailable, err)
	}
	if vmID != evidence.VMID || commitSHAValue != evidence.CommitSHA || state != "running" || cleanupRequested || unknownOutcome {
		return fmt.Errorf("%w: bootstrap evidence does not match a running reservation", ErrConflict)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm.bootstrap.ready", "runner_vm", evidence.ReservationID,
		"ok", "running", "ready", "", map[string]any{
			"evidence_id": evidence.EvidenceID, "vmid": evidence.VMID,
			"commit_sha": evidence.CommitSHA, "guest_os": evidence.GuestOS,
			"guest_architecture": evidence.GuestArchitecture, "service_account": evidence.ServiceAccount,
			"runner_binary_sha256":   evidence.RunnerBinarySHA256,
			"agent_binary_sha256":    evidence.AgentBinarySHA256,
			"readiness_sha256":       evidence.ReadinessSHA256,
			"jit_config_sha256":      evidence.JITConfigSHA256,
			"jit_config_expires_at":  evidence.JITConfigExpiresAt.UTC(),
			"bootstrap_started_at":   evidence.StartedAt.UTC(),
			"bootstrap_completed_at": evidence.CompletedAt.UTC(),
			"bootstrap_duration_ms":  evidence.CompletedAt.Sub(evidence.StartedAt).Milliseconds(),
			"prepared_at":            evidence.PreparedAt.UTC(),
		}); err != nil {
		return fmt.Errorf("%w: append runner bootstrap evidence: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit runner bootstrap evidence: %v", ErrUnavailable, err)
	}
	return nil
}

// RunnerVMResolution must be produced after independent provider observation
// (Proxmox task/config evidence or Terraform state reconciliation). A boolean
// claim without source-specific evidence cannot close an unknown operation.
type RunnerVMResolution struct {
	Outcome              string // succeeded or failed (verified no effect)
	EvidenceID           string
	Source               string // upid, clone_marker, operator, or terraform_state
	ObservedAt           time.Time
	PostStateVerified    bool
	OperatorApprovalID   string
	PlanSHA256           string
	StateIdentitySHA256  string
	ReconciliationSHA256 string
	TerraformStateHasVM  bool
	TerraformVMAbsent    bool
	TerraformVMStatus    string
}

func nullablePositive(value int64) any {
	if value <= 0 {
		return nil
	}
	return value
}

const runnerVMColumns = `id::text,scale_set_id,job_id,runner_request_id,workflow_run_id,
	workflow_attempt,repository,workflow_ref,commit_sha,vmid,node,pool,storage,vm_name,
	template_vmid,template_name,template_digest,creation_operation_id::text,state,generation,
	unknown_outcome,cleanup_requested,current_operation_id::text,external_runner_id,external_runner_name,
	unclaimed_deadline,claimed_at,created_at,updated_at,ended_at`

func scanRunnerVM(row pgx.Row) (RunnerVM, error) {
	var vm RunnerVM
	var operation, runnerName, templateDigest sql.NullString
	var runnerID sql.NullInt64
	var ended, claimed sql.NullTime
	err := row.Scan(&vm.ID, &vm.ScaleSetID, &vm.JobID, &vm.RunnerRequestID,
		&vm.WorkflowRunID, &vm.WorkflowAttempt, &vm.Repository, &vm.WorkflowRef,
		&vm.CommitSHA, &vm.VMID, &vm.Node, &vm.Pool, &vm.Storage, &vm.Name,
		&vm.TemplateVMID, &vm.TemplateName, &templateDigest, &vm.CreationOperationID, &vm.State,
		&vm.Generation, &vm.UnknownOutcome, &vm.CleanupRequested, &operation, &runnerID, &runnerName,
		&vm.UnclaimedDeadline, &claimed, &vm.CreatedAt, &vm.UpdatedAt, &ended)
	vm.TemplateDigest = templateDigest.String
	vm.CurrentOperationID, vm.ExternalRunnerID, vm.ExternalRunnerName = operation.String, runnerID.Int64, runnerName.String
	if claimed.Valid {
		vm.ClaimedAt = &claimed.Time
	}
	if ended.Valid {
		vm.EndedAt = &ended.Time
	}
	return vm, err
}

func validRunnerVMInput(in RunnerVMInput) bool {
	return in.ScaleSetID > 0 && len(in.JobID) > 0 && len(in.JobID) <= 128 &&
		in.RunnerRequestID > 0 && in.WorkflowRunID > 0 && in.WorkflowAttempt > 0 &&
		len(in.Repository) > 0 && len(in.Repository) <= 512 &&
		len(in.WorkflowRef) > 0 && len(in.WorkflowRef) <= 1024 &&
		commitSHA.MatchString(in.CommitSHA) && in.VMID >= 9000 &&
		in.TemplateVMID >= 100 && in.TemplateVMID != in.VMID &&
		len(in.Node) > 0 && len(in.Node) <= 128 && len(in.Pool) > 0 && len(in.Pool) <= 128 &&
		len(in.Storage) > 0 && len(in.Storage) <= 128 && runnerVMName.MatchString(in.Name) &&
		runnerVMName.MatchString(in.TemplateName) && commitSHA.MatchString(in.TemplateDigest)
}

// ReserveRunnerVM inserts a permanent job-attempt reservation. Repeating the
// exact key and identity returns the original markers; changed identity or an
// active VMID collision fails without changing any existing reservation.
//
// The unclaimed deadline is part of the reservation, not a later decoration:
// a single-use credential is minted against this row, and the deadline is
// what says when an unused one stops being live. A replay returns the
// original row with its original deadline, so a repeated delivery can never
// extend the life of a reservation nobody ever claimed.
func (s *Store) ReserveRunnerVM(ctx context.Context, actor string, in RunnerVMInput, unclaimedDeadline time.Time) (RunnerVM, bool, error) {
	if s == nil || s.pool == nil || actor == "" || len(actor) > 256 || !validRunnerVMInput(in) {
		return RunnerVM{}, false, ErrInvalid
	}
	if err := ValidUnclaimedDeadline(time.Now().UTC(), unclaimedDeadline); err != nil {
		return RunnerVM{}, false, err
	}
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.ReadCommitted})
	if err != nil {
		return RunnerVM{}, false, fmt.Errorf("%w: begin VM reservation: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	key := fmt.Sprintf("runner-vm:%d:%s", in.ScaleSetID, in.JobID)
	if _, err := tx.Exec(ctx, "SELECT pg_advisory_xact_lock(hashtextextended($1, 9))", key); err != nil {
		return RunnerVM{}, false, fmt.Errorf("%w: VM reservation lock: %v", ErrUnavailable, err)
	}
	existing, err := scanRunnerVM(tx.QueryRow(ctx, `SELECT `+runnerVMColumns+` FROM runner_vms
		WHERE scale_set_id=$1 AND job_id=$2`, in.ScaleSetID, in.JobID))
	if err == nil {
		if existing.RunnerVMInput != in {
			return RunnerVM{}, false, fmt.Errorf("%w: job attempt reservation changed", ErrConflict)
		}
		return existing, false, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		return RunnerVM{}, false, fmt.Errorf("%w: reservation lookup: %v", ErrUnavailable, err)
	}
	id, err := NewID()
	if err != nil {
		return RunnerVM{}, false, fmt.Errorf("%w: reservation ID: %v", ErrUnavailable, err)
	}
	creationID, err := NewID()
	if err != nil {
		return RunnerVM{}, false, fmt.Errorf("%w: creation operation ID: %v", ErrUnavailable, err)
	}
	_, err = tx.Exec(ctx, `INSERT INTO runner_vms
		(id,scale_set_id,job_id,runner_request_id,workflow_run_id,workflow_attempt,
		repository,workflow_ref,commit_sha,vmid,node,pool,storage,vm_name,
		template_vmid,template_name,template_digest,creation_operation_id,state,unclaimed_deadline)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,'reserved',$19)`,
		id, in.ScaleSetID, in.JobID, in.RunnerRequestID, in.WorkflowRunID,
		in.WorkflowAttempt, in.Repository, in.WorkflowRef, in.CommitSHA, in.VMID,
		in.Node, in.Pool, in.Storage, in.Name, in.TemplateVMID, in.TemplateName, in.TemplateDigest,
		creationID, unclaimedDeadline.UTC())
	if err != nil {
		return RunnerVM{}, false, fmt.Errorf("%w: reserve exact VM: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm.reserved", "runner_vm", id,
		"ok", "", "reserved", "", map[string]any{"scale_set_id": in.ScaleSetID,
			"job_id": in.JobID, "workflow_attempt": in.WorkflowAttempt,
			"node": in.Node, "vmid": in.VMID, "creation_operation_id": creationID,
			"unclaimed_deadline": unclaimedDeadline.UTC().Format(time.RFC3339Nano)}); err != nil {
		return RunnerVM{}, false, fmt.Errorf("%w: audit VM reservation: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return RunnerVM{}, false, fmt.Errorf("%w: commit VM reservation: %v", ErrUnavailable, err)
	}
	vm, err := s.GetRunnerVM(ctx, id)
	return vm, true, err
}

func (s *Store) GetRunnerVM(ctx context.Context, reservationID string) (RunnerVM, error) {
	if s == nil || s.pool == nil || !ValidID(reservationID) {
		return RunnerVM{}, ErrInvalid
	}
	vm, err := scanRunnerVM(s.pool.QueryRow(ctx, `SELECT `+runnerVMColumns+` FROM runner_vms WHERE id=$1`, reservationID))
	if errors.Is(err, pgx.ErrNoRows) {
		return RunnerVM{}, ErrNotFound
	}
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: read runner VM: %v", ErrUnavailable, err)
	}
	return vm, nil
}

// GetRunnerVMByJob must be called before choosing a VMID on inbox replay.
// A prior reservation's exact identity wins over a newly available VMID.
func (s *Store) GetRunnerVMByJob(ctx context.Context, scaleSetID int64, jobID string) (RunnerVM, error) {
	if s == nil || s.pool == nil || scaleSetID <= 0 || jobID == "" || len(jobID) > 128 {
		return RunnerVM{}, ErrInvalid
	}
	vm, err := scanRunnerVM(s.pool.QueryRow(ctx, `SELECT `+runnerVMColumns+`
		FROM runner_vms WHERE scale_set_id=$1 AND job_id=$2`, scaleSetID, jobID))
	if errors.Is(err, pgx.ErrNoRows) {
		return RunnerVM{}, ErrNotFound
	}
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: read job VM reservation: %v", ErrUnavailable, err)
	}
	return vm, nil
}

func runnerVMOperationStates(kind, from string) (string, string, bool) {
	switch kind {
	case "clone":
		return "reserved", "cloning", from == "reserved"
	case "start":
		return "stopped", "starting", from == "stopped"
	case "stop":
		return "draining", "stopping", from == "draining"
	case "destroy":
		return "stopped", "deleting", from == "stopped"
	default:
		return "", "", false
	}
}

func validateVMSafety(now time.Time, kind string, proof RunnerVMSafetyEvidence) error {
	if kind != "stop" && kind != "destroy" {
		return nil
	}
	if !ValidID(proof.EvidenceID) || proof.ObservedAt.IsZero() ||
		proof.ObservedAt.After(now.Add(time.Second)) || now.Sub(proof.ObservedAt) > 10*time.Second ||
		!proof.Drained || !proof.NoActiveJob {
		return ErrDenied
	}
	if kind == "destroy" && (!proof.RunnerDeregistered || !ValidID(proof.ApprovalID) ||
		!commitSHA.MatchString(proof.ExpectedConfigDigest)) {
		return ErrDenied
	}
	return nil
}

// BeginRunnerVMOperation commits the external mutation intent first. If an
// unresolved operation already exists it returns the same ID with
// PriorRequestIssued=true; the caller must reconcile, never call Proxmox again.
func (s *Store) BeginRunnerVMOperation(ctx context.Context, actor, reservationID string, expectedGeneration int64, kind string, proof RunnerVMSafetyEvidence) (RunnerVMOperation, error) {
	if s == nil || s.pool == nil || actor == "" || len(actor) > 256 || !ValidID(reservationID) || expectedGeneration < 1 {
		return RunnerVMOperation{}, ErrInvalid
	}
	tx, err := s.pool.BeginTx(ctx, pgx.TxOptions{IsoLevel: pgx.ReadCommitted})
	if err != nil {
		return RunnerVMOperation{}, fmt.Errorf("%w: begin VM intent: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	vm, err := scanRunnerVM(tx.QueryRow(ctx, `SELECT `+runnerVMColumns+` FROM runner_vms WHERE id=$1 FOR UPDATE`, reservationID))
	if errors.Is(err, pgx.ErrNoRows) {
		return RunnerVMOperation{}, ErrNotFound
	}
	if err != nil {
		return RunnerVMOperation{}, fmt.Errorf("%w: lock VM reservation: %v", ErrUnavailable, err)
	}
	if vm.UnknownOutcome {
		operation, err := getRunnerVMOperation(ctx, tx, vm.CurrentOperationID)
		if err != nil || operation.Kind != kind || operation.Status != "unresolved" {
			return RunnerVMOperation{}, fmt.Errorf("%w: different unresolved VM operation", ErrConflict)
		}
		operation.PriorRequestIssued = true
		return operation, nil
	}
	if vm.Generation != expectedGeneration {
		return RunnerVMOperation{}, fmt.Errorf("%w: stale VM generation", ErrConflict)
	}
	if (kind == "clone" || kind == "start") && vm.CleanupRequested {
		return RunnerVMOperation{}, fmt.Errorf("%w: completed runner cannot be cloned or restarted", ErrConflict)
	}
	if kind == "destroy" && !vm.CleanupRequested {
		return RunnerVMOperation{}, fmt.Errorf("%w: cleanup was not requested", ErrDenied)
	}
	if kind == "clone" && !commitSHA.MatchString(vm.TemplateDigest) {
		return RunnerVMOperation{}, fmt.Errorf("%w: reservation lacks reviewed template digest", ErrDenied)
	}
	_, pending, allowed := runnerVMOperationStates(kind, vm.State)
	if !allowed {
		return RunnerVMOperation{}, fmt.Errorf("%w: cannot %s VM in %s", ErrConflict, kind, vm.State)
	}
	var now time.Time
	if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&now); err != nil {
		return RunnerVMOperation{}, fmt.Errorf("%w: VM safety clock: %v", ErrUnavailable, err)
	}
	if err := validateVMSafety(now, kind, proof); err != nil {
		return RunnerVMOperation{}, err
	}
	if kind == "destroy" && proof.ExternalRunnerID != vm.ExternalRunnerID {
		return RunnerVMOperation{}, ErrDenied
	}
	opID := vm.CreationOperationID
	if kind != "clone" {
		opID, err = NewID()
		if err != nil {
			return RunnerVMOperation{}, fmt.Errorf("%w: operation ID: %v", ErrUnavailable, err)
		}
	}
	nextGeneration := vm.Generation + 1
	_, err = tx.Exec(ctx, `INSERT INTO runner_vm_operations
		(id,runner_vm_id,kind,from_state,pending_state,generation,status,
		safety_evidence_id,safety_observed_at,expected_config_digest,approval_id,
		runner_deregistered,safety_runner_id)
		VALUES ($1,$2,$3,$4,$5,$6,'unresolved',$7,$8,$9,$10,$11,$12)`,
		opID, vm.ID, kind, vm.State, pending, nextGeneration,
		nullable(proof.EvidenceID), nullTime(proof.ObservedAt), nullable(proof.ExpectedConfigDigest),
		nullable(proof.ApprovalID), proof.RunnerDeregistered, nullablePositive(proof.ExternalRunnerID))
	if err != nil {
		return RunnerVMOperation{}, fmt.Errorf("%w: VM operation was already issued: %v", ErrConflict, err)
	}
	tag, err := tx.Exec(ctx, `UPDATE runner_vms SET state=$3,generation=$4,
		unknown_outcome=true,current_operation_id=$5,updated_at=clock_timestamp()
		WHERE id=$1 AND generation=$2 AND unknown_outcome=false`,
		vm.ID, vm.Generation, pending, nextGeneration, opID)
	if err != nil || tag.RowsAffected() != 1 {
		return RunnerVMOperation{}, fmt.Errorf("%w: VM intent CAS: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm."+kind+".intended", "runner_vm", vm.ID,
		"unresolved", vm.State, pending, "", map[string]any{"operation_id": opID,
			"generation": nextGeneration, "evidence_id": proof.EvidenceID}); err != nil {
		return RunnerVMOperation{}, fmt.Errorf("%w: audit VM intent: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return RunnerVMOperation{}, fmt.Errorf("%w: commit VM intent: %v", ErrUnavailable, err)
	}
	return RunnerVMOperation{ID: opID, RunnerVMID: vm.ID, Kind: kind,
		FromState: vm.State, PendingState: pending, Generation: nextGeneration,
		Status: "unresolved"}, nil
}

func getRunnerVMOperation(ctx context.Context, tx pgx.Tx, operationID string) (RunnerVMOperation, error) {
	var op RunnerVMOperation
	var upid, providerKind, terraformVersion, planSHA, moduleSHA, inputSHA, providerLockSHA, stateIdentitySHA, reconciliationSHA sql.NullString
	var terraformApplyStartedAt sql.NullTime
	err := tx.QueryRow(ctx, `SELECT id::text,runner_vm_id::text,kind,from_state,pending_state,
		generation,status,upid,provider_kind,terraform_version,plan_sha256,module_sha256,input_sha256,
		provider_lock_sha256,state_identity_sha256,reconciliation_sha256,terraform_apply_started_at FROM runner_vm_operations WHERE id=$1`, operationID).
		Scan(&op.ID, &op.RunnerVMID, &op.Kind, &op.FromState, &op.PendingState,
			&op.Generation, &op.Status, &upid, &providerKind, &terraformVersion, &planSHA,
			&moduleSHA, &inputSHA, &providerLockSHA, &stateIdentitySHA, &reconciliationSHA, &terraformApplyStartedAt)
	op.UPID = upid.String
	if errors.Is(err, pgx.ErrNoRows) {
		return RunnerVMOperation{}, ErrNotFound
	}
	op.ProviderKind, op.TerraformVersion = providerKind.String, terraformVersion.String
	op.PlanSHA256, op.ModuleSHA256, op.InputSHA256, op.ProviderLockSHA256, op.StateIdentitySHA256 = planSHA.String, moduleSHA.String, inputSHA.String, providerLockSHA.String, stateIdentitySHA.String
	if err != nil {
		return RunnerVMOperation{}, fmt.Errorf("%w: VM operation lookup: %v", ErrUnavailable, err)
	}
	op.ReconciliationSHA256 = reconciliationSHA.String
	if terraformApplyStartedAt.Valid {
		startedAt := terraformApplyStartedAt.Time
		op.TerraformApplyStartedAt = &startedAt
	}
	return op, nil
}

func (s *Store) GetRunnerVMOperation(ctx context.Context, operationID string) (RunnerVMOperation, error) {
	if s == nil || s.pool == nil || !ValidID(operationID) {
		return RunnerVMOperation{}, ErrInvalid
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return RunnerVMOperation{}, fmt.Errorf("%w: begin VM operation lookup: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	return getRunnerVMOperation(ctx, tx, operationID)
}

// RecordRunnerVMTerraformPlan binds an immutable saved plan and its execution inputs
// to the one unresolved VM operation before Terraform apply is allowed.
func (s *Store) RecordRunnerVMTerraformPlan(ctx context.Context, actor, reservationID string, expectedGeneration int64, operationID string, evidence RunnerVMTerraformPlanEvidence) error {
	if s == nil || s.pool == nil || actor == "" || len(actor) > 256 ||
		!ValidID(reservationID) || !ValidID(operationID) || expectedGeneration < 1 ||
		!runnerVMTerraformVersion.MatchString(evidence.TerraformVersion) ||
		!runnerVMHexSHA256.MatchString(evidence.PlanSHA256) ||
		!runnerVMHexSHA256.MatchString(evidence.ModuleSHA256) ||
		!runnerVMHexSHA256.MatchString(evidence.InputSHA256) ||
		!runnerVMHexSHA256.MatchString(evidence.ProviderLockSHA256) ||
		!runnerVMHexSHA256.MatchString(evidence.StateIdentitySHA256) {
		return ErrInvalid
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin Terraform plan evidence: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var generation int64
	var currentID string
	var unknown bool
	err = tx.QueryRow(ctx, `SELECT generation,current_operation_id::text,unknown_outcome
		FROM runner_vms WHERE id=$1 FOR UPDATE`, reservationID).Scan(&generation, &currentID, &unknown)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("%w: lock VM plan evidence: %v", ErrUnavailable, err)
	}
	if generation != expectedGeneration || !unknown || currentID != operationID {
		return ErrConflict
	}
	op, err := getRunnerVMOperation(ctx, tx, operationID)
	if err != nil {
		return err
	}
	if op.RunnerVMID != reservationID || op.Status != "unresolved" || op.Generation != generation {
		return ErrConflict
	}
	if op.ProviderKind == "terraform" {
		if op.TerraformVersion == evidence.TerraformVersion && op.PlanSHA256 == evidence.PlanSHA256 &&
			op.ModuleSHA256 == evidence.ModuleSHA256 && op.InputSHA256 == evidence.InputSHA256 &&
			op.ProviderLockSHA256 == evidence.ProviderLockSHA256 && op.StateIdentitySHA256 == evidence.StateIdentitySHA256 {
			return nil
		}
		return fmt.Errorf("%w: Terraform plan evidence changed for operation", ErrConflict)
	}
	if op.ProviderKind != "proxmox" || op.UPID != "" {
		return ErrConflict
	}
	var databaseNow time.Time
	if err := tx.QueryRow(ctx, `SELECT clock_timestamp()`).Scan(&databaseNow); err != nil {
		return fmt.Errorf("%w: Terraform plan clock: %v", ErrUnavailable, err)
	}
	if evidence.PreparedAt.IsZero() || evidence.PreparedAt.After(databaseNow.Add(time.Second)) || databaseNow.Sub(evidence.PreparedAt) > 30*time.Second {
		return ErrDenied
	}
	_, err = tx.Exec(ctx, `UPDATE runner_vm_operations SET provider_kind='terraform',
		terraform_version=$2,plan_sha256=$3,module_sha256=$4,input_sha256=$5,
		provider_lock_sha256=$6,state_identity_sha256=$7
		WHERE id=$1 AND status='unresolved' AND provider_kind='proxmox' AND upid IS NULL`,
		operationID, evidence.TerraformVersion, evidence.PlanSHA256, evidence.ModuleSHA256,
		evidence.InputSHA256, evidence.ProviderLockSHA256, evidence.StateIdentitySHA256)
	if err != nil {
		return fmt.Errorf("%w: record Terraform plan evidence: %v", ErrUnavailable, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm.terraform.plan_prepared", "runner_vm",
		reservationID, "unresolved", "", "", "", map[string]any{
			"operation_id": operationID, "terraform_version": evidence.TerraformVersion,
			"plan_sha256": evidence.PlanSHA256, "module_sha256": evidence.ModuleSHA256,
			"input_sha256": evidence.InputSHA256, "provider_lock_sha256": evidence.ProviderLockSHA256,
			"state_identity_sha256": evidence.StateIdentitySHA256}); err != nil {
		return fmt.Errorf("%w: audit Terraform plan evidence: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit Terraform plan evidence: %v", ErrUnavailable, err)
	}
	return nil
}

// BeginRunnerVMTerraformApply commits a one-way apply intent for the exact
// immutable plan. The caller may invoke Terraform only when this returns true.
// A false result means an earlier process may already have invoked apply and
// the only safe next action is provider/state reconciliation.
func (s *Store) BeginRunnerVMTerraformApply(ctx context.Context, actor, reservationID string, expectedGeneration int64, operationID, planSHA256 string) (bool, error) {
	if s == nil || s.pool == nil || actor == "" || len(actor) > 256 || !ValidID(reservationID) ||
		!ValidID(operationID) || expectedGeneration < 1 || !runnerVMHexSHA256.MatchString(planSHA256) {
		return false, ErrInvalid
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return false, fmt.Errorf("%w: begin Terraform apply intent: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var generation int64
	var currentID string
	var unknown bool
	if err := tx.QueryRow(ctx, `SELECT generation,current_operation_id::text,unknown_outcome
		FROM runner_vms WHERE id=$1 FOR UPDATE`, reservationID).Scan(&generation, &currentID, &unknown); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return false, ErrNotFound
		}
		return false, fmt.Errorf("%w: lock Terraform apply intent: %v", ErrUnavailable, err)
	}
	if generation != expectedGeneration || currentID != operationID || !unknown {
		return false, ErrConflict
	}
	op, err := getRunnerVMOperation(ctx, tx, operationID)
	if err != nil {
		return false, err
	}
	if op.RunnerVMID != reservationID || op.Generation != generation || op.Status != "unresolved" ||
		op.ProviderKind != "terraform" || op.PlanSHA256 != planSHA256 || op.PlanSHA256 == "" {
		return false, ErrConflict
	}
	if op.TerraformApplyStartedAt != nil {
		if err := tx.Commit(ctx); err != nil {
			return false, fmt.Errorf("%w: commit Terraform apply replay: %v", ErrUnavailable, err)
		}
		return false, nil
	}
	var databaseNow time.Time
	if err := tx.QueryRow(ctx, `SELECT clock_timestamp()`).Scan(&databaseNow); err != nil {
		return false, fmt.Errorf("%w: Terraform apply clock: %v", ErrUnavailable, err)
	}
	tag, err := tx.Exec(ctx, `UPDATE runner_vm_operations SET terraform_apply_started_at=$3
		WHERE id=$1 AND generation=$2 AND status='unresolved' AND provider_kind='terraform'
		AND plan_sha256=$4 AND terraform_apply_started_at IS NULL`,
		operationID, expectedGeneration, databaseNow, planSHA256)
	if err != nil || tag.RowsAffected() != 1 {
		return false, fmt.Errorf("%w: commit Terraform apply intent: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm.terraform.apply_intended", "runner_vm",
		reservationID, "unresolved", "planned", "apply_intended", "", map[string]any{
			"operation_id": operationID, "plan_sha256": planSHA256}); err != nil {
		return false, fmt.Errorf("%w: audit Terraform apply intent: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return false, fmt.Errorf("%w: commit Terraform apply intent: %v", ErrUnavailable, err)
	}
	return true, nil
}

// RecordRunnerVMUPID binds the Proxmox task ID to the one unresolved intent.
// A lost UPID leaves the operation unknown rather than authorizing a retry.
func (s *Store) RecordRunnerVMUPID(ctx context.Context, actor, reservationID string, expectedGeneration int64, operationID, upid string) error {
	if s == nil || s.pool == nil || actor == "" || len(actor) > 256 || !ValidID(reservationID) ||
		!ValidID(operationID) || expectedGeneration < 1 || len(upid) > 512 || !runnerVMUPID.MatchString(upid) {
		return ErrInvalid
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin UPID record: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var generation int64
	var currentID string
	var unknown bool
	err = tx.QueryRow(ctx, `SELECT generation,current_operation_id::text,unknown_outcome
		FROM runner_vms WHERE id=$1 FOR UPDATE`, reservationID).
		Scan(&generation, &currentID, &unknown)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("%w: lock VM for UPID: %v", ErrUnavailable, err)
	}
	if generation != expectedGeneration || !unknown || currentID != operationID {
		return ErrConflict
	}
	var previous sql.NullString
	var providerKind string
	err = tx.QueryRow(ctx, `SELECT upid,provider_kind FROM runner_vm_operations
		WHERE id=$1 AND runner_vm_id=$2 AND status='unresolved' FOR UPDATE`, operationID, reservationID).Scan(&previous, &providerKind)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrConflict
	}
	if err != nil {
		return fmt.Errorf("%w: read current UPID: %v", ErrUnavailable, err)
	}
	if providerKind != "proxmox" {
		return fmt.Errorf("%w: UPID is only valid for Proxmox operations", ErrConflict)
	}
	if previous.Valid {
		if previous.String != upid {
			return fmt.Errorf("%w: UPID changed for same operation", ErrConflict)
		}
		return nil
	}
	_, err = tx.Exec(ctx, `UPDATE runner_vm_operations SET upid=$2 WHERE id=$1 AND upid IS NULL`, operationID, upid)
	if err != nil {
		return fmt.Errorf("%w: record UPID: %v", ErrUnavailable, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm.operation.upid_recorded", "runner_vm", reservationID,
		"unresolved", "", "", "", map[string]any{"operation_id": operationID, "upid": upid}); err != nil {
		return fmt.Errorf("%w: audit UPID: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("%w: commit UPID: %v", ErrUnavailable, err)
	}
	return nil
}

func validateVMResolution(now time.Time, op RunnerVMOperation, proof RunnerVMResolution) error {
	if proof.Outcome != "succeeded" && proof.Outcome != "failed" ||
		!ValidID(proof.EvidenceID) || !proof.PostStateVerified ||
		proof.ObservedAt.IsZero() || proof.ObservedAt.After(now.Add(time.Second)) ||
		now.Sub(proof.ObservedAt) > 30*time.Second {
		return ErrDenied
	}
	switch proof.Source {
	case "upid":
		if op.ProviderKind != "proxmox" || op.UPID == "" {
			return ErrDenied
		}
	case "clone_marker":
		if op.ProviderKind != "proxmox" || op.Kind != "clone" {
			return ErrDenied
		}
	case "operator":
		if !ValidID(proof.OperatorApprovalID) {
			return ErrDenied
		}
	case "terraform_state":
		if op.ProviderKind != "terraform" || op.TerraformApplyStartedAt == nil || proof.PlanSHA256 != op.PlanSHA256 ||
			proof.StateIdentitySHA256 != op.StateIdentitySHA256 ||
			!runnerVMHexSHA256.MatchString(proof.ReconciliationSHA256) {
			return ErrDenied
		}
		if !validTerraformObservedState(op.Kind, proof.Outcome, proof.TerraformStateHasVM,
			proof.TerraformVMAbsent, proof.TerraformVMStatus) {
			return ErrDenied
		}
	case "terraform_preflight":
		if proof.Outcome != "failed" || (op.ProviderKind != "proxmox" && op.ProviderKind != "terraform") ||
			op.UPID != "" || op.TerraformApplyStartedAt != nil || proof.ReconciliationSHA256 != "" {
			return ErrDenied
		}
		if op.ProviderKind == "proxmox" && (op.PlanSHA256 != "" || proof.PlanSHA256 != "" || proof.StateIdentitySHA256 != "") {
			return ErrDenied
		}
		if op.ProviderKind == "terraform" && (op.PlanSHA256 == "" || proof.PlanSHA256 != op.PlanSHA256 || proof.StateIdentitySHA256 != op.StateIdentitySHA256) {
			return ErrDenied
		}
	default:
		return ErrDenied
	}
	return nil
}

// validTerraformObservedState requires the runtime's parsed Terraform state and
// independent Proxmox observation to agree with both the operation and outcome.
func validTerraformObservedState(kind, outcome string, stateHasVM, vmAbsent bool, vmStatus string) bool {
	switch kind {
	case "clone":
		return outcome == "succeeded" && stateHasVM && !vmAbsent && vmStatus == "stopped" ||
			outcome == "failed" && !stateHasVM && vmAbsent && vmStatus == ""
	case "start":
		return outcome == "succeeded" && stateHasVM && !vmAbsent && vmStatus == "running" ||
			outcome == "failed" && stateHasVM && !vmAbsent && vmStatus == "stopped"
	case "stop":
		return outcome == "succeeded" && stateHasVM && !vmAbsent && vmStatus == "stopped" ||
			outcome == "failed" && stateHasVM && !vmAbsent && vmStatus == "running"
	case "destroy":
		return outcome == "succeeded" && !stateHasVM && vmAbsent && vmStatus == "" ||
			outcome == "failed" && stateHasVM && !vmAbsent && vmStatus == "stopped"
	default:
		return false
	}
}

// lockTerraformReconciliationState binds proof to the state snapshot persisted
// by the authenticated Terraform backend and prevents concurrent state writes
// or lock acquisition until the resolution transaction commits.
func lockTerraformReconciliationState(ctx context.Context, tx pgx.Tx, reservationID, proofSHA string) error {
	var stateSHA sql.NullString
	var locked bool
	err := tx.QueryRow(ctx, `SELECT state_sha256,lock_id IS NOT NULL
		FROM runner_vm_terraform_states WHERE runner_vm_id=$1 FOR UPDATE`, reservationID).
		Scan(&stateSHA, &locked)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrDenied
	}
	if err != nil {
		return fmt.Errorf("%w: lock Terraform reconciliation snapshot: %v", ErrUnavailable, err)
	}
	if locked {
		return ErrDenied
	}
	expected := sha256.Sum256([]byte("ra8ci-no-state"))
	if stateSHA.Valid {
		if !runnerVMHexSHA256.MatchString(stateSHA.String) || proofSHA != stateSHA.String {
			return ErrDenied
		}
		return nil
	}
	if proofSHA != hex.EncodeToString(expected[:]) {
		return ErrDenied
	}
	return nil
}

func VMOperationSuccessState(kind string) string {
	switch kind {
	case "clone", "stop":
		return "stopped"
	case "start":
		return "running"
	case "destroy":
		return "released"
	default:
		return ""
	}
}

// ResolveRunnerVMOperation closes an unknown outcome only with fresh,
// independently observed post-state evidence. A failed mutation must mean a
// verified no-effect; otherwise the operation remains unresolved.
func (s *Store) ResolveRunnerVMOperation(ctx context.Context, actor, reservationID string, expectedGeneration int64, operationID string, proof RunnerVMResolution) (RunnerVM, error) {
	if s == nil || s.pool == nil || actor == "" || len(actor) > 256 ||
		!ValidID(reservationID) || !ValidID(operationID) || expectedGeneration < 1 {
		return RunnerVM{}, ErrInvalid
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: begin VM reconciliation: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	vm, err := scanRunnerVM(tx.QueryRow(ctx, `SELECT `+runnerVMColumns+` FROM runner_vms WHERE id=$1 FOR UPDATE`, reservationID))
	if errors.Is(err, pgx.ErrNoRows) {
		return RunnerVM{}, ErrNotFound
	}
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: lock VM reconciliation: %v", ErrUnavailable, err)
	}
	if vm.Generation != expectedGeneration || !vm.UnknownOutcome || vm.CurrentOperationID != operationID {
		return RunnerVM{}, ErrConflict
	}
	op, err := getRunnerVMOperation(ctx, tx, operationID)
	if err != nil {
		return RunnerVM{}, err
	}
	if op.RunnerVMID != reservationID || op.Status != "unresolved" || op.Generation != vm.Generation || op.PendingState != vm.State {
		return RunnerVM{}, ErrConflict
	}
	var now time.Time
	if err := tx.QueryRow(ctx, "SELECT clock_timestamp()").Scan(&now); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: reconciliation clock: %v", ErrUnavailable, err)
	}
	if err := validateVMResolution(now, op, proof); err != nil {
		return RunnerVM{}, err
	}
	if proof.Source == "terraform_state" {
		if err := lockTerraformReconciliationState(ctx, tx, reservationID, proof.ReconciliationSHA256); err != nil {
			return RunnerVM{}, err
		}
	}
	nextState := op.FromState
	if proof.Outcome == "succeeded" {
		nextState = VMOperationSuccessState(op.Kind)
		if op.Kind == "start" && vm.CleanupRequested {
			nextState = "draining"
		}
	}
	if nextState == "" {
		return RunnerVM{}, ErrConflict
	}
	_, err = tx.Exec(ctx, `UPDATE runner_vm_operations SET status=$2,resolved_at=clock_timestamp(),
		resolution_evidence_id=$3,resolution_source=$4,resolution_observed_at=$5,
		resolution_approval_id=$6,reconciliation_sha256=$7 WHERE id=$1 AND status='unresolved'`,
		op.ID, proof.Outcome, proof.EvidenceID, proof.Source, proof.ObservedAt,
		nullable(proof.OperatorApprovalID), nullable(proof.ReconciliationSHA256))
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: close VM operation: %v", ErrUnavailable, err)
	}
	tag, err := tx.Exec(ctx, `UPDATE runner_vms SET state=$3,generation=generation+1,
		unknown_outcome=false,current_operation_id=NULL,updated_at=clock_timestamp(),
		ended_at=CASE WHEN $3='released' THEN clock_timestamp() ELSE NULL END
		WHERE id=$1 AND generation=$2 AND current_operation_id=$4 AND unknown_outcome=true`,
		reservationID, vm.Generation, nextState, operationID)
	if err != nil || tag.RowsAffected() != 1 {
		return RunnerVM{}, fmt.Errorf("%w: VM reconciliation CAS: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm."+op.Kind+"."+proof.Outcome,
		"runner_vm", reservationID, proof.Outcome, vm.State, nextState, "",
		map[string]any{"operation_id": operationID, "generation": vm.Generation + 1,
			"evidence_id": proof.EvidenceID, "source": proof.Source,
			"reconciliation_sha256":  proof.ReconciliationSHA256,
			"terraform_state_has_vm": proof.TerraformStateHasVM,
			"terraform_vm_absent":    proof.TerraformVMAbsent,
			"terraform_vm_status":    proof.TerraformVMStatus}); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: audit VM reconciliation: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: commit VM reconciliation: %v", ErrUnavailable, err)
	}
	return s.GetRunnerVM(ctx, reservationID)
}

func (s *Store) transitionRunnerVM(ctx context.Context, actor, id string, expectedGeneration int64, from []string, to, action string, runnerID int64, runnerName string) (RunnerVM, error) {
	if s == nil || s.pool == nil || actor == "" || len(actor) > 256 || !ValidID(id) || expectedGeneration < 1 {
		return RunnerVM{}, ErrInvalid
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: begin VM transition: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	vm, err := scanRunnerVM(tx.QueryRow(ctx, `SELECT `+runnerVMColumns+` FROM runner_vms WHERE id=$1 FOR UPDATE`, id))
	if errors.Is(err, pgx.ErrNoRows) {
		return RunnerVM{}, ErrNotFound
	}
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: lock VM transition: %v", ErrUnavailable, err)
	}
	allowed := false
	for _, state := range from {
		allowed = allowed || vm.State == state
	}
	if !allowed || vm.UnknownOutcome || vm.Generation != expectedGeneration {
		return RunnerVM{}, ErrConflict
	}
	var runnerIDValue any
	var runnerNameValue any
	if runnerID > 0 {
		runnerIDValue, runnerNameValue = runnerID, runnerName
	} else {
		runnerIDValue = nullablePositive(vm.ExternalRunnerID)
		runnerNameValue = nullable(vm.ExternalRunnerName)
	}
	tag, err := tx.Exec(ctx, `UPDATE runner_vms SET state=$3,generation=generation+1,
		external_runner_id=$4,external_runner_name=$5,
		cleanup_requested=cleanup_requested OR $6,updated_at=clock_timestamp()
		WHERE id=$1 AND generation=$2 AND unknown_outcome=false`,
		id, vm.Generation, to, runnerIDValue, runnerNameValue, to == "draining")
	if err != nil || tag.RowsAffected() != 1 {
		return RunnerVM{}, fmt.Errorf("%w: VM transition CAS: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, actor, action, "runner_vm", id, "ok",
		vm.State, to, "", map[string]any{"generation": vm.Generation + 1,
			"external_runner_id": runnerID}); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: audit VM transition: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: commit VM transition: %v", ErrUnavailable, err)
	}
	return s.GetRunnerVM(ctx, id)
}

// MarkRunnerVMRegistered binds the external runner ID after the exact VM is
// running. The runner identity is not accepted during initial reservation.
func (s *Store) MarkRunnerVMRegistered(ctx context.Context, actor, reservationID string, expectedGeneration, externalRunnerID int64, externalRunnerName string) (RunnerVM, error) {
	if externalRunnerID <= 0 || len(externalRunnerName) == 0 || len(externalRunnerName) > 256 {
		return RunnerVM{}, ErrInvalid
	}
	return s.transitionRunnerVM(ctx, actor, reservationID, expectedGeneration,
		[]string{"running"}, "registered", "runner_vm.registered", externalRunnerID, externalRunnerName)
}

// MarkRunnerVMDraining forbids another start/registration while the scaler
// obtains fresh drained/no-active-job evidence for stop.
func (s *Store) MarkRunnerVMDraining(ctx context.Context, actor, reservationID string, expectedGeneration int64) (RunnerVM, error) {
	if s == nil || s.pool == nil || actor == "" || len(actor) > 256 || !ValidID(reservationID) || expectedGeneration < 1 {
		return RunnerVM{}, ErrInvalid
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: begin VM drain: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	vm, err := scanRunnerVM(tx.QueryRow(ctx, `SELECT `+runnerVMColumns+` FROM runner_vms WHERE id=$1 FOR UPDATE`, reservationID))
	if errors.Is(err, pgx.ErrNoRows) {
		return RunnerVM{}, ErrNotFound
	}
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: lock VM drain: %v", ErrUnavailable, err)
	}
	if vm.CleanupRequested {
		return vm, nil
	}
	if vm.Generation != expectedGeneration {
		return RunnerVM{}, ErrConflict
	}
	nextState := vm.State
	if !vm.UnknownOutcome {
		switch vm.State {
		case "reserved", "stopped":
			// Job ended before clone/start. Keep the safe state for abandon/destroy.
		case "running", "registered":
			nextState = "draining"
		default:
			return RunnerVM{}, ErrConflict
		}
	}
	nextGeneration := vm.Generation
	if !vm.UnknownOutcome {
		nextGeneration++
	}
	tag, err := tx.Exec(ctx, `UPDATE runner_vms SET state=$3,cleanup_requested=true,
		generation=generation+CASE WHEN unknown_outcome THEN 0 ELSE 1 END,
		updated_at=clock_timestamp()
		WHERE id=$1 AND generation=$2 AND cleanup_requested=false`,
		vm.ID, vm.Generation, nextState)
	if err != nil || tag.RowsAffected() != 1 {
		return RunnerVM{}, fmt.Errorf("%w: drain fence CAS: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm.draining", "runner_vm", vm.ID,
		"ok", vm.State, nextState, "", map[string]any{"generation": nextGeneration,
			"pending_operation": vm.UnknownOutcome}); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: audit VM drain: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: commit VM drain: %v", ErrUnavailable, err)
	}
	return s.GetRunnerVM(ctx, vm.ID)
}

// MarkRunnerVMClaimed records that a job actually took this runner, which
// ends the reaper's interest in it for good. It is deliberately not a
// generation-fenced transition: claiming is not a Proxmox mutation, and
// bumping the generation here would invalidate an in-flight operation CAS
// that has nothing to do with the job starting.
//
// Idempotent by design. The forge redelivers, and the second delivery must
// not move the claim timestamp, so the write is fenced on claimed_at IS NULL
// and a row already claimed is returned unchanged.
func (s *Store) MarkRunnerVMClaimed(ctx context.Context, actor, reservationID string) (RunnerVM, error) {
	if s == nil || s.pool == nil || actor == "" || len(actor) > 256 || !ValidID(reservationID) {
		return RunnerVM{}, ErrInvalid
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: begin claim: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	vm, err := scanRunnerVM(tx.QueryRow(ctx, `SELECT `+runnerVMColumns+`
		FROM runner_vms WHERE id=$1 FOR UPDATE`, reservationID))
	if errors.Is(err, pgx.ErrNoRows) {
		return RunnerVM{}, ErrNotFound
	}
	if err != nil {
		return RunnerVM{}, fmt.Errorf("%w: lock claim: %v", ErrUnavailable, err)
	}
	if vm.ClaimedAt != nil {
		return vm, nil
	}
	if vm.State == "released" {
		return RunnerVM{}, fmt.Errorf("%w: reservation already released", ErrConflict)
	}
	tag, err := tx.Exec(ctx, `UPDATE runner_vms SET claimed_at=clock_timestamp(),
		updated_at=clock_timestamp() WHERE id=$1 AND claimed_at IS NULL`, vm.ID)
	if err != nil || tag.RowsAffected() != 1 {
		return RunnerVM{}, fmt.Errorf("%w: claim CAS: %v", ErrConflict, err)
	}
	if err := appendAudit(ctx, tx, actor, "runner_vm.claimed", "runner_vm", vm.ID,
		"ok", vm.State, vm.State, "", map[string]any{
			"unclaimed_deadline": vm.UnclaimedDeadline.UTC().Format(time.RFC3339Nano)}); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: audit claim: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return RunnerVM{}, fmt.Errorf("%w: commit claim: %v", ErrUnavailable, err)
	}
	return s.GetRunnerVM(ctx, vm.ID)
}

// ListExpiredUnclaimedRunnerVMs is the unclaimed-runner reaper's work queue:
// reservations whose deadline has passed with no job ever taking them,
// soonest deadline first. The WHERE clause is built from unclaimedCandidate,
// the same predicate UnclaimedExpired states in Go, so the queue and the
// guard a caller re-runs under its own lock cannot drift apart.
func (s *Store) ListExpiredUnclaimedRunnerVMs(ctx context.Context, scaleSetID int64, now time.Time, limit int) ([]RunnerVM, error) {
	if s == nil || s.pool == nil || scaleSetID <= 0 || now.IsZero() || limit < 1 || limit > 1000 {
		return nil, ErrInvalid
	}
	query := `SELECT ` + runnerVMColumns + ` FROM runner_vms
		WHERE scale_set_id=$1 AND ` + fmt.Sprintf(unclaimedCandidate, 2) + `
		ORDER BY unclaimed_deadline,id LIMIT $3`
	rows, err := s.pool.Query(ctx, query, scaleSetID, now.UTC(), limit)
	if err != nil {
		return nil, fmt.Errorf("%w: list expired unclaimed VMs: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	var result []RunnerVM
	for rows.Next() {
		vm, err := scanRunnerVM(rows)
		if err != nil {
			return nil, fmt.Errorf("%w: expired unclaimed VM scan: %v", ErrUnavailable, err)
		}
		result = append(result, vm)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: expired unclaimed VM rows: %v", ErrUnavailable, err)
	}
	return result, nil
}

// ListUnresolvedRunnerVMs is the restart queue. Every row needs explicit
// Proxmox reconciliation before a new mutation can be issued.
func (s *Store) ListUnresolvedRunnerVMs(ctx context.Context, scaleSetID int64, limit int) ([]RunnerVM, error) {
	if s == nil || s.pool == nil || scaleSetID <= 0 || limit < 1 || limit > 1000 {
		return nil, ErrInvalid
	}
	rows, err := s.pool.Query(ctx, `SELECT `+runnerVMColumns+` FROM runner_vms
		WHERE scale_set_id=$1 AND unknown_outcome=true ORDER BY updated_at,id LIMIT $2`, scaleSetID, limit)
	if err != nil {
		return nil, fmt.Errorf("%w: list unresolved VMs: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	var result []RunnerVM
	for rows.Next() {
		vm, err := scanRunnerVM(rows)
		if err != nil {
			return nil, fmt.Errorf("%w: unresolved VM scan: %v", ErrUnavailable, err)
		}
		result = append(result, vm)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: unresolved VM rows: %v", ErrUnavailable, err)
	}
	return result, nil
}
