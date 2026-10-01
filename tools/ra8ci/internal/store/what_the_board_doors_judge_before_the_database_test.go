// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"errors"
	"math"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// aHILCompletion is a well-formed terminal report for one attempt: everything
// the completion door judges before the transaction opens is right, so each
// case below can spoil exactly one thing.
func aHILCompletion() BoardHILCompletion {
	return BoardHILCompletion{
		AttemptID:  doorReservation,
		LeaseID:    doorOperation,
		Generation: 3,
		Result:     "failed",
		Steps:      []HILStep{{Key: "hil.alive", State: "failed"}},
	}
}

func aHeldBoard() board.Snapshot {
	return board.Snapshot{BoardID: doorBoard, Generation: 3,
		Lease: &board.Lease{ID: doorReservation, Generation: 3}}
}

// The peer authorizer fails SHUT. A plane that cannot reach its grant table
// answers ErrDenied, not ErrUnavailable: an unreachable grant is an ungranted
// peer, and reporting the outage instead would invite a caller to retry its
// way onto a board it was never granted.
func TestThePeerAuthorizerFailsShutRatherThanReportingAnOutage(t *testing.T) {
	unopened := &Store{}

	err := func() (err error) {
		_, err = unopened.AuthorizeBoardPeer(context.Background(), nil, "bsikar/ra8-firmware", doorBoard)
		return
	}()
	if !errors.Is(err, ErrDenied) {
		t.Fatalf("a peer authorized by an unopened plane: err %v, want ErrDenied", err)
	}
	if errors.Is(err, ErrUnavailable) || errors.Is(err, ErrInvalid) {
		t.Fatalf("the authorizer reported its own state instead of denying: %v", err)
	}

	// With a pool, a peer that presented no verified leaf is still denied at
	// the gate, so the database is never asked about it.
	if _, err := unreachablePlane(t).AuthorizeBoardPeer(context.Background(), nil, "bsikar/ra8-firmware", doorBoard); errors.Is(err, ErrUnavailable) {
		t.Fatalf("a peer with no verified leaf reached the database: %v", err)
	}
}

// A neutral challenge is derived for one board agent on one board for one
// purpose. The server wearing its own hat cannot ask for one: a challenge
// exists to prove a party other than the server observed the bench.
func TestANeutralChallengeNamesOneAgentBoardAndPurpose(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	agent := boardAgentOn(doorBoard)

	for _, refusal := range []struct {
		what    string
		actor   BoardActor
		version uint64
		purpose string
	}{
		{"an actor with no identity", BoardActor{kind: "board_agent", role: "board_agent", boardID: doorBoard}, 1, "release"},
		{"the server's own kind", boardCallerOn(doorBoard, "system", "system"), 1, "release"},
		{"an operator on no board", boardCallerOn("", "operator", "operator"), 1, "release"},
		{"a padded board identifier", boardCallerOn(" "+doorBoard, "board_agent", "board_agent"), 1, "release"},
		{"a version past the column bound", agent, uint64(math.MaxInt64) + 1, "release"},
		{"a purpose the challenge does not serve", agent, 1, "handoff"},
		{"no purpose at all", agent, 1, ""},
		{"a purpose in the wrong case", agent, 1, "Release"},
	} {
		_, err := plane.IssueBoardNeutralChallenge(ctx, refusal.actor, refusal.version, refusal.purpose)
		refusedBefore(t, refusal.what, err)
	}

	for _, accepted := range []struct {
		what    string
		version uint64
		purpose string
	}{
		{"a release challenge", 1, "release"},
		{"a recovery challenge", 1, "recovery"},
		{"a version exactly at the column bound", math.MaxInt64, "release"},
		{"an unversioned request", 0, "release"},
	} {
		_, err := plane.IssueBoardNeutralChallenge(ctx, agent, accepted.version, accepted.purpose)
		reached(t, accepted.what, err)
	}
}

