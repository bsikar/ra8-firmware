//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheUPIDAnOperationKeeps pins RecordRunnerVMUPID's doors
// (runner_vms.go:680-706). A UPID is the only handle the plane keeps on a
// Proxmox task whose outcome it does not yet know, so it is written once,
// against the reservation's current unresolved operation at its fenced
// generation: a retry carrying the same UPID is answered as done without a
// second audit row, a different UPID for the same operation is refused,
// and so is any UPID once the operation is resolved. That last case is a
// conflict, not the database being unavailable: a resolved reservation
// holds NULL as its current operation, which the readers must compare
// rather than fail to scan.
func TestIntegrationTheUPIDAnOperationKeeps(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	vm, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
	if err != nil {
		t.Fatal(err)
	}
	clone, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "clone", RunnerVMSafetyEvidence{})
	if err != nil {
		t.Fatal(err)
	}
	const upid = "UPID:pve:021:clone"
	recorded := func(t *testing.T) (string, int) {
		t.Helper()
		var stored *string
		if err := pool.QueryRow(ctx, "SELECT upid FROM runner_vm_operations WHERE id=$1", clone.ID).Scan(&stored); err != nil {
			t.Fatal(err)
		}
		var audits int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM audit WHERE action='runner_vm.operation.upid_recorded'
			AND target_type='runner_vm' AND target_id=$1`, vm.ID).Scan(&audits); err != nil {
			t.Fatal(err)
		}
		if stored == nil {
			return "", audits
		}
		return *stored, audits
	}

	t.Run("a UPID aimed anywhere but the current operation is refused", func(t *testing.T) {
		cases := []struct {
			name        string
			reservation string
			generation  int64
			operation   string
			want        error
		}{
			{"an unknown reservation", mustID(t), clone.Generation, clone.ID, ErrNotFound},
			{"a generation behind the fence", vm.ID, clone.Generation - 1, clone.ID, ErrConflict},
			{"a generation ahead of the fence", vm.ID, clone.Generation + 1, clone.ID, ErrConflict},
			{"an operation that is not current", vm.ID, clone.Generation, mustID(t), ErrConflict},
		}
		for _, c := range cases {
			if err := s.RecordRunnerVMUPID(ctx, "scaler", c.reservation, c.generation, c.operation, upid); !errors.Is(err, c.want) {
				t.Fatalf("%s: want %v, got %v", c.name, c.want, err)
			}
		}
		if stored, audits := recorded(t); stored != "" || audits != 0 {
			t.Fatalf("a refused UPID was kept: upid=%q audits=%d", stored, audits)
		}
	})

	t.Run("the first UPID is kept and its retry is answered without a second audit", func(t *testing.T) {
		for attempt := 1; attempt <= 2; attempt++ {
			if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, clone.Generation, clone.ID, upid); err != nil {
				t.Fatalf("attempt %d: %v", attempt, err)
			}
		}
		if stored, audits := recorded(t); stored != upid || audits != 1 {
			t.Fatalf("UPID not kept exactly once: upid=%q audits=%d", stored, audits)
		}
	})

	t.Run("a different UPID for the same operation is refused", func(t *testing.T) {
		err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, clone.Generation, clone.ID, "UPID:pve:022:clone")
		if !errors.Is(err, ErrConflict) || !strings.Contains(err.Error(), "UPID changed for same operation") {
			t.Fatalf("changed UPID: %v", err)
		}
		if stored, audits := recorded(t); stored != upid || audits != 1 {
			t.Fatalf("changed UPID overwrote the kept one: upid=%q audits=%d", stored, audits)
		}
	})

	t.Run("no UPID is taken once the operation is resolved", func(t *testing.T) {
		stopped, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, clone.Generation, clone.ID, vmResolution(t))
		if err != nil || stopped.UnknownOutcome {
			t.Fatalf("resolve: %+v %v", stopped, err)
		}
		for _, generation := range []int64{clone.Generation, stopped.Generation} {
			if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, generation, clone.ID, upid); !errors.Is(err, ErrConflict) {
				t.Fatalf("UPID after resolution at generation %d: %v", generation, err)
			}
		}
		if _, audits := recorded(t); audits != 1 {
			t.Fatalf("UPID after resolution audited: %d", audits)
		}
		// The Terraform apply door reads the same NULL current operation
		// and must refuse it the same way.
		replayed, err := s.BeginRunnerVMTerraformApply(ctx, "scaler", vm.ID, stopped.Generation, clone.ID, strings.Repeat("d", 64))
		if replayed || !errors.Is(err, ErrConflict) {
			t.Fatalf("Terraform apply after resolution: replayed=%v err=%v", replayed, err)
		}
	})
}
