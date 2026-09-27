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

// leaseAt is a page row for a board with its own deadline, so a test can
// order two rows the way the read orders them.
func leaseAt(boardID, leaseID string, version uint64, expiresAt time.Time) store.ExpiredBoardLease {
	return store.ExpiredBoardLease{BoardID: boardID, LeaseID: leaseID, Holder: "holder-" + leaseID,
		ExpiresAt: expiresAt, Version: version}
}

func TestOneRowPerBoardLeavesADistinctPageAlone(t *testing.T) {
	page := []store.ExpiredBoardLease{lease("a", 3), lease("b", 9), lease("c", 4)}
	kept, duplicates := oneRowPerBoard(page)
	if duplicates != 0 {
		t.Fatalf("duplicates on a distinct page: got %d, want 0", duplicates)
	}
	if len(kept) != 3 {
		t.Fatalf("kept: got %d rows, want 3", len(kept))
	}
	for i, got := range kept {
		if got != page[i] {
			t.Fatalf("row %d changed: got %+v, want %+v", i, got, page[i])
		}
	}
}

func TestOneRowPerBoardIsAnIdentityOnShortPages(t *testing.T) {
	if kept, duplicates := oneRowPerBoard(nil); kept != nil || duplicates != 0 {
		t.Fatalf("empty page: got %+v, %d", kept, duplicates)
	}
	one := []store.ExpiredBoardLease{lease("a", 3)}
	kept, duplicates := oneRowPerBoard(one)
	if duplicates != 0 || len(kept) != 1 || kept[0] != one[0] {
		t.Fatalf("single row page: got %+v, %d", kept, duplicates)
	}
}

// The holder's active lease and a queued waiter's pending lease are both live
// rows on one board, and both come back when both deadlines have passed.
func TestOneRowPerBoardCollapsesAHolderAndItsQueuedWaiter(t *testing.T) {
	older := epoch.Add(-10 * time.Minute)
	newer := epoch.Add(-time.Minute)
	page := []store.ExpiredBoardLease{
		leaseAt("bench-1", "active", 7, older),
		leaseAt("bench-1", "pending", 7, newer),
	}
	kept, duplicates := oneRowPerBoard(page)
	if duplicates != 1 {
		t.Fatalf("duplicates: got %d, want 1", duplicates)
	}
	if len(kept) != 1 {
		t.Fatalf("kept: got %d rows, want 1", len(kept))
	}
	// The read orders by deadline, so the first row is the lease that has
	// been past its deadline longest, and that is the one kept.
	if kept[0].LeaseID != "active" || !kept[0].ExpiresAt.Equal(older) {
		t.Fatalf("kept the wrong row: got %+v", kept[0])
	}
}

func TestOneRowPerBoardKeepsEveryOtherBoardInOrder(t *testing.T) {
	page := []store.ExpiredBoardLease{
		leaseAt("a", "a-1", 2, epoch.Add(-9*time.Minute)),
		leaseAt("b", "b-1", 5, epoch.Add(-8*time.Minute)),
		leaseAt("a", "a-2", 2, epoch.Add(-7*time.Minute)),
		leaseAt("c", "c-1", 6, epoch.Add(-6*time.Minute)),
		leaseAt("b", "b-2", 5, epoch.Add(-5*time.Minute)),
	}
	kept, duplicates := oneRowPerBoard(page)
	if duplicates != 2 {
		t.Fatalf("duplicates: got %d, want 2", duplicates)
	}
	want := []string{"a", "b", "c"}
	if len(kept) != len(want) {
		t.Fatalf("kept: got %d rows, want %d", len(kept), len(want))
	}
	for i, got := range kept {
		if got.BoardID != want[i] {
			t.Fatalf("row %d: got board %s, want %s", i, got.BoardID, want[i])
		}
	}
	if kept[0].LeaseID != "a-1" || kept[1].LeaseID != "b-1" {
		t.Fatalf("kept rows are not the oldest per board: %+v", kept)
	}
}

func TestOneRowPerBoardDoesNotMutateThePageItWasGiven(t *testing.T) {
	page := []store.ExpiredBoardLease{lease("a", 1), lease("a", 1), lease("b", 2)}
	before := make([]store.ExpiredBoardLease, len(page))
	copy(before, page)
	if _, duplicates := oneRowPerBoard(page); duplicates != 1 {
		t.Fatalf("duplicates: got %d, want 1", duplicates)
	}
	for i := range page {
		if page[i] != before[i] {
			t.Fatalf("row %d of the caller's page changed: got %+v, want %+v", i, page[i], before[i])
		}
	}
}

