// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
)

// Who may ask for each board transition, and what the store binds onto the
// request before the reducer ever sees it.
//
// This is the whole authorization surface of the board, and it runs entirely
// before a transaction opens, so none of it needs a database. Two properties
// carry the weight. First, the actor's own identity is STAMPED onto every
// command that is admitted, so a body claiming some other actor cannot reach
// the reducer under that name. Second, the two receipt-carrying commands take
// the proof the CALLER was handed rather than the one the body states, and
// CompleteRecovery additionally takes the agent high-water from the SNAPSHOT,
// so an operator closing a recovery cannot declare the board's agent further
// ahead than the board itself has observed.
//
// The board binding is checked before any of it: an actor authorized for one
// board is denied on another whatever the command says, which is what stops a
// grant for a spare bench from moving the board under a running job.

const (
	commandWaiterID = "01996f90-3415-7cfe-8ff1-600058131b10"
	commandLeaseID  = "01996f90-3415-7cfe-8ff1-600058131b11"
)

// boardHeldBy is a board whose lease is held by the named actor, with one
// waiter queued for the same actor: the shape in which every holder command
// and every waiter command is legal at once.
func boardHeldBy(holder string) board.Snapshot {
	granted := time.Date(2026, 9, 24, 17, 0, 0, 0, time.UTC)
	return board.Snapshot{
		BoardID:        "board-1",
		Phase:          board.Active,
		Generation:     3,
		AgentHighWater: 9,
		Version:        7,
		Lease: &board.Lease{
			ID: commandLeaseID, WaiterID: commandWaiterID, Holder: holder,
			Class: board.ClassCI, Reason: "nightly", Generation: 3,
			GrantedAt: granted, ExpiresAt: granted.Add(time.Hour), RequestedDuration: time.Hour,
		},
		Queue: []board.Waiter{waiterHeldBy(commandWaiterID, holder)},
	}
}

