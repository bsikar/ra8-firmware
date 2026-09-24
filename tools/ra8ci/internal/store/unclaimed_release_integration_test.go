//go:build integration

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"
)

// The case #1473 names: a job cancelled after its credential was minted. The
// reservation exists, nothing ever claimed it, and the deadline passes.
func TestIntegrationReleaseUnclaimedRunnerVM(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	in := runnerVMTestInput(t)
	now := time.Now().UTC()
	vm, created, err := s.ReserveRunnerVM(ctx, "scaler", in, now.Add(MinUnclaimedLease))
	if err != nil || !created {
		t.Fatalf("reserve: created=%v err=%v", created, err)
	}

	expireUnclaimedDeadline(ctx, t, s, vm.ID)
	expired := listOneExpiredUnclaimed(ctx, t, s, in.ScaleSetID, vm.ID)
	if !UnclaimedExpired(expired, time.Now().UTC()) {
		t.Fatalf("queued reservation does not read as expired: %+v", expired)
	}

	released, err := s.ReleaseUnclaimedRunnerVM(ctx, "unclaimed-reaper", vm.ID)
	if err != nil {
		t.Fatalf("release: %v", err)
	}
	if released.State != "released" || released.EndedAt == nil {
		t.Fatalf("release left the reservation live: state=%s ended_at=%v", released.State, released.EndedAt)
	}
	if released.Generation != vm.Generation+1 {
		t.Fatalf("release did not invalidate an in-flight CAS: %d -> %d", vm.Generation, released.Generation)
	}
	if UnclaimedReservation(released) {
		t.Fatal("a released reservation is still in the reaper queue")
	}

	// Idempotent: the sequence is resumed from the top after an ambiguous
	// failure, so the commit step is asked to happen twice.
	again, err := s.ReleaseUnclaimedRunnerVM(ctx, "unclaimed-reaper", vm.ID)
	if err != nil {
		t.Fatalf("second release: %v", err)
	}
	if again.Generation != released.Generation || !again.EndedAt.Equal(*released.EndedAt) {
		t.Fatalf("second release moved the row: %+v -> %+v", released, again)
	}

	rows, err := s.ListExpiredUnclaimedRunnerVMs(ctx, in.ScaleSetID, time.Now().UTC().Add(MaxUnclaimedLease), 50)
	if err != nil {
		t.Fatalf("queue after release: %v", err)
	}
	for _, row := range rows {
		if row.ID == vm.ID {
			t.Fatal("the released reservation came back in the queue")
		}
	}
	if _, err := s.ReleaseUnclaimedRunnerVM(ctx, "unclaimed-reaper", mustID(t)); !errors.Is(err, ErrNotFound) {
		t.Fatalf("release of an unknown reservation: %v", err)
	}
}

// The race the reaper exists to lose safely: the job takes the runner while
// the batch is being walked.
func TestIntegrationReleaseUnclaimedRefusesAClaimedReservation(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	in := runnerVMTestInput(t)
	vm, created, err := s.ReserveRunnerVM(ctx, "scaler", in, time.Now().UTC().Add(MinUnclaimedLease))
	if err != nil || !created {
		t.Fatalf("reserve: created=%v err=%v", created, err)
	}
	expireUnclaimedDeadline(ctx, t, s, vm.ID)
	if _, err := s.MarkRunnerVMClaimed(ctx, "dispatch", vm.ID); err != nil {
		t.Fatalf("claim: %v", err)
	}
	if _, err := s.ReleaseUnclaimedRunnerVM(ctx, "unclaimed-reaper", vm.ID); !errors.Is(err, ErrConflict) {
		t.Fatalf("expired-but-claimed reservation released: %v", err)
	}
	after, err := s.GetRunnerVM(ctx, vm.ID)
	if err != nil {
		t.Fatalf("re-read: %v", err)
	}
	if after.State == "released" || after.EndedAt != nil {
		t.Fatalf("live work torn down: state=%s ended_at=%v", after.State, after.EndedAt)
	}
}

// Concurrent passes over the same expired reservation: exactly one of them
// may perform the release, and none of them may report success on a row it
// did not settle.
func TestIntegrationReleaseUnclaimedIsSingleWinner(t *testing.T) {
	s, _ := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	in := runnerVMTestInput(t)
	vm, created, err := s.ReserveRunnerVM(ctx, "scaler", in, time.Now().UTC().Add(MinUnclaimedLease))
	if err != nil || !created {
		t.Fatalf("reserve: created=%v err=%v", created, err)
	}
	expireUnclaimedDeadline(ctx, t, s, vm.ID)

	const reapers = 6
	var wait sync.WaitGroup
	start := make(chan struct{})
	results := make([]RunnerVM, reapers)
	errs := make([]error, reapers)
	for i := 0; i < reapers; i++ {
		wait.Add(1)
		go func(i int) {
			defer wait.Done()
			<-start
			results[i], errs[i] = s.ReleaseUnclaimedRunnerVM(ctx, "unclaimed-reaper", vm.ID)
		}(i)
	}
	close(start)
	wait.Wait()

	var settled int
	for i, err := range errs {
		if err != nil {
			t.Fatalf("reaper %d: %v", i, err)
		}
		if results[i].State != "released" {
			t.Fatalf("reaper %d returned a live reservation: %s", i, results[i].State)
		}
		if results[i].Generation == vm.Generation+1 {
			settled++
		}
	}
	// Every caller sees a released row; the generation says the write
	// happened once. A second write would have produced generation+2.
	if settled != reapers {
		t.Fatalf("release ran more than once: %d of %d reapers saw generation %d",
			settled, reapers, vm.Generation+1)
	}
}

// expireUnclaimedDeadline moves a reservation's deadline into the past so the
// reaper's queue offers it, without the test waiting out a real lease. The
// deadline column is deliberately not writable through the store, so the test
// reaches for SQL rather than an API that should not exist.
func expireUnclaimedDeadline(ctx context.Context, t *testing.T, s *Store, reservationID string) {
	t.Helper()
	tag, err := s.pool.Exec(ctx, `UPDATE runner_vms
		SET unclaimed_deadline=created_at + interval '1 second' WHERE id=$1`, reservationID)
	if err != nil || tag.RowsAffected() != 1 {
		t.Fatalf("expire deadline: %v rows=%d", err, tag.RowsAffected())
	}
}

func listOneExpiredUnclaimed(ctx context.Context, t *testing.T, s *Store, scaleSetID int64, reservationID string) RunnerVM {
	t.Helper()
	rows, err := s.ListExpiredUnclaimedRunnerVMs(ctx, scaleSetID, time.Now().UTC(), 50)
	if err != nil {
		t.Fatalf("list expired: %v", err)
	}
	for _, row := range rows {
		if row.ID == reservationID {
			return row
		}
	}
	t.Fatalf("reservation %s never reached the reaper queue (%d rows)", reservationID, len(rows))
	return RunnerVM{}
}