// The expired-lease sweep reads zero as "the default page", where the two
// client-facing page doors read zero as "no page at all". The difference is
// who is asking: this door is the server's own reaper, and a reaper that
// refused an unspecified page size would simply stop reclaiming leases.
func TestTheExpiredLeaseSweepReadsZeroAsItsDefaultPage(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	now := time.Date(2026, 9, 30, 12, 0, 0, 0, time.UTC)

	if _, err := (&Store{}).ExpiredBoardLeases(ctx, now, 1); !errors.Is(err, ErrUnavailable) {
		t.Fatalf("a sweep from an unopened plane: err %v, want ErrUnavailable", err)
	} else if errors.Is(err, ErrInvalid) {
		t.Fatalf("an unopened plane judged the sweep parameters: %v", err)
	}

	for _, refusal := range []struct {
		what  string
		now   time.Time
		limit int
	}{
		{"a sweep with no current time", time.Time{}, 1},
		{"a negative page", now, -1},
		{"a page past the sweep bound", now, maxExpiredLeasePage + 1},
	} {
		_, err := plane.ExpiredBoardLeases(ctx, refusal.now, refusal.limit)
		refusedBefore(t, refusal.what, err)
	}

	for _, accepted := range []struct {
		what  string
		limit int
	}{
		{"an unspecified page, which takes the default", 0},
		{"a single lease", 1},
		{"a page exactly at the sweep bound", maxExpiredLeasePage},
	} {
		_, err := plane.ExpiredBoardLeases(ctx, now, accepted.limit)
		reached(t, accepted.what, err)
	}
}

// Held yield work separates two different refusals: a malformed request is
// ErrInvalid, while a well-formed request about a board holding no lease is
// ErrConflict. A caller told "invalid" would go looking for a bug in its own
// request; a caller told "conflict" knows nobody holds the bench.
func TestAnIdleBoardIsAConflictNotAMalformedYieldRequest(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()

	for _, refusal := range []struct {
		what     string
		ctx      context.Context
		snapshot board.Snapshot
	}{
		{"no context", nil, aHeldBoard()},
		{"no board", ctx, board.Snapshot{Lease: &board.Lease{ID: doorReservation}}},
		{"a padded board identifier", ctx, board.Snapshot{BoardID: " " + doorBoard, Lease: &board.Lease{ID: doorReservation}}},
		{"a board identifier past the column bound", ctx, board.Snapshot{BoardID: strings.Repeat("b", 129), Lease: &board.Lease{ID: doorReservation}}},
	} {
		_, _, err := plane.HeldYieldWork(refusal.ctx, refusal.snapshot)
		refusedBefore(t, refusal.what, err)
	}

	for _, idle := range []struct {
		what     string
		snapshot board.Snapshot
	}{
		{"a board with no lease at all", board.Snapshot{BoardID: doorBoard}},
		{"a lease with no identity", board.Snapshot{BoardID: doorBoard, Lease: &board.Lease{}}},
	} {
		_, _, err := plane.HeldYieldWork(ctx, idle.snapshot)
		if !errors.Is(err, ErrConflict) {
			t.Fatalf("%s: err %v, want ErrConflict", idle.what, err)
		}
		if errors.Is(err, ErrInvalid) {
			t.Fatalf("%s: an idle board was reported as a malformed request: %v", idle.what, err)
		}
	}

	_, _, err := plane.HeldYieldWork(ctx, aHeldBoard())
	reached(t, "a board holding a live lease", err)
}

