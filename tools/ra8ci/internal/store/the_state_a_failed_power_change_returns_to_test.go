//go:build integration

package store

import (
	"context"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheStateAFailedPowerChangeReturnsTo pins the failed outcome
// of every non-clone kind end to end against the real schema
// (runner_vms.go:884 with runnerVMOperationStates :357). A proven failure
// returns the VM to the state the operation left (start and destroy to
// stopped, stop to draining) at a fresh generation, with no current
// operation. A failed destroy does not release the VM or stamp ended_at.
// In each case the same kind can be begun again and then succeed, so a
// failure is a setback the lifecycle can retry, not a dead end.
func TestIntegrationTheStateAFailedPowerChangeReturnsTo(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	const runnerID = 7751
	attempt := func(t *testing.T, vm RunnerVM, kind, upid, outcome string, proof RunnerVMSafetyEvidence) RunnerVM {
		t.Helper()
		op, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, kind, proof)
		if err != nil {
			t.Fatalf("%s: %v", kind, err)
		}
		if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, op.Generation, op.ID, upid); err != nil {
			t.Fatalf("%s UPID: %v", kind, err)
		}
		resolution := vmResolution(t)
		resolution.Outcome = outcome
		resolution.ObservedAt = time.Now().UTC()
		next, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, op.Generation, op.ID, resolution)
		if err != nil {
			t.Fatalf("%s %s: %v", kind, outcome, err)
		}
		var status string
		if err := pool.QueryRow(ctx, "SELECT status FROM runner_vm_operations WHERE id=$1", op.ID).Scan(&status); err != nil {
			t.Fatal(err)
		}
		if status != outcome || next.Generation != op.Generation+1 || next.UnknownOutcome || next.CurrentOperationID != "" {
			t.Fatalf("%s %s left op %s and VM %+v", kind, outcome, status, next)
		}
		return next
	}
	outcomeAudits := func(t *testing.T, vmID, action string) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM audit WHERE action=$1
			AND target_type='runner_vm' AND target_id=$2`, action, vmID).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	stopProof := func(t *testing.T) RunnerVMSafetyEvidence {
		return RunnerVMSafetyEvidence{EvidenceID: mustID(t), ObservedAt: time.Now().UTC(), Drained: true, NoActiveJob: true}
	}
	destroyProof := func(t *testing.T) RunnerVMSafetyEvidence {
		return RunnerVMSafetyEvidence{EvidenceID: mustID(t), ObservedAt: time.Now().UTC(), Drained: true, NoActiveJob: true,
			RunnerDeregistered: true, ExpectedConfigDigest: strings.Repeat("c", 40), ApprovalID: mustID(t),
			ExternalRunnerID: runnerID}
	}

	t.Run("a failed start returns the VM to stopped and a retry can start it", func(t *testing.T) {
		vm, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
		if err != nil {
			t.Fatal(err)
		}
		stopped := attempt(t, vm, "clone", "UPID:pve:071:clone", "succeeded", RunnerVMSafetyEvidence{})
		failed := attempt(t, stopped, "start", "UPID:pve:072:start", "failed", RunnerVMSafetyEvidence{})
		if failed.State != "stopped" || failed.EndedAt != nil {
			t.Fatalf("failed start left %s", failed.State)
		}
		if n := outcomeAudits(t, vm.ID, "runner_vm.start.failed"); n != 1 {
			t.Fatalf("failed start audited %d times", n)
		}
		running := attempt(t, failed, "start", "UPID:pve:073:start", "succeeded", RunnerVMSafetyEvidence{})
		if running.State != "running" {
			t.Fatalf("retried start left %s", running.State)
		}
	})

	t.Run("a failed stop keeps draining and a failed destroy keeps the VM unreleased", func(t *testing.T) {
		draining := drainingRunnerVM(t, ctx, s, runnerID)
		failedStop := attempt(t, draining, "stop", "UPID:pve:074:stop", "failed", stopProof(t))
		if failedStop.State != "draining" || !failedStop.CleanupRequested {
			t.Fatalf("failed stop left %s cleanup=%v", failedStop.State, failedStop.CleanupRequested)
		}
		stopped := attempt(t, failedStop, "stop", "UPID:pve:075:stop", "succeeded", stopProof(t))
		if stopped.State != "stopped" {
			t.Fatalf("retried stop left %s", stopped.State)
		}
		failedDestroy := attempt(t, stopped, "destroy", "UPID:pve:076:destroy", "failed", destroyProof(t))
		if failedDestroy.State != "stopped" || failedDestroy.EndedAt != nil {
			t.Fatalf("failed destroy released the VM: %s ended=%v", failedDestroy.State, failedDestroy.EndedAt)
		}
		for action, want := range map[string]int{"runner_vm.stop.failed": 1, "runner_vm.destroy.failed": 1,
			"runner_vm.stop.succeeded": 1, "runner_vm.destroy.succeeded": 0} {
			if n := outcomeAudits(t, draining.ID, action); n != want {
				t.Fatalf("%s audited %d times, want %d", action, n, want)
			}
		}
		released := attempt(t, failedDestroy, "destroy", "UPID:pve:077:destroy", "succeeded", destroyProof(t))
		if released.State != "released" || released.EndedAt == nil {
			t.Fatalf("retried destroy left %s ended=%v", released.State, released.EndedAt)
		}
	})
}
