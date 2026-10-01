//go:build integration

package store

import (
	"context"
	"errors"
	"testing"
	"time"
)

// TestIntegrationTheFenceADrainAndClaimKeep pins MarkRunnerVMDraining and
// MarkRunnerVMClaimed past their argument guards (runner_vms.go:988-1090).
// Draining is the one-way switch that forbids another start: it is fenced
// on the generation the first time and answered unchanged once set, and a
// VM with an operation in flight keeps its state, generation and operation
// so the in-flight CAS still lands. Claiming is deliberately unfenced and
// idempotent: a redelivered claim must not move the timestamp or write a
// second audit row.
func TestIntegrationTheFenceADrainAndClaimKeep(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	audits := func(t *testing.T, action, vmID string) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM audit WHERE action=$1
			AND target_type='runner_vm' AND target_id=$2`, action, vmID).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	reserve := func(t *testing.T) RunnerVM {
		t.Helper()
		vm, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
		if err != nil {
			t.Fatal(err)
		}
		return vm
	}

	t.Run("a drain aimed at no reservation or a stale generation is refused", func(t *testing.T) {
		vm := reserve(t)
		if _, err := s.MarkRunnerVMDraining(ctx, "scaler", mustID(t), vm.Generation); !errors.Is(err, ErrNotFound) {
			t.Fatalf("unknown reservation: %v", err)
		}
		for _, generation := range []int64{vm.Generation - 1, vm.Generation + 1} {
			if generation < 1 {
				continue
			}
			if _, err := s.MarkRunnerVMDraining(ctx, "scaler", vm.ID, generation); !errors.Is(err, ErrConflict) {
				t.Fatalf("generation %d: %v", generation, err)
			}
		}
		after, err := s.GetRunnerVM(ctx, vm.ID)
		if err != nil {
			t.Fatal(err)
		}
		if after.CleanupRequested || after.Generation != vm.Generation || after.State != "reserved" ||
			audits(t, "runner_vm.draining", vm.ID) != 0 {
			t.Fatalf("a refused drain moved the VM: %+v", after)
		}
	})

	t.Run("a drain is fenced once and answered unchanged after", func(t *testing.T) {
		vm := reserve(t)
		drained, err := s.MarkRunnerVMDraining(ctx, "scaler", vm.ID, vm.Generation)
		if err != nil {
			t.Fatal(err)
		}
		// A reservation the job left before clone keeps its safe state.
		if !drained.CleanupRequested || drained.State != "reserved" || drained.Generation != vm.Generation+1 {
			t.Fatalf("first drain: %+v", drained)
		}
		for _, generation := range []int64{vm.Generation, drained.Generation, drained.Generation + 5} {
			again, err := s.MarkRunnerVMDraining(ctx, "scaler", vm.ID, generation)
			if err != nil {
				t.Fatalf("replay at generation %d: %v", generation, err)
			}
			if again.Generation != drained.Generation || again.State != drained.State || !again.CleanupRequested {
				t.Fatalf("replay at generation %d moved the VM: %+v", generation, again)
			}
		}
		if n := audits(t, "runner_vm.draining", vm.ID); n != 1 {
			t.Fatalf("drain audited %d times", n)
		}
	})

	t.Run("a drain over an operation in flight leaves its fence alone", func(t *testing.T) {
		vm := reserve(t)
		clone, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "clone", RunnerVMSafetyEvidence{})
		if err != nil {
			t.Fatal(err)
		}
		drained, err := s.MarkRunnerVMDraining(ctx, "scaler", vm.ID, clone.Generation)
		if err != nil {
			t.Fatal(err)
		}
		if !drained.CleanupRequested || drained.State != "cloning" || drained.Generation != clone.Generation ||
			!drained.UnknownOutcome || drained.CurrentOperationID != clone.ID {
			t.Fatalf("drain over an in-flight clone: %+v", drained)
		}
		// The in-flight operation's own fence still holds, so its UPID and
		// its outcome both land.
		if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, clone.Generation, clone.ID, "UPID:pve:031:clone"); err != nil {
			t.Fatalf("UPID after drain: %v", err)
		}
		resolved, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, clone.Generation, clone.ID, vmResolution(t))
		if err != nil || resolved.UnknownOutcome || !resolved.CleanupRequested {
			t.Fatalf("resolve after drain: %+v %v", resolved, err)
		}
	})

	t.Run("a claim is kept once and its redelivery answered unchanged", func(t *testing.T) {
		vm := reserve(t)
		if _, err := s.MarkRunnerVMClaimed(ctx, "forge", mustID(t)); !errors.Is(err, ErrNotFound) {
			t.Fatalf("unknown reservation: %v", err)
		}
		first, err := s.MarkRunnerVMClaimed(ctx, "forge", vm.ID)
		if err != nil || first.ClaimedAt == nil {
			t.Fatalf("first claim: %+v %v", first, err)
		}
		// Claiming is not a Proxmox mutation and must not bump the fence.
		if first.Generation != vm.Generation || first.State != vm.State {
			t.Fatalf("claim moved the fence: %+v", first)
		}
		second, err := s.MarkRunnerVMClaimed(ctx, "forge", vm.ID)
		if err != nil || second.ClaimedAt == nil || !second.ClaimedAt.Equal(*first.ClaimedAt) {
			t.Fatalf("redelivered claim: %+v %v", second, err)
		}
		if n := audits(t, "runner_vm.claimed", vm.ID); n != 1 {
			t.Fatalf("claim audited %d times", n)
		}
	})
}
