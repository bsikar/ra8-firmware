// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// A runner VM is a machine in a lab: cloned from a template, started, drained,
// stopped, destroyed. Every one of those steps is an external mutation that
// cannot be taken back, so the plane decides whether it is allowed before it
// asks Proxmox or Terraform for anything. These are those decisions, all of
// them pure, none of them needing a database.

const (
	// runnerVMTestEvidence and runnerVMTestApproval are well-formed
	// identifiers; ValidID requires the shape, not a row in a table.
	runnerVMTestEvidence = "01996f90-3415-7cfe-8ff1-600058131aff"
	runnerVMTestApproval = "01996f90-3415-7cfe-8ff1-600058131afe"
)

func aRunnerVMInput() RunnerVMInput {
	return RunnerVMInput{
		ScaleSetID: 1, JobID: "job-1", RunnerRequestID: 2, WorkflowRunID: 3,
		WorkflowAttempt: 1, Repository: "bsikar/ra8-firmware", WorkflowRef: "refs/heads/main",
		CommitSHA: strings.Repeat("a", 40), VMID: 9000, Node: "lab-1", Pool: "ra8",
		Storage: "local-lvm", Name: "ra8-lab-runner-1", TemplateVMID: 100,
		TemplateName: "ra8-lab-template", TemplateDigest: strings.Repeat("b", 40),
	}
}

// A reservation is the permanent record of a job attempt, so the identity in
// it is judged before it is written rather than patched afterwards.
func TestAReservationStatesAWholeMachine(t *testing.T) {
	if !validRunnerVMInput(aRunnerVMInput()) {
		t.Fatal("a whole reservation was refused")
	}

	for name, bend := range map[string]func(*RunnerVMInput){
		"no scale set":              func(in *RunnerVMInput) { in.ScaleSetID = 0 },
		"a negative scale set":      func(in *RunnerVMInput) { in.ScaleSetID = -1 },
		"no job":                    func(in *RunnerVMInput) { in.JobID = "" },
		"a job past its bound":      func(in *RunnerVMInput) { in.JobID = strings.Repeat("j", 129) },
		"no runner request":         func(in *RunnerVMInput) { in.RunnerRequestID = 0 },
		"no workflow run":           func(in *RunnerVMInput) { in.WorkflowRunID = 0 },
		"no workflow attempt":       func(in *RunnerVMInput) { in.WorkflowAttempt = 0 },
		"no repository":             func(in *RunnerVMInput) { in.Repository = "" },
		"a repository past 512":     func(in *RunnerVMInput) { in.Repository = strings.Repeat("r", 513) },
		"no workflow ref":           func(in *RunnerVMInput) { in.WorkflowRef = "" },
		"a ref past 1024":           func(in *RunnerVMInput) { in.WorkflowRef = strings.Repeat("f", 1025) },
		"a short commit":            func(in *RunnerVMInput) { in.CommitSHA = strings.Repeat("a", 39) },
		"a shouted commit":          func(in *RunnerVMInput) { in.CommitSHA = strings.Repeat("A", 40) },
		"a VMID under the floor":    func(in *RunnerVMInput) { in.VMID = 8999 },
		"a template under its own":  func(in *RunnerVMInput) { in.TemplateVMID = 99 },
		"a template that is the VM": func(in *RunnerVMInput) { in.TemplateVMID = 9000; in.VMID = 9000 },
		"no node":                   func(in *RunnerVMInput) { in.Node = "" },
		"a node past its bound":     func(in *RunnerVMInput) { in.Node = strings.Repeat("n", 129) },
		"no pool":                   func(in *RunnerVMInput) { in.Pool = "" },
		"no storage":                func(in *RunnerVMInput) { in.Storage = "" },
		"a name off the lab prefix": func(in *RunnerVMInput) { in.Name = "runner-1" },
		"a shouted name":            func(in *RunnerVMInput) { in.Name = "RA8-LAB-RUNNER-1" },
		"a template off the prefix": func(in *RunnerVMInput) { in.TemplateName = "template" },
		"no template digest":        func(in *RunnerVMInput) { in.TemplateDigest = "" },
	} {
		in := aRunnerVMInput()
		bend(&in)
		if validRunnerVMInput(in) {
			t.Fatalf("a reservation with %s was accepted", name)
		}
	}
}

