package board

import (
	"strings"
	"testing"
	"time"
)

// freeBoardWaiter is a valid queue entry, so a case moves only the phase it
// sits beside.
func freeBoardWaiter(id string, sequence uint64) Waiter {
	return Waiter{
		ID:       id,
		LeaseID:  "lease-" + id,
		Holder:   "agent-" + id,
		Class:    ClassCI,
		Reason:   "bench debug",
		Duration: 20 * time.Minute,
		Sequence: sequence,
		QueuedAt: time.Date(2026, 9, 26, 9, int(sequence), 0, 0, time.UTC),
	}
}

// freeBoardSnapshot carries the given queue in the given phase with no lease,
// which every phase this door admits can hold.
func freeBoardSnapshot(phase Phase, queue ...Waiter) Snapshot {
	var highest uint64
	for _, w := range queue {
		if w.Sequence > highest {
			highest = w.Sequence
		}
	}
	return Snapshot{
		BoardID:      "board-a",
		Phase:        phase,
		Version:      4,
		NextSequence: highest,
		Queue:        queue,
	}
}

func TestAReadyBoardWithNoQueueIsAdmitted(t *testing.T) {
	if err := checkAReadyBoardKeepsNoWaiters(Ready, nil); err != nil {
		t.Fatalf("an empty ready board was refused: %v", err)
	}
	if err := checkAReadyBoardKeepsNoWaiters(Ready, []Waiter{}); err != nil {
		t.Fatalf("an empty but allocated queue was refused: %v", err)
	}
	if err := Validate(freeBoardSnapshot(Ready)); err != nil {
		t.Fatalf("a ready board with no waiters was refused: %v", err)
	}
}

func TestAReadyBoardKeepingAWaiterIsRefused(t *testing.T) {
	err := checkAReadyBoardKeepsNoWaiters(Ready, []Waiter{freeBoardWaiter("w1", 1)})
	if !IsCode(err, Conflict) {
		t.Fatalf("a ready board holding a waiter was admitted: %v", err)
	}
}

func TestTheRefusalNamesTheQueueItFound(t *testing.T) {
	queue := []Waiter{freeBoardWaiter("w1", 4), freeBoardWaiter("w2", 5)}
	err := checkAReadyBoardKeepsNoWaiters(Ready, queue)
	if err == nil {
		t.Fatal("a ready board holding two waiters was admitted")
	}
	for _, want := range []string{"ready board", "2", `"w1"`, "4"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal %q does not name %q", err.Error(), want)
		}
	}
}

func TestTheRecoveryPhasesKeepTheirQueue(t *testing.T) {
	// The waiters are meant to still be there when the board comes back:
	// grantNext declines in every one of these, so nothing could have
	// served them.
	for _, phase := range []Phase{RecoveryRequired, Recovering, Quarantined} {
		queue := []Waiter{freeBoardWaiter("w1", 1), freeBoardWaiter("w2", 2)}
		if err := checkAReadyBoardKeepsNoWaiters(phase, queue); err != nil {
			t.Fatalf("phase %q was refused its queue: %v", phase, err)
		}
		if err := Validate(freeBoardSnapshot(phase, queue...)); err != nil {
			t.Fatalf("phase %q was refused through Validate: %v", phase, err)
		}
	}
}