// TestEveryBoardCommandIsHeldToItsOwnAudience walks the full command set on
// both sides: the actor the store admits, and one that is refused for the
// reason that command cares about. A command nobody is allowed to ask for is
// worse than a missing feature, and a command everybody is allowed to ask for
// is worse still, so both directions are pinned in one table.
func TestEveryBoardCommandIsHeldToItsOwnAudience(t *testing.T) {
	held := boardHeldBy("actor-1")
	elsewhere := boardHeldBy("someone-else")

	submitter := boardActor("agent", "submitter")
	operator := boardActor("human", "operator")
	agent := boardActor("board_agent", "board_agent")
	system := boardActor("system", "system")

	queued := board.Waiter{ID: commandWaiterID, LeaseID: commandLeaseID, Holder: "actor-1", Class: board.ClassAI, Sequence: 1}

	for _, c := range []struct {
		name    string
		actor   BoardActor
		before  board.Snapshot
		command board.Command
		allowed bool
	}{
		{"a submitter queues for its own class", submitter, held, board.Enqueue{Waiter: queued}, true},
		{"nobody queues on another holder's behalf", submitter, held,
			board.Enqueue{Waiter: board.Waiter{ID: commandWaiterID, LeaseID: commandLeaseID, Holder: "someone-else", Class: board.ClassAI}}, false},
		{"a queued waiter needs a usable identifier", submitter, held,
			board.Enqueue{Waiter: board.Waiter{ID: "not-a-uuid", LeaseID: commandLeaseID, Holder: "actor-1", Class: board.ClassAI}}, false},
		{"and a usable lease identifier with it", submitter, held,
			board.Enqueue{Waiter: board.Waiter{ID: commandWaiterID, LeaseID: "not-a-uuid", Holder: "actor-1", Class: board.ClassAI}}, false},
		{"a submitter cannot queue in a class it may not speak for", submitter, held,
			board.Enqueue{Waiter: board.Waiter{ID: commandWaiterID, LeaseID: commandLeaseID, Holder: "actor-1", Class: board.ClassHuman}}, false},

		{"a waiter cancels its own place", submitter, held, board.CancelWaiter{WaiterID: commandWaiterID}, true},
		{"an operator cancels anyone's place", operator, elsewhere, board.CancelWaiter{WaiterID: commandWaiterID}, true},
		{"nobody else cancels another's place", submitter, elsewhere, board.CancelWaiter{WaiterID: commandWaiterID}, false},

		{"a waiter may ask the holder to yield", submitter, held, board.RequestYield{WaiterID: commandWaiterID}, true},
		{"a stranger may not", submitter, elsewhere, board.RequestYield{WaiterID: commandWaiterID}, false},
		{"and an operator may not either, having no place in the queue", operator, elsewhere, board.RequestYield{WaiterID: commandWaiterID}, false},

		{"the holder drains its own lease", submitter, held, board.BeginDrain{}, true},
		{"a non-holder does not", submitter, elsewhere, board.BeginDrain{}, false},
		{"the holder releases its own lease", submitter, held, board.Release{}, true},
		{"a non-holder does not", submitter, elsewhere, board.Release{}, false},
		{"the holder extends its own lease", submitter, held, board.Extend{}, true},
		{"a non-holder does not", submitter, elsewhere, board.Extend{}, false},
		{"the holder beats for its own lease", submitter, held, board.HolderHeartbeat{}, true},
		{"a superseded holder still beating is denied", submitter, elsewhere, board.HolderHeartbeat{}, false},

		{"the board agent acknowledges a grant", agent, held, board.AcknowledgeGrant{}, true},
		{"an operator does not acknowledge grants", operator, held, board.AcknowledgeGrant{}, false},
		{"the board agent reports its generation", agent, held, board.ObserveAgentGeneration{}, true},
		{"an operator does not report it", operator, held, board.ObserveAgentGeneration{}, false},
		{"the board agent may declare itself unavailable", agent, held, board.AgentUnavailable{}, true},
		{"and the system may declare it for them", system, held, board.AgentUnavailable{}, true},
		{"an operator may not", operator, held, board.AgentUnavailable{}, false},

		{"an operator begins a recovery", operator, held, board.BeginRecovery{}, true},
		{"the board agent does not begin its own recovery", agent, held, board.BeginRecovery{}, false},
		{"an operator completes a recovery", operator, held, board.CompleteRecovery{}, true},
		{"the board agent does not complete it", agent, held, board.CompleteRecovery{}, false},

		{"an operator quarantines a board", operator, held, board.Quarantine{}, true},
		{"so does the system", system, held, board.Quarantine{}, true},
		{"a submitter does not", submitter, held, board.Quarantine{}, false},

		{"only the system ticks the clock", system, held, board.Tick{}, true},
		{"an operator does not tick it", operator, held, board.Tick{}, false},
	} {
		t.Run(c.name, func(t *testing.T) {
			bound, err := authorizeAndBindCommand(c.actor, c.before, c.command, "receipt-1")
			if c.allowed {
				if err != nil {
					t.Fatalf("refused a command this actor may ask for: %v", err)
				}
				if bound == nil {
					t.Fatal("admitted the command but bound nothing")
				}
				return
			}
			if !errors.Is(err, ErrDenied) {
				t.Fatalf("err %v, want ErrDenied", err)
			}
			if bound != nil {
				t.Fatalf("refused the command but still handed back %T", bound)
			}
		})
	}
}

// TestAnActorIsBoundToTheBoardItWasAuthorizedFor pins the check that runs
// ahead of the command entirely. A grant names a board; a request naming
// another one is denied whatever the command is and whoever holds the lease,
// which is what stops a grant for a spare bench from moving the board under a
// running job.
func TestAnActorIsBoundToTheBoardItWasAuthorizedFor(t *testing.T) {
	actor := boardActor("human", "operator")
	other := boardHeldBy("actor-1")
	other.BoardID = "board-2"
	if _, err := authorizeAndBindCommand(actor, other, board.Quarantine{}, ""); !errors.Is(err, ErrDenied) {
		t.Fatalf("err %v, want ErrDenied for a board this actor was not authorized for", err)
	}
	// Even the command an operator is otherwise most entitled to.
	if _, err := authorizeAndBindCommand(actor, other, board.BeginRecovery{}, ""); !errors.Is(err, ErrDenied) {
		t.Fatalf("err %v, want ErrDenied", err)
	}
}

