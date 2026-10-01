//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheDoorsAReleasedVMKeepsShut pins what a reservation
// answers once its destroy has resolved and it is released
// (runner_vms.go:836, :1073). Release is terminal: a late claim from the
// forge is refused as a conflict naming the release rather than stamping
// a claim on a VM that no longer exists, a late drain is answered
// unchanged, and no further Proxmox operation can be opened on it.
func TestIntegrationTheDoorsAReleasedVMKeepsShut(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	const runnerID = 7741
	vm := drainingRunnerVM(t, ctx, s, runnerID)
	resolve := func(t *testing.T, generation int64, kind, upid string, proof RunnerVMSafetyEvidence) RunnerVM {
		t.Helper()
		op, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, generation, kind, proof)
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
	stopped := resolve(t, vm.Generation, "stop", "UPID:pve:041:stop", RunnerVMSafetyEvidence{
		EvidenceID: mustID(t), ObservedAt: time.Now().UTC(), Drained: true, NoActiveJob: true})
	released := resolve(t, stopped.Generation, "destroy", "UPID:pve:042:destroy", RunnerVMSafetyEvidence{
		EvidenceID: mustID(t), ObservedAt: time.Now().UTC(), Drained: true, NoActiveJob: true,
		RunnerDeregistered: true, ExpectedConfigDigest: strings.Repeat("c", 40), ApprovalID: mustID(t),
		ExternalRunnerID: runnerID})
	if released.State != "released" || released.EndedAt == nil || released.ClaimedAt != nil || released.UnknownOutcome {
		t.Fatalf("destroy did not release the VM: %+v", released)
	}
	claims := func(t *testing.T, vmID string) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM audit WHERE action='runner_vm.claimed'
			AND target_type='runner_vm' AND target_id=$1`, vmID).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	unchanged := func(t *testing.T) {
		t.Helper()
		after, err := s.GetRunnerVM(ctx, vm.ID)
		if err != nil {
			t.Fatal(err)
		}
		if after.State != "released" || after.Generation != released.Generation || after.ClaimedAt != nil ||
			after.EndedAt == nil || !after.EndedAt.Equal(*released.EndedAt) || after.CurrentOperationID != "" {
			t.Fatalf("a released VM moved: %+v", after)
		}
	}

	t.Run("a late claim is refused naming the release", func(t *testing.T) {
		claimed, err := s.MarkRunnerVMClaimed(ctx, "forge", vm.ID)
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "reservation already released") {
			t.Fatalf("late claim: %+v %v", claimed, err)
		}
		if claimed.ClaimedAt != nil || claims(t, vm.ID) != 0 {
			t.Fatalf("a refused claim was kept: %+v", claimed)
		}
		unchanged(t)
	})

	t.Run("a late drain is answered unchanged", func(t *testing.T) {
		drained, err := s.MarkRunnerVMDraining(ctx, "scaler", vm.ID, released.Generation)
		if err != nil || drained.State != "released" || drained.Generation != released.Generation {
			t.Fatalf("late drain: %+v %v", drained, err)
		}
		unchanged(t)
	})

	t.Run("no operation opens on a released VM", func(t *testing.T) {
		for _, kind := range []string{"clone", "start", "stop", "destroy"} {
			op, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, released.Generation, kind, RunnerVMSafetyEvidence{
				EvidenceID: mustID(t), ObservedAt: time.Now().UTC(), Drained: true, NoActiveJob: true,
				RunnerDeregistered: true, ExpectedConfigDigest: strings.Repeat("c", 40), ApprovalID: mustID(t),
				ExternalRunnerID: runnerID})
			if op.ID != "" || (!errors.Is(err, ErrConflict) && !errors.Is(err, ErrDenied)) {
				t.Fatalf("%s on a released VM: %+v %v", kind, op, err)
			}
		}
		unchanged(t)
	})

	t.Run("an unreleased reservation still takes its claim", func(t *testing.T) {
		live, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
		if err != nil {
			t.Fatal(err)
		}
		claimed, err := s.MarkRunnerVMClaimed(ctx, "forge", live.ID)
		if err != nil || claimed.ClaimedAt == nil || claims(t, live.ID) != 1 {
			t.Fatalf("control claim: %+v %v", claimed, err)
		}
	})
}
