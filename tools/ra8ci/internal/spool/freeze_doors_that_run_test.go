// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// A door with no call site refuses nothing. checkEachStepCanBeNamed and
// checkTheResultNamesTheFrozenTask were both written against Finish and both
// tested by calling them directly, which passes whether or not Finish ever
// reaches them. These tests drive the whole freeze instead, so the wiring is
// what is under test rather than the rule.

func freezeForTest(t *testing.T) (*Spool, Entry) {
	t.Helper()
	spool, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	metadata := Metadata{Source: filable("bsikar/ra8-firmware", "ra8ci/dev"),
		Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 600}
	entry, err := spool.BeginWithMetadata("format-check", strings.Repeat("b", 64), metadata)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	return spool, entry
}

func TestFinishRefusesAStepThePlaneCannotName(t *testing.T) {
	spool, entry := freezeForTest(t)
	unnamed := executor.Result{TaskName: "format-check",
		Steps: []executor.StepResult{measuredStep("")}}
	if _, err := spool.Finish(entry, unnamed, nil); !errors.Is(err, errUnnameableStep) {
		t.Fatalf("a step stating no name was written into a terminal record: %v", err)
	}
}

func TestFinishRefusesTwoStepsSharingOneName(t *testing.T) {
	spool, entry := freezeForTest(t)
	twice := executor.Result{TaskName: "format-check",
		Steps: []executor.StepResult{measuredStep("build"), measuredStep("build")}}
	if _, err := spool.Finish(entry, twice, nil); !errors.Is(err, errUnnameableStep) {
		t.Fatalf("two steps sharing one name were written into a terminal record: %v", err)
	}
}

func TestFinishRefusesAResultNamingAnotherTask(t *testing.T) {
	spool, entry := freezeForTest(t)
	elsewhere := executor.Result{TaskName: "unit-tests"}
	if _, err := spool.Finish(entry, elsewhere, nil); !errors.Is(err, errResultNamesAnotherTask) {
		t.Fatalf("a result naming another task was written into a terminal record: %v", err)
	}
}

// The freeze stays no stricter than the far end on the shapes the far end
// keeps: a result naming nothing is how an attempt that came apart before the
// executor filled the name in reaches history, and named steps pass.
func TestFinishStillWritesDownAnOrdinaryAttempt(t *testing.T) {
	spool, entry := freezeForTest(t)
	ordinary := executor.Result{TaskName: "format-check",
		Steps: []executor.StepResult{measuredStep("build"), measuredStep("check")}}
	if _, err := spool.Finish(entry, ordinary, nil); err != nil {
		t.Fatalf("an ordinary attempt was refused: %v", err)
	}
	spool, entry = freezeForTest(t)
	if _, err := spool.Finish(entry, executor.Result{}, errors.New("torn down")); err != nil {
		t.Fatalf("a result naming nothing was refused: %v", err)
	}
}
