//go:build integration

package store

import (
	"bytes"
	"context"
	"crypto/aes"
	"crypto/cipher"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"strconv"
	"strings"
	"testing"
	"time"
)

func runnerVMTestInput(t *testing.T) RunnerVMInput {
	t.Helper()
	id := strings.ReplaceAll(mustID(t), "-", "")
	number, err := strconv.ParseInt(id[len(id)-8:], 16, 64)
	if err != nil {
		t.Fatal(err)
	}
	return RunnerVMInput{
		ScaleSetID: 1000 + number%1000000, JobID: mustID(t), RunnerRequestID: number + 1,
		WorkflowRunID: number + 2, WorkflowAttempt: 1,
		Repository:  "bsikar/ra8-firmware",
		WorkflowRef: "bsikar/ra8-firmware/.github/workflows/ci.yml@refs/heads/dev",
		CommitSHA:   strings.Repeat("a", 40), VMID: int(9000 + number%1000000),
		Node: "pve", Pool: "ra8-tf-lab", Storage: "ra8-tf-lab",
		Name: "ra8-lab-" + id[:12], TemplateVMID: 9001,
		TemplateName: "ra8-lab-template", TemplateDigest: strings.Repeat("b", 40),
	}
}

// testUnclaimedDeadline is the deadline a reservation in these tests carries.
// Every reservation has to have one, so the fixture takes the default lease
// rather than letting each test invent a duration.
func testUnclaimedDeadline() time.Time {
	return time.Now().UTC().Add(DefaultUnclaimedLease)
}

func vmResolution(t *testing.T) RunnerVMResolution {
	t.Helper()
	return RunnerVMResolution{Outcome: "succeeded", EvidenceID: mustID(t),
		Source: "upid", ObservedAt: time.Now().UTC(), PostStateVerified: true}
}

