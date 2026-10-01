//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheFailureAPreflightMayProve pins ResolveRunnerVMOperation's
// fence (runner_vms.go:855-870) and its failed-outcome arm (:884), reached
// through the terraform_preflight source (:760). A preflight may only prove
// that nothing happened: a failed outcome for an operation that never reached
// Proxmox (no UPID) and never started an apply. It returns the VM to the state
// it left, past a fresh generation, with exactly one audit row. Everything
// else it is offered is refused and leaves the operation unresolved.
func TestIntegrationTheFailureAPreflightMayProve(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	planSHA := strings.Repeat("a", 64)
	stateSHA := strings.Repeat("e", 64)
	preflightPending := func(t *testing.T, bind bool) (RunnerVM, RunnerVMOperation) {
		t.Helper()
		vm, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
		if err != nil {
			t.Fatal(err)
		}
		op, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "clone", RunnerVMSafetyEvidence{})
		if err != nil {
			t.Fatal(err)
		}
		if bind {
			if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", vm.ID, op.Generation, op.ID, RunnerVMTerraformPlanEvidence{
				TerraformVersion: "1.10.5", PlanSHA256: planSHA, ModuleSHA256: strings.Repeat("b", 64),
				InputSHA256: strings.Repeat("c", 64), ProviderLockSHA256: strings.Repeat("d", 64),
				StateIdentitySHA256: stateSHA, PreparedAt: time.Now().UTC()}); err != nil {
				t.Fatal(err)
			}
		}
		return vm, op
	}
	preflightProof := func(t *testing.T) RunnerVMResolution {
		t.Helper()
		proof := vmResolution(t)
		proof.Outcome = "failed"
		proof.Source = "terraform_preflight"
		proof.ObservedAt = time.Now().UTC()
		return proof
	}
	outcomeOf := func(t *testing.T, vmID, opID, action string) (string, int) {
		t.Helper()
		var status string
		if err := pool.QueryRow(ctx, "SELECT status FROM runner_vm_operations WHERE id=$1", opID).Scan(&status); err != nil {
			t.Fatal(err)
		}
		var audits int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM audit WHERE action=$1
			AND target_type='runner_vm' AND target_id=$2`, action, vmID).Scan(&audits); err != nil {
			t.Fatal(err)
		}
		return status, audits
	}

	t.Run("a resolution aimed past the fence is refused", func(t *testing.T) {
		vm, op := preflightPending(t, false)
		if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", mustID(t), op.Generation, op.ID, preflightProof(t)); !errors.Is(err, ErrNotFound) {
			t.Fatalf("unknown reservation: %v", err)
		}
		for name, c := range map[string]struct {
			generation int64
			operation  string
		}{
			"a generation behind": {op.Generation - 1, op.ID},
			"a generation ahead":  {op.Generation + 1, op.ID},
			"another operation":   {op.Generation, mustID(t)},
		} {
			if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, c.generation, c.operation, preflightProof(t)); !errors.Is(err, ErrConflict) {
				t.Fatalf("%s: want ErrConflict, got %v", name, err)
			}
		}
		if status, audits := outcomeOf(t, vm.ID, op.ID, "runner_vm.clone.failed"); status != "unresolved" || audits != 0 {
			t.Fatalf("a fenced-off resolution landed: %s %d", status, audits)
		}
	})

	t.Run("a preflight is refused anything but proof that nothing happened", func(t *testing.T) {
		vm, op := preflightPending(t, false)
		refusals := map[string]func(*RunnerVMResolution){
			"a claimed success":        func(p *RunnerVMResolution) { p.Outcome = "succeeded" },
			"a plan on a Proxmox op":   func(p *RunnerVMResolution) { p.PlanSHA256 = planSHA },
			"a state identity":         func(p *RunnerVMResolution) { p.StateIdentitySHA256 = stateSHA },
			"a reconciliation digest":  func(p *RunnerVMResolution) { p.ReconciliationSHA256 = strings.Repeat("f", 64) },
			"an unverified post-state": func(p *RunnerVMResolution) { p.PostStateVerified = false },
			"an observation 60s stale": func(p *RunnerVMResolution) { p.ObservedAt = time.Now().Add(-time.Minute) },
			"an observation 60s ahead": func(p *RunnerVMResolution) { p.ObservedAt = time.Now().Add(time.Minute) },
			"an unknown source":        func(p *RunnerVMResolution) { p.Source = "hunch" },
			"an invalid evidence id":   func(p *RunnerVMResolution) { p.EvidenceID = "not-an-id" },
		}
		for name, spoil := range refusals {
			proof := preflightProof(t)
			spoil(&proof)
			if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, proof); !errors.Is(err, ErrDenied) {
				t.Fatalf("%s: want ErrDenied, got %v", name, err)
			}
		}
		if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, op.Generation, op.ID, "UPID:pve:061:clone"); err != nil {
			t.Fatal(err)
		}
		if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, preflightProof(t)); !errors.Is(err, ErrDenied) {
			t.Fatalf("a preflight over a task Proxmox accepted: %v", err)
		}
		if status, audits := outcomeOf(t, vm.ID, op.ID, "runner_vm.clone.failed"); status != "unresolved" || audits != 0 {
			t.Fatalf("a refused preflight landed: %s %d", status, audits)
		}
	})

	t.Run("a terraform preflight must name the bound plan and never follow an apply", func(t *testing.T) {
		vm, op := preflightPending(t, true)
		for name, spoil := range map[string]func(*RunnerVMResolution){
			"no plan":                func(p *RunnerVMResolution) { p.StateIdentitySHA256 = stateSHA },
			"another plan":           func(p *RunnerVMResolution) { p.PlanSHA256, p.StateIdentitySHA256 = strings.Repeat("9", 64), stateSHA },
			"another state identity": func(p *RunnerVMResolution) { p.PlanSHA256, p.StateIdentitySHA256 = planSHA, strings.Repeat("9", 64) },
		} {
			proof := preflightProof(t)
			spoil(&proof)
			if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, proof); !errors.Is(err, ErrDenied) {
				t.Fatalf("%s: want ErrDenied, got %v", name, err)
			}
		}
		if start, err := s.BeginRunnerVMTerraformApply(ctx, "scaler", vm.ID, op.Generation, op.ID, planSHA); !start || err != nil {
			t.Fatalf("apply: %v %v", start, err)
		}
		proof := preflightProof(t)
		proof.PlanSHA256, proof.StateIdentitySHA256 = planSHA, stateSHA
		if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, proof); !errors.Is(err, ErrDenied) {
			t.Fatalf("a preflight after the apply began: %v", err)
		}
		if status, audits := outcomeOf(t, vm.ID, op.ID, "runner_vm.clone.failed"); status != "unresolved" || audits != 0 {
			t.Fatalf("a refused terraform preflight landed: %s %d", status, audits)
		}
	})

	t.Run("a proven failure returns the VM where it started, once", func(t *testing.T) {
		for _, bind := range []bool{false, true} {
			vm, op := preflightPending(t, bind)
			proof := preflightProof(t)
			if bind {
				proof.PlanSHA256, proof.StateIdentitySHA256 = planSHA, stateSHA
			}
			got, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, proof)
			if err != nil {
				t.Fatalf("terraform=%v: %v", bind, err)
			}
			if got.State != vm.State || got.Generation != op.Generation+1 || got.UnknownOutcome || got.CurrentOperationID != "" || got.EndedAt != nil {
				t.Fatalf("terraform=%v: failure moved the VM: %+v (was %s)", bind, got, vm.State)
			}
			if status, audits := outcomeOf(t, vm.ID, op.ID, "runner_vm.clone.failed"); status != "failed" || audits != 1 {
				t.Fatalf("terraform=%v: failure not recorded once: %s %d", bind, status, audits)
			}
			if _, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, preflightProof(t)); !errors.Is(err, ErrConflict) {
				t.Fatalf("terraform=%v: a second resolution: %v", bind, err)
			}
			if _, audits := outcomeOf(t, vm.ID, op.ID, "runner_vm.clone.failed"); audits != 1 {
				t.Fatalf("terraform=%v: a second resolution audited", bind)
			}
		}
	})
}
