// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"context"
	"math"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
)

// What the five board-actor doors judge before the database, the last of this
// guard family after #2717 and #2719.
//
// Same fixture: unreachablePlane's pool is built but points nowhere, so an
// argument term is the only thing that can answer invalid and a well-formed
// call goes past it and fails reaching the database instead.
//
// Every door here is reached by a board agent holding a lease on one bench.
// Two things are therefore judged at every one of them: that the caller is a
// board agent rather than the server wearing its own hat, and that the bench
// it names is the bench it holds. The second is what stops a lease on one
// bench from being spent against another, and it is checked here rather than
// in SQL because a query with the wrong board ID would succeed.

const doorBoard = "bench-one"

// a catalog with a digest, which is all these doors read from one before the
// transaction opens. The integration suite's stub is behind a build tag, so
// this is the plain-build equivalent.
type doorCatalog struct {
	digest string
	task   catalog.Task
	known  bool
}

func (c doorCatalog) Digest() string { return c.digest }

func (c doorCatalog) Task(string) (catalog.Task, bool) { return c.task, c.known }

func boardAgentOn(boardID string) BoardActor {
	return boardCallerOn(boardID, "board_agent", "board_agent")
}

func tokenFor(boardID string) board.Token {
	return board.Token{BoardID: boardID, LeaseID: doorReservation, Generation: 3}
}

// The history read binds three things to each other: the caller is a board
// agent, the bench it names is one it could hold, and the task definition it
// hands over describes that same bench. A definition for another bench is the
// case that matters, because everything else about it is well formed.
func TestABoardHistoryReadBindsTheBenchToTheDefinition(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	definition := hilContract()
	if catalog.ValidateHILTaskMetadata(definition) != nil {
		t.Fatalf("the shared HIL fixture is not valid metadata: %v", catalog.ValidateHILTaskMetadata(definition))
	}
	if definition.BoardID != doorBoard {
		t.Fatalf("the shared HIL fixture names %q, not the bench this test holds", definition.BoardID)
	}
	agent := boardAgentOn(doorBoard)

	for _, c := range []struct {
		name       string
		actor      BoardActor
		definition catalog.HILTask
	}{
		{"the server wearing its own hat", boardCallerOn(doorBoard, "system", "system"), definition},
		{"an operator", boardCallerOn(doorBoard, "human", "operator"), definition},
		{"an agent whose role does not match its kind", boardCallerOn(doorBoard, "board_agent", "operator"), definition},
		{"no bench", boardAgentOn(""), definition},
		{"a padded bench", boardAgentOn(" bench-one"), definition},
		{"a bench past its column", boardAgentOn(strings.Repeat("b", 129)), definition},
		{"no definition", agent, catalog.HILTask{}},
	} {
		_, _, err := plane.BoardHILObservations(ctx, c.actor, c.definition)
		refusedBefore(t, c.name, err)
	}

	// The binding itself: a whole definition, for a different bench.
	elsewhere := definition
	elsewhere.BoardID = "bench-two"
	_, _, err := plane.BoardHILObservations(ctx, agent, elsewhere)
	refusedBefore(t, "a definition for another bench", err)

	// And the mirror: the same definition, with the agent on that bench.
	_, _, err = plane.BoardHILObservations(ctx, boardAgentOn("bench-two"), elsewhere)
	if err == nil {
		t.Fatal("an agent on the bench its definition names was refused with no error at all")
	}

	_, _, err = plane.BoardHILObservations(nil, agent, definition) //nolint:staticcheck // the nil is the case
	refusedBefore(t, "no context", err)
	_, _, err = plane.BoardHILObservations(ctx, agent, definition)
	reached(t, "an agent reading its own bench's history", err)
}

