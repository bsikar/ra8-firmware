// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardsweep

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

var epoch = time.Date(2026, 9, 24, 12, 0, 0, 0, time.UTC)

type tick struct {
	boardID string
	version uint64
	now     time.Time
}

type fakeBoards struct {
	expired []store.ExpiredBoardLease
	readErr error
	limit   int
	ticks   []tick
	answer  func(boardID string) ([]board.Event, error)
}

func (f *fakeBoards) ExpiredBoardLeases(_ context.Context, _ time.Time, limit int) ([]store.ExpiredBoardLease, error) {
	f.limit = limit
	if f.readErr != nil {
		return nil, f.readErr
	}
	return f.expired, nil
}

func (f *fakeBoards) TickBoard(_ context.Context, boardID string, expectedVersion uint64, now time.Time) (board.Snapshot, []board.Event, error) {
	f.ticks = append(f.ticks, tick{boardID: boardID, version: expectedVersion, now: now})
	if f.answer == nil {
		return board.Snapshot{}, nil, nil
	}
	events, err := f.answer(boardID)
	return board.Snapshot{}, events, err
}

func lease(id string, version uint64) store.ExpiredBoardLease {
	return store.ExpiredBoardLease{BoardID: id, LeaseID: "lease-" + id, Holder: "holder-" + id,
		ExpiresAt: epoch.Add(-time.Minute), Version: version}
}

func expiredEvents() []board.Event {
	return []board.Event{{Kind: board.LeaseExpired}}
}

func TestNewRefusesASweepWithNoLedger(t *testing.T) {
	if _, err := New(nil, 10); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("nil ledger: got %v, want ErrInvalid", err)
	}
	if _, err := New(&fakeBoards{}, -1); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("negative batch: got %v, want ErrInvalid", err)
	}
}

func TestPassTicksEveryExpiredBoardUnderTheVersionItWasRead(t *testing.T) {
	boards := &fakeBoards{expired: []store.ExpiredBoardLease{lease("a", 3), lease("b", 9)},
		answer: func(string) ([]board.Event, error) { return expiredEvents(), nil }}
	sweeper, err := New(boards, 0)
	if err != nil {
		t.Fatalf("new: %v", err)
	}
	report, err := sweeper.Pass(context.Background(), epoch)
	if err != nil {
		t.Fatalf("pass: %v", err)
	}
	if report != (Report{Found: 2, Reclaimed: 2}) {
		t.Fatalf("report: got %+v", report)
	}
	if boards.limit != defaultBatch {
		t.Fatalf("batch: got %d, want %d", boards.limit, defaultBatch)
	}
	want := []tick{{boardID: "a", version: 3, now: epoch}, {boardID: "b", version: 9, now: epoch}}
	if len(boards.ticks) != len(want) {
		t.Fatalf("ticks: got %+v", boards.ticks)
	}
	for i, got := range boards.ticks {
		if got.boardID != want[i].boardID || got.version != want[i].version || !got.now.Equal(want[i].now) {
			t.Fatalf("tick %d: got %+v, want %+v", i, got, want[i])
		}
	}
}

// A board that moved between the read and the tick was expired by whatever
// moved it, because every transition expires first. That is not a failure.
func TestPassCountsAVersionConflictAsOvertaken(t *testing.T) {
	boards := &fakeBoards{expired: []store.ExpiredBoardLease{lease("a", 3)},
		answer: func(string) ([]board.Event, error) {
			return nil, fmt.Errorf("%w: board version 4, expected 3", store.ErrConflict)
		}}
	sweeper, _ := New(boards, 10)
	report, err := sweeper.Pass(context.Background(), epoch)
	if err != nil {
		t.Fatalf("pass: %v", err)
	}
	if report != (Report{Found: 1, Overtaken: 1}) {
		t.Fatalf("report: got %+v", report)
	}
}