// One stuck board must read as one board, and the second row must not cost a
// board lock to learn the version it was read at is stale.
func TestAPassTicksADuplicatedBoardOnce(t *testing.T) {
	older := epoch.Add(-10 * time.Minute)
	newer := epoch.Add(-time.Minute)
	boards := &fakeBoards{
		expired: []store.ExpiredBoardLease{
			leaseAt("bench-1", "active", 7, older),
			leaseAt("bench-1", "pending", 7, newer),
		},
		answer: func(string) ([]board.Event, error) { return expiredEvents(), nil },
	}
	sweeper, err := New(boards, 10)
	if err != nil {
		t.Fatalf("new: %v", err)
	}
	report, err := sweeper.Pass(context.Background(), epoch)
	if err != nil {
		t.Fatalf("pass: %v", err)
	}
	if report != (Report{Found: 1, Reclaimed: 1, Duplicated: 1}) {
		t.Fatalf("report: got %+v", report)
	}
	if len(boards.ticks) != 1 {
		t.Fatalf("ticks: got %+v, want one", boards.ticks)
	}
	if boards.ticks[0].boardID != "bench-1" || boards.ticks[0].version != 7 {
		t.Fatalf("tick: got %+v", boards.ticks[0])
	}
}

// Without the collapse this pass would read "found 2, reclaimed 1, already
// reclaimed 1" for one board and one reclamation.
func TestADuplicatedBoardIsNotCountedAsOvertaken(t *testing.T) {
	ticked := 0
	boards := &fakeBoards{
		expired: []store.ExpiredBoardLease{lease("a", 3), lease("a", 3)},
		answer: func(string) ([]board.Event, error) {
			ticked++
			if ticked > 1 {
				return nil, fmt.Errorf("%w: board version 4, expected 3", store.ErrConflict)
			}
			return expiredEvents(), nil
		},
	}
	sweeper, _ := New(boards, 10)
	report, err := sweeper.Pass(context.Background(), epoch)
	if err != nil {
		t.Fatalf("pass: %v", err)
	}
	if report.Overtaken != 0 {
		t.Fatalf("a duplicate row was counted as an overtaken board: %+v", report)
	}
	if report != (Report{Found: 1, Reclaimed: 1, Duplicated: 1}) {
		t.Fatalf("report: got %+v", report)
	}
}

// Found is the number of boards the pass is acting on, so the three outcomes
// still account for it exactly once the duplicates are set aside.
func TestFoundCountsBoardsNotRows(t *testing.T) {
	boards := &fakeBoards{
		expired: []store.ExpiredBoardLease{lease("a", 1), lease("a", 1), lease("b", 2), lease("a", 1)},
		answer:  func(string) ([]board.Event, error) { return expiredEvents(), nil },
	}
	sweeper, _ := New(boards, 10)
	report, err := sweeper.Pass(context.Background(), epoch)
	if err != nil {
		t.Fatalf("pass: %v", err)
	}
	if report.Found != report.Reclaimed+report.Overtaken+report.Failed {
		t.Fatalf("the outcomes do not account for what was found: %+v", report)
	}
	if report != (Report{Found: 2, Reclaimed: 2, Duplicated: 2}) {
		t.Fatalf("report: got %+v", report)
	}
}

func TestADuplicatedPageIsWorthReading(t *testing.T) {
	report := Report{Found: 1, Reclaimed: 1, Duplicated: 1}
	if !report.Notable() {
		t.Fatal("a pass that set rows aside stayed silent")
	}
	line := report.String()
	if !strings.Contains(line, "found 1") || !strings.Contains(line, "duplicate row(s) 1") {
		t.Fatalf("report line %q does not say what it set aside", line)
	}
}

// A quiet pass is still quiet, and the line a quiet bench would print must
// not start claiming duplicates it did not see.
func TestAQuietPassStillSaysNothingAboutDuplicates(t *testing.T) {
	if (Report{}).Notable() {
		t.Fatal("an empty pass asked to be printed")
	}
	if !strings.Contains((Report{}).String(), "duplicate row(s) 0") {
		t.Fatalf("report line %q", (Report{}).String())
	}
}

// The duplicates are counted off the page the ledger handed back, so a failed
// read reports nothing about them.
func TestAFailedReadReportsNoDuplicates(t *testing.T) {
	boards := &fakeBoards{readErr: fmt.Errorf("%w: expired lease query", store.ErrUnavailable)}
	sweeper, _ := New(boards, 10)
	report, err := sweeper.Pass(context.Background(), epoch)
	if !errors.Is(err, store.ErrUnavailable) {
		t.Fatalf("read failure: got %v", err)
	}
	if report != (Report{}) {
		t.Fatalf("report: got %+v", report)
	}
}

// A cancelled pass reports the duplicates it had already set aside, for the
// same reason it reports the failures it had already collected: the counts
// are what happened.
func TestACancelledPassKeepsTheDuplicatesItSetAside(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	boards := &fakeBoards{
		expired: []store.ExpiredBoardLease{lease("a", 3), lease("a", 3), lease("b", 9)},
		answer:  func(string) ([]board.Event, error) { cancel(); return expiredEvents(), nil },
	}
	sweeper, _ := New(boards, 10)
	report, err := sweeper.Pass(ctx, epoch)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("cancelled: got %v", err)
	}
	if report.Duplicated != 1 || report.Found != 2 || report.Reclaimed != 1 {
		t.Fatalf("report: got %+v", report)
	}
	if len(boards.ticks) != 1 {
		t.Fatalf("ticks: got %+v, want one", boards.ticks)
	}
}
