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

// closedDoor refuses every job and counts what it was asked, so a test can
// tell a refusal that happened at the door from one that happened after a
// step had already begun work.
type closedDoor struct {
	asked  int
	reason error
}

func (d *closedDoor) Allow(_ context.Context, _ github.Job) error {
	d.asked++
	return d.reason
}

// silentResolver fails the way a GitHub API outage does: no metadata, no
// claim about the job either way.
type silentResolver struct{ err error }

func (r silentResolver) Resolve(context.Context, github.Job) (Metadata, error) {
	return Metadata{}, r.err
}

// Admission is asked before each bucket, and a refusal there stops the whole
// message rather than the one job. A scale set told it may not act on a job
// must not act on the rest of the batch either, since the batch is what the
// forge considers one decision.
func TestARefusedJobStopsTheMessageInEveryBucket(t *testing.T) {
	for _, tc := range []struct {
		bucket  string
		message func(job github.Job) github.Message
	}{
		{"assigned", func(job github.Job) github.Message {
			return github.Message{ScaleSetID: 42, Assigned: []github.Job{assignedJob(job)}}
		}},
		{"started", func(job github.Job) github.Message {
			return github.Message{ScaleSetID: 42, Started: []github.Job{job}}
		}},
		{"completed", func(job github.Job) github.Message {
			return github.Message{ScaleSetID: 42, Completed: []github.Job{completedJob(job)}}
		}},
	} {
		t.Run(tc.bucket, func(t *testing.T) {
			h, ledger, fake, _, job := testHarness(t)
			door := &closedDoor{reason: errors.New("workflow is not admitted to this scale set")}
			h.admission = door

			err := h.Process(context.Background(), tc.message(job))
			if err == nil || !strings.Contains(err.Error(), "not admitted") {
				t.Fatalf("error = %v, want the door's own reason", err)
			}
			if door.asked != 1 {
				t.Fatalf("admission asked %d times, want once", door.asked)
			}
			if ledger.reserveCalls != 0 {
				t.Fatalf("a refused job reserved %d times", ledger.reserveCalls)
			}
			fake.mu.Lock()
			defer fake.mu.Unlock()
			if fake.cloneCalls != 0 || fake.startCalls != 0 {
				t.Fatalf("a refused job touched the hypervisor: clone=%d start=%d",
					fake.cloneCalls, fake.startCalls)
			}
		})
	}
}

// A message for some other scale set is refused before admission is even
// consulted: this handler has no standing to judge another set's jobs.
func TestAMessageForAnotherScaleSetIsRefusedAtTheDoor(t *testing.T) {
	h, _, _, _, job := testHarness(t)
	door := &closedDoor{}
	h.admission = door
	err := h.Process(context.Background(), github.Message{ScaleSetID: 43, Assigned: []github.Job{assignedJob(job)}})
	if err == nil || !strings.Contains(err.Error(), "foreign or unconfigured") {
		t.Fatalf("error = %v, want a foreign scale-set refusal", err)
	}
	if door.asked != 0 {
		t.Fatalf("admission was consulted %d times for another scale set", door.asked)
	}
}

// Metadata is fetched independently of the message precisely so a job cannot
// describe itself. When that fetch fails there is nothing to check the job
// against, so no reservation is opened.
func TestAnAssignmentWithNoIndependentMetadataReservesNothing(t *testing.T) {
	h, ledger, fake, _, job := testHarness(t)
	outage := errors.New("GitHub metadata API is unavailable")
	h.metadata = silentResolver{err: outage}
	if err := h.assigned(context.Background(), assignedJob(job)); !errors.Is(err, outage) {
		t.Fatalf("error = %v, want the resolver's own failure", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if fake.cloneCalls != 0 || ledger.reserveCalls != 0 {
		t.Fatalf("a job with no metadata was acted on: clone=%d reserve=%d",
			fake.cloneCalls, ledger.reserveCalls)
	}
}

// Every reservation carries a deadline the unclaimed reaper can read, so an
// unset lease is the default rather than "no deadline". A reservation with
// no deadline would sit holding a minted credential forever.
func TestAnUnsetLeaseIsTheDefaultAndNeverNoDeadline(t *testing.T) {
	if got := (Options{}).unclaimedLease(); got != store.DefaultUnclaimedLease {
		t.Fatalf("lease = %v, want the default %v", got, store.DefaultUnclaimedLease)
	}
	if got := (Options{UnclaimedLease: time.Minute}).unclaimedLease(); got != time.Minute {
		t.Fatalf("lease = %v, want the configured minute", got)
	}
}

// The reconcile batch is defaulted when unset and refused outside its bounds,
// because a batch of zero silently reconciles nothing and an unbounded one
// holds the ledger for as long as the backlog is deep.
func TestTheReconcileBatchIsDefaultedAndBounded(t *testing.T) {
	h, _, _, _, _ := testHarness(t)
	base := h.config
	if base.MaxReconcileBatch != 64 {
		t.Fatalf("default batch = %d, want 64", base.MaxReconcileBatch)
	}
	for _, size := range []int{-1, 1001} {
		cfg := base
		cfg.MaxReconcileBatch = size
		_, err := NewHandler(cfg, h.ledger, h.vms, h.metadata, h.bootstrap, h.runners, h.backup, h.admission)
		if err == nil || !strings.Contains(err.Error(), "batch out of range") {
			t.Fatalf("batch %d = %v, want a refusal naming the range", size, err)
		}
	}
	cfg := base
	cfg.MaxReconcileBatch = 1000
	if _, err := NewHandler(cfg, h.ledger, h.vms, h.metadata, h.bootstrap, h.runners, h.backup, h.admission); err != nil {
		t.Fatalf("the top of the range was refused: %v", err)
	}
}