// The floors are floors, not fences: the first legal VMID and the first legal
// template VMID are both accepted, and the lab name may be a single character
// after its prefix.
func TestAReservationAcceptsItsFloorsExactly(t *testing.T) {
	for name, bend := range map[string]func(*RunnerVMInput){
		"the first guest VMID":      func(in *RunnerVMInput) { in.VMID = 9000 },
		"the first template VMID":   func(in *RunnerVMInput) { in.TemplateVMID = 100 },
		"a job at its bound":        func(in *RunnerVMInput) { in.JobID = strings.Repeat("j", 128) },
		"a repository at 512":       func(in *RunnerVMInput) { in.Repository = strings.Repeat("r", 512) },
		"a ref at 1024":             func(in *RunnerVMInput) { in.WorkflowRef = strings.Repeat("f", 1024) },
		"the shortest lab name":     func(in *RunnerVMInput) { in.Name = "ra8-lab-a" },
		"the longest lab name":      func(in *RunnerVMInput) { in.Name = "ra8-lab-a" + strings.Repeat("b", 54) },
		"a name one past its cap":   func(in *RunnerVMInput) { in.Name = "ra8-lab-a" + strings.Repeat("b", 55) },
		"a name starting on a dash": func(in *RunnerVMInput) { in.Name = "ra8-lab--a" },
	} {
		in := aRunnerVMInput()
		bend(&in)
		accepted := validRunnerVMInput(in)
		wantAccepted := name != "a name one past its cap" && name != "a name starting on a dash"
		if accepted != wantAccepted {
			t.Fatalf("%s: accepted = %v, want %v", name, accepted, wantAccepted)
		}
	}
}

// Each operation names the state it must start from and the state it moves
// through. A kind nobody declared is not allowed from anywhere, which is what
// keeps a typo from being read as a permitted transition.
func TestEveryVMOperationNamesTheStateItStartsFrom(t *testing.T) {
	for kind, want := range map[string]struct{ from, pending string }{
		"clone":   {from: "reserved", pending: "cloning"},
		"start":   {from: "stopped", pending: "starting"},
		"stop":    {from: "draining", pending: "stopping"},
		"destroy": {from: "stopped", pending: "deleting"},
	} {
		from, pending, allowed := runnerVMOperationStates(kind, want.from)
		if from != want.from || pending != want.pending || !allowed {
			t.Fatalf("%s from %q answered (%q,%q,%v)", kind, want.from, from, pending, allowed)
		}
		if _, _, allowed := runnerVMOperationStates(kind, "running"); allowed {
			t.Fatalf("%s was allowed from running", kind)
		}
	}

	for _, kind := range []string{"", "CLONE", "delete", "restart", "reserve"} {
		from, pending, allowed := runnerVMOperationStates(kind, "reserved")
		if from != "" || pending != "" || allowed {
			t.Fatalf("%q answered (%q,%q,%v), want nothing allowed", kind, from, pending, allowed)
		}
	}
}

// A clone or a start needs no safety proof: nothing is running yet to be
// interrupted. Stopping or destroying a machine does, because a job could be
// on it.
func TestOnlyStoppingAndDestroyingNeedProof(t *testing.T) {
	for _, kind := range []string{"clone", "start", "", "restart"} {
		if err := validateVMSafety(time.Now(), kind, RunnerVMSafetyEvidence{}); err != nil {
			t.Fatalf("%q was asked for safety evidence: %v", kind, err)
		}
	}
}

func aStopProof(now time.Time) RunnerVMSafetyEvidence {
	return RunnerVMSafetyEvidence{
		EvidenceID: runnerVMTestEvidence, ObservedAt: now.Add(-time.Second),
		Drained: true, NoActiveJob: true,
	}
}

