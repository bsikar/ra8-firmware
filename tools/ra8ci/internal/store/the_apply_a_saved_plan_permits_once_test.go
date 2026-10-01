//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheApplyASavedPlanPermitsOnce pins BeginRunnerVMTerraformApply
// past its argument guard (runner_vms.go:617-659). The caller may run
// terraform apply only when this answers true, so it must answer true
// exactly once per operation: only for the current unresolved operation at
// its fenced generation, only once a plan is bound, and only for that exact
// plan. A replay answers false with no error, so a retrying caller learns
// the apply is already intended without being licensed to start a second.
func TestIntegrationTheApplyASavedPlanPermitsOnce(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	planSHA := strings.Repeat("a", 64)
	applyInFlight := func(t *testing.T, bind bool) (RunnerVM, RunnerVMOperation) {
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
				StateIdentitySHA256: strings.Repeat("e", 64), PreparedAt: time.Now().UTC()}); err != nil {
				t.Fatal(err)
			}
		}
		return vm, op
	}
	applied := func(t *testing.T, vmID, opID string) (*time.Time, int) {
		t.Helper()
		var started *time.Time
		if err := pool.QueryRow(ctx, "SELECT terraform_apply_started_at FROM runner_vm_operations WHERE id=$1",
			opID).Scan(&started); err != nil {
			t.Fatal(err)
		}
		var audits int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM audit WHERE action='runner_vm.terraform.apply_intended'
			AND target_type='runner_vm' AND target_id=$1`, vmID).Scan(&audits); err != nil {
			t.Fatal(err)
		}
		return started, audits
	}
	vm, op := applyInFlight(t, true)

	t.Run("an apply aimed anywhere but the bound plan is refused", func(t *testing.T) {
		cases := []struct {
			name        string
			reservation string
			generation  int64
			operation   string
			plan        string
			want        error
		}{
			{"an unknown reservation", mustID(t), op.Generation, op.ID, planSHA, ErrNotFound},
			{"a generation ahead of the fence", vm.ID, op.Generation + 1, op.ID, planSHA, ErrConflict},
			{"an operation that is not current", vm.ID, op.Generation, mustID(t), planSHA, ErrConflict},
			{"some other plan", vm.ID, op.Generation, op.ID, strings.Repeat("f", 64), ErrConflict},
		}
		for _, c := range cases {
			start, err := s.BeginRunnerVMTerraformApply(ctx, "scaler", c.reservation, c.generation, c.operation, c.plan)
			if start || !errors.Is(err, c.want) {
				t.Fatalf("%s: want %v, got start=%v err=%v", c.name, c.want, start, err)
			}
		}
		if started, audits := applied(t, vm.ID, op.ID); started != nil || audits != 0 {
			t.Fatalf("a refused apply was intended: %v %d", started, audits)
		}
	})

	t.Run("an operation with no bound plan permits no apply", func(t *testing.T) {
		bare, bareOp := applyInFlight(t, false)
		start, err := s.BeginRunnerVMTerraformApply(ctx, "scaler", bare.ID, bareOp.Generation, bareOp.ID, planSHA)
		if start || !errors.Is(err, ErrConflict) {
			t.Fatalf("apply without a plan: start=%v err=%v", start, err)
		}
		if started, audits := applied(t, bare.ID, bareOp.ID); started != nil || audits != 0 {
			t.Fatalf("an unplanned apply was intended: %v %d", started, audits)
		}
	})

	t.Run("the bound plan permits one apply and its replay is told it already began", func(t *testing.T) {
		start, err := s.BeginRunnerVMTerraformApply(ctx, "scaler", vm.ID, op.Generation, op.ID, planSHA)
		if !start || err != nil {
			t.Fatalf("first apply: start=%v err=%v", start, err)
		}
		first, audits := applied(t, vm.ID, op.ID)
		if first == nil || audits != 1 {
			t.Fatalf("apply intent not recorded once: %v %d", first, audits)
		}
		for attempt := 2; attempt <= 3; attempt++ {
			start, err := s.BeginRunnerVMTerraformApply(ctx, "scaler", vm.ID, op.Generation, op.ID, planSHA)
			if start || err != nil {
				t.Fatalf("replay %d licensed another apply: start=%v err=%v", attempt, start, err)
			}
		}
		again, audits := applied(t, vm.ID, op.ID)
		if again == nil || !again.Equal(*first) || audits != 1 {
			t.Fatalf("replay moved the apply intent: %v -> %v, audits %d", first, again, audits)
		}
	})
}