// Reclaimed means this pass expired it, read off the events, not off the
// phase the board happens to be in afterwards.
func TestPassCountsATickThatExpiredNothingAsOvertaken(t *testing.T) {
	boards := &fakeBoards{expired: []store.ExpiredBoardLease{lease("a", 3)},
		answer: func(string) ([]board.Event, error) {
			return []board.Event{{Kind: board.GrantCreated}}, nil
		}}
	sweeper, _ := New(boards, 10)
	report, err := sweeper.Pass(context.Background(), epoch)
	if err != nil {
		t.Fatalf("pass: %v", err)
	}
	if report != (Report{Found: 1, Overtaken: 1}) {
		t.Fatalf("report: got %+v", report)
	}
}

// The boards behind a bad row are exactly the ones the sweep exists to
// reclaim, so one failure never ends the pass.
func TestPassKeepsGoingPastOneFailedBoard(t *testing.T) {
	boards := &fakeBoards{expired: []store.ExpiredBoardLease{lease("a", 1), lease("b", 2), lease("c", 3)},
		answer: func(boardID string) ([]board.Event, error) {
			if boardID == "b" {
				return nil, errors.New("board lock: connection reset")
			}
			return expiredEvents(), nil
		}}
	sweeper, _ := New(boards, 10)
	report, err := sweeper.Pass(context.Background(), epoch)
	if err == nil {
		t.Fatal("pass: want the failure reported")
	}
	if report != (Report{Found: 3, Reclaimed: 2, Failed: 1}) {
		t.Fatalf("report: got %+v", report)
	}
	if len(boards.ticks) != 3 {
		t.Fatalf("ticks: got %d, want 3", len(boards.ticks))
	}
	if !strings.Contains(err.Error(), "reclaim board b") {
		t.Fatalf("error: got %q, want it to name the board that failed", err)
	}
}

func TestPassReportsAFailedRead(t *testing.T) {
	boards := &fakeBoards{readErr: fmt.Errorf("%w: expired lease query", store.ErrUnavailable)}
	sweeper, _ := New(boards, 10)
	report, err := sweeper.Pass(context.Background(), epoch)
	if !errors.Is(err, store.ErrUnavailable) {
		t.Fatalf("read failure: got %v, want ErrUnavailable", err)
	}
	if report != (Report{}) {
		t.Fatalf("report: got %+v", report)
	}
	if len(boards.ticks) != 0 {
		t.Fatalf("ticks after a failed read: got %d, want 0", len(boards.ticks))
	}
}

func TestPassStopsWhenTheContextIsCancelled(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	boards := &fakeBoards{expired: []store.ExpiredBoardLease{lease("a", 1), lease("b", 2)},
		answer: func(string) ([]board.Event, error) { cancel(); return expiredEvents(), nil }}
	sweeper, _ := New(boards, 10)
	report, err := sweeper.Pass(ctx, epoch)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled: got %v, want context.Canceled", err)
	}
	if report.Reclaimed != 1 || len(boards.ticks) != 1 {
		t.Fatalf("report %+v after %d ticks, want the pass to stop at one", report, len(boards.ticks))
	}
}

func TestPassRefusesAClocklessSweep(t *testing.T) {
	sweeper, _ := New(&fakeBoards{}, 10)
	if _, err := sweeper.Pass(context.Background(), time.Time{}); !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("zero clock: got %v, want ErrInvalid", err)
	}
}

func TestPassOverAnEmptyLedgerTicksNothing(t *testing.T) {
	boards := &fakeBoards{}
	sweeper, _ := New(boards, 10)
	report, err := sweeper.Pass(context.Background(), epoch)
	if err != nil {
		t.Fatalf("pass: %v", err)
	}
	if report != (Report{}) || len(boards.ticks) != 0 {
		t.Fatalf("report %+v, ticks %d", report, len(boards.ticks))
	}
}

// *store.Store is what production passes. If it stops satisfying the ledger
// this package reads, the sweep goes dead and this fails at compile time.
func TestStoreSatisfiesTheSweptLedger(t *testing.T) {
	var _ Boards = (*store.Store)(nil)
}
