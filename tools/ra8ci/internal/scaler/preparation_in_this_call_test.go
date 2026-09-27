// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"strings"
	"testing"
	"time"
)

// bracket is one bootstrap call: the two readings the handler takes around
// Prepare. Each test states a preparation stamp against it.
func bracket() (string, time.Time, time.Time) {
	started := time.Date(2026, 9, 26, 22, 0, 0, 0, time.UTC)
	return "0199502b-1234-7abc-8abc-0123456789ab", started, started.Add(40 * time.Second)
}

func TestAPreparationInsideItsOwnCallIsAccepted(t *testing.T) {
	id, started, completed := bracket()
	for _, at := range []time.Time{started, started.Add(time.Second), started.Add(20 * time.Second), completed} {
		if err := checkPreparationHappenedInThisCall(id, started, completed, at); err != nil {
			t.Fatalf("a preparation stamped inside its own call was refused: %v", err)
		}
	}
}

// The two clocks are read a moment apart, so a stamp just outside either edge
// is the ordinary case rather than a replay.
func TestTheClockAllowanceIsHonouredAtBothEdges(t *testing.T) {
	id, started, completed := bracket()
	if err := checkPreparationHappenedInThisCall(id, started, completed, started.Add(-preparationClockSkew)); err != nil {
		t.Fatalf("a preparation a second before the call began was refused: %v", err)
	}
	if err := checkPreparationHappenedInThisCall(id, started, completed, completed.Add(preparationClockSkew)); err != nil {
		t.Fatalf("a preparation a second after the call returned was refused: %v", err)
	}
}

// The shape this exists for: a receipt from an earlier bootstrap of the same
// reservation, which carries the same identity fields and is still well
// inside the five minutes every other door allows.
func TestAReceiptFromAnEarlierBootstrapIsRefused(t *testing.T) {
	id, started, completed := bracket()
	err := checkPreparationHappenedInThisCall(id, started, completed, started.Add(-90*time.Second))
	if err == nil {
		t.Fatal("a preparation stamped before the bootstrap was asked for was accepted")
	}
	if !strings.Contains(err.Error(), id) || !strings.Contains(err.Error(), "before the bootstrap was asked for") {
		t.Fatalf("the refusal did not name the reservation or what is wrong with the stamp: %v", err)
	}
	if !strings.Contains(err.Error(), "1m30s") {
		t.Fatalf("the refusal did not say how far outside the call the stamp sits: %v", err)
	}
}

func TestEveryStampBeforeTheCallIsRefused(t *testing.T) {
	id, started, completed := bracket()
	for _, early := range []time.Duration{preparationClockSkew + time.Millisecond, 5 * time.Second, 4 * time.Minute} {
		if err := checkPreparationHappenedInThisCall(id, started, completed, started.Add(-early)); err == nil {
			t.Fatalf("a preparation %s before the call was accepted", early)
		}
	}
}

func TestAStampAfterTheCallReturnedIsRefused(t *testing.T) {
	id, started, completed := bracket()
	err := checkPreparationHappenedInThisCall(id, started, completed, completed.Add(30*time.Second))
	if err == nil {
		t.Fatal("a preparation stamped after the bootstrap returned was accepted")
	}
	if !strings.Contains(err.Error(), "after the bootstrap returned") {
		t.Fatalf("the refusal did not say the stamp is later than the call: %v", err)
	}
}

func TestAnUnbracketedBootstrapIsRefused(t *testing.T) {
	id, started, completed := bracket()
	if err := checkPreparationHappenedInThisCall(id, time.Time{}, completed, started); err == nil {
		t.Fatal("a bootstrap with no start reading was accepted")
	}
	if err := checkPreparationHappenedInThisCall(id, started, time.Time{}, started); err == nil {
		t.Fatal("a bootstrap with no completion reading was accepted")
	}
}

func TestAnInvertedBracketIsRefused(t *testing.T) {
	id, started, completed := bracket()
	err := checkPreparationHappenedInThisCall(id, completed, started, started)
	if err == nil {
		t.Fatal("a bootstrap that returned before it was asked for was accepted")
	}
	if !strings.Contains(err.Error(), "before it was asked for") {
		t.Fatalf("the refusal did not name the inverted bracket: %v", err)
	}
}

// A call that returns in the same instant it was asked for is a cached or
// already-reconciled preparation, and the allowance still admits it.
func TestAnInstantBootstrapIsAccepted(t *testing.T) {
	id, started, _ := bracket()
	if err := checkPreparationHappenedInThisCall(id, started, started, started); err != nil {
		t.Fatalf("a bootstrap that returned immediately was refused: %v", err)
	}
}

// A zero preparation stamp is refused here too, and by the same arm: it sits
// far before any real bracket. bootstrapRunning refuses it separately, so this
// only pins that the rule does not quietly admit it.
func TestAZeroPreparationStampIsRefused(t *testing.T) {
	id, started, completed := bracket()
	if err := checkPreparationHappenedInThisCall(id, started, completed, time.Time{}); err == nil {
		t.Fatal("a receipt with no preparation stamp at all was accepted")
	}
}

func TestTheAllowanceIsTheOneTheOtherDoorsState(t *testing.T) {
	if preparationClockSkew != time.Second {
		t.Fatalf("the clock allowance is %s, not the second bootstrapRunning and the ledger allow", preparationClockSkew)
	}
}
