//go:build integration

package store

import (
	"context"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5/pgxpool"
)

// What the bench answers when the reaper asks who holds a board.
//
// ListLiveBoardLeasesByHolder is the read behind the unclaimed sequence's
// third step. The reaper is walked precisely when the plane's picture of a
// reservation has already gone wrong once, so this lookup decides whether a
// board gets taken away from work that is running fine. Its argument
// refusals are already held by unclaimed_lease_test.go against a nil store;
// what was never held is the query itself: which leases count as live, which
// holder they are read under, and whether the row comes back whole.

// leasedUnder grants one board and returns it held. The grant goes through
// the real command path rather than an INSERT, so the row read back is the
// row the plane actually writes, and the holder is the granting principal
// itself, which is the only holder the board will accept.
func leasedUnder(t *testing.T, ctx context.Context, s *Store, pool *pgxpool.Pool,
	now time.Time) (string, BoardActor, board.Waiter, board.Snapshot) {
	t.Helper()
	boardID := "board-" + mustID(t)
	human := boardTestActor(t, ctx, s, pool, boardID, "human", "board_human")
	agent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(),
		Class: board.ClassHuman, Reason: "live lease fixture", Duration: time.Hour}
	granted, _, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatalf("enqueue on %s: %v", boardID, err)
	}
	active, _, err := s.ApplyBoardCommand(ctx, agent, board.AcknowledgeGrant{
		LeaseID: waiter.LeaseID, Generation: granted.Generation,
		InstalledGeneration: granted.Generation,
	}, granted.Version, nil, nil, now.Add(time.Second))
	if err != nil || active.Phase != board.Active {
		t.Fatalf("acknowledge on %s: %+v %v", boardID, active, err)
	}
	return boardID, human, waiter, active
}

func TestIntegrationLiveLeasesAreReadUnderTheirOwnHolder(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	base := time.Now().UTC().Truncate(time.Second)

	boardID, human, waiter, _ := leasedUnder(t, ctx, st, pool, base)
	strangerBoard, stranger, _, _ := leasedUnder(t, ctx, st, pool, base.Add(time.Minute))

	held, err := st.ListLiveBoardLeasesByHolder(ctx, human.ID())
	if err != nil {
		t.Fatalf("live lease lookup failed: %v", err)
	}
	if len(held) != 1 {
		t.Fatalf("want the holder's one board, got %d: %+v", len(held), held)
	}

	// The row has to come back whole. An operator deciding whether to stop a
	// lease needs its own identity and the window it runs for, not a count,
	// and the reaper needs the board it is actually about.
	lease := held[0]
	if lease.BoardID != boardID {
		t.Fatalf("lease names board %q, want %q", lease.BoardID, boardID)
	}
	if lease.BoardID == strangerBoard || lease.HolderID == stranger.ID() {
		t.Fatal("another holder's board was reported as this holder's")
	}
	if lease.HolderID != human.ID() {
		t.Fatalf("lease is reported under %q", lease.HolderID)
	}
	if lease.ID != waiter.LeaseID {
		t.Fatalf("lease id %q is not the granted lease %q", lease.ID, waiter.LeaseID)
	}
	if lease.State != "active" {
		t.Fatalf("an acknowledged grant reads as %q", lease.State)
	}
	if lease.Generation < 1 {
		t.Fatalf("generation %d was not carried", lease.Generation)
	}
	if lease.Priority == "" {
		t.Fatalf("priority was not carried: %+v", lease)
	}
	if lease.GrantedAt.IsZero() || !lease.ExpiresAt.After(lease.GrantedAt) {
		t.Fatalf("the lease window was not carried: %+v", lease)
	}

	// Each holder is answered for separately, so the stranger's own lookup
	// still finds the stranger's board. A lookup that returned everything, or
	// nothing, would pass a one-holder assertion on its own.
	theirs, err := st.ListLiveBoardLeasesByHolder(ctx, stranger.ID())
	if err != nil || len(theirs) != 1 || theirs[0].BoardID != strangerBoard {
		t.Fatalf("the other holder's board did not read back: %+v, %v", theirs, err)
	}

	// A holder nobody has ever been is an empty answer, not an error: the
	// reaper asks about a reservation id and a runner name on every walk, and
	// the expected answer for a reservation nobody claimed is nothing at all.
	none, err := st.ListLiveBoardLeasesByHolder(ctx, "holder-"+mustID(t))
	if err != nil {
		t.Fatalf("an unknown holder was refused rather than answered: %v", err)
	}
	if len(none) != 0 {
		t.Fatalf("an unknown holder holds %d leases", len(none))
	}
}

func TestIntegrationAReleasedBoardIsNoLongerHeld(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	base := time.Now().UTC().Truncate(time.Second)

	boardID, human, waiter, active := leasedUnder(t, ctx, st, pool, base)

	before, err := st.ListLiveBoardLeasesByHolder(ctx, human.ID())
	if err != nil || len(before) != 1 || before[0].BoardID != boardID {
		t.Fatalf("the held board was not reported: %+v, %v", before, err)
	}

	// Releasing has to take the board out of the live set. This is the half
	// that decides whether the reaper may proceed: reading a finished lease
	// as live stops a walk that should have continued, and the reaper is
	// walked exactly when something has already gone wrong once.
	releaseHeldBoard(t, ctx, st, human, active, waiter, base.Add(time.Minute))

	after, err := st.ListLiveBoardLeasesByHolder(ctx, human.ID())
	if err != nil {
		t.Fatalf("lookup after release failed: %v", err)
	}
	if len(after) != 0 {
		t.Fatalf("a released lease is still reported as held: %+v", after)
	}
}