// A HIL attempt is claimed against a lease, so the identifiers are judged by
// shape before a row is locked. The queue claim additionally names the catalog
// it believes it is working from: an agent running an older catalog must be
// refused rather than allowed to start work nobody can later match.
func TestABoardHILClaimNamesItsLeaseAndItsCatalog(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	agent := boardAgentOn(doorBoard)
	facts := StartAttemptInput{}
	whole := doorCatalog{digest: strings.Repeat("a", 64), known: true}
	commit := strings.Repeat("b", 40)

	for _, c := range []struct {
		name          string
		actor         BoardActor
		task, leaseID string
	}{
		{"the server wearing its own hat", boardCallerOn(doorBoard, "system", "system"), doorReservation, doorOperation},
		{"an agent whose role does not match its kind", boardCallerOn(doorBoard, "board_agent", "operator"), doorReservation, doorOperation},
		{"no bench", boardAgentOn(""), doorReservation, doorOperation},
		{"no task", agent, "", doorOperation},
		{"a task that is a name", agent, "hil-alive", doorOperation},
		{"no lease", agent, doorReservation, ""},
		{"a lease that is a name", agent, doorReservation, "lease-1"},
	} {
		_, err := plane.StartBoardHILAttempt(ctx, c.actor, c.task, c.leaseID, facts)
		refusedBefore(t, c.name, err)
	}
	_, err := plane.StartBoardHILAttempt(ctx, agent, doorReservation, doorOperation, facts)
	reached(t, "an agent starting an attempt on its own lease", err)

	for _, c := range []struct {
		name        string
		actor       BoardActor
		leaseID     string
		definitions HILDefinitionCatalog
		commit      string
	}{
		{"the server wearing its own hat", boardCallerOn(doorBoard, "system", "system"), doorOperation, whole, commit},
		{"no bench", boardAgentOn(""), doorOperation, whole, commit},
		{"a lease that is a name", agent, "lease-1", whole, commit},
		{"no catalog at all", agent, doorOperation, nil, commit},
		{"a catalog with no digest", agent, doorOperation, doorCatalog{}, commit},
		{"no commit", agent, doorOperation, whole, ""},
		{"a commit that is a branch", agent, doorOperation, whole, "refs/heads/main"},
		{"a digest-sized commit", agent, doorOperation, whole, strings.Repeat("b", 64)},
		{"an uppercase commit", agent, doorOperation, whole, strings.Repeat("B", 40)},
	} {
		_, err := plane.ClaimNextBoardHILAttempt(ctx, c.actor, c.leaseID, facts, c.definitions, c.commit)
		refusedBefore(t, c.name, err)
	}
	_, err = plane.ClaimNextBoardHILAttempt(ctx, agent, doorOperation, facts, whole, commit)
	reached(t, "an agent claiming from a catalog it names", err)
}

