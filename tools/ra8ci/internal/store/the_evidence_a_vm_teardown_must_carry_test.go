//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"
)

// TestIntegrationTheEvidenceAVMTeardownMustCarry pins the safety evidence
// BeginRunnerVMOperation demands before it records a stop or a destroy
// (runner_vms.go:435-439): evidence that is missing, stale by more than ten
// seconds, dated in the future, or that does not say the runner drained
// with no active job is refused, and a destroy must also prove the runner
// was deregistered under an approval and name the very runner the
// reservation registered. Every refusal is ErrDenied, hands back no
// operation and records no intent. The controls stop and destroy the same
// VM with honest evidence. Stale and future stamps sit a full minute past
// their windows so the database clock never decides them by a hair.
func TestIntegrationTheEvidenceAVMTeardownMustCarry(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	t.Run("an unknown reservation is not found", func(t *testing.T) {
		operation, err := s.BeginRunnerVMOperation(ctx, "scaler", mustID(t), 1, "clone", RunnerVMSafetyEvidence{})
		if !errors.Is(err, ErrNotFound) || operation.ID != "" {
			t.Fatalf("unknown reservation: %+v %v", operation, err)
		}
	})

	const runnerID = 7735
	vm := drainingRunnerVM(t, ctx, s, runnerID)
	operations := func(t *testing.T) int {
		t.Helper()
		var n int
		if err := pool.QueryRow(ctx, "SELECT COUNT(*) FROM runner_vm_operations WHERE runner_vm_id=$1", vm.ID).Scan(&n); err != nil {
			t.Fatal(err)
		}
		return n
	}
	refused := func(t *testing.T, generation int64, kind string, proof RunnerVMSafetyEvidence) {
		t.Helper()
		before := operations(t)
		operation, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, generation, kind, proof)
		if !errors.Is(err, ErrDenied) || errors.Is(err, ErrConflict) || operation.ID != "" {
			t.Fatalf("%s with this evidence was not denied: %+v %v", kind, operation, err)
		}
		if after := operations(t); after != before {
			t.Fatalf("denied %s recorded an intent: %d -> %d", kind, before, after)
		}
		now, err := s.GetRunnerVM(ctx, vm.ID)
		if err != nil || now.Generation != generation || now.CurrentOperationID != "" {
			t.Fatalf("denied %s moved the reservation: %+v %v", kind, now, err)
		}
	}
	honestStop := func() RunnerVMSafetyEvidence {
		return RunnerVMSafetyEvidence{EvidenceID: mustID(t), ObservedAt: time.Now().UTC(), Drained: true, NoActiveJob: true}
	}

	stopRefusals := map[string]func(*RunnerVMSafetyEvidence){
		"no evidence at all":             func(p *RunnerVMSafetyEvidence) { *p = RunnerVMSafetyEvidence{} },
		"an evidence ID that is no UUID": func(p *RunnerVMSafetyEvidence) { p.EvidenceID = "evidence" },
		"evidence a minute stale":        func(p *RunnerVMSafetyEvidence) { p.ObservedAt = time.Now().UTC().Add(-70 * time.Second) },
		"evidence dated a minute ahead":  func(p *RunnerVMSafetyEvidence) { p.ObservedAt = time.Now().UTC().Add(time.Minute) },
		"a runner that has not drained":  func(p *RunnerVMSafetyEvidence) { p.Drained = false },
		"a job still active":             func(p *RunnerVMSafetyEvidence) { p.NoActiveJob = false },
	}
	for name, spoil := range stopRefusals {
		t.Run("stop refused: "+name, func(t *testing.T) {
			proof := honestStop()
			spoil(&proof)
			refused(t, vm.Generation, "stop", proof)
		})
	}

	var stopped RunnerVM
	t.Run("an honest stop is recorded", func(t *testing.T) {
		stop, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "stop", honestStop())
		if err != nil || stop.Kind != "stop" || stop.PendingState != "stopping" {
			t.Fatalf("honest stop: %+v %v", stop, err)
		}
		if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, stop.Generation, stop.ID, "UPID:pve:013:stop"); err != nil {
			t.Fatal(err)
		}
		stopped, err = s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, stop.Generation, stop.ID, vmResolution(t))
		if err != nil || stopped.State != "stopped" {
			t.Fatalf("stop reconcile: %+v %v", stopped, err)
		}
		vm = stopped
	})

	honestDestroy := func() RunnerVMSafetyEvidence {
		return RunnerVMSafetyEvidence{EvidenceID: mustID(t), ObservedAt: time.Now().UTC(),
			Drained: true, NoActiveJob: true, RunnerDeregistered: true,
			ExpectedConfigDigest: strings.Repeat("c", 40), ApprovalID: mustID(t), ExternalRunnerID: runnerID}
	}
	destroyRefusals := map[string]func(*RunnerVMSafetyEvidence){
		"a runner still registered":      func(p *RunnerVMSafetyEvidence) { p.RunnerDeregistered = false },
		"no approval":                    func(p *RunnerVMSafetyEvidence) { p.ApprovalID = "" },
		"a config digest that is no SHA": func(p *RunnerVMSafetyEvidence) { p.ExpectedConfigDigest = "c" },
		"evidence a minute stale":        func(p *RunnerVMSafetyEvidence) { p.ObservedAt = time.Now().UTC().Add(-70 * time.Second) },
		"some other runner":              func(p *RunnerVMSafetyEvidence) { p.ExternalRunnerID = runnerID + 1 },
	}
	for name, spoil := range destroyRefusals {
		t.Run("destroy refused: "+name, func(t *testing.T) {
			proof := honestDestroy()
			spoil(&proof)
			refused(t, vm.Generation, "destroy", proof)
		})
	}

	t.Run("an honest destroy is recorded", func(t *testing.T) {
		destroy, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, vm.Generation, "destroy", honestDestroy())
		if err != nil || destroy.Kind != "destroy" || destroy.PendingState != "deleting" {
			t.Fatalf("honest destroy: %+v %v", destroy, err)
		}
	})
}

