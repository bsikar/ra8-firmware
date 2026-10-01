//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheBootstrapARunningVMMayProve pins
// RecordRunnerVMBootstrapEvidence (runner_vms.go:128-189). Evidence is judged
// on its own terms before the ledger is read: the guest, the service account,
// every digest, and each timestamp's freshness bound. It then binds only to
// the exact reservation, by VMID and commit, while that VM is running,
// settled and not being cleaned up. A refusal writes no audit row; accepted
// evidence writes exactly one.
func TestIntegrationTheBootstrapARunningVMMayProve(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
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
	proof := func(t *testing.T, vm RunnerVM) RunnerVMBootstrapEvidence {
		now := time.Now()
		return RunnerVMBootstrapEvidence{
			ReservationID: vm.ID, VMID: vm.VMID, CommitSHA: vm.CommitSHA,
			GuestOS: "linux", GuestArchitecture: "amd64", ServiceAccount: "ra8ci",
			RunnerBinarySHA256: strings.Repeat("1", 64), AgentBinarySHA256: strings.Repeat("2", 64),
			ReadinessSHA256: strings.Repeat("3", 64), JITConfigSHA256: strings.Repeat("4", 64),
			JITConfigExpiresAt: now.Add(30 * time.Minute), EvidenceID: mustID(t),
			StartedAt: now.Add(-time.Minute), CompletedAt: now.Add(-time.Second), PreparedAt: now.Add(-time.Second),
		}
	}
	readies := func(t *testing.T, vmID string) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, `SELECT COUNT(*) FROM audit WHERE action='runner_vm.bootstrap.ready'
			AND target_type='runner_vm' AND target_id=$1`, vmID).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}

	t.Run("evidence that fails its own terms is refused before the ledger is read", func(t *testing.T) {
		vm := reserved(t)
		now := time.Now()
		for name, spoil := range map[string]func(*RunnerVMBootstrapEvidence){
			"a malformed reservation id":    func(e *RunnerVMBootstrapEvidence) { e.ReservationID = "nope" },
			"a VMID below the lab range":    func(e *RunnerVMBootstrapEvidence) { e.VMID = 8999 },
			"a short commit":                func(e *RunnerVMBootstrapEvidence) { e.CommitSHA = "abc" },
			"an unknown guest OS":           func(e *RunnerVMBootstrapEvidence) { e.GuestOS = "darwin" },
			"an unknown architecture":       func(e *RunnerVMBootstrapEvidence) { e.GuestArchitecture = "386" },
			"another service account":       func(e *RunnerVMBootstrapEvidence) { e.ServiceAccount = "root" },
			"a short runner digest":         func(e *RunnerVMBootstrapEvidence) { e.RunnerBinarySHA256 = "1" },
			"an uppercase agent digest":     func(e *RunnerVMBootstrapEvidence) { e.AgentBinarySHA256 = strings.Repeat("A", 64) },
			"no readiness digest":           func(e *RunnerVMBootstrapEvidence) { e.ReadinessSHA256 = "" },
			"no JIT digest":                 func(e *RunnerVMBootstrapEvidence) { e.JITConfigSHA256 = "" },
			"a malformed evidence id":       func(e *RunnerVMBootstrapEvidence) { e.EvidenceID = "nope" },
			"no start":                      func(e *RunnerVMBootstrapEvidence) { e.StartedAt = time.Time{} },
			"no prepared time":              func(e *RunnerVMBootstrapEvidence) { e.PreparedAt = time.Time{} },
			"a start after completion":      func(e *RunnerVMBootstrapEvidence) { e.StartedAt = e.CompletedAt.Add(time.Second) },
			"a bootstrap over five minutes": func(e *RunnerVMBootstrapEvidence) { e.StartedAt = e.CompletedAt.Add(-6 * time.Minute) },
			"a completion in the future":    func(e *RunnerVMBootstrapEvidence) { e.CompletedAt = now.Add(time.Minute); e.StartedAt = now },
			"a stale completion": func(e *RunnerVMBootstrapEvidence) {
				e.CompletedAt = now.Add(-6 * time.Minute)
				e.StartedAt = e.CompletedAt.Add(-time.Second)
			},
			"a preparation in the future": func(e *RunnerVMBootstrapEvidence) { e.PreparedAt = now.Add(time.Minute) },
			"a stale preparation":         func(e *RunnerVMBootstrapEvidence) { e.PreparedAt = now.Add(-6 * time.Minute) },
			"an expired JIT config":       func(e *RunnerVMBootstrapEvidence) { e.JITConfigExpiresAt = now.Add(-time.Second) },
			"a JIT config over an hour":   func(e *RunnerVMBootstrapEvidence) { e.JITConfigExpiresAt = now.Add(2 * time.Hour) },
		} {
			e := proof(t, vm)
			spoil(&e)
			if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", e); !errors.Is(err, ErrInvalid) {
				t.Fatalf("%s: want ErrInvalid, got %v", name, err)
			}
		}
		for name, actor := range map[string]string{"no actor": "", "an oversized actor": strings.Repeat("a", 257)} {
			if err := s.RecordRunnerVMBootstrapEvidence(ctx, actor, proof(t, vm)); !errors.Is(err, ErrInvalid) {
				t.Fatalf("%s: want ErrInvalid, got %v", name, err)
			}
		}
		unknown := proof(t, vm)
		unknown.ReservationID = mustID(t)
		if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", unknown); !errors.Is(err, ErrNotFound) {
			t.Fatalf("an unknown reservation: %v", err)
		}
		if n := readies(t, vm.ID); n != 0 {
			t.Fatalf("refused evidence audited %d times", n)
		}
	})

	t.Run("evidence binds only to the exact running and settled reservation", func(t *testing.T) {
		vm := reserved(t)
		if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", proof(t, vm)); !errors.Is(err, ErrConflict) {
			t.Fatalf("a reserved VM: %v", err)
		}
		stopped := settle(t, vm, "clone", "UPID:pve:101:clone")
		if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", proof(t, stopped)); !errors.Is(err, ErrConflict) {
			t.Fatalf("a stopped VM: %v", err)
		}
		start, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, stopped.Generation, "start", RunnerVMSafetyEvidence{})
		if err != nil {
			t.Fatal(err)
		}
		if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", proof(t, stopped)); !errors.Is(err, ErrConflict) {
			t.Fatalf("a VM mid-start: %v", err)
		}
		if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, start.Generation, start.ID, "UPID:pve:102:start"); err != nil {
			t.Fatal(err)
		}
		running, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, start.Generation, start.ID, vmResolution(t))
		if err != nil || running.State != "running" {
			t.Fatalf("start: %+v %v", running, err)
		}
		otherVMID := proof(t, running)
		otherVMID.VMID = running.VMID + 1
		if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", otherVMID); !errors.Is(err, ErrConflict) {
			t.Fatalf("another VMID: %v", err)
		}
		otherCommit := proof(t, running)
		otherCommit.CommitSHA = strings.Repeat("c", 40)
		if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", otherCommit); !errors.Is(err, ErrConflict) {
			t.Fatalf("another commit: %v", err)
		}
		if n := readies(t, vm.ID); n != 0 {
			t.Fatalf("refused evidence audited %d times", n)
		}
		accepted := proof(t, running)
		if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", accepted); err != nil {
			t.Fatalf("exact evidence for a running VM: %v", err)
		}
		var evidenceID, newState string
		if err := pool.QueryRow(ctx, `SELECT reason->>'evidence_id', new_state FROM audit
			WHERE action='runner_vm.bootstrap.ready' AND target_type='runner_vm' AND target_id=$1`, vm.ID).
			Scan(&evidenceID, &newState); err != nil {
			t.Fatal(err)
		}
		if evidenceID != accepted.EvidenceID || newState != "ready" {
			t.Fatalf("accepted evidence audited as %q %q", evidenceID, newState)
		}
		drained, err := s.MarkRunnerVMDraining(ctx, "scaler", vm.ID, running.Generation)
		if err != nil || drained.State != "draining" {
			t.Fatalf("drain: %+v %v", drained, err)
		}
		if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", proof(t, drained)); !errors.Is(err, ErrConflict) {
			t.Fatalf("a VM being cleaned up: %v", err)
		}
		if n := readies(t, vm.ID); n != 1 {
			t.Fatalf("bootstrap audited %d times, want 1", n)
		}
	})
}
