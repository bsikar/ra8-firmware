// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Between the queue read and a reservation's turn, the row can move. The
// re-read is where the pass finds out, and what it finds decides whether the
// pass walks on or stops: a row that has gone is a job that took the runner,
// which is an ordinary outcome, while a re-read that fails or answers with
// somebody else's reservation is an answer the pass cannot read, and
// revoking on it would kill live work.

// rereadQueue answers the queue read with one candidate and the re-read with
// whatever this pass is being shown instead.
type rereadQueue struct {
	candidate store.RunnerVM
	answer    store.RunnerVM
	reason    error
	reads     int
}

func (q *rereadQueue) ListExpiredUnclaimedRunnerVMs(context.Context, int64, time.Time, int) ([]store.RunnerVM, error) {
	return []store.RunnerVM{q.candidate}, nil
}

func (q *rereadQueue) GetRunnerVM(context.Context, string) (store.RunnerVM, error) {
	q.reads++
	if q.reason != nil {
		return store.RunnerVM{}, q.reason
	}
	return q.answer, nil
}

func TestAReservationThatMovedUnderTheReadIsNotRevokedOnIt(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	candidate := expiredVM("claimed-away", now.Add(-time.Hour))
	other := expiredVM("somebody-else", now.Add(-time.Hour))

	for _, attempt := range []struct {
		name     string
		queue    *rereadQueue
		says     []string
		claimed  int
		complete bool
	}{
		{
			name:     "the row is gone by the time its turn comes",
			queue:    &rereadQueue{candidate: candidate, reason: store.ErrNotFound},
			claimed:  1,
			complete: true,
		},
		{
			name:  "the re-read cannot be made at all",
			queue: &rereadQueue{candidate: candidate, reason: errors.New("ledger unreachable")},
			says:  []string{"re-read expired reservation", candidate.ID, "ledger unreachable"},
		},
		{
			name:  "the re-read answers with another reservation",
			queue: &rereadQueue{candidate: candidate, answer: other},
			says:  []string{other.ID, candidate.ID},
		},
	} {
		revoker := &fakeRevoker{}
		report, err := reaperAt(t, now, attempt.queue, revoker).Reap(context.Background())

		if attempt.complete {
			if err != nil {
				t.Errorf("%s: an ordinary outcome was reported as an incomplete pass: %v", attempt.name, err)
			}
		} else {
			if !errors.Is(err, ErrUnclaimedIncomplete) {
				t.Errorf("%s: an unreadable answer did not stop the pass: %v", attempt.name, err)
			}
			for _, want := range attempt.says {
				if err == nil || !strings.Contains(err.Error(), want) {
					t.Errorf("%s: the refusal never mentions %q: %v", attempt.name, want, err)
				}
			}
		}
		if len(revoker.order) != 0 {
			t.Errorf("%s: a reservation was revoked on an answer the pass could not read: %v",
				attempt.name, revoker.order)
		}
		if report.Scanned != 1 || report.Reaped != 0 || report.Partial != 0 || report.Claimed != attempt.claimed {
			t.Errorf("%s: report = %+v", attempt.name, report)
		}
		if attempt.queue.reads != 1 {
			t.Errorf("%s: the candidate was re-read %d times", attempt.name, attempt.queue.reads)
		}
	}
}

// A pass it cannot run is refused on what it was handed, and the empty report
// it hands back is the truth about a pass that never started.
func TestAPassWithoutWhatItRunsOnIsRefusedBeforeTheQueueIsRead(t *testing.T) {
	now := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	sound := reaperAt(t, now, &fakeUnclaimedQueue{}, &fakeRevoker{})

	for _, attempt := range []struct {
		name   string
		reaper *UnclaimedReaper
		ctx    context.Context
	}{
		{"no reaper at all", nil, context.Background()},
		{"no queue", &UnclaimedReaper{revoker: &fakeRevoker{}}, context.Background()},
		{"no revoker", &UnclaimedReaper{queue: &fakeUnclaimedQueue{}}, context.Background()},
		{"no context", sound, nil},
	} {
		report, err := attempt.reaper.Reap(attempt.ctx)
		if err == nil {
			t.Errorf("%s: a pass ran on nothing: %+v", attempt.name, report)
			continue
		}
		if !strings.Contains(err.Error(), "invalid unclaimed reaper") {
			t.Errorf("%s: the refusal read %v", attempt.name, err)
		}
		if report.Scanned != 0 || report.Reaped != 0 || report.Claimed != 0 ||
			report.Partial != 0 || len(report.Steps) != 0 {
			t.Errorf("%s: a pass that never started still reported %+v", attempt.name, report)
		}
	}
}