// The HIL completion door judges the caller, the attempt it names, the
// catalog it was assigned from, and the terminal result, in that order. The
// terminal result is a second, separate refusal: a report whose result and
// evidence contradict each other is rejected before any step is stored.
func TestABoardHILCompletionIsJudgedBeforeItIsStored(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	agent := boardAgentOn(doorBoard)
	whole := doorCatalog{digest: strings.Repeat("a", 64), known: true}
	commit := strings.Repeat("a", 40)

	spoiled := func(change func(*BoardHILCompletion)) BoardHILCompletion {
		in := aHILCompletion()
		change(&in)
		return in
	}

	for _, refusal := range []struct {
		what        string
		actor       BoardActor
		in          BoardHILCompletion
		definitions HILDefinitionCatalog
		commit      string
	}{
		{"an operator reporting a board agent's work", boardCallerOn(doorBoard, "operator", "operator"), aHILCompletion(), whole, commit},
		{"a board agent whose role says otherwise", boardCallerOn(doorBoard, "board_agent", "operator"), aHILCompletion(), whole, commit},
		{"an agent on no board", boardCallerOn("", "board_agent", "board_agent"), aHILCompletion(), whole, commit},
		{"an attempt identifier that is not canonical", agent, spoiled(func(in *BoardHILCompletion) { in.AttemptID = "not-an-attempt" }), whole, commit},
		{"a lease identifier that is not canonical", agent, spoiled(func(in *BoardHILCompletion) { in.LeaseID = "not-a-lease" }), whole, commit},
		{"an ungenerated lease", agent, spoiled(func(in *BoardHILCompletion) { in.Generation = 0 }), whole, commit},
		{"no steps at all", agent, spoiled(func(in *BoardHILCompletion) { in.Steps = nil }), whole, commit},
		{"more steps than the report holds", agent, spoiled(func(in *BoardHILCompletion) { in.Steps = make([]HILStep, 65) }), whole, commit},
		{"no catalog", agent, aHILCompletion(), nil, commit},
		{"a catalog with no digest", agent, aHILCompletion(), doorCatalog{}, commit},
		{"a commit that is not a full SHA", agent, aHILCompletion(), whole, "abc123"},
		{"no trusted commit", agent, aHILCompletion(), whole, ""},
	} {
		err := plane.CompleteBoardHILAttempt(ctx, refusal.actor, refusal.in, refusal.definitions, refusal.commit)
		refusedBefore(t, refusal.what, err)
	}

	// The second guard: the arguments are well formed, but the terminal
	// result does not hold together.
	exit := func(code int) *int { return &code }
	for _, contradiction := range []struct {
		what string
		in   BoardHILCompletion
	}{
		{"a success with no exit code", spoiled(func(in *BoardHILCompletion) {
			in.Result, in.EvidenceComplete = "succeeded", true
		})},
		{"a success that exited non-zero", spoiled(func(in *BoardHILCompletion) {
			in.Result, in.EvidenceComplete, in.ChildExitCode = "succeeded", true, exit(1)
		})},
		{"a success with incomplete evidence", spoiled(func(in *BoardHILCompletion) {
			in.Result, in.ChildExitCode = "succeeded", exit(0)
		})},
		{"a failure that also hit its deadline", spoiled(func(in *BoardHILCompletion) { in.HitDeadline = true })},
		{"a timeout that did not hit its deadline", spoiled(func(in *BoardHILCompletion) { in.Result = "timed_out" })},
		{"a result the attempt machine has no edge for", spoiled(func(in *BoardHILCompletion) { in.Result = "done" })},
		{"an exit code no child can return", spoiled(func(in *BoardHILCompletion) { in.ChildExitCode = exit(256) })},
		{"a negative exit code", spoiled(func(in *BoardHILCompletion) { in.ChildExitCode = exit(-1) })},
	} {
		err := plane.CompleteBoardHILAttempt(ctx, agent, contradiction.in, whole, commit)
		refusedBefore(t, contradiction.what, err)
	}

	for _, accepted := range []struct {
		what string
		in   BoardHILCompletion
	}{
		{"a failed attempt", aHILCompletion()},
		{"a succeeded attempt with complete evidence", spoiled(func(in *BoardHILCompletion) {
			in.Result, in.EvidenceComplete, in.ChildExitCode = "succeeded", true, exit(0)
		})},
		{"a timeout that hit its deadline", spoiled(func(in *BoardHILCompletion) {
			in.Result, in.HitDeadline = "timed_out", true
		})},
		{"a report holding the most steps allowed", spoiled(func(in *BoardHILCompletion) {
			in.Steps = make([]HILStep, 64)
		})},
	} {
		err := plane.CompleteBoardHILAttempt(ctx, agent, accepted.in, whole, commit)
		reached(t, accepted.what, err)
	}
}
