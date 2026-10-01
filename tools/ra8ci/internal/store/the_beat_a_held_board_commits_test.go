//go:build integration

package store

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/jackc/pgx/v5/pgxpool"
)

// The one board write that records state and emits no event.
//
// Every other command owes an event for every version it advances, and the
// transition path is built around that. A heartbeat has none of it by design,
// so it gets its own write, and the cost of a second write path is a second
// way to change a board. These pin what that second way may and may not do
// once it reaches the database: the beat has to survive a restart, because a
// beat kept in memory only loses exactly the evidence a crashed holder is
// judged by; and it has to leave everything else alone, because "event-free"
// must not become a way to move a board without a record.
//
// boardWriteFor and livenessOnly are already held by board_heartbeat_test.go
// over snapshot pairs. What those cannot see is the commit: whether the row
// actually moved, whether anything else moved with it, and whether the
// compare-and-set still refuses a stale writer. That is all below.

// heldBoard brings a board to Active with a live lease and returns the actors,
// the lease and the snapshot, which is the state a heartbeat needs to exist at
// all.
func heldBoard(t *testing.T, ctx context.Context, s *Store, pool *pgxpool.Pool,
	now time.Time) (string, BoardActor, BoardActor, board.Waiter, board.Snapshot) {
	t.Helper()
	boardID := "board-" + mustID(t)
	human := boardTestActor(t, ctx, s, pool, boardID, "human", "board_human")
	agent := boardTestActor(t, ctx, s, pool, boardID, "board_agent", "board_agent")
	waiter := board.Waiter{ID: mustID(t), LeaseID: mustID(t), Holder: human.ID(),
		Class: board.ClassHuman, Reason: "liveness fixture", Duration: time.Hour}
	granted, _, err := s.ApplyBoardCommand(ctx, human, board.Enqueue{Waiter: waiter}, 0, nil, nil, now)
	if err != nil {
		t.Fatalf("enqueue: %v", err)
	}
	active, _, err := s.ApplyBoardCommand(ctx, agent, board.AcknowledgeGrant{
		LeaseID: waiter.LeaseID, Generation: granted.Generation,
		InstalledGeneration: granted.Generation,
	}, granted.Version, nil, nil, now.Add(time.Second))
	if err != nil || active.Phase != board.Active {
		t.Fatalf("acknowledge grant: %+v %v", active, err)
	}
	return boardID, human, agent, waiter, active
}

// boardLedger is what a write must not disturb: the event and audit history,
// and the projected lease.
type boardLedger struct {
	events, audits int
	leaseState     string
	leaseExpires   time.Time
	leaseVersion   int64
}

func readBoardLedger(t *testing.T, ctx context.Context, pool *pgxpool.Pool,
	boardID, leaseID string) boardLedger {
	t.Helper()
	var l boardLedger
	if err := pool.QueryRow(ctx,
		`SELECT COUNT(*) FROM board_events WHERE board_id=$1`, boardID).Scan(&l.events); err != nil {
		t.Fatalf("count events: %v", err)
	}
	if err := pool.QueryRow(ctx,
		`SELECT COUNT(*) FROM audit WHERE target_type='board' AND target_id=$1`,
		boardID).Scan(&l.audits); err != nil {
		t.Fatalf("count audit: %v", err)
	}
	if err := pool.QueryRow(ctx,
		`SELECT state, expires_at, version FROM board_leases WHERE id=$1`, leaseID).
		Scan(&l.leaseState, &l.leaseExpires, &l.leaseVersion); err != nil {
		t.Fatalf("read lease: %v", err)
	}
	return l
}

