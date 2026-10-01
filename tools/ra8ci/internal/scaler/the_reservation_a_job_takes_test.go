// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A reservation is the one place a GitHub job becomes a claim on real
// hardware, so every way it can be replayed, raced or lied to has to end in a
// refusal rather than a second VM. These tests hold the lookup, conflict and
// exhaustion paths around it.

type lookupAnswer struct {
	vm  store.RunnerVM
	err error
}

// scriptedLedger answers the by-job lookup from a script the test owns and
// fails the reserve on demand, leaving every other method to the harness
// ledger.
type scriptedLedger struct {
	*memoryLedger
	answers    []lookupAnswer
	asked      int
	reserveErr error
	reserves   int
	reserved   []store.RunnerVMInput
}

func (s *scriptedLedger) GetRunnerVMByJob(_ context.Context, _ int64, _ string) (store.RunnerVM, error) {
	answer := s.answers[len(s.answers)-1]
	if s.asked < len(s.answers) {
		answer = s.answers[s.asked]
	}
	s.asked++
	return answer.vm, answer.err
}

func (s *scriptedLedger) ReserveRunnerVM(_ context.Context, _ string, input store.RunnerVMInput, _ time.Time) (store.RunnerVM, bool, error) {
	s.reserves++
	s.reserved = append(s.reserved, input)
	if s.reserveErr != nil {
		return store.RunnerVM{}, false, s.reserveErr
	}
	return store.RunnerVM{ID: "01996f90-3415-7cfe-8ff1-600058131b00", RunnerVMInput: input,
		CreationOperationID: "01996f90-3415-7cfe-8ff1-600058131b01", State: "reserved", Generation: 1}, true, nil
}

func scripted(t *testing.T, answers ...lookupAnswer) (*Handler, *scriptedLedger, github.Job) {
	t.Helper()
	handler, ledger, _, _, job := testHarness(t)
	scripted := &scriptedLedger{memoryLedger: ledger, answers: answers}
	handler.ledger = scripted
	return handler, scripted, job
}

// The input a fresh reservation would be written with, taken from the handler
// itself so the fixtures cannot drift from what the code computes.
func computedInput(t *testing.T, handler *Handler, job github.Job, vmid int) store.RunnerVMInput {
	t.Helper()
	input, err := handler.jobInput(context.Background(), job, vmid)
	if err != nil {
		t.Fatalf("job input: %v", err)
	}
	if input.ScaleSetID != 42 || input.VMID != vmid || input.Name != "ra8-lab-ci-"+itoa(vmid) ||
		input.WorkflowAttempt != 1 || input.CommitSHA != testSHA || input.Repository != job.Owner+"/"+job.Repository {
		t.Fatalf("job input = %+v", input)
	}
	return input
}

func itoa(value int) string {
	if value == 0 {
		return "0"
	}
	digits := ""
	for value > 0 {
		digits = string(rune('0'+value%10)) + digits
		value /= 10
	}
	return digits
}

// A replayed webhook must land on the reservation already written for that
// job. If the job now computes to different immutable input, the replay is
// refused rather than quietly rebinding hardware to new identity.
func TestAReplayedJobCannotChangeAnImmutableReservation(t *testing.T) {
	handler, _, job := scripted(t, lookupAnswer{err: store.ErrNotFound})
	input := computedInput(t, handler, job, 9000)
	drifted := input
	drifted.WorkflowAttempt = 9
	handler, ledger, job := scripted(t, lookupAnswer{vm: store.RunnerVM{ID: "01996f90-3415-7cfe-8ff1-600058131b02", RunnerVMInput: drifted}})
	_, err := handler.reservation(context.Background(), job)
	if err == nil || !strings.Contains(err.Error(), "replayed GitHub job changed immutable VM reservation") {
		t.Fatalf("reservation = %v", err)
	}
	if ledger.reserves != 0 {
		t.Fatalf("a drifted replay reserved %d VM(s)", ledger.reserves)
	}
}

// A replay that does match is still driven through the approval fence: a
// reservation written under approvals that have since changed is refused.
func TestAReplayIsStillHeldToCurrentApprovals(t *testing.T) {
	handler, _, job := scripted(t, lookupAnswer{err: store.ErrNotFound})
	input := computedInput(t, handler, job, 9000)
	foreign := store.RunnerVM{ID: "01996f90-3415-7cfe-8ff1-600058131b03", RunnerVMInput: input}
	foreign.Node = "someone-elses-node"
	handler, ledger, job := scripted(t, lookupAnswer{vm: foreign})
	if _, err := handler.reservation(context.Background(), job); err == nil {
		t.Fatal("a reservation outside current approvals was replayed")
	}
	if ledger.reserves != 0 {
		t.Fatalf("a refused replay reserved %d VM(s)", ledger.reserves)
	}
}

// A lookup that fails for any reason other than "not found" is handed back:
// an unreadable ledger must never be read as an empty one, which would mint a
// second VM for a job that already holds one.
func TestAnUnreadableLedgerIsNeverReadAsNoReservation(t *testing.T) {
	unreadable := errors.New("reservation lookup failed")
	handler, ledger, job := scripted(t, lookupAnswer{err: unreadable})
	if _, err := handler.reservation(context.Background(), job); !errors.Is(err, unreadable) {
		t.Fatalf("reservation = %v, want the lookup failure", err)
	}
	if ledger.reserves != 0 {
		t.Fatalf("an unreadable ledger reserved %d VM(s)", ledger.reserves)
	}
}

