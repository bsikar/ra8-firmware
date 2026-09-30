// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardsweep

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A pass over a sweeper that was never wired has to refuse rather than fault.
// New is the only thing that hands out a wired sweeper, but a zero value and a
// nil pointer are both spellable by a caller that skipped it, and a sweep that
// panicked there would take down the process running every other pass.
func TestAPassRefusesASweeperThatWasNeverWired(t *testing.T) {
	for name, sweeper := range map[string]*Sweeper{
		"a nil sweeper":  nil,
		"a zero sweeper": {},
	} {
		report, err := sweeper.Pass(context.Background(), time.Now())
		if err == nil {
			t.Errorf("%s ran a pass and answered %+v", name, report)
			continue
		}
		// The refusal is the store's invalid-input kind, so a caller that
		// already separates a bad argument from a ledger failure keeps doing
		// so here.
		if !errors.Is(err, store.ErrInvalid) {
			t.Errorf("%s was refused with the wrong kind: %v", name, err)
		}
		// Nothing is claimed alongside the refusal: a report carrying counts
		// would read as a pass that found nothing to do.
		if report != (Report{}) {
			t.Errorf("%s answered %+v alongside the refusal", name, report)
		}
	}
}

// The wiring is judged before the context and the clock, so an unwired sweeper
// is named as unwired even when the call is unusable in every other way too.
func TestAnUnwiredSweeperIsNamedAheadOfTheContextAndClock(t *testing.T) {
	report, err := (*Sweeper)(nil).Pass(nil, time.Time{})
	if err == nil {
		t.Fatalf("an unwired sweeper ran a pass and answered %+v", report)
	}
	if !errors.Is(err, store.ErrInvalid) {
		t.Fatalf("refused with the wrong kind: %v", err)
	}
	if !strings.Contains(err.Error(), "not wired") {
		t.Fatalf("err = %v, want the wiring named rather than the context", err)
	}
}