func TestIntegrationABeatIsRecordedAndNothingElseIs(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	now := time.Now().UTC().Truncate(time.Second)
	boardID, human, _, waiter, active := heldBoard(t, ctx, s, pool, now)

	before := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)
	beatAt := now.Add(30 * time.Second)
	beaten, events, err := s.ApplyBoardCommand(ctx, human, board.HolderHeartbeat{
		LeaseID: waiter.LeaseID, Generation: active.Generation,
	}, active.Version, nil, nil, beatAt)
	if err != nil {
		t.Fatalf("heartbeat: %v", err)
	}
	if len(events) != 0 {
		t.Fatalf("the beat returned %d events, want none: it is the event-free write", len(events))
	}
	if beaten.Version != active.Version+1 {
		t.Fatalf("beat left the version at %d, want %d", beaten.Version, active.Version+1)
	}
	if !beaten.Lease.LastHeartbeatAt.Equal(beatAt) {
		t.Fatalf("beat recorded %v, want %v", beaten.Lease.LastHeartbeatAt, beatAt)
	}

	// The whole reason this path exists rather than keeping the beat in
	// memory: a reader that never saw the call has to see the beat.
	stored, err := s.GetBoard(ctx, boardID)
	if err != nil {
		t.Fatalf("re-read board: %v", err)
	}
	if stored.Version != beaten.Version {
		t.Fatalf("stored version %d, want the beaten %d", stored.Version, beaten.Version)
	}
	if stored.Lease == nil || !stored.Lease.LastHeartbeatAt.Equal(beatAt) {
		t.Fatalf("stored beat is %+v, want %v", stored.Lease, beatAt)
	}
	if stored.Phase != board.Active {
		t.Fatalf("a beat moved the phase to %s, want it still Active", stored.Phase)
	}

	// And nothing else moved. An event or audit row here would be the
	// manufactured record this path exists to avoid; a changed lease row
	// would mean a beat can edit a grant.
	after := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)
	if after.events != before.events {
		t.Fatalf("the beat wrote %d board events, want none", after.events-before.events)
	}
	if after.audits != before.audits {
		t.Fatalf("the beat wrote %d audit rows, want none", after.audits-before.audits)
	}
	if after.leaseState != before.leaseState || !after.leaseExpires.Equal(before.leaseExpires) ||
		after.leaseVersion != before.leaseVersion {
		t.Fatalf("the beat changed the projected lease: %+v -> %+v", before, after)
	}
}

func TestIntegrationABeatThatObservesNothingWritesNothing(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	now := time.Now().UTC().Truncate(time.Second)
	boardID, human, _, waiter, active := heldBoard(t, ctx, s, pool, now)

	// The grant is itself an observation of the holder, so a beat stamped
	// before it makes the holder no more recently seen than it already is.
	// Such a beat has to be a no-op rather than a version bump, because a
	// version that moves with neither an event nor a later beat is exactly
	// the defect boardWriteFor refuses.
	before := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)
	stale, events, err := s.ApplyBoardCommand(ctx, human, board.HolderHeartbeat{
		LeaseID: waiter.LeaseID, Generation: active.Generation,
	}, active.Version, nil, nil, now.Add(-time.Minute))
	if err != nil {
		t.Fatalf("out-of-order beat: %v", err)
	}
	if len(events) != 0 {
		t.Fatalf("out-of-order beat returned %d events, want none", len(events))
	}
	if stale.Version != active.Version {
		t.Fatalf("out-of-order beat moved the version to %d, want it left at %d",
			stale.Version, active.Version)
	}
	stored, err := s.GetBoard(ctx, boardID)
	if err != nil {
		t.Fatalf("re-read board: %v", err)
	}
	if stored.Version != active.Version {
		t.Fatalf("stored version moved to %d on a beat that observed nothing", stored.Version)
	}
	if !stored.Lease.LastHeartbeatAt.Equal(active.Lease.LastHeartbeatAt) {
		t.Fatalf("an earlier beat moved the observation back to %v, want %v",
			stored.Lease.LastHeartbeatAt, active.Lease.LastHeartbeatAt)
	}
	after := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)
	if after != before {
		t.Fatalf("a beat that observed nothing still wrote: %+v -> %+v", before, after)
	}
}

func TestIntegrationBeatsAdvanceOneVersionEachAndTheLastOneStands(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	now := time.Now().UTC().Truncate(time.Second)
	boardID, human, _, waiter, active := heldBoard(t, ctx, s, pool, now)

	// A held board beats for as long as it is held, so the path has to be
	// repeatable: each beat one version, each one visible to the next
	// reader, and no event history accumulating behind them.
	before := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)
	snapshot := active
	var last time.Time
	for i := 1; i <= 4; i++ {
		last = now.Add(time.Duration(i) * 30 * time.Second)
		next, _, err := s.ApplyBoardCommand(ctx, human, board.HolderHeartbeat{
			LeaseID: waiter.LeaseID, Generation: snapshot.Generation,
		}, snapshot.Version, nil, nil, last)
		if err != nil {
			t.Fatalf("beat %d: %v", i, err)
		}
		if next.Version != snapshot.Version+1 {
			t.Fatalf("beat %d moved the version %d -> %d, want one step",
				i, snapshot.Version, next.Version)
		}
		snapshot = next
	}
	stored, err := s.GetBoard(ctx, boardID)
	if err != nil {
		t.Fatalf("re-read board: %v", err)
	}
	if stored.Version != active.Version+4 {
		t.Fatalf("four beats left the version at %d, want %d", stored.Version, active.Version+4)
	}
	if !stored.Lease.LastHeartbeatAt.Equal(last) {
		t.Fatalf("the last beat did not stand: %v, want %v", stored.Lease.LastHeartbeatAt, last)
	}
	after := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)
	if after.events != before.events || after.audits != before.audits {
		t.Fatalf("four beats wrote %d events and %d audit rows, want none of either",
			after.events-before.events, after.audits-before.audits)
	}
}