// The proof has to be recent, which is the whole point of it: a drained
// reading from a minute ago says nothing about the machine now.
func TestStoppingAMachineNeedsAFreshDrainedReading(t *testing.T) {
	now := time.Now()

	if err := validateVMSafety(now, "stop", aStopProof(now)); err != nil {
		t.Fatalf("a fresh drained reading was refused: %v", err)
	}

	for name, bend := range map[string]func(*RunnerVMSafetyEvidence){
		"no evidence ID":        func(p *RunnerVMSafetyEvidence) { p.EvidenceID = "" },
		"an evidence ID by eye": func(p *RunnerVMSafetyEvidence) { p.EvidenceID = "evidence-1" },
		"never observed":        func(p *RunnerVMSafetyEvidence) { p.ObservedAt = time.Time{} },
		"observed in future":    func(p *RunnerVMSafetyEvidence) { p.ObservedAt = now.Add(2 * time.Second) },
		"observed too long ago": func(p *RunnerVMSafetyEvidence) { p.ObservedAt = now.Add(-11 * time.Second) },
		"not drained":           func(p *RunnerVMSafetyEvidence) { p.Drained = false },
		"a job still on it":     func(p *RunnerVMSafetyEvidence) { p.NoActiveJob = false },
	} {
		proof := aStopProof(now)
		bend(&proof)
		if err := validateVMSafety(now, "stop", proof); !errors.Is(err, ErrDenied) {
			t.Fatalf("stopping with %s answered %v, want denied", name, err)
		}
	}
}

// Destroying asks for everything stopping asks for and three things more: the
// runner deregistered, an operator's approval, and the configuration digest
// the approval was given against.
func TestDestroyingAMachineAsksForMoreThanStopping(t *testing.T) {
	now := time.Now()

	stopProof := aStopProof(now)
	if err := validateVMSafety(now, "destroy", stopProof); !errors.Is(err, ErrDenied) {
		t.Fatalf("a stop-grade proof destroyed a machine: %v", err)
	}

	destroyProof := stopProof
	destroyProof.RunnerDeregistered = true
	destroyProof.ApprovalID = runnerVMTestApproval
	destroyProof.ExpectedConfigDigest = strings.Repeat("c", 40)
	if err := validateVMSafety(now, "destroy", destroyProof); err != nil {
		t.Fatalf("a whole destroy proof was refused: %v", err)
	}

	for name, bend := range map[string]func(*RunnerVMSafetyEvidence){
		"the runner still registered": func(p *RunnerVMSafetyEvidence) { p.RunnerDeregistered = false },
		"nobody approving it":         func(p *RunnerVMSafetyEvidence) { p.ApprovalID = "" },
		"an approval by eye":          func(p *RunnerVMSafetyEvidence) { p.ApprovalID = "approved-by-me" },
		"no configuration digest":     func(p *RunnerVMSafetyEvidence) { p.ExpectedConfigDigest = "" },
		"a digest of the wrong width": func(p *RunnerVMSafetyEvidence) { p.ExpectedConfigDigest = strings.Repeat("c", 64) },
	} {
		proof := destroyProof
		bend(&proof)
		if err := validateVMSafety(now, "destroy", proof); !errors.Is(err, ErrDenied) {
			t.Fatalf("destroying with %s answered %v, want denied", name, err)
		}
	}
}

// The success state is per operation and is what the reservation is moved to
// once the outcome is proven. A kind nobody declared has no success state at
// all, so it can never be recorded as having finished.
func TestEveryVMOperationNamesWhereSuccessLeavesTheMachine(t *testing.T) {
	for kind, want := range map[string]string{
		"clone":   "stopped",
		"stop":    "stopped",
		"start":   "running",
		"destroy": "released",
		"":        "",
		"delete":  "",
		"CLONE":   "",
	} {
		if state := VMOperationSuccessState(kind); state != want {
			t.Fatalf("%q succeeds into %q, want %q", kind, state, want)
		}
	}
}

// A positive count is written as itself; anything else is written as SQL NULL
// rather than as a zero a later reader would take for a real measurement.
func TestACountIsWrittenOrLeftUnstated(t *testing.T) {
	for _, value := range []int64{1, 2, 1 << 40} {
		if written := nullablePositive(value); written != any(value) {
			t.Fatalf("%d was written as %v", value, written)
		}
	}
	for _, value := range []int64{0, -1, -(1 << 40)} {
		if written := nullablePositive(value); written != nil {
			t.Fatalf("%d was written as %v, want NULL", value, written)
		}
	}
}