func TestIntegrationRunnerVMLifecycleAndNoRestartAfterDrain(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	in := runnerVMTestInput(t)
	vm, created, err := s.ReserveRunnerVM(ctx, "scaler", in, testUnclaimedDeadline())
	if err != nil || !created || vm.State != "reserved" || vm.Generation != 1 || !ValidID(vm.CreationOperationID) {
		t.Fatalf("reserve exact VM: %+v created=%v err=%v", vm, created, err)
	}
	replayed, created, err := s.ReserveRunnerVM(ctx, "scaler", in, testUnclaimedDeadline())
	if err != nil || created || replayed.ID != vm.ID || replayed.CreationOperationID != vm.CreationOperationID {
		t.Fatalf("job replay changed identity: %+v created=%v err=%v", replayed, created, err)
	}
	byJob, err := s.GetRunnerVMByJob(ctx, in.ScaleSetID, in.JobID)
	if err != nil || byJob.ID != vm.ID {
		t.Fatalf("job lookup: %+v %v", byJob, err)
	}
	changed := in
	changed.VMID++
	if _, _, err := s.ReserveRunnerVM(ctx, "scaler", changed, testUnclaimedDeadline()); !errors.Is(err, ErrConflict) {
		t.Fatalf("changed VMID rebound job: %v", err)
	}
	other := runnerVMTestInput(t)
	other.VMID = in.VMID
	if _, _, err := s.ReserveRunnerVM(ctx, "scaler", other, testUnclaimedDeadline()); !errors.Is(err, ErrConflict) {
		t.Fatalf("active VMID reused: %v", err)
	}
	clone, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "clone", RunnerVMSafetyEvidence{})
	if err != nil || clone.ID != vm.CreationOperationID || clone.PriorRequestIssued || clone.Generation != 2 {
		t.Fatalf("clone intent before call: %+v %v", clone, err)
	}
	retry, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "clone", RunnerVMSafetyEvidence{})
	if err != nil || retry.ID != clone.ID || !retry.PriorRequestIssued {
		t.Fatalf("unknown clone was reissued: %+v %v", retry, err)
	}
	unresolved, err := s.ListUnresolvedRunnerVMs(ctx, in.ScaleSetID, 10)
	if err != nil || len(unresolved) == 0 || unresolved[0].CurrentOperationID != clone.ID {
		t.Fatalf("clone intent lost on restart: %+v %v", unresolved, err)
	}
	if _, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, clone.Generation, "start", RunnerVMSafetyEvidence{}); !errors.Is(err, ErrConflict) {
		t.Fatalf("new action started while clone unknown: %v", err)
	}
	if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, clone.Generation, clone.ID, "UPID:pve:001:clone"); err != nil {
		t.Fatal(err)
	}
	stopped, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, clone.Generation, clone.ID, vmResolution(t))
	if err != nil || stopped.State != "stopped" || stopped.Generation != 3 || stopped.UnknownOutcome {
		t.Fatalf("clone reconcile: %+v %v", stopped, err)
	}
	start, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, stopped.Generation, "start", RunnerVMSafetyEvidence{})
	if err != nil {
		t.Fatal(err)
	}
	if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, start.Generation, start.ID, "UPID:pve:002:start"); err != nil {
		t.Fatal(err)
	}
	running, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, start.Generation, start.ID, vmResolution(t))
	if err != nil || running.State != "running" {
		t.Fatalf("start reconcile: %+v %v", running, err)
	}
	bootstrapEvidence := RunnerVMBootstrapEvidence{
		ReservationID: vm.ID, VMID: vm.VMID, CommitSHA: vm.CommitSHA,
		GuestOS: "linux", GuestArchitecture: "amd64", ServiceAccount: "ra8ci",
		RunnerBinarySHA256: strings.Repeat("1", 64), AgentBinarySHA256: strings.Repeat("2", 64),
		ReadinessSHA256: strings.Repeat("3", 64), JITConfigSHA256: strings.Repeat("4", 64),
		JITConfigExpiresAt: time.Now().Add(30 * time.Minute), EvidenceID: mustID(t),
		StartedAt: time.Now().Add(-time.Second), CompletedAt: time.Now(), PreparedAt: time.Now(),
	}
	if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", bootstrapEvidence); err != nil {
		t.Fatalf("record validated bootstrap evidence: %v", err)
	}
	var bootstrapAudit string
	if err := pool.QueryRow(ctx, "SELECT reason::text FROM audit WHERE action='runner_vm.bootstrap.ready' AND target_id=$1", vm.ID).Scan(&bootstrapAudit); err != nil {
		t.Fatalf("read bootstrap audit: %v", err)
	}
	var bootstrapReason map[string]json.RawMessage
	if err := json.Unmarshal([]byte(bootstrapAudit), &bootstrapReason); err != nil {
		t.Fatalf("decode bootstrap audit: %v", err)
	}
	var bootstrapDurationMS int64
	if err := json.Unmarshal(bootstrapReason["bootstrap_duration_ms"], &bootstrapDurationMS); err != nil || bootstrapDurationMS <= 0 {
		t.Fatalf("bootstrap duration missing or invalid: %d, %v", bootstrapDurationMS, err)
	}
	if !strings.Contains(bootstrapAudit, bootstrapEvidence.ReadinessSHA256) ||
		bootstrapReason["jit_config"] != nil || bootstrapReason["encoded_config"] != nil {
		t.Fatalf("bootstrap audit lacks digest metadata or contains JIT bytes: %s", bootstrapAudit)
	}
	registered, err := s.MarkRunnerVMRegistered(ctx, "scaler", vm.ID, running.Generation, 7734, "ra8ci-runner-7734")
	if err != nil || registered.State != "registered" || registered.ExternalRunnerID != 7734 {
		t.Fatalf("runner registration: %+v %v", registered, err)
	}
	draining, err := s.MarkRunnerVMDraining(ctx, "scaler", vm.ID, registered.Generation)
	if err != nil || draining.State != "draining" || !draining.CleanupRequested {
		t.Fatalf("drain fence: %+v %v", draining, err)
	}
	stopProof := RunnerVMSafetyEvidence{EvidenceID: mustID(t), ObservedAt: time.Now().UTC(),
		Drained: true, NoActiveJob: true}
	stop, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, draining.Generation, "stop", stopProof)
	if err != nil {
		t.Fatal(err)
	}
	if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, stop.Generation, stop.ID, "UPID:pve:003:stop"); err != nil {
		t.Fatal(err)
	}
	stopped, err = s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, stop.Generation, stop.ID, vmResolution(t))
	if err != nil || stopped.State != "stopped" || !stopped.CleanupRequested {
		t.Fatalf("stop reconcile: %+v %v", stopped, err)
	}
	if _, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, stopped.Generation, "start", RunnerVMSafetyEvidence{}); !errors.Is(err, ErrConflict) {
		t.Fatalf("completed VM was restarted: %v", err)
	}
	destroyProof := RunnerVMSafetyEvidence{EvidenceID: mustID(t), ObservedAt: time.Now().UTC(),
		Drained: true, NoActiveJob: true, RunnerDeregistered: true,
		ExpectedConfigDigest: strings.Repeat("c", 40), ApprovalID: mustID(t), ExternalRunnerID: 7734}
	destroy, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, stopped.Generation, "destroy", destroyProof)
	if err != nil {
		t.Fatal(err)
	}
	if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, destroy.Generation, destroy.ID, "UPID:pve:004:destroy"); err != nil {
		t.Fatal(err)
	}
	released, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, destroy.Generation, destroy.ID, vmResolution(t))
	if err != nil || released.State != "released" || released.EndedAt == nil {
		t.Fatalf("destroy reconcile: %+v %v", released, err)
	}
	var audits int
	if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM audit WHERE target_type='runner_vm' AND target_id=$1", vm.ID).Scan(&audits); err != nil || audits < 11 {
		t.Fatalf("VM lifecycle audit missing: %d %v", audits, err)
	}
	other.VMID = in.VMID
	if _, created, err := s.ReserveRunnerVM(ctx, "scaler", other, testUnclaimedDeadline()); err != nil || !created {
		t.Fatalf("released VMID could not be reserved again: created=%v %v", created, err)
	}
}