func TestIntegrationABeatCannotOverwriteABoardThatMovedUnderneathIt(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	now := time.Now().UTC().Truncate(time.Second)
	boardID, human, _, waiter, active := heldBoard(t, ctx, s, pool, now)

	// The compare-and-set is the same one the transition path uses, and it is
	// what keeps the cheaper write from being the weaker one: a beat holding a
	// stale version must not land on a board someone else moved.
	moved, _, err := s.ApplyBoardCommand(ctx, human, board.HolderHeartbeat{
		LeaseID: waiter.LeaseID, Generation: active.Generation,
	}, active.Version, nil, nil, now.Add(30*time.Second))
	if err != nil {
		t.Fatalf("first beat: %v", err)
	}
	before := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)

	_, _, err = s.ApplyBoardCommand(ctx, human, board.HolderHeartbeat{
		LeaseID: waiter.LeaseID, Generation: active.Generation,
	}, active.Version, nil, nil, now.Add(60*time.Second))
	if !errors.Is(err, ErrConflict) {
		t.Fatalf("a beat on a stale version answered %v, want ErrConflict", err)
	}

	stored, err := s.GetBoard(ctx, boardID)
	if err != nil {
		t.Fatalf("re-read board: %v", err)
	}
	if stored.Version != moved.Version {
		t.Fatalf("the refused beat still moved the version to %d, want %d",
			stored.Version, moved.Version)
	}
	if !stored.Lease.LastHeartbeatAt.Equal(moved.Lease.LastHeartbeatAt) {
		t.Fatalf("the refused beat still recorded %v, want %v",
			stored.Lease.LastHeartbeatAt, moved.Lease.LastHeartbeatAt)
	}

	// The board did not move, but the attempt is not forgotten: a writer
	// holding a stale version is audited even though it changed nothing, so
	// a client beating against a board it has lost leaves a trace. No board
	// event, because nothing happened to the board.
	after := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)
	if after.events != before.events {
		t.Fatalf("a stale beat wrote %d board events, want none", after.events-before.events)
	}
	if after.audits != before.audits+1 {
		t.Fatalf("a stale beat wrote %d audit rows, want exactly the one refusal",
			after.audits-before.audits)
	}
	if after.leaseState != before.leaseState || !after.leaseExpires.Equal(before.leaseExpires) ||
		after.leaseVersion != before.leaseVersion {
		t.Fatalf("a stale beat changed the projected lease: %+v -> %+v", before, after)
	}
}