// A segment is the operation that must finish before the next checkpoint, so
// its bound is judged here: a segment with no bound, or one longer than a day,
// would leave a bench held by a lease nobody can reason about.
func TestASegmentStatesABenchATokenAndABound(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	agent := boardAgentOn(doorBoard)
	token := tokenFor(doorBoard)
	const key = "flash-restore"

	type opening struct {
		name    string
		actor   BoardActor
		version uint64
		token   board.Token
		attempt string
		key     string
		bound   time.Duration
		margin  time.Duration
	}
	whole := opening{actor: agent, version: 4, token: token, attempt: doorReservation,
		key: key, bound: time.Hour, margin: time.Minute}
	bend := func(name string, edit func(*opening)) opening {
		o := whole
		edit(&o)
		o.name = name
		return o
	}

	for _, c := range []opening{
		bend("the server itself", func(o *opening) { o.actor = boardCallerOn(doorBoard, "system", "system") }),
		bend("a caller with no identity", func(o *opening) { o.actor = BoardActor{kind: "board_agent", boardID: doorBoard} }),
		bend("no bench", func(o *opening) { o.actor = boardAgentOn("") }),
		// A token for another bench is the case the SQL could not
		// catch: the row it names exists, it is just not this lease.
		bend("a token for another bench", func(o *opening) { o.token = tokenFor("bench-two") }),
		bend("a version past the column", func(o *opening) { o.version = math.MaxInt64 + 1 }),
		bend("no attempt", func(o *opening) { o.attempt = "" }),
		bend("an attempt that is a name", func(o *opening) { o.attempt = "attempt-1" }),
		bend("no key", func(o *opening) { o.key = "" }),
		bend("a padded key", func(o *opening) { o.key = " flash-restore" }),
		bend("a key with a control character", func(o *opening) { o.key = "flash\trestore" }),
		bend("a key past its column", func(o *opening) { o.key = strings.Repeat("k", 129) }),
		bend("no bound", func(o *opening) { o.bound = 0 }),
		bend("a bound running backwards", func(o *opening) { o.bound = -time.Second }),
		bend("a bound past a day", func(o *opening) { o.bound = 24*time.Hour + time.Second }),
		// Sub-millisecond is its own term: the column holds
		// milliseconds, so a bound that rounds to nothing is refused
		// rather than written as an instant deadline.
		bend("a bound below the column's resolution", func(o *opening) { o.bound = time.Microsecond }),
		bend("a margin running backwards", func(o *opening) { o.margin = -time.Second }),
		bend("a margin past a day", func(o *opening) { o.margin = 24*time.Hour + time.Second }),
	} {
		_, err := plane.BeginBoardSegment(ctx, c.actor, c.version, c.token, c.attempt, c.key, c.bound, c.margin)
		refusedBefore(t, c.name, err)
	}

	// The generation is judged by a guard of its own, after the rest.
	far := token
	far.Generation = math.MaxInt64 + 1
	_, err := plane.BeginBoardSegment(ctx, agent, 4, far, doorReservation, key, time.Hour, time.Minute)
	refusedBefore(t, "a generation past the column", err)

	_, err = plane.BeginBoardSegment(ctx, agent, 4, token, doorReservation, key, 24*time.Hour, 24*time.Hour)
	reached(t, "a segment bounded at a day", err)
	_, err = plane.BeginBoardSegment(ctx, agent, 4, token, doorReservation, key, time.Millisecond, 0)
	reached(t, "a segment at the column's resolution with no margin", err)
}

// Finishing a segment states how it ended, and only three endings exist. An
// unrecognised outcome is refused rather than stored, because the row is what
// a later reconcile reads to decide whether the bench was left clean.
func TestFinishingASegmentStatesOneOfThreeEndings(t *testing.T) {
	plane, ctx := unreachablePlane(t), context.Background()
	agent := boardAgentOn(doorBoard)
	token := tokenFor(doorBoard)
	segment := "01996f90-3415-7cfe-8ff1-600058131b12"

	for _, c := range []struct {
		name    string
		actor   BoardActor
		segment string
		token   board.Token
		attempt string
		outcome string
	}{
		{"the server itself", boardCallerOn(doorBoard, "system", "system"), segment, token, doorReservation, "completed"},
		{"no bench", boardAgentOn(""), segment, token, doorReservation, "completed"},
		{"no segment", agent, "", token, doorReservation, "completed"},
		{"a segment that is a name", agent, "segment-1", token, doorReservation, "completed"},
		{"a token for another bench", agent, segment, tokenFor("bench-two"), doorReservation, "completed"},
		{"a token with no lease", agent, segment, board.Token{BoardID: doorBoard, Generation: 3}, doorReservation, "completed"},
		{"a token at generation zero", agent, segment, board.Token{BoardID: doorBoard, LeaseID: doorReservation}, doorReservation, "completed"},
		{"no attempt", agent, segment, token, "", "completed"},
		{"no outcome", agent, segment, token, doorReservation, ""},
		{"an outcome nobody records", agent, segment, token, doorReservation, "abandoned"},
		{"a capitalised outcome", agent, segment, token, doorReservation, "Completed"},
	} {
		refusedBefore(t, c.name, plane.FinishBoardSegment(ctx, c.actor, c.segment, c.token, c.attempt, c.outcome))
	}

	for _, ending := range []string{"completed", "failed", "yielded"} {
		reached(t, "a segment "+ending, plane.FinishBoardSegment(ctx, agent, segment, token, doorReservation, ending))
	}
}