func TestIntegrationRunnerVMEarlyCompletionAndUnknownStart(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	vm, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
	if err != nil {
		t.Fatal(err)
	}
	completed, err := s.MarkRunnerVMDraining(ctx, "scaler", vm.ID, vm.Generation)
	if err != nil || !completed.CleanupRequested || completed.State != "reserved" {
		t.Fatalf("early completion did not fence reserve: %+v %v", completed, err)
	}
	if _, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, completed.Generation, "start", RunnerVMSafetyEvidence{}); !errors.Is(err, ErrConflict) {
		t.Fatalf("early completed job started: %v", err)
	}
	if _, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, completed.Generation, "clone", RunnerVMSafetyEvidence{}); !errors.Is(err, ErrConflict) {
		t.Fatalf("early completed job cloned: %v", err)
	}
	stillReserved, err := s.GetRunnerVM(ctx, vm.ID)
	if err != nil || stillReserved.State != "reserved" || !stillReserved.CleanupRequested {
		t.Fatalf("unverified absence released reservation: %+v %v", stillReserved, err)
	}
	second, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
	if err != nil {
		t.Fatal(err)
	}
	clone, err := s.BeginRunnerVMOperation(ctx, "scaler", second.ID, second.Generation, "clone", RunnerVMSafetyEvidence{})
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", second.ID, clone.Generation, clone.ID,
		RunnerVMResolution{Outcome: "succeeded", EvidenceID: mustID(t), Source: "upid",
			ObservedAt: time.Now().UTC(), PostStateVerified: true}); !errors.Is(err, ErrDenied) {
		t.Fatalf("lost clone UPID accepted as UPID proof: %v", err)
	}
	stopped, err := s.ResolveRunnerVMOperation(ctx, "operator", second.ID, clone.Generation, clone.ID,
		RunnerVMResolution{Outcome: "succeeded", EvidenceID: mustID(t), Source: "clone_marker",
			ObservedAt: time.Now().UTC(), PostStateVerified: true})
	if err != nil || stopped.State != "stopped" {
		t.Fatalf("exact clone marker could not reconcile: %+v %v", stopped, err)
	}
	start, err := s.BeginRunnerVMOperation(ctx, "scaler", second.ID, stopped.Generation, "start", RunnerVMSafetyEvidence{})
	if err != nil {
		t.Fatal(err)
	}
	completed, err = s.MarkRunnerVMDraining(ctx, "scaler", second.ID, start.Generation)
	if err != nil || !completed.CleanupRequested || completed.Generation != start.Generation {
		t.Fatalf("inflight start not fenced by Completed: %+v %v", completed, err)
	}
	_, err = s.ResolveRunnerVMOperation(ctx, "scaler", second.ID, start.Generation, start.ID,
		RunnerVMResolution{Outcome: "succeeded", EvidenceID: mustID(t), Source: "upid",
			ObservedAt: time.Now().UTC(), PostStateVerified: true})
	if !errors.Is(err, ErrDenied) {
		t.Fatalf("start without UPID was auto-reconciled: %v", err)
	}
	if err := s.RecordRunnerVMUPID(ctx, "scaler", second.ID, start.Generation, start.ID, "UPID:pve:005:start"); err != nil {
		t.Fatal(err)
	}
	draining, err := s.ResolveRunnerVMOperation(ctx, "scaler", second.ID, start.Generation, start.ID, vmResolution(t))
	if err != nil || draining.State != "draining" || !draining.CleanupRequested {
		t.Fatalf("completed start became runnable: %+v %v", draining, err)
	}
}
func TestIntegrationRunnerVMTerraformPlanEvidenceIsImmutableAndFenced(t *testing.T) {
	s, pool := integrationStore(t)
	block, err := aes.NewCipher(bytes.Repeat([]byte{0x5a}, 32))
	if err != nil {
		t.Fatal(err)
	}
	s.terraformStateAEAD, err = cipher.NewGCM(block)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	input := runnerVMTestInput(t)
	vm, created, err := s.ReserveRunnerVM(ctx, "scaler", input, testUnclaimedDeadline())
	if err != nil || !created {
		t.Fatalf("reserve VM: %+v, %v", vm, err)
	}
	op, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "clone", RunnerVMSafetyEvidence{})
	if err != nil {
		t.Fatal(err)
	}
	evidence := RunnerVMTerraformPlanEvidence{
		TerraformVersion: "1.10.5", PlanSHA256: strings.Repeat("c", 64),
		ModuleSHA256: strings.Repeat("d", 64), InputSHA256: strings.Repeat("e", 64),
		ProviderLockSHA256: strings.Repeat("f", 64), StateIdentitySHA256: strings.Repeat("1", 64),
		PreparedAt: time.Now().UTC(),
	}
	if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", vm.ID, op.Generation, op.ID, evidence); err != nil {
		t.Fatal(err)
	}
	stored, err := s.GetRunnerVMOperation(ctx, op.ID)
	if err != nil || stored.ProviderKind != "terraform" ||
		stored.PlanSHA256 != evidence.PlanSHA256 ||
		stored.ModuleSHA256 != evidence.ModuleSHA256 ||
		stored.InputSHA256 != evidence.InputSHA256 ||
		stored.ProviderLockSHA256 != evidence.ProviderLockSHA256 ||
		stored.StateIdentitySHA256 != evidence.StateIdentitySHA256 {
		t.Fatalf("Terraform plan evidence was not stored: %+v, %v", stored, err)
	}
	if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, op.Generation, op.ID, "UPID:pve:006:terraform"); !errors.Is(err, ErrConflict) {
		t.Fatalf("Terraform operation accepted a Proxmox UPID: %v", err)
	}
	if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID,
		RunnerVMResolution{Outcome: "succeeded", EvidenceID: mustID(t), Source: "clone_marker",
			ObservedAt: time.Now().UTC(), PostStateVerified: true}); !errors.Is(err, ErrDenied) {
		t.Fatalf("Terraform operation accepted a Proxmox clone marker: %v", err)
	}
	if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", vm.ID, op.Generation, op.ID, evidence); err != nil {
		t.Fatalf("exact plan replay failed: %v", err)
	}
	staleReplay := evidence
	staleReplay.PreparedAt = time.Now().Add(-time.Hour)
	if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", vm.ID, op.Generation, op.ID, staleReplay); err != nil {
		t.Fatalf("late exact plan replay was not idempotent: %v", err)
	}
	changed := evidence
	changed.PlanSHA256 = strings.Repeat("2", 64)
	if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", vm.ID, op.Generation, op.ID, changed); !errors.Is(err, ErrConflict) {
		t.Fatalf("changed saved plan replaced durable evidence: %v", err)
	}
	if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", vm.ID, op.Generation-1, op.ID, evidence); !errors.Is(err, ErrConflict) {
		t.Fatalf("stale VM generation recorded plan evidence: %v", err)
	}
	if begun, err := s.BeginRunnerVMTerraformApply(ctx, "scaler", vm.ID, op.Generation, op.ID, strings.Repeat("2", 64)); !errors.Is(err, ErrConflict) || begun {
		t.Fatalf("apply intent accepted another plan: begun=%t err=%v", begun, err)
	}
	// Simulate multiple controller processes racing after they independently load
	// the same saved plan. PostgreSQL must elect exactly one process to invoke
	// apply; all others are reconcile-only, even under simultaneous requests.
	type applyIntentResult struct {
		begun bool
		err   error
	}
	const contenders = 8
	results := make(chan applyIntentResult, contenders)
	for i := 0; i < contenders; i++ {
		go func() {
			begun, err := s.BeginRunnerVMTerraformApply(ctx, "scaler", vm.ID, op.Generation, op.ID, evidence.PlanSHA256)
			results <- applyIntentResult{begun: begun, err: err}
		}()
	}
	admitted := 0
	for i := 0; i < contenders; i++ {
		result := <-results
		if result.err != nil {
			t.Fatalf("concurrent Terraform apply intent: %v", result.err)
		}
		if result.begun {
			admitted++
		}
	}
	if admitted != 1 {
		t.Fatalf("concurrent apply intents admitted %d processes; want exactly one", admitted)
	}
	if begun, err := s.BeginRunnerVMTerraformApply(ctx, "scaler", vm.ID, op.Generation, op.ID, evidence.PlanSHA256); err != nil || begun {
		t.Fatalf("Terraform apply replay was not forced to reconcile: begun=%t err=%v", begun, err)
	}
	stored, err = s.GetRunnerVMOperation(ctx, op.ID)
	if err != nil || stored.TerraformApplyStartedAt == nil {
		t.Fatalf("Terraform apply intent was not durable: %+v err=%v", stored, err)
	}
	lockID := mustID(t)
	lockBytes, err := json.Marshal(TerraformStateLock{ID: lockID, Operation: "OperationTypeApply",
		Who: "ra8ci@test", Version: "1.10.5", Path: "runner/" + vm.ID})
	if err != nil {
		t.Fatal(err)
	}
	if _, acquired, err := s.LockRunnerVMTerraformState(ctx, "scaler", vm.ID, lockBytes); err != nil || !acquired {
		t.Fatalf("acquire backend state lock: acquired=%t err=%v", acquired, err)
	}
	stateBody := []byte(`{"version":4,"terraform_version":"1.10.5","serial":1,"lineage":"123e4567-e89b-42d3-a456-426614174000","resources":[]}`)
	if err := s.WriteRunnerVMTerraformState(ctx, "scaler", vm.ID, lockID, stateBody); err != nil {
		t.Fatalf("persist encrypted Terraform state through backend API: %v", err)
	}
	if _, released, err := s.UnlockRunnerVMTerraformState(ctx, "scaler", vm.ID, lockBytes); err != nil || !released {
		t.Fatalf("release backend state lock: released=%t err=%v", released, err)
	}
	stateDigest := sha256.Sum256(stateBody)
	expectedStateSHA := hex.EncodeToString(stateDigest[:])
	var stateSHA string
	var stateCiphertext []byte
	if err := pool.QueryRow(ctx, `SELECT state_sha256,state_ciphertext FROM runner_vm_terraform_states WHERE runner_vm_id=$1`, vm.ID).Scan(&stateSHA, &stateCiphertext); err != nil {
		t.Fatalf("read persisted state digest: %v", err)
	}
	if stateSHA != expectedStateSHA {
		t.Fatalf("stored state digest %q was not derived from submitted snapshot; want %q", stateSHA, expectedStateSHA)
	}
	if len(stateCiphertext) == 0 || bytes.Equal(stateCiphertext, stateBody) || bytes.Contains(stateCiphertext, stateBody) {
		t.Fatal("Terraform state was not encrypted at rest")
	}
	proof := RunnerVMResolution{
		Outcome: "succeeded", EvidenceID: mustID(t), Source: "terraform_state",
		ObservedAt: time.Now().UTC(), PostStateVerified: true,
		PlanSHA256: evidence.PlanSHA256, StateIdentitySHA256: evidence.StateIdentitySHA256,
		ReconciliationSHA256: expectedStateSHA, TerraformStateHasVM: true,
		TerraformVMStatus: "stopped",
	}
	badProof := proof
	badProof.PlanSHA256 = strings.Repeat("3", 64)
	if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, badProof); !errors.Is(err, ErrDenied) {
		t.Fatalf("Terraform reconciliation accepted another plan identity: %v", err)
	}
	badProof = proof
	badProof.StateIdentitySHA256 = strings.Repeat("3", 64)
	if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, badProof); !errors.Is(err, ErrDenied) {
		t.Fatalf("Terraform reconciliation accepted another state identity: %v", err)
	}
	badProof = proof
	badProof.ReconciliationSHA256 = strings.Repeat("5", 64)
	if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, badProof); !errors.Is(err, ErrDenied) {
		t.Fatalf("Terraform accepted a fabricated state digest: %v", err)
	}
	badProof = proof
	badProof.TerraformVMStatus = "running"
	if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, badProof); !errors.Is(err, ErrDenied) {
		t.Fatalf("Terraform accepted state/VM observation mismatch: %v", err)
	}
	badProof = proof
	badProof.Outcome = "failed"
	if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, badProof); !errors.Is(err, ErrDenied) {
		t.Fatalf("Terraform accepted failed outcome without verified no-effect: %v", err)
	}
	if _, err := pool.Exec(ctx, `UPDATE runner_vm_terraform_states SET lock_id=$2,
		lock_info='{}'::jsonb,locked_at=clock_timestamp() WHERE runner_vm_id=$1`, vm.ID, mustID(t)); err != nil {
		t.Fatal(err)
	}
	if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, proof); !errors.Is(err, ErrDenied) {
		t.Fatalf("Terraform resolution accepted a locked backend state: %v", err)
	}
	if _, err := pool.Exec(ctx, `UPDATE runner_vm_terraform_states SET lock_id=NULL,
		lock_info=NULL,locked_at=NULL WHERE runner_vm_id=$1`, vm.ID); err != nil {
		t.Fatal(err)
	}
	reconciled, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, proof)
	if err != nil || reconciled.State != "stopped" || reconciled.UnknownOutcome {
		t.Fatalf("Terraform state proof did not resolve the operation: %+v, %v", reconciled, err)
	}
	resolved, err := s.GetRunnerVMOperation(ctx, op.ID)
	if err != nil || resolved.Status != "succeeded" || resolved.ProviderKind != "terraform" || resolved.ReconciliationSHA256 != proof.ReconciliationSHA256 {
		t.Fatalf("Terraform reconciliation evidence was not retained: %+v, %v", resolved, err)
	}
}
func TestValidTerraformObservedStateAcceptsVerifiedNoEffectOutcomes(t *testing.T) {
	// @par MC/DC: each expected observation below makes the operation/outcome valid;
	// changing the state-presence, VM-absence, or status condition makes it invalid.
	cases := []struct {
		kind, outcome, status string
		hasVM, absent         bool
	}{
		{"clone", "failed", "", false, true},
		{"start", "failed", "stopped", true, false},
		{"stop", "failed", "running", true, false},
		{"destroy", "failed", "stopped", true, false},
	}
	for _, test := range cases {
		if !validTerraformObservedState(test.kind, test.outcome, test.hasVM, test.absent, test.status) {
			t.Errorf("valid verified no-effect evidence rejected: %+v", test)
		}
	}
	if validTerraformObservedState("clone", "failed", true, true, "running") {
		t.Fatal("inconsistent failed/no-effect evidence accepted")
	}
}

