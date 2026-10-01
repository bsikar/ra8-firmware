//go:build integration

package store

import (
	"context"
	"testing"
	"time"
)

// What the sweep's read actually finds.
//
// ExpiredBoardLeases is the read behind the board sweep: TickBoard is the
// transition that reclaims a board whose holder died, and nothing asks an
// idle board anything, so without this read a bench that goes quiet at
// minute two of an hour lease stays held until some unrelated request
// happens to arrive. The argument refusals are already held without a
// database by what_the_board_doors_judge_before_the_database_test.go. What
// was never held is the query: which leases it finds, in what order, and
// whether the row it hands the tick is one the tick can actually apply under.
//
// Every assertion here is about boards this test planted. The read is global
// by design, so a count over the whole table would be a claim about whatever
// else the package left behind.

// expiredAmong picks this test's own boards out of a global sweep page,
// keeping the order the read returned them in.
func expiredAmong(page []ExpiredBoardLease, boardIDs ...string) []ExpiredBoardLease {
	wanted := make(map[string]struct{}, len(boardIDs))
	for _, id := range boardIDs {
		wanted[id] = struct{}{}
	}
	mine := make([]ExpiredBoardLease, 0, len(boardIDs))
	for _, lease := range page {
		if _, ours := wanted[lease.BoardID]; ours {
			mine = append(mine, lease)
		}
	}
	return mine
}

func TestIntegrationTheSweepFindsTheBoardsWhoseHolderNeverCameBack(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	// Granting in the past is what makes these leases findable without
	// waiting an hour, and it puts them ahead of anything else in the table
	// under the read's own ordering, so a page bound cannot hide them.
	base := time.Now().UTC().Add(-72 * time.Hour).Truncate(time.Second)
	earlyBoard, early, _, _ := leasedUnder(t, ctx, st, pool, base)
	lateBoard, late, _, _ := leasedUnder(t, ctx, st, pool, base.Add(10*time.Minute))

	page, err := st.ExpiredBoardLeases(ctx, time.Now().UTC(), maxExpiredLeasePage)
	if err != nil {
		t.Fatalf("the sweep read failed: %v", err)
	}
	mine := expiredAmong(page, earlyBoard, lateBoard)
	if len(mine) != 2 {
		t.Fatalf("want both expired boards, got %d: %+v", len(mine), mine)
	}

	// Oldest deadline first, because the sweep reclaims in bites and the
	// board that has been stuck longest is the one to free next.
	if mine[0].BoardID != earlyBoard || mine[1].BoardID != lateBoard {
		t.Fatalf("expired boards are not ordered by deadline: %+v", mine)
	}

	// The row has to be one the tick can apply under: its own lease, the
	// holder to tell an operator who died, and a snapshot version past zero,
	// since a tick expecting version zero would re-earn its conflict forever.
	found := mine[0]
	if found.Holder != early.ID() {
		t.Fatalf("lease is reported under holder %q", found.Holder)
	}
	if found.LeaseID == "" || found.LeaseID == mine[1].LeaseID {
		t.Fatalf("lease identity is wrong: %+v", mine)
	}
	if found.Version == 0 {
		t.Fatalf("board %s was handed to the tick at version 0", found.BoardID)
	}
	if found.ExpiresAt.IsZero() || found.ExpiresAt.Location() != time.UTC {
		t.Fatalf("deadline is not a UTC instant: %+v", found)
	}
	if !mine[1].ExpiresAt.After(found.ExpiresAt) {
		t.Fatalf("the later grant does not expire later: %+v", mine)
	}
	if mine[1].Holder != late.ID() {
		t.Fatalf("the later board is reported under holder %q", mine[1].Holder)
	}

	// A board whose deadline has not arrived is not the sweep's to reclaim.
	// Read a second before the earlier board runs out: it must find neither,
	// which is also what proves the first read was about the deadline rather
	// than about the lease merely existing.
	soon, err := st.ExpiredBoardLeases(ctx, found.ExpiresAt.Add(-time.Second), maxExpiredLeasePage)
	if err != nil {
		t.Fatalf("the early sweep read failed: %v", err)
	}
	if held := expiredAmong(soon, earlyBoard, lateBoard); len(held) != 0 {
		t.Fatalf("a lease inside its deadline was offered for reclaim: %+v", held)
	}

	// The deadline itself counts as passed, so a lease is reclaimable the
	// instant it runs out rather than a tick later.
	at, err := st.ExpiredBoardLeases(ctx, found.ExpiresAt, maxExpiredLeasePage)
	if err != nil {
		t.Fatalf("the boundary sweep read failed: %v", err)
	}
	if due := expiredAmong(at, earlyBoard); len(due) != 1 {
		t.Fatalf("a lease exactly at its deadline was not reclaimable: %+v", due)
	}
}

func TestIntegrationTheSweepLeavesAFinishedLeaseAlone(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	// Both boards are granted now, so the release below happens inside the
	// lease, the way a holder actually gives a board back. The sweep is then
	// read from past both deadlines: a released lease whose deadline has
	// since passed is exactly the row a broken state filter would offer.
	now := time.Now().UTC().Truncate(time.Second)
	releasedBoard, human, waiter, active := leasedUnder(t, ctx, st, pool, now)
	stillHeld, _, _, _ := leasedUnder(t, ctx, st, pool, now)
	afterBoth := now.Add(2 * time.Hour)

	before, err := st.ExpiredBoardLeases(ctx, afterBoth, maxExpiredLeasePage)
	if err != nil {
		t.Fatalf("the sweep read failed: %v", err)
	}
	if len(expiredAmong(before, releasedBoard, stillHeld)) != 2 {
		t.Fatalf("both boards should be past their deadline by then")
	}

	releaseHeldBoard(t, ctx, st, human, active, waiter, now.Add(time.Minute))

	after, err := st.ExpiredBoardLeases(ctx, afterBoth, maxExpiredLeasePage)
	if err != nil {
		t.Fatalf("the sweep read after release failed: %v", err)
	}

	// Reclaiming a finished lease would tick a board that is already free,
	// under a version that belongs to whoever holds it now.
	if gone := expiredAmong(after, releasedBoard); len(gone) != 0 {
		t.Fatalf("a released lease is still offered for reclaim: %+v", gone)
	}
	if still := expiredAmong(after, stillHeld); len(still) != 1 {
		t.Fatalf("releasing one board hid another still held: %+v", still)
	}

	// A page bound is honoured, which is what keeps one pass from holding
	// the sweep behind a long backlog.
	one, err := st.ExpiredBoardLeases(ctx, afterBoth, 1)
	if err != nil {
		t.Fatalf("a single-lease page failed: %v", err)
	}
	if len(one) != 1 {
		t.Fatalf("a page of one returned %d rows", len(one))
	}
}
