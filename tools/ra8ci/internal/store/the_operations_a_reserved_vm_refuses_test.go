//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheOperationsAReservedVMRefuses pins the doors
// BeginRunnerVMOperation judges after it has locked the reservation and
// before it records any intent: a stale generation fence, a destroy nobody
// asked to clean up, and a kind the reservation's state does not admit.
// Each refusal names its reason and leaves the reservation exactly as it
// was, with no operation row, so a caller that retries after reconciling
// is never fenced by an intent it did not commit. The control clones the
// same reservation at its true generation.
func TestIntegrationTheOperationsAReservedVMRefuses(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	vm, created, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
	if err != nil || !created || vm.State != "reserved" {
		t.Fatalf("reservation fixture: %+v created=%v err=%v", vm, created, err)
	}
	operations := func(t *testing.T) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM runner_vm_operations WHERE runner_vm_id=$1", vm.ID).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	untouched := func(t *testing.T) {
		t.Helper()
		now, err := s.GetRunnerVM(ctx, vm.ID)
		if err != nil || now.State != "reserved" || now.Generation != vm.Generation || now.CurrentOperationID != "" {
			t.Fatalf("a refused operation moved the reservation: %+v %v", now, err)
		}
		if n := operations(t); n != 0 {
			t.Fatalf("a refused operation recorded %d intents", n)
		}
	}

	refusals := []struct {
		name       string
		generation int64
		kind       string
		want       error
		reason     string
	}{
		{"a generation ahead of the reservation", vm.Generation + 1, "clone", ErrConflict, "stale VM generation"},
		{"a destroy nobody asked to clean up", vm.Generation, "destroy", ErrDenied, "cleanup was not requested"},
		{"a stop on a reservation that is not draining", vm.Generation, "stop", ErrConflict, "cannot stop VM in reserved"},
		{"a start on a reservation that never stopped", vm.Generation, "start", ErrConflict, "cannot start VM in reserved"},
		{"a kind the plane does not know", vm.Generation, "reboot", ErrConflict, "cannot reboot VM in reserved"},
	}
	for _, refusal := range refusals {
		t.Run(refusal.name, func(t *testing.T) {
			operation, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, refusal.generation, refusal.kind, RunnerVMSafetyEvidence{})
			if !errors.Is(err, refusal.want) || err == nil || !strings.Contains(err.Error(), refusal.reason) {
				t.Fatalf("want %v naming %q, got %v", refusal.want, refusal.reason, err)
			}
			if operation.ID != "" {
				t.Fatalf("refused operation handed back an intent: %+v", operation)
			}
			untouched(t)
		})
	}

	t.Run("the same reservation clones at its true generation", func(t *testing.T) {
		operation, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "clone", RunnerVMSafetyEvidence{})
		if err != nil || operation.ID == "" || operation.Kind != "clone" || operation.FromState != "reserved" ||
			operation.PendingState != "cloning" || operation.PriorRequestIssued {
			t.Fatalf("clone at the true generation was not recorded: %+v %v", operation, err)
		}
		if n := operations(t); n != 1 {
			t.Fatalf("clone recorded %d intents, want 1", n)
		}
	})
}