// TestTheStoreStampsTheActorOntoEveryCommandItAdmits is the binding half. The
// request body never gets to say who asked: whatever it claims, the admitted
// command carries the authenticated actor's own identifier, so the reducer and
// the event it writes name the peer the certificate named.
func TestTheStoreStampsTheActorOntoEveryCommandItAdmits(t *testing.T) {
	held := boardHeldBy("actor-1")
	submitter := boardActor("agent", "submitter")
	operator := boardActor("human", "operator")
	agent := boardActor("board_agent", "board_agent")
	system := boardActor("system", "system")
	queued := board.Waiter{ID: commandWaiterID, LeaseID: commandLeaseID, Holder: "actor-1", Class: board.ClassAI, Sequence: 1}

	// Every command below states a different actor in its body.
	for _, c := range []struct {
		name    string
		actor   BoardActor
		command board.Command
		actorOf func(board.Command) string
	}{
		{"enqueue", submitter, board.Enqueue{Actor: "liar", Waiter: queued},
			func(c board.Command) string { return c.(board.Enqueue).Actor }},
		{"cancel", submitter, board.CancelWaiter{Actor: "liar", WaiterID: commandWaiterID},
			func(c board.Command) string { return c.(board.CancelWaiter).Actor }},
		{"yield", submitter, board.RequestYield{Actor: "liar", WaiterID: commandWaiterID},
			func(c board.Command) string { return c.(board.RequestYield).Actor }},
		{"drain", submitter, board.BeginDrain{Actor: "liar"},
			func(c board.Command) string { return c.(board.BeginDrain).Actor }},
		{"release", submitter, board.Release{Actor: "liar"},
			func(c board.Command) string { return c.(board.Release).Actor }},
		{"extend", submitter, board.Extend{Actor: "liar"},
			func(c board.Command) string { return c.(board.Extend).Actor }},
		{"heartbeat", submitter, board.HolderHeartbeat{Actor: "liar"},
			func(c board.Command) string { return c.(board.HolderHeartbeat).Actor }},
		{"acknowledge", agent, board.AcknowledgeGrant{Actor: "liar"},
			func(c board.Command) string { return c.(board.AcknowledgeGrant).Actor }},
		{"observe", agent, board.ObserveAgentGeneration{Actor: "liar"},
			func(c board.Command) string { return c.(board.ObserveAgentGeneration).Actor }},
		{"unavailable", agent, board.AgentUnavailable{Actor: "liar"},
			func(c board.Command) string { return c.(board.AgentUnavailable).Actor }},
		{"begin recovery", operator, board.BeginRecovery{Actor: "liar"},
			func(c board.Command) string { return c.(board.BeginRecovery).Actor }},
		{"complete recovery", operator, board.CompleteRecovery{Actor: "liar"},
			func(c board.Command) string { return c.(board.CompleteRecovery).Actor }},
		{"quarantine", operator, board.Quarantine{Actor: "liar"},
			func(c board.Command) string { return c.(board.Quarantine).Actor }},
		{"tick", system, board.Tick{Actor: "liar"},
			func(c board.Command) string { return c.(board.Tick).Actor }},
	} {
		t.Run(c.name, func(t *testing.T) {
			bound, err := authorizeAndBindCommand(c.actor, held, c.command, "receipt-1")
			if err != nil {
				t.Fatal(err)
			}
			if got := c.actorOf(bound); got != c.actor.id {
				t.Fatalf("command carries actor %q, want the authenticated %q", got, c.actor.id)
			}
		})
	}
}

// TestTheReceiptComesFromTheCallerNotTheBody pins the two commands that carry
// a neutral receipt. The proof is the one the caller was handed on the way in,
// so a body naming some other receipt cannot launder a stale or borrowed
// attestation into the transition that consumes it.
func TestTheReceiptComesFromTheCallerNotTheBody(t *testing.T) {
	held := boardHeldBy("actor-1")

	release, err := authorizeAndBindCommand(boardActor("agent", "submitter"), held,
		board.Release{NeutralReceipt: "a receipt the body made up"}, "proof-from-the-door")
	if err != nil {
		t.Fatal(err)
	}
	if got := release.(board.Release).NeutralReceipt; got != "proof-from-the-door" {
		t.Fatalf("release carries receipt %q, want the caller's", got)
	}

	complete, err := authorizeAndBindCommand(boardActor("human", "operator"), held,
		board.CompleteRecovery{NeutralReceipt: "a receipt the body made up", AgentHighWater: 999}, "proof-from-the-door")
	if err != nil {
		t.Fatal(err)
	}
	done := complete.(board.CompleteRecovery)
	if done.NeutralReceipt != "proof-from-the-door" {
		t.Fatalf("recovery carries receipt %q, want the caller's", done.NeutralReceipt)
	}
	// And the high-water comes from the board, not from the operator closing
	// the recovery: observing the agent further ahead is a separate
	// authenticated transition.
	if done.AgentHighWater != held.AgentHighWater {
		t.Fatalf("recovery declares agent high-water %d, want the board's %d", done.AgentHighWater, held.AgentHighWater)
	}
}

// The switch's default arm, which refuses an unsupported command as invalid
// rather than denied, is unreachable from here and from anywhere else outside
// the board package: board.Command is a sealed interface (its boardCommand
// method is unexported), so every value that can be passed in is one of the
// fourteen arms above. It is left uncovered deliberately rather than opened up
// with an exported stub whose only purpose is to be rejected.
