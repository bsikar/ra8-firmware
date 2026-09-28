// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"strings"
	"testing"

	"github.com/actions/scaleset"
)

// A runner is a machine this plane can hand credentials to and take away
// again, so every answer GitHub gives about one is checked against this
// session's own scale set before it is believed or acted on.
//
// fakeScaleSetAdmin shares a single err across all four calls, so it cannot
// let a lookup succeed and a removal fail. scriptedAdmin can.
type scriptedAdmin struct {
	byName      *scaleset.RunnerReference
	byNameErr   error
	runner      *scaleset.RunnerReference
	runnerErr   error
	removeErr   error
	lookedUp    []string
	removals    []int64
	jitRequests int
}

func (a *scriptedAdmin) GenerateJitRunnerConfig(context.Context, *scaleset.RunnerScaleSetJitRunnerSetting, int) (*scaleset.RunnerScaleSetJitRunnerConfig, error) {
	a.jitRequests++
	return nil, errors.New("unexpected JIT request")
}

func (a *scriptedAdmin) GetRunner(context.Context, int) (*scaleset.RunnerReference, error) {
	return a.runner, a.runnerErr
}

func (a *scriptedAdmin) GetRunnerByName(_ context.Context, name string) (*scaleset.RunnerReference, error) {
	a.lookedUp = append(a.lookedUp, name)
	return a.byName, a.byNameErr
}

func (a *scriptedAdmin) RemoveRunner(_ context.Context, id int64) error {
	a.removals = append(a.removals, id)
	return a.removeErr
}

// A name this plane would not have minted is refused before GitHub is
// asked, since a lookup is also a way to learn that a name exists.
func TestAnUnmintableRunnerNameIsNeverLookedUp(t *testing.T) {
	admin := &scriptedAdmin{}
	session := &Session{admin: admin, scaleSetID: 42}
	for _, name := range []string{
		"", "-leading-dash", "ra8_lab_9", "ra8 lab", "../etc/passwd", "ra8-lab-9/extra",
		strings.Repeat("r", 65), "RA8-lab-9\n",
	} {
		if _, ok, err := session.RunnerByName(context.Background(), name); err == nil || ok {
			t.Fatalf("name %q was looked up: ok=%v err=%v", name, ok, err)
		}
	}
	// The bound itself is exact: one character shorter is a name.
	if _, _, err := session.RunnerByName(context.Background(), strings.Repeat("r", 64)); err != nil {
		t.Fatalf("a 64-character name was refused: %v", err)
	}
	if len(admin.lookedUp) != 1 {
		t.Fatalf("names that reached GitHub: %v", admin.lookedUp)
	}

	// The same holds for a session that is not in a position to ask.
	unusable := map[string]*Session{
		"no session":    nil,
		"no admin":      {scaleSetID: 42},
		"no scale set":  {admin: admin},
		"a negative id": {admin: admin, scaleSetID: -1},
	}
	for name, s := range unusable {
		if _, ok, err := s.RunnerByName(context.Background(), "ra8-lab-9"); err == nil || ok {
			t.Fatalf("%s looked a runner up: ok=%v err=%v", name, ok, err)
		}
		if err := s.RemoveRunner(context.Background(), 9); err == nil {
			t.Fatalf("%s removed a runner", name)
		}
	}
	if _, ok, err := session.RunnerByName(nil, "ra8-lab-9"); err == nil || ok {
		t.Fatalf("a nil caller looked a runner up: ok=%v err=%v", ok, err)
	}
	if len(admin.lookedUp) != 1 || len(admin.removals) != 0 {
		t.Fatalf("looked up %v, removed %v", admin.lookedUp, admin.removals)
	}
}