// A reservation carries its deadline from the moment it exists, a replayed
// delivery gets the original deadline back rather than a fresh one, and a
// claim takes the row out of the reaper's queue for good.
func TestIntegrationUnclaimedDeadlineAndClaim(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	in := runnerVMTestInput(t)
	before := time.Now().UTC()
	deadline := before.Add(DefaultUnclaimedLease)
	vm, created, err := s.ReserveRunnerVM(ctx, "scaler", in, deadline)
	if err != nil || !created {
		t.Fatalf("reserve: %+v created=%v err=%v", vm, created, err)
	}
	if vm.UnclaimedDeadline.IsZero() || vm.UnclaimedDeadline.Sub(deadline).Abs() > time.Second {
		t.Fatalf("reservation did not carry its deadline: %s want %s", vm.UnclaimedDeadline, deadline)
	}
	if vm.ClaimedAt != nil {
		t.Fatalf("a fresh reservation is claimed: %v", vm.ClaimedAt)
	}
	if !UnclaimedReservation(vm) || UnclaimedExpired(vm, before) {
		t.Fatalf("fresh reservation reads as expired: %+v", vm)
	}

	// A redelivery must not extend the life of a credential nobody took.
	replayed, created, err := s.ReserveRunnerVM(ctx, "scaler", in, before.Add(MaxUnclaimedLease))
	if err != nil || created {
		t.Fatalf("replay: created=%v err=%v", created, err)
	}
	if !replayed.UnclaimedDeadline.Equal(vm.UnclaimedDeadline) {
		t.Fatalf("replay moved the deadline %s -> %s", vm.UnclaimedDeadline, replayed.UnclaimedDeadline)
	}

	claimed, err := s.MarkRunnerVMClaimed(ctx, "dispatch", vm.ID)
	if err != nil || claimed.ClaimedAt == nil {
		t.Fatalf("claim: %+v %v", claimed, err)
	}
	if claimed.Generation != vm.Generation {
		t.Fatalf("claim bumped the operation generation %d -> %d", vm.Generation, claimed.Generation)
	}
	again, err := s.MarkRunnerVMClaimed(ctx, "dispatch", vm.ID)
	if err != nil || again.ClaimedAt == nil || !again.ClaimedAt.Equal(*claimed.ClaimedAt) {
		t.Fatalf("second claim moved the timestamp: %+v %v", again, err)
	}
	if UnclaimedExpired(again, time.Now().UTC().Add(MaxUnclaimedLease)) {
		t.Fatal("a claimed reservation came back as expired")
	}
	if _, err := s.MarkRunnerVMClaimed(ctx, "dispatch", mustID(t)); !errors.Is(err, ErrNotFound) {
		t.Fatalf("claim of an unknown reservation: %v", err)
	}
	if _, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), time.Now().UTC().Add(time.Second)); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a one second lease was accepted: %v", err)
	}
}

