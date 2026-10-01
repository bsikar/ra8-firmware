// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// frozenFor builds the record half of the pair, and ranAs the result half, so
// each test states only the two names it is about.
func frozenFor(task string) Entry {
	return Entry{ID: "0123456789abcdef0123456789abcdef", Task: task, SyncState: "running"}
}

func ranAs(task string) executor.Result {
	return executor.Result{TaskName: task}
}

func TestAResultNamingTheFrozenTaskIsFrozen(t *testing.T) {
	if err := checkTheResultNamesTheFrozenTask(frozenFor("build"), ranAs("build")); err != nil {
		t.Fatalf("a result naming the frozen task refused: %v", err)
	}
}

func TestAResultNamingAnotherTaskIsRefused(t *testing.T) {
	err := checkTheResultNamesTheFrozenTask(frozenFor("build"), ranAs("test"))
	if !errors.Is(err, errResultNamesAnotherTask) {
		t.Fatalf("want errResultNamesAnotherTask, got %v", err)
	}
	if !strings.Contains(err.Error(), `"build"`) || !strings.Contains(err.Error(), `"test"`) {
		t.Fatalf("refusal does not name both tasks: %v", err)
	}
}

// An attempt that came apart before the executor filled its account in names
// nothing, and the server takes that record, so the freeze does too.
func TestAResultNamingNothingIsLeftToTheDoorThatClassifies(t *testing.T) {
	if err := checkTheResultNamesTheFrozenTask(frozenFor("build"), ranAs("")); err != nil {
		t.Fatalf("a result naming nothing refused: %v", err)
	}
}

// The names are compared as written. A record frozen for no task at all is the
// identity door's business, not this one's, so a result naming nothing beside
// it is not this door's refusal either.
func TestARecordFrozenForNoTaskIsLeftToTheIdentityDoor(t *testing.T) {
	if err := checkTheResultNamesTheFrozenTask(frozenFor(""), ranAs("")); err != nil {
		t.Fatalf("an unnamed pair refused here rather than at the identity door: %v", err)
	}
}

// A record frozen for no task whose result names one is still a disagreement:
// the evidence claims something the record never did.
func TestAResultNamingATaskTheRecordDidNotIsRefused(t *testing.T) {
	err := checkTheResultNamesTheFrozenTask(frozenFor(""), ranAs("build"))
	if !errors.Is(err, errResultNamesAnotherTask) {
		t.Fatalf("want errResultNamesAnotherTask, got %v", err)
	}
}

// The comparison is exact rather than forgiving: case and surrounding space are
// part of the name the plane files, and the store holds a task name to
// TrimSpace equality of its own.
func TestATaskNameDifferingOnlyInCaseIsRefused(t *testing.T) {
	err := checkTheResultNamesTheFrozenTask(frozenFor("build"), ranAs("Build"))
	if !errors.Is(err, errResultNamesAnotherTask) {
		t.Fatalf("want errResultNamesAnotherTask, got %v", err)
	}
}

func TestATaskNameDifferingOnlyInSpaceIsRefused(t *testing.T) {
	err := checkTheResultNamesTheFrozenTask(frozenFor("build"), ranAs("build "))
	if !errors.Is(err, errResultNamesAnotherTask) {
		t.Fatalf("want errResultNamesAnotherTask, got %v", err)
	}
}

// The door judges the two names and nothing else about the result: the steps,
// the exits and the stamps are other doors' business at the same freeze.
func TestTheTaskNameDoorJudgesOnlyTheTwoNames(t *testing.T) {
	result := ranAs("build")
	result.ExitCode = 70000
	result.Steps = []executor.StepResult{{Name: "", TimedOut: true, Cancelled: true}}
	if err := checkTheResultNamesTheFrozenTask(frozenFor("build"), result); err != nil {
		t.Fatalf("the name door judged something else about the result: %v", err)
	}
}