// A runner GitHub does not know is a normal absence, not a failure: the
// scaler asks this question about machines it may already have destroyed.
func TestAnAbsentRunnerIsAnAnswerNotAFailure(t *testing.T) {
	admin := &scriptedAdmin{}
	session := &Session{admin: admin, scaleSetID: 42}
	got, ok, err := session.RunnerByName(context.Background(), "ra8-lab-9")
	if err != nil || ok || got != (RunnerIdentity{}) {
		t.Fatalf("RunnerByName = %+v, %v, %v", got, ok, err)
	}
	if len(admin.lookedUp) != 1 || admin.lookedUp[0] != "ra8-lab-9" {
		t.Fatalf("the name asked about was %v", admin.lookedUp)
	}

	// Removing one is then a no-op rather than a call GitHub has to refuse.
	if err := session.RemoveRunner(context.Background(), 9); err != nil {
		t.Fatalf("removing an absent runner = %v", err)
	}
	if len(admin.removals) != 0 {
		t.Fatalf("an absent runner was removed: %v", admin.removals)
	}
}

// An identity that does not answer to the name asked about, or belongs to
// another scale set, is refused by name rather than returned.
func TestALookupThatAnswersWithSomebodyElseIsRefused(t *testing.T) {
	for name, runner := range map[string]*scaleset.RunnerReference{
		"another name":      {ID: 9, Name: "ra8-lab-8", RunnerScaleSetID: 42},
		"another scale set": {ID: 9, Name: "ra8-lab-9", RunnerScaleSetID: 43},
		"no scale set":      {ID: 9, Name: "ra8-lab-9"},
		"no id":             {ID: 0, Name: "ra8-lab-9", RunnerScaleSetID: 42},
		"a negative id":     {ID: -3, Name: "ra8-lab-9", RunnerScaleSetID: 42},
	} {
		admin := &scriptedAdmin{byName: runner}
		session := &Session{admin: admin, scaleSetID: 42}
		got, ok, err := session.RunnerByName(context.Background(), "ra8-lab-9")
		if err == nil || !strings.Contains(err.Error(), "foreign identity") || ok || got != (RunnerIdentity{}) {
			t.Fatalf("%s was accepted: %+v, %v, %v", name, got, ok, err)
		}
	}
}

// A lookup GitHub could not answer is carried out with its own reason
// attached, so an operator is not told a live runner is gone.
func TestALookupGitHubCouldNotAnswerIsNotAnAbsence(t *testing.T) {
	admin := &scriptedAdmin{byNameErr: errors.New("502 Bad Gateway")}
	session := &Session{admin: admin, scaleSetID: 42}
	got, ok, err := session.RunnerByName(context.Background(), "ra8-lab-9")
	if err == nil || !strings.Contains(err.Error(), "look up GitHub runner by name") ||
		!strings.Contains(err.Error(), "502 Bad Gateway") || ok || got != (RunnerIdentity{}) {
		t.Fatalf("RunnerByName = %+v, %v, %v", got, ok, err)
	}
}

// A removal is not reported as done when GitHub refused it, and the reason
// names the runner, since an unremoved runner keeps drawing jobs.
func TestARefusedRemovalIsReportedWithTheRunnerNamed(t *testing.T) {
	admin := &scriptedAdmin{
		runner:    &scaleset.RunnerReference{ID: 9, Name: "ra8-lab-9", RunnerScaleSetID: 42},
		removeErr: errors.New("409 Conflict"),
	}
	session := &Session{admin: admin, scaleSetID: 42}
	err := session.RemoveRunner(context.Background(), 9)
	if err == nil || !strings.Contains(err.Error(), "remove GitHub runner 9") ||
		!strings.Contains(err.Error(), "409 Conflict") {
		t.Fatalf("RemoveRunner = %v", err)
	}
	if len(admin.removals) != 1 || admin.removals[0] != 9 {
		t.Fatalf("removals = %v", admin.removals)
	}

	// A lookup that fails on the way to a removal stops it outright.
	failing := &scriptedAdmin{runnerErr: errors.New("503 Service Unavailable")}
	session = &Session{admin: failing, scaleSetID: 42}
	if err := session.RemoveRunner(context.Background(), 9); err == nil ||
		!strings.Contains(err.Error(), "503 Service Unavailable") {
		t.Fatalf("RemoveRunner over a failed lookup = %v", err)
	}
	if len(failing.removals) != 0 {
		t.Fatalf("a runner was removed without being confirmed: %v", failing.removals)
	}
}
