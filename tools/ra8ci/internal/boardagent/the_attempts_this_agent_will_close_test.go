// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

const (
	soundAttemptID = "01996f90-3415-7cfe-8ff1-600058131aff"
	otherAttemptID = "01996f90-3415-7cfe-8ff1-600058131ab0"
)

// soundCompletion pairs an assignment and a completion that agree with the
// token in every way the door checks, so each case below can break exactly
// one of them.
func soundCompletion(t *testing.T, token boardclient.LeaseToken) (store.BoardHILAssignment, store.BoardHILCompletion) {
	t.Helper()
	_, _, task := timedHILFixture(t)
	started := time.Now().UTC()
	assignment := store.BoardHILAssignment{
		Attempt: store.Attempt{ID: soundAttemptID, StartedAt: started,
			DeadlineAt: started.Add(time.Minute), TaskID: "01996f90-3415-7cfe-8ff1-600058131b11",
			AttemptNo: 1, State: "running"},
		Task: task, CatalogSHA256: "reviewed-catalog"}
	completion := store.BoardHILCompletion{AttemptID: soundAttemptID, LeaseID: token.LeaseID,
		Generation: token.Generation, Result: "passed", EvidenceComplete: true}
	return assignment, completion
}

// A completion closes the exact attempt the server selected, so the token, the
// assignment and the completion must agree before the client is called at all.
// Any disagreement is an invalid agent: persisting a result against the wrong
// attempt or lease is not something a later check could undo.
func TestCompleteHILAttemptRefusesAnAttemptThatDoesNotAgree(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)
	assignment, completion := soundCompletion(t, token)

	if err := agent.CompleteHILAttempt(context.Background(), token, assignment, completion); err != nil {
		t.Fatalf("a sound completion was refused: %v", err)
	}
	if client.completed != 1 || client.completion.AttemptID != soundAttemptID {
		t.Fatalf("the client saw completed=%d %+v", client.completed, client.completion)
	}

	otherBoard := token
	otherBoard.BoardID = "ek-ra8m1"

	noLease := token
	noLease.LeaseID = "not-a-uuid"

	noGeneration := token
	noGeneration.Generation = 0

	otherAttempt := completion
	otherAttempt.AttemptID = otherAttemptID

	unreadableAttempt := completion
	unreadableAttempt.AttemptID = "attempt-1"

	otherLease := completion
	otherLease.LeaseID = "01996f90-3415-7cfe-8ff1-600058131ab1"

	otherGeneration := completion
	otherGeneration.Generation = token.Generation + 1

	notHIL := assignment
	notHIL.Task.Scope = "unit"

	shared := assignment
	shared.Task.BoardPolicy = "shared"

	noHILBlock := assignment
	noHILBlock.Task.HIL = nil

	anotherBoardsTask := assignment
	anotherBoardsHIL := *assignment.Task.HIL
	anotherBoardsHIL.BoardID = "ek-ra8m1"
	anotherBoardsTask.Task.HIL = &anotherBoardsHIL

	unreviewed := assignment
	unreviewed.Task.Version = 0

	before := client.completed
	for name, item := range map[string]struct {
		token      boardclient.LeaseToken
		assignment store.BoardHILAssignment
		completion store.BoardHILCompletion
	}{
		"another board's token":        {otherBoard, assignment, completion},
		"a lease that is not an id":    {noLease, assignment, completion},
		"no generation":                {noGeneration, assignment, completion},
		"another attempt":              {token, assignment, otherAttempt},
		"an attempt that is not an id": {token, assignment, unreadableAttempt},
		"another lease":                {token, assignment, otherLease},
		"another generation":           {token, assignment, otherGeneration},
		"a task that is not HIL":       {token, notHIL, completion},
		"a shared board":               {token, shared, completion},
		"a task with no HIL block":     {token, noHILBlock, completion},
		"another board's task":         {token, anotherBoardsTask, completion},
		"a task the catalog refuses":   {token, unreviewed, completion},
	} {
		if err := agent.CompleteHILAttempt(context.Background(), item.token, item.assignment, item.completion); !errors.Is(err, ErrInvalidAgent) {
			t.Errorf("%s: err = %v, want ErrInvalidAgent", name, err)
		}
	}
	if client.completed != before {
		t.Fatalf("a refused completion still reached the client: completed = %d, want %d", client.completed, before)
	}

	var noContext context.Context
	if err := agent.CompleteHILAttempt(noContext, token, assignment, completion); !errors.Is(err, ErrInvalidAgent) {
		t.Errorf("no context: err = %v, want ErrInvalidAgent", err)
	}
	var absent *Agent
	if err := absent.CompleteHILAttempt(context.Background(), token, assignment, completion); !errors.Is(err, ErrInvalidAgent) {
		t.Errorf("no agent: err = %v, want ErrInvalidAgent", err)
	}
}

