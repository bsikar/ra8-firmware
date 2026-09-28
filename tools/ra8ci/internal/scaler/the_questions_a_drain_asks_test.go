// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/actions/scaleset"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A drain asks GitHub twice: once before it deregisters the runner, to be
// sure the runner it is about to remove is the one this reservation owns,
// and once after, to be sure the removal actually took. Either question can
// go unanswered, and the two are not the same failure: the first means
// nothing was touched, the second means a runner may well be gone while the
// plane cannot say so. The wordings have to tell them apart.

// failingLookup wraps the observer's admin fake rather than replacing it, so
// a chosen lookup fails and every other call behaves as it always does.
type failingLookup struct {
	*observerAdminFake
	failOn int
	seen   int
	reason error
}

func (a *failingLookup) RunnerByID(ctx context.Context, id int) (github.RunnerIdentity, bool, error) {
	a.seen++
	if a.seen == a.failOn {
		return github.RunnerIdentity{}, false, a.reason
	}
	return a.observerAdminFake.RunnerByID(ctx, id)
}

func drainable(t *testing.T) (*observerAdminFake, store.RunnerVM, github.Job) {
	t.Helper()
	_, admin, vm, job := observerFixture(t)
	vm.State = "draining"
	vm.CleanupRequested = true
	job.Kind = scaleset.MessageTypeJobCompleted
	job.Result = "Succeeded"
	job.FinishTime = time.Now().UTC()
	return admin, vm, job
}

func TestADrainSaysWhichOfItsTwoQuestionsWentUnanswered(t *testing.T) {
	for _, attempt := range []struct {
		name    string
		failOn  int
		says    string
		removes int
	}{
		{"the question before the removal", 1, "verify runner before deregistration", 0},
		{"the question after it", 2, "verify runner deregistration", 1},
	} {
		admin, vm, job := drainable(t)
		scripted := &failingLookup{observerAdminFake: admin, failOn: attempt.failOn, reason: errors.New("GitHub API unavailable")}
		observer, err := NewGitHubRunnerObserver(42, scripted)
		if err != nil {
			t.Fatal(err)
		}

		evidence, err := observer.DrainAndDeregister(context.Background(), vm, job)
		if err == nil {
			t.Errorf("%s: an unanswered drain was accepted: %+v", attempt.name, evidence)
			continue
		}
		if !strings.Contains(err.Error(), attempt.says) {
			t.Errorf("%s: the refusal read %v", attempt.name, err)
		}
		if !strings.Contains(err.Error(), "GitHub API unavailable") {
			t.Errorf("%s: the refusal dropped GitHub's own reason: %v", attempt.name, err)
		}
		if evidence.EvidenceID != "" || evidence.RunnerDeregistered || evidence.Drained {
			t.Errorf("%s: an unanswered drain still claimed evidence: %+v", attempt.name, evidence)
		}
		if admin.removeCalls != attempt.removes {
			t.Errorf("%s: the runner was removed %d times, wanted %d", attempt.name, admin.removeCalls, attempt.removes)
		}
	}
}

// The two wordings are the only thing an operator has to tell the two
// questions apart, so they must not be the same sentence.
func TestTheTwoDrainRefusalsAreNotTheSameSentence(t *testing.T) {
	said := make(map[string]int, 2)
	for _, failOn := range []int{1, 2} {
		admin, vm, job := drainable(t)
		observer, err := NewGitHubRunnerObserver(42, &failingLookup{
			observerAdminFake: admin, failOn: failOn, reason: errors.New("boom")})
		if err != nil {
			t.Fatal(err)
		}
		if _, err := observer.DrainAndDeregister(context.Background(), vm, job); err != nil {
			said[err.Error()]++
		}
	}
	if len(said) != 2 {
		t.Fatalf("the two unanswered questions read the same: %v", said)
	}
}

// A call the caller has already given up on is refused on the record it was
// handed, before GitHub is asked anything at all.
func TestAnObservationCalledOffIsRefusedBeforeGitHubIsAsked(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	observer, admin, vm, job := observerFixture(t)
	if _, err := observer.Registered(ctx, vm, job); err == nil {
		t.Error("a called-off registration was accepted")
	}

	drainAdmin, drainVM, drainJob := drainable(t)
	drainObserver, err := NewGitHubRunnerObserver(42, drainAdmin)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := drainObserver.DrainAndDeregister(ctx, drainVM, drainJob); err == nil {
		t.Error("a called-off drain was accepted")
	}

	if admin.calls != 0 || drainAdmin.calls != 0 || drainAdmin.removeCalls != 0 {
		t.Fatalf("a called-off observation asked GitHub anyway: %d / %d lookups, %d removals",
			admin.calls, drainAdmin.calls, drainAdmin.removeCalls)
	}
}