// Two schedulers racing the same job: the loser's conflict is resolved by
// reading back the winner's reservation, which is returned as-is.
func TestALostRaceAdoptsTheWinningReservation(t *testing.T) {
	handler, _, job := scripted(t, lookupAnswer{err: store.ErrNotFound})
	input := computedInput(t, handler, job, 9000)
	winner := store.RunnerVM{ID: "01996f90-3415-7cfe-8ff1-600058131b04", RunnerVMInput: input, State: "reserved", Generation: 1}
	handler, ledger, job := scripted(t, lookupAnswer{err: store.ErrNotFound}, lookupAnswer{vm: winner})
	ledger.reserveErr = store.ErrConflict
	got, err := handler.reservation(context.Background(), job)
	if err != nil {
		t.Fatalf("reservation = %v", err)
	}
	if got.ID != winner.ID {
		t.Fatalf("reservation = %+v, want the winning reservation", got)
	}
	if ledger.reserves != 1 {
		t.Fatalf("the loser attempted %d reservation(s)", ledger.reserves)
	}
}

// A conflict resolved by a reservation carrying different identity is a
// genuine disagreement about what this job is, and is refused.
func TestALostRaceRefusesAReservationThatChangedIdentity(t *testing.T) {
	handler, _, job := scripted(t, lookupAnswer{err: store.ErrNotFound})
	input := computedInput(t, handler, job, 9000)
	drifted := input
	drifted.CommitSHA = strings.Repeat("c", 64)
	other := store.RunnerVM{ID: "01996f90-3415-7cfe-8ff1-600058131b05", RunnerVMInput: drifted}
	handler, ledger, job := scripted(t, lookupAnswer{err: store.ErrNotFound}, lookupAnswer{vm: other})
	ledger.reserveErr = store.ErrConflict
	_, err := handler.reservation(context.Background(), job)
	if err == nil || !strings.Contains(err.Error(), "concurrent reservation changed job identity") {
		t.Fatalf("reservation = %v", err)
	}
}

// Every approved VMID conflicting with no reservation to adopt is capacity
// exhaustion, and says so rather than reaching past the approved set.
func TestAFullApprovedSetIsReportedAsExhaustion(t *testing.T) {
	handler, ledger, job := scripted(t, lookupAnswer{err: store.ErrNotFound})
	ledger.reserveErr = store.ErrConflict
	_, err := handler.reservation(context.Background(), job)
	if err == nil || !strings.Contains(err.Error(), "no approved disposable VMID is free") {
		t.Fatalf("reservation = %v", err)
	}
	if ledger.reserves != len(handler.config.VMIDs) {
		t.Fatalf("%d reserve attempt(s) over %d approved VMID(s)", ledger.reserves, len(handler.config.VMIDs))
	}
	for _, input := range ledger.reserved {
		if !containsID(handler.config.VMIDs, input.VMID) {
			t.Fatalf("a reservation was attempted on unapproved VMID %d", input.VMID)
		}
	}
}

// A reserve that fails for any other reason stops there: it is not a
// conflict, so there is nothing to adopt and nothing to retry.
func TestAReserveFailureIsNotTreatedAsAConflict(t *testing.T) {
	refused := errors.New("reservation write failed")
	handler, ledger, job := scripted(t, lookupAnswer{err: store.ErrNotFound})
	ledger.reserveErr = refused
	if _, err := handler.reservation(context.Background(), job); !errors.Is(err, refused) {
		t.Fatalf("reservation = %v, want the write failure", err)
	}
	if ledger.reserves != 1 {
		t.Fatalf("a non-conflict failure was retried: %d attempts", ledger.reserves)
	}
}

// A fresh reservation is written for the first approved VMID and handed back.
func TestAFreshJobTakesTheFirstApprovedVMID(t *testing.T) {
	handler, ledger, job := scripted(t, lookupAnswer{err: store.ErrNotFound})
	got, err := handler.reservation(context.Background(), job)
	if err != nil {
		t.Fatalf("reservation = %v", err)
	}
	if got.VMID != handler.config.VMIDs[0] || ledger.reserves != 1 {
		t.Fatalf("reservation = %+v over %d attempt(s)", got, ledger.reserves)
	}
}

// Untrusted job fields never reach a reservation: the job has to agree with
// the metadata the resolver returned, and the commit has to be a digest. The
// repository is not in this table on purpose: the resolver here derives it
// from the job itself, so the two can never disagree.
func TestAJobThatDisagreesWithItsMetadataIsRefused(t *testing.T) {
	for name, damage := range map[string]func(*github.Job){
		"no job ID":          func(job *github.Job) { job.JobID = "" },
		"no runner request":  func(job *github.Job) { job.RunnerRequestID = 0 },
		"negative run ID":    func(job *github.Job) { job.WorkflowRunID = -1 },
		"no workflow run ID": func(job *github.Job) { job.WorkflowRunID = 0 },
	} {
		handler, ledger, job := scripted(t, lookupAnswer{err: store.ErrNotFound})
		damage(&job)
		_, err := handler.reservation(context.Background(), job)
		if err == nil || !strings.Contains(err.Error(), "trusted GitHub metadata does not match scale-set job") {
			t.Fatalf("%s = %v", name, err)
		}
		if ledger.reserves != 0 {
			t.Fatalf("%s reserved %d VM(s)", name, ledger.reserves)
		}
	}
}