// A control client that cannot close an attempt is named as the reason rather
// than letting the caller believe the result was persisted.
func TestCompleteHILAttemptRefusesAClientThatCannotClose(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)
	assignment, completion := soundCompletion(t, token)
	agent.client = agent.client.(*testSegmentControlClient).testControlClient
	err := agent.CompleteHILAttempt(context.Background(), token, assignment, completion)
	if !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("err = %v, want ErrInvalidAgent", err)
	}
}

// A claim reaches for hardware, so the token is judged before the segment gate
// is entered and before the server is asked for work.
func TestClaimNextHILAttemptRefusesATokenItCannotUse(t *testing.T) {
	agent, client, token := newActiveSegmentAgent(t)

	otherBoard := token
	otherBoard.BoardID = "ek-ra8m1"

	noLease := token
	noLease.LeaseID = ""

	unreadableLease := token
	unreadableLease.LeaseID = "lease-1"

	before := client.claimCount
	for name, ask := range map[string]boardclient.LeaseToken{
		"another board's token":     otherBoard,
		"no lease":                  noLease,
		"a lease that is not an id": unreadableLease,
	} {
		if _, err := agent.ClaimNextHILAttempt(context.Background(), ask, "runner-1", 4, 1<<30, 0.5, nil); !errors.Is(err, ErrInvalidAgent) {
			t.Errorf("%s: err = %v, want ErrInvalidAgent", name, err)
		}
	}
	var noContext context.Context
	if _, err := agent.ClaimNextHILAttempt(noContext, token, "runner-1", 4, 1<<30, 0.5, nil); !errors.Is(err, ErrInvalidAgent) {
		t.Errorf("no context: err = %v, want ErrInvalidAgent", err)
	}
	var absent *Agent
	if _, err := absent.ClaimNextHILAttempt(context.Background(), token, "runner-1", 4, 1<<30, 0.5, nil); !errors.Is(err, ErrInvalidAgent) {
		t.Errorf("no agent: err = %v, want ErrInvalidAgent", err)
	}
	if client.claimCount != before {
		t.Fatalf("a refused claim still asked the server: claims = %d, want %d", client.claimCount, before)
	}

	assignment, err := agent.ClaimNextHILAttempt(context.Background(), token, "runner-1", 4, 1<<30, 0.5, nil)
	if err != nil || assignment == nil || client.claimedLease != token.LeaseID {
		t.Fatalf("a sound claim: assignment = %+v, lease = %q, err = %v", assignment, client.claimedLease, err)
	}
}

// A client that holds no claim path is named rather than silently answering no
// work, which a runner would read as an idle board.
func TestClaimNextHILAttemptRefusesAClientThatCannotClaim(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)
	agent.client = agent.client.(*testSegmentControlClient).testControlClient
	if _, err := agent.ClaimNextHILAttempt(context.Background(), token, "runner-1", 4, 1<<30, 0.5, nil); !errors.Is(err, ErrInvalidAgent) {
		t.Fatalf("err = %v, want ErrInvalidAgent", err)
	}
}

// Starting a segment needs three independent authorizations to agree, so a
// token that names no lease or no generation is an invalid argument before any
// of them is consulted.
func TestCanStartSegmentRefusesATokenBeforeAskingTheServer(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)

	otherBoard := token
	otherBoard.BoardID = "ek-ra8m1"

	noLease := token
	noLease.LeaseID = ""

	noGeneration := token
	noGeneration.Generation = 0

	for name, ask := range map[string]boardclient.LeaseToken{
		"another board's token": otherBoard,
		"no lease":              noLease,
		"no generation":         noGeneration,
	} {
		err := agent.CanStartSegment(context.Background(), ask, time.Second, 0)
		var refusal *board.Error
		if !errors.As(err, &refusal) || refusal.Code != board.InvalidArgument {
			t.Errorf("%s: err = %v, want a board.InvalidArgument", name, err)
		}
	}

	var noContext context.Context
	if err := agent.CanStartSegment(noContext, token, time.Second, 0); err == nil {
		t.Error("no context was accepted")
	}
	var absent *Agent
	if err := absent.CanStartSegment(context.Background(), token, time.Second, 0); err == nil {
		t.Error("no agent was accepted")
	}
}

// The durable generation and the server's view of it have to be the same
// generation the token names. When they are not, the board needs recovery
// rather than another segment, and saying so is the whole point of keeping a
// local high-water mark at all.
func TestCanStartSegmentDemandsTheDurableAndServerGenerationsAgree(t *testing.T) {
	agent, _, token := newActiveSegmentAgent(t)
	if err := agent.CanStartSegment(context.Background(), token, time.Second, 0); err != nil {
		t.Fatalf("a sound segment was refused: %v", err)
	}

	ahead := token
	ahead.Generation = token.Generation + 1
	err := agent.CanStartSegment(context.Background(), ahead, time.Second, 0)
	var refusal *board.Error
	if !errors.As(err, &refusal) || refusal.Code != board.RecoveryNecessary {
		t.Fatalf("a token ahead of the durable generation: err = %v, want RecoveryNecessary", err)
	}
}