func TestIntegrationADeniedBeatIsNotAnEventFreeWrite(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// A beat naming a lease or generation the board is not holding is denied
	// by the reducer rather than refused ahead of it, and a denial is one of
	// the audited set. So it takes the ORDINARY transition path: it advances
	// the version and leaves an ActionDenied event behind. That is the
	// property worth holding on to, because it is what keeps the event-free
	// write reachable only for a beat that was actually accepted. A denial
	// routed down the liveness path instead would move a board with no record
	// of who tried.
	//
	// Each case gets its own board: a denial moves the version, so sharing one
	// would leave the next case holding a stale version and refused for an
	// entirely different reason.
	for _, c := range []struct {
		name    string
		command func(waiter board.Waiter, active board.Snapshot) board.HolderHeartbeat
	}{
		{"another lease", func(_ board.Waiter, active board.Snapshot) board.HolderHeartbeat {
			return board.HolderHeartbeat{LeaseID: mustID(t), Generation: active.Generation}
		}},
		{"another generation", func(waiter board.Waiter, active board.Snapshot) board.HolderHeartbeat {
			return board.HolderHeartbeat{LeaseID: waiter.LeaseID, Generation: active.Generation + 1}
		}},
	} {
		t.Run(c.name, func(t *testing.T) {
			now := time.Now().UTC().Truncate(time.Second)
			boardID, human, _, waiter, active := heldBoard(t, ctx, s, pool, now)
			before := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)

			if _, _, err := s.ApplyBoardCommand(ctx, human, c.command(waiter, active),
				active.Version, nil, nil, now.Add(30*time.Second)); err == nil {
				t.Fatal("a beat for a lease the board is not holding was accepted")
			}

			stored, err := s.GetBoard(ctx, boardID)
			if err != nil {
				t.Fatalf("re-read board: %v", err)
			}
			if stored.Version != active.Version+1 {
				t.Fatalf("a denied beat left the version at %d, want %d: a denial is a transition",
					stored.Version, active.Version+1)
			}
			after := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)
			if after.events != before.events+1 {
				t.Fatalf("a denied beat wrote %d board events, want the one denial",
					after.events-before.events)
			}
			if after.audits != before.audits+1 {
				t.Fatalf("a denied beat wrote %d audit rows, want the one denial",
					after.audits-before.audits)
			}
			var kind string
			if err := pool.QueryRow(ctx, `SELECT kind FROM board_events WHERE board_id=$1
				ORDER BY event_seq DESC LIMIT 1`, boardID).Scan(&kind); err != nil {
				t.Fatalf("read the last event: %v", err)
			}
			if kind != string(board.ActionDenied) {
				t.Fatalf("the denied beat left a %q event, want %q", kind, board.ActionDenied)
			}

			// The holder is no more recently seen for having been denied.
			if stored.Lease == nil ||
				!stored.Lease.LastHeartbeatAt.Equal(active.Lease.LastHeartbeatAt) {
				t.Fatalf("a denied beat moved the observation to %+v, want %v",
					stored.Lease, active.Lease.LastHeartbeatAt)
			}
			if after.leaseState != before.leaseState ||
				!after.leaseExpires.Equal(before.leaseExpires) {
				t.Fatalf("a denied beat changed the projected lease: %+v -> %+v", before, after)
			}
		})
	}
}

func TestIntegrationAReleasedBoardHasNoHolderToBeAlive(t *testing.T) {
	s, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	now := time.Now().UTC().Truncate(time.Second)
	boardID, human, agent, waiter, active := heldBoard(t, ctx, s, pool, now)

	released := releaseHeldBoard(t, ctx, s, human, active, waiter, now.Add(time.Minute))
	if released.Lease != nil {
		t.Fatalf("release left a lease behind: %+v", released.Lease)
	}
	before := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID)

	// livenessOnly refuses an event-free write without a lease on both sides,
	// so a beat arriving after the holder let go has nothing to record.
	if _, _, err := s.ApplyBoardCommand(ctx, agent, board.HolderHeartbeat{
		LeaseID: waiter.LeaseID, Generation: active.Generation,
	}, released.Version, nil, nil, now.Add(2*time.Minute)); err == nil {
		t.Fatal("a released board accepted a beat")
	}
	stored, err := s.GetBoard(ctx, boardID)
	if err != nil {
		t.Fatalf("re-read board: %v", err)
	}
	if stored.Lease != nil {
		t.Fatalf("a beat on a released board recreated a lease: %+v", stored.Lease)
	}
	if after := readBoardLedger(t, ctx, pool, boardID, waiter.LeaseID); after.leaseState != before.leaseState {
		t.Fatalf("a beat on a released board revived the projected lease: %q -> %q",
			before.leaseState, after.leaseState)
	}
}

// releaseHeldBoard releases the lease with a verified neutral receipt.
func releaseHeldBoard(t *testing.T, ctx context.Context, s *Store, human BoardActor,
	active board.Snapshot, waiter board.Waiter, at time.Time) board.Snapshot {
	t.Helper()
	receipt := "private-test-receipt-" + mustID(t)
	challenge, err := s.IssueBoardNeutralChallenge(ctx, human, active.Version, "release")
	if err != nil {
		t.Fatalf("neutral challenge: %v", err)
	}
	released, _, err := s.ApplyBoardCommand(ctx, human, board.Release{
		LeaseID: waiter.LeaseID, Generation: active.Generation,
	}, active.Version, &NeutralSubmission{ChallengeID: challenge.ID, Receipt: []byte(receipt)},
		exactNeutralVerifier{challenge: challenge, receipt: receipt}, at)
	if err != nil {
		t.Fatalf("release: %v", err)
	}
	return released
}
