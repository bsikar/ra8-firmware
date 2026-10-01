//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationThePlanAVMOperationBinds pins RecordRunnerVMTerraformPlan
// past its argument guard (runner_vms.go:542-575) and the provider split it
// creates with RecordRunnerVMUPID (:699). A saved plan binds to the one
// unresolved operation at its fenced generation, only while that operation
// is still a bare Proxmox intent with no UPID, and only from a plan freshly
// prepared by the database's clock. Once bound, the same plan is answered
// as done, a different plan is refused, and a Proxmox UPID is refused for
// what is now a Terraform operation; a UPID already recorded closes the
// door to a plan the same way.
func TestIntegrationThePlanAVMOperationBinds(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	inFlight := func(t *testing.T) (RunnerVM, RunnerVMOperation) {
		t.Helper()
		vm, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
		if err != nil {
			t.Fatal(err)
		}
		op, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "clone", RunnerVMSafetyEvidence{})
		if err != nil {
			t.Fatal(err)
		}
		return vm, op
	}
	plan := func() RunnerVMTerraformPlanEvidence {
		return RunnerVMTerraformPlanEvidence{TerraformVersion: "1.10.5", PlanSHA256: strings.Repeat("a", 64),
			ModuleSHA256: strings.Repeat("b", 64), InputSHA256: strings.Repeat("c", 64),
			ProviderLockSHA256: strings.Repeat("d", 64), StateIdentitySHA256: strings.Repeat("e", 64),
			PreparedAt: time.Now().UTC()}
	}
	bound := func(t *testing.T, vmID, opID string) (string, string, int) {
		t.Helper()
		var provider string
		var planSHA *string
		if err := pool.QueryRow(ctx, "SELECT provider_kind,plan_sha256 FROM runner_vm_operations WHERE id=$1",
			opID).Scan(&provider, &planSHA); err != nil {
			t.Fatal(err)
		}
		var audits int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM audit WHERE action='runner_vm.terraform.plan_prepared'
			AND target_type='runner_vm' AND target_id=$1`, vmID).Scan(&audits); err != nil {
			t.Fatal(err)
		}
		if planSHA == nil {
			return provider, "", audits
		}
		return provider, *planSHA, audits
	}
	vm, op := inFlight(t)

	t.Run("a plan aimed anywhere but the current operation is refused", func(t *testing.T) {
		cases := []struct {
			name        string
			reservation string
			generation  int64
			operation   string
			want        error
		}{
			{"an unknown reservation", mustID(t), op.Generation, op.ID, ErrNotFound},
			{"a generation ahead of the fence", vm.ID, op.Generation + 1, op.ID, ErrConflict},
			{"an operation that is not current", vm.ID, op.Generation, mustID(t), ErrConflict},
		}
		for _, c := range cases {
			if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", c.reservation, c.generation, c.operation, plan()); !errors.Is(err, c.want) {
				t.Fatalf("%s: want %v, got %v", c.name, c.want, err)
			}
		}
		if provider, sha, audits := bound(t, vm.ID, op.ID); provider != "proxmox" || sha != "" || audits != 0 {
			t.Fatalf("a refused plan was bound: %s %q %d", provider, sha, audits)
		}
	})

	t.Run("a plan not freshly prepared is refused", func(t *testing.T) {
		stamps := map[string]time.Time{
			"no preparation time":        {},
			"prepared a minute ago":      time.Now().UTC().Add(-time.Minute),
			"prepared a minute from now": time.Now().UTC().Add(time.Minute),
		}
		for name, at := range stamps {
			evidence := plan()
			evidence.PreparedAt = at
			if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", vm.ID, op.Generation, op.ID, evidence); !errors.Is(err, ErrDenied) {
				t.Fatalf("%s: %v", name, err)
			}
		}
		if provider, sha, audits := bound(t, vm.ID, op.ID); provider != "proxmox" || sha != "" || audits != 0 {
			t.Fatalf("a stale plan was bound: %s %q %d", provider, sha, audits)
		}
	})

	t.Run("an honest plan binds once and its replay is answered as done", func(t *testing.T) {
		for attempt := 1; attempt <= 2; attempt++ {
			if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", vm.ID, op.Generation, op.ID, plan()); err != nil {
				t.Fatalf("attempt %d: %v", attempt, err)
			}
		}
		if provider, sha, audits := bound(t, vm.ID, op.ID); provider != "terraform" || sha != strings.Repeat("a", 64) || audits != 1 {
			t.Fatalf("plan not bound exactly once: %s %q %d", provider, sha, audits)
		}
	})

	t.Run("a different plan for the bound operation is refused", func(t *testing.T) {
		evidence := plan()
		evidence.InputSHA256 = strings.Repeat("f", 64)
		err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", vm.ID, op.Generation, op.ID, evidence)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "Terraform plan evidence changed for operation") {
			t.Fatalf("changed plan: %v", err)
		}
		if _, sha, audits := bound(t, vm.ID, op.ID); sha != strings.Repeat("a", 64) || audits != 1 {
			t.Fatalf("changed plan overwrote the bound one: %q %d", sha, audits)
		}
	})

	t.Run("a Terraform operation takes no Proxmox UPID", func(t *testing.T) {
		err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, op.Generation, op.ID, "UPID:pve:051:clone")
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "UPID is only valid for Proxmox operations") {
			t.Fatalf("UPID on a Terraform operation: %v", err)
		}
		var upid *string
		if err := pool.QueryRow(ctx, "SELECT upid FROM runner_vm_operations WHERE id=$1", op.ID).Scan(&upid); err != nil {
			t.Fatal(err)
		}
		if upid != nil {
			t.Fatalf("a UPID was kept on a Terraform operation: %q", *upid)
		}
	})

	t.Run("a Proxmox operation with a UPID takes no plan", func(t *testing.T) {
		other, otherOp := inFlight(t)
		if err := s.RecordRunnerVMUPID(ctx, "scaler", other.ID, otherOp.Generation, otherOp.ID, "UPID:pve:052:clone"); err != nil {
			t.Fatal(err)
		}
		if err := s.RecordRunnerVMTerraformPlan(ctx, "scaler", other.ID, otherOp.Generation, otherOp.ID, plan()); !errors.Is(err, ErrConflict) {
			t.Fatalf("plan after UPID: %v", err)
		}
		if provider, sha, audits := bound(t, other.ID, otherOp.ID); provider != "proxmox" || sha != "" || audits != 0 {
			t.Fatalf("a plan was bound over a UPID: %s %q %d", provider, sha, audits)
		}
	})
}