func TestALiveBoardKeepsItsQueue(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("holder", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	s, _ = applyForTest(t, s, request("waiting", ClassCI), testEpoch.Add(2*time.Second))
	if s.Phase != Active || len(s.Queue) != 1 {
		t.Fatalf("expected an active board with one waiter: %#v", s)
	}
	if err := Validate(s); err != nil {
		t.Fatalf("an active board was refused its waiter: %v", err)
	}
}

func TestTheDoorIsReachedThroughValidateOnAReadyBoard(t *testing.T) {
	s := freeBoardSnapshot(Ready, freeBoardWaiter("w1", 1))
	if !IsCode(Validate(s), Conflict) {
		t.Fatal("Validate admitted a ready board holding a waiter")
	}
	if !strings.Contains(Validate(s).Error(), "ready board retains") {
		t.Fatalf("Validate refused for another reason: %v", Validate(s))
	}
}

func TestAnInvalidWaiterKeepsItsOwnRefusal(t *testing.T) {
	// Judged after the per-waiter scan, so a row that is wrong in itself is
	// named as such rather than blamed on the phase it sits in.
	bad := freeBoardWaiter("w1", 1)
	bad.Holder = ""
	err := Validate(freeBoardSnapshot(Ready, bad))
	if !IsCode(err, Conflict) {
		t.Fatalf("an invalid waiter was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "invalid waiter") {
		t.Fatalf("the phase refusal displaced the waiter refusal: %v", err)
	}
}

func TestAShuffledQueueKeepsItsOwnRefusal(t *testing.T) {
	// Same order of judgement against the arrival-order door, which names
	// the two rows at fault.
	s := freeBoardSnapshot(Ready, freeBoardWaiter("w2", 2), freeBoardWaiter("w1", 1))
	err := Validate(s)
	if !IsCode(err, Conflict) {
		t.Fatalf("a shuffled queue on a ready board was admitted: %v", err)
	}
	if !strings.Contains(err.Error(), "arrival order") {
		t.Fatalf("the phase refusal displaced the order refusal: %v", err)
	}
}

func TestAWaiterArrivingAtAFreeBoardIsGrantedInTheSameCommand(t *testing.T) {
	s := boardForTest(t)
	if s.Phase != Ready || len(s.Queue) != 0 {
		t.Fatalf("a new board is not free and empty: %#v", s)
	}
	next, _ := applyForTest(t, s, request("first", ClassCI), testEpoch)
	if next.Phase != GrantPending || len(next.Queue) != 0 || next.Lease == nil {
		t.Fatalf("enqueue left a free board holding its queue: %#v", next)
	}
}

func TestReleasingToAWaitingQueueGrantsRatherThanGoingReady(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("holder", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	s, _ = applyForTest(t, s, request("next-up", ClassCI), testEpoch.Add(2*time.Second))
	s = releaseNeutral(t, s, testEpoch.Add(3*time.Second))
	if s.Phase != GrantPending || len(s.Queue) != 0 {
		t.Fatalf("release left a ready board holding its queue: %#v", s)
	}
	if s.Lease == nil || s.Lease.WaiterID != "next-up" {
		t.Fatalf("release granted the wrong waiter: %#v", s.Lease)
	}
}

func TestReleasingAnEmptyQueueLeavesAReadyBoard(t *testing.T) {
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("only", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	s = releaseNeutral(t, s, testEpoch.Add(2*time.Second))
	if s.Phase != Ready || len(s.Queue) != 0 || s.Lease != nil {
		t.Fatalf("release did not leave a free board: %#v", s)
	}
	if err := Validate(s); err != nil {
		t.Fatalf("a released board was refused: %v", err)
	}
}

func TestCompletingRecoveryGrantsTheQueueItKept(t *testing.T) {
	// The other path into Ready. It hands the snapshot to grantNext for the
	// same reason release does, so the waiters recovery retained are served
	// by the command that ends it.
	s := boardForTest(t)
	s, _ = applyForTest(t, s, request("holder", ClassCI), testEpoch)
	s = ack(t, s, testEpoch.Add(time.Second))
	s, _ = applyForTest(t, s, request("kept", ClassCI), testEpoch.Add(2*time.Second))
	s, _ = applyForTest(t, s, AgentUnavailable{Actor: "server", Reason: "heartbeat lost"}, testEpoch.Add(3*time.Second))
	s, _ = applyForTest(t, s, BeginRecovery{Actor: "operator", PlanID: "profile-v1", Reason: "restore"}, testEpoch.Add(4*time.Second))
	if len(s.Queue) != 1 {
		t.Fatalf("recovery dropped the queue it should keep: %#v", s)
	}
	s, _ = applyForTest(t, s, CompleteRecovery{Actor: "operator", NeutralReceipt: "neutral-proof", AgentHighWater: s.AgentHighWater}, testEpoch.Add(5*time.Second))
	if s.Phase != GrantPending || len(s.Queue) != 0 || s.Lease == nil {
		t.Fatalf("completed recovery left a ready board holding its queue: %#v", s)
	}
}

func TestAnUnknownPhaseIsNotThisDoorsRefusal(t *testing.T) {
	// The phase switch in Validate owns that one, and it runs first.
	err := Validate(freeBoardSnapshot(Phase("wandering"), freeBoardWaiter("w1", 1)))
	if !IsCode(err, InvalidArgument) {
		t.Fatalf("an unknown phase was not refused as such: %v", err)
	}
}