// The reaper queue is exactly the reservations UnclaimedExpired names, in
// deadline order: the SQL predicate and the Go one are the same sentence.
func TestIntegrationListExpiredUnclaimedRunnerVMs(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	scaleSet := runnerVMTestInput(t).ScaleSetID
	now := time.Now().UTC()

	reserve := func(lease time.Duration) RunnerVM {
		t.Helper()
		in := runnerVMTestInput(t)
		in.ScaleSetID = scaleSet
		vm, created, err := s.ReserveRunnerVM(ctx, "scaler", in, now.Add(lease))
		if err != nil || !created {
			t.Fatalf("reserve: %v", err)
		}
		return vm
	}
	soon := reserve(2 * time.Minute)
	later := reserve(10 * time.Minute)
	fresh := reserve(MaxUnclaimedLease)
	taken := reserve(3 * time.Minute)
	if _, err := s.MarkRunnerVMClaimed(ctx, "dispatch", taken.ID); err != nil {
		t.Fatal(err)
	}

	after := now.Add(time.Hour)
	expired, err := s.ListExpiredUnclaimedRunnerVMs(ctx, scaleSet, after, 100)
	if err != nil {
		t.Fatal(err)
	}
	var ids []string
	for _, vm := range expired {
		ids = append(ids, vm.ID)
		if !UnclaimedExpired(vm, after) {
			t.Fatalf("queued row %s is not expired in Go: %+v", vm.ID, vm)
		}
	}
	if len(ids) != 2 || ids[0] != soon.ID || ids[1] != later.ID {
		t.Fatalf("queue %v, want [%s %s] in deadline order", ids, soon.ID, later.ID)
	}
	for _, vm := range []RunnerVM{fresh, taken} {
		for _, got := range ids {
			if got == vm.ID {
				t.Fatalf("%s should not be in the reaper queue", vm.ID)
			}
		}
	}
	none, err := s.ListExpiredUnclaimedRunnerVMs(ctx, scaleSet, now.Add(-time.Hour), 100)
	if err != nil || len(none) != 0 {
		t.Fatalf("queue before any deadline: %d rows %v", len(none), err)
	}
	if _, err := s.ListExpiredUnclaimedRunnerVMs(ctx, scaleSet, time.Time{}, 100); !errors.Is(err, ErrInvalid) {
		t.Fatalf("zero clock accepted: %v", err)
	}
	if _, err := s.ListExpiredUnclaimedRunnerVMs(ctx, scaleSet, after, 0); !errors.Is(err, ErrInvalid) {
		t.Fatalf("zero limit accepted: %v", err)
	}
}
