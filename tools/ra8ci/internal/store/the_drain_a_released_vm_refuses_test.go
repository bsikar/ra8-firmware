//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheDrainAReleasedVMRefuses pins how a drain meets a VM on
// its way to release. A destroy is refused until cleanup is requested
// (runner_vms.go:421), so every released VM carries the request, and a drain
// asked of it afterwards takes the idempotent return at :1007 rather than
// MarkRunnerVMDraining's default arm (:1020), which no settled state reaches.
// Before the request a stale generation is a conflict; after it the drain is
// idempotent whatever state and generation the VM has reached.
func TestIntegrationTheDrainAReleasedVMRefuses(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	settle := func(t *testing.T, vm RunnerVM, kind, upid string, proof RunnerVMSafetyEvidence) RunnerVM {
		t.Helper()
		op, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, kind, proof)
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
	destroyProof := func(t *testing.T) RunnerVMSafetyEvidence {
		return RunnerVMSafetyEvidence{EvidenceID: mustID(t), ObservedAt: time.Now().UTC(), Drained: true, NoActiveJob: true,
			RunnerDeregistered: true, ExpectedConfigDigest: strings.Repeat("c", 40), ApprovalID: mustID(t)}
	}
	stoppedVM := func(t *testing.T, upid string) RunnerVM {
		t.Helper()
		vm, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
		if err != nil {
			t.Fatal(err)
		}
		return settle(t, vm, "clone", upid, RunnerVMSafetyEvidence{})
	}
	drains := func(t *testing.T, vmID string) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM audit WHERE action='runner_vm.draining'
			AND target_type='runner_vm' AND target_id=$1`, vmID).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}

	t.Run("no VM is released without a cleanup request", func(t *testing.T) {
		stopped := stoppedVM(t, "UPID:pve:091:clone")
		if _, err := s.BeginRunnerVMOperation(ctx, "scaler", stopped.ID, stopped.Generation, "destroy", destroyProof(t)); !errors.Is(err, ErrDenied) ||
			!strings.Contains(err.Error(), "cleanup was not requested") {
			t.Fatalf("destroy without a cleanup request: %v", err)
		}
		got, err := s.GetRunnerVM(ctx, stopped.ID)
		if err != nil {
			t.Fatal(err)
		}
		if got.State != "stopped" || got.CleanupRequested || got.UnknownOutcome || got.Generation != stopped.Generation {
			t.Fatalf("a refused destroy moved the VM: %+v", got)
		}
		var intents int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM runner_vm_operations WHERE runner_vm_id=$1 AND kind='destroy'`,
			stopped.ID).Scan(&intents); err != nil || intents != 0 {
			t.Fatalf("a refused destroy left %d operations: %v", intents, err)
		}
	})

	t.Run("a stale generation is refused before cleanup is requested", func(t *testing.T) {
		stopped := stoppedVM(t, "UPID:pve:093:clone")
		for _, gen := range []int64{stopped.Generation - 1, stopped.Generation + 1} {
			if _, err := s.MarkRunnerVMDraining(ctx, "scaler", stopped.ID, gen); !errors.Is(err, ErrConflict) {
				t.Fatalf("generation %d: %v", gen, err)
			}
		}
		if n := drains(t, stopped.ID); n != 0 {
			t.Fatalf("a refused drain audited %d times", n)
		}
	})

	t.Run("once cleanup is requested a drain is idempotent through release", func(t *testing.T) {
		stopped := stoppedVM(t, "UPID:pve:094:clone")
		drained, err := s.MarkRunnerVMDraining(ctx, "scaler", stopped.ID, stopped.Generation)
		if err != nil {
			t.Fatal(err)
		}
		if drained.State != "stopped" || !drained.CleanupRequested || drained.Generation != stopped.Generation+1 {
			t.Fatalf("drain of a stopped VM left %+v", drained)
		}
		again, err := s.MarkRunnerVMDraining(ctx, "scaler", stopped.ID, stopped.Generation)
		if err != nil || again.Generation != drained.Generation || !again.CleanupRequested {
			t.Fatalf("a repeated drain at the old generation: %+v %v", again, err)
		}
		released := settle(t, drained, "destroy", "UPID:pve:095:destroy", destroyProof(t))
		if released.State != "released" || !released.CleanupRequested {
			t.Fatalf("destroy after drain left %+v", released)
		}
		late, err := s.MarkRunnerVMDraining(ctx, "scaler", released.ID, released.Generation)
		if err != nil || late.State != "released" || late.Generation != released.Generation {
			t.Fatalf("a drain after a requested cleanup released: %+v %v", late, err)
		}
		if n := drains(t, stopped.ID); n != 1 {
			t.Fatalf("drain audited %d times, want 1", n)
		}
	})
}
