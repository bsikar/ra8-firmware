// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func (q *fakeUnclaimedQueue) GetRunnerVM(_ context.Context, id string) (store.RunnerVM, error) {
	for _, vm := range q.rows {
		if vm.ID == id {
			return vm, nil
		}
	}
	return store.RunnerVM{}, store.ErrNotFound
}

type staleUnclaimedQueue struct {
	candidate store.RunnerVM
	current   store.RunnerVM
}

func (q staleUnclaimedQueue) ListExpiredUnclaimedRunnerVMs(context.Context, int64, time.Time, int) ([]store.RunnerVM, error) {
	return []store.RunnerVM{q.candidate}, nil
}

func (q staleUnclaimedQueue) GetRunnerVM(_ context.Context, id string) (store.RunnerVM, error) {
	if id != q.current.ID {
		return store.RunnerVM{}, store.ErrNotFound
	}
	return q.current, nil
}

func TestUnclaimedReaperRechecksTheCurrentReservationBeforeRevoking(t *testing.T) {
	now := time.Date(2026, 9, 25, 12, 0, 0, 0, time.UTC)
	candidate := expiredVM("claimed", now.Add(-time.Hour))
	claimedAt := now.Add(-time.Minute)
	current := candidate
	current.ClaimedAt = &claimedAt
	queue := staleUnclaimedQueue{candidate: candidate, current: current}
	revoker := &fakeRevoker{}
	reaper, err := NewUnclaimedReaper(UnclaimedReaperConfig{
		ScaleSetID: 42,
		BatchSize:  10,
		Now:        func() time.Time { return now },
	}, queue, revoker)
	if err != nil {
		t.Fatal(err)
	}

	report, err := reaper.Reap(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if report.Scanned != 1 || report.Claimed != 1 || report.Reaped != 0 || report.Partial != 0 {
		t.Fatalf("report %+v, want the newly claimed candidate skipped", report)
	}
	if len(revoker.order) != 0 {
		t.Fatalf("revoked a reservation claimed after the queue read: %v", revoker.order)
	}
}
