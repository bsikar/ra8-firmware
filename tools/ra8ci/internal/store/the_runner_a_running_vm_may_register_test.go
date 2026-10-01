//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheRunnerARunningVMMayRegister pins MarkRunnerVMRegistered
// and the transitionRunnerVM it rides on (runner_vms.go:929-987). A runner
// identity binds only to a VM that is running, settled and at the expected
// generation. Binding moves the VM to registered at the next generation with
// exactly one audit row. A runner identity already bound to another live VM
// in the same scale set is refused as a conflict, never reported as an
// outage; the same number in another scale set is a different runner.
func TestIntegrationTheRunnerARunningVMMayRegister(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	const firstRunner, secondRunner = 7761, 7762
	settle := func(t *testing.T, vm RunnerVM, kind, upid string) RunnerVM {
		t.Helper()
		op, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, kind, RunnerVMSafetyEvidence{})
		if err != nil {
			t.Fatalf("%s: %v", kind, err)
		}
		if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, op.Generation, op.ID, upid); err != nil {
			t.Fatalf("%s UPID: %v", kind, err)
		}
		next, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, vmResolution(t))
		if err != nil {
			t.Fatalf("%s resolve: %v", kind, err)
		}
		return next
	}
	reserved := func(t *testing.T) RunnerVM {
		t.Helper()
		vm, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
		if err != nil {
			t.Fatal(err)
		}
		return vm
	}
	registrations := func(t *testing.T, vmID string) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM audit WHERE action='runner_vm.registered'
			AND target_type='runner_vm' AND target_id=$1`, vmID).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}

	t.Run("an incomplete registration is refused before the ledger is read", func(t *testing.T) {
		vm := reserved(t)
		for name, c := range map[string]struct {
			actor, id string
			gen, rid  int64
			rname     string
		}{
			"no actor":             {"", vm.ID, vm.Generation, firstRunner, "ra8ci-runner"},
			"an oversized actor":   {strings.Repeat("a", 257), vm.ID, vm.Generation, firstRunner, "ra8ci-runner"},
			"a malformed id":       {"scaler", "nope", vm.Generation, firstRunner, "ra8ci-runner"},
			"generation zero":      {"scaler", vm.ID, 0, firstRunner, "ra8ci-runner"},
			"no runner id":         {"scaler", vm.ID, vm.Generation, 0, "ra8ci-runner"},
			"a negative runner id": {"scaler", vm.ID, vm.Generation, -1, "ra8ci-runner"},
			"no runner name":       {"scaler", vm.ID, vm.Generation, firstRunner, ""},
			"an oversized name":    {"scaler", vm.ID, vm.Generation, firstRunner, strings.Repeat("r", 257)},
		} {
			if _, err := s.MarkRunnerVMRegistered(ctx, c.actor, c.id, c.gen, c.rid, c.rname); !errors.Is(err, ErrInvalid) {
				t.Fatalf("%s: want ErrInvalid, got %v", name, err)
			}
		}
		if _, err := s.MarkRunnerVMRegistered(ctx, "scaler", mustID(t), 1, firstRunner, "ra8ci-runner"); !errors.Is(err, ErrNotFound) {
			t.Fatalf("unknown reservation: %v", err)
		}
	})

	t.Run("a VM that is not running and settled takes no runner", func(t *testing.T) {
		vm := reserved(t)
		if _, err := s.MarkRunnerVMRegistered(ctx, "scaler", vm.ID, vm.Generation, firstRunner, "ra8ci-runner"); !errors.Is(err, ErrConflict) {
			t.Fatalf("a reserved VM: %v", err)
		}
		stopped := settle(t, vm, "clone", "UPID:pve:081:clone")
		if _, err := s.MarkRunnerVMRegistered(ctx, "scaler", vm.ID, stopped.Generation, firstRunner, "ra8ci-runner"); !errors.Is(err, ErrConflict) {
			t.Fatalf("a stopped VM: %v", err)
		}
		start, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, stopped.Generation, "start", RunnerVMSafetyEvidence{})
		if err != nil {
			t.Fatal(err)
		}
		if _, err := s.MarkRunnerVMRegistered(ctx, "scaler", vm.ID, start.Generation, firstRunner, "ra8ci-runner"); !errors.Is(err, ErrConflict) {
			t.Fatalf("a VM mid-start: %v", err)
		}
		got, err := s.GetRunnerVM(ctx, vm.ID)
		if err != nil {
			t.Fatal(err)
		}
		if got.ExternalRunnerID != 0 || got.State != "starting" || registrations(t, vm.ID) != 0 {
			t.Fatalf("a refused registration bound %+v", got)
		}
	})

	t.Run("a running VM takes its runner once and a reused identity is a conflict", func(t *testing.T) {
		vm := reserved(t)
		running := settle(t, settle(t, vm, "clone", "UPID:pve:082:clone"), "start", "UPID:pve:083:start")
		for _, gen := range []int64{running.Generation - 1, running.Generation + 1} {
			if _, err := s.MarkRunnerVMRegistered(ctx, "scaler", vm.ID, gen, firstRunner, "ra8ci-runner-a"); !errors.Is(err, ErrConflict) {
				t.Fatalf("generation %d: %v", gen, err)
			}
		}
		registered, err := s.MarkRunnerVMRegistered(ctx, "scaler", vm.ID, running.Generation, firstRunner, "ra8ci-runner-a")
		if err != nil {
			t.Fatal(err)
		}
		if registered.State != "registered" || registered.Generation != running.Generation+1 ||
			registered.ExternalRunnerID != firstRunner || registered.ExternalRunnerName != "ra8ci-runner-a" || registered.CleanupRequested {
			t.Fatalf("registration left %+v", registered)
		}
		if _, err := s.MarkRunnerVMRegistered(ctx, "scaler", vm.ID, running.Generation, firstRunner, "ra8ci-runner-a"); !errors.Is(err, ErrConflict) {
			t.Fatalf("a replayed registration: %v", err)
		}
		if n := registrations(t, vm.ID); n != 1 {
			t.Fatalf("registration audited %d times", n)
		}

		sameSet := runnerVMTestInput(t)
		sameSet.ScaleSetID = vm.ScaleSetID
		other, _, err := s.ReserveRunnerVM(ctx, "scaler", sameSet, testUnclaimedDeadline())
		if err != nil {
			t.Fatal(err)
		}
		otherRunning := settle(t, settle(t, other, "clone", "UPID:pve:084:clone"), "start", "UPID:pve:085:start")
		if _, err := s.MarkRunnerVMRegistered(ctx, "scaler", other.ID, otherRunning.Generation, firstRunner, "ra8ci-runner-a"); !errors.Is(err, ErrConflict) {
			t.Fatalf("a runner identity bound twice in one scale set: %v", err)
		}
		if got, err := s.GetRunnerVM(ctx, other.ID); err != nil || got.State != "running" || got.ExternalRunnerID != 0 {
			t.Fatalf("a refused duplicate moved the VM: %+v %v", got, err)
		}
		if n := registrations(t, other.ID); n != 0 {
			t.Fatalf("a refused duplicate audited %d times", n)
		}
		taken, err := s.MarkRunnerVMRegistered(ctx, "scaler", other.ID, otherRunning.Generation, secondRunner, "ra8ci-runner-b")
		if err != nil || taken.ExternalRunnerID != secondRunner || taken.State != "registered" {
			t.Fatalf("a fresh identity on the same VM: %+v %v", taken, err)
		}

		// Runner IDs are scoped to their scale set (runner_vms_one_active_runner_idx
		// is on scale_set_id, external_runner_id), so the same number in
		// another set is a different runner and binds.
		otherSet := runnerVMTestInput(t)
		otherSet.ScaleSetID = vm.ScaleSetID + 1
		elsewhere, _, err := s.ReserveRunnerVM(ctx, "scaler", otherSet, testUnclaimedDeadline())
		if err != nil {
			t.Fatal(err)
		}
		elsewhereRunning := settle(t, settle(t, elsewhere, "clone", "UPID:pve:086:clone"), "start", "UPID:pve:087:start")
		bound, err := s.MarkRunnerVMRegistered(ctx, "scaler", elsewhere.ID, elsewhereRunning.Generation, firstRunner, "ra8ci-runner-a")
		if err != nil || bound.ExternalRunnerID != firstRunner {
			t.Fatalf("the same runner number in another scale set: %+v %v", bound, err)
		}
	})
}