// drainingRunnerVM walks one reservation through clone, start, bootstrap,
// registration and drain, the only road to a VM a stop may be asked of.
func drainingRunnerVM(t *testing.T, ctx context.Context, s *Store, runnerID int64) RunnerVM {
	t.Helper()
	vm, _, err := s.ReserveRunnerVM(ctx, "scaler", runnerVMTestInput(t), testUnclaimedDeadline())
	if err != nil {
		t.Fatal(err)
	}
	advance := func(generation int64, kind, upid string) RunnerVM {
		t.Helper()
		operation, err := s.BeginRunnerVMOperation(ctx, "scaler", vm.ID, generation, kind, RunnerVMSafetyEvidence{})
		if err != nil {
			t.Fatalf("%s: %v", kind, err)
		}
		if err := s.RecordRunnerVMUPID(ctx, "scaler", vm.ID, operation.Generation, operation.ID, upid); err != nil {
			t.Fatal(err)
		}
		next, err := s.ResolveRunnerVMOperation(ctx, "scaler", vm.ID, operation.Generation, operation.ID, vmResolution(t))
		if err != nil {
			t.Fatalf("%s reconcile: %v", kind, err)
		}
		return next
	}
	stopped := advance(vm.Generation, "clone", "UPID:pve:011:clone")
	running := advance(stopped.Generation, "start", "UPID:pve:012:start")
	if err := s.RecordRunnerVMBootstrapEvidence(ctx, "scaler", RunnerVMBootstrapEvidence{
		ReservationID: vm.ID, VMID: vm.VMID, CommitSHA: vm.CommitSHA,
		GuestOS: "linux", GuestArchitecture: "amd64", ServiceAccount: "ra8ci",
		RunnerBinarySHA256: strings.Repeat("1", 64), AgentBinarySHA256: strings.Repeat("2", 64),
		ReadinessSHA256: strings.Repeat("3", 64), JITConfigSHA256: strings.Repeat("4", 64),
		JITConfigExpiresAt: time.Now().Add(30 * time.Minute), EvidenceID: mustID(t),
		StartedAt: time.Now().Add(-time.Second), CompletedAt: time.Now(), PreparedAt: time.Now(),
	}); err != nil {
		t.Fatalf("bootstrap evidence: %v", err)
	}
	registered, err := s.MarkRunnerVMRegistered(ctx, "scaler", vm.ID, running.Generation, runnerID, "ra8ci-runner-door")
	if err != nil {
		t.Fatalf("registration: %v", err)
	}
	draining, err := s.MarkRunnerVMDraining(ctx, "scaler", vm.ID, registered.Generation)
	if err != nil || draining.State != "draining" {
		t.Fatalf("drain: %+v %v", draining, err)
	}
	return draining
}
