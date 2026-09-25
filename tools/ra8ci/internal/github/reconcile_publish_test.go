// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

func intendedRun(t *testing.T, mode CheckRunMode, state string) TaskCheckRun {
	t.Helper()
	names := catalogNames(t)
	if len(names) == 0 {
		t.Fatal("catalog carries no tasks")
	}
	run, err := NewTaskCheckRun(mode, names[0], reconcilerHead, state)
	if err != nil {
		t.Fatalf("NewTaskCheckRun: %v", err)
	}
	return run
}

func publishedAs(run TaskCheckRun, id int64, status, conclusion, title string) PublishedCheckRuns {
	return PublishedCheckRuns{HeadSHA: run.HeadSHA, Runs: []PublishedCheckRun{{
		ID: id, Name: run.Name, Mode: run.Mode, Status: status, Conclusion: conclusion, Title: title,
	}}}
}

// A commit carrying nothing under this name has not been published to, and
// that is the one answer that lets a caller post.
func TestNothingUnderTheNameMeansPublishIt(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	other := PublishedCheckRuns{HeadSHA: run.HeadSHA, Runs: []PublishedCheckRun{
		{ID: 1, Name: run.Name + "-other", Mode: run.Mode, Status: "completed", Conclusion: "success"},
	}}
	reconciled, err := ReconcilePublish(run, other)
	if err != nil {
		t.Fatalf("ReconcilePublish: %v", err)
	}
	if reconciled.Decision != PublishNeeded || !reconciled.Repeat() || len(reconciled.Existing) != 0 {
		t.Fatalf("reconciled = %+v", reconciled)
	}
}

// The run is already there, so it is not posted again. This is the whole point
// of the reconciliation the contract asks for.
func TestARunAlreadyOnTheCommitIsNotPostedAgain(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	reconciled, err := ReconcilePublish(run, publishedAs(run, 5, "completed", run.Conclusion, run.Title))
	if err != nil {
		t.Fatalf("ReconcilePublish: %v", err)
	}
	if reconciled.Decision != PublishSettled || reconciled.Repeat() {
		t.Fatalf("reconciled = %+v", reconciled)
	}
	if len(reconciled.Existing) != 1 || reconciled.Existing[0].ID != 5 {
		t.Fatalf("existing = %+v", reconciled.Existing)
	}
}

// Two identical runs under one name is a repeat that already happened. It is
// still settled: posting a third would only make it worse.
func TestTwoAgreeingRunsAreStillSettled(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	published := publishedAs(run, 5, "completed", run.Conclusion, run.Title)
	published.Runs = append(published.Runs, PublishedCheckRun{
		ID: 9, Name: run.Name, Mode: run.Mode, Status: "completed", Conclusion: run.Conclusion, Title: run.Title,
	})
	reconciled, err := ReconcilePublish(run, published)
	if err != nil {
		t.Fatalf("ReconcilePublish: %v", err)
	}
	if reconciled.Decision != PublishSettled || len(reconciled.Existing) != 2 {
		t.Fatalf("reconciled = %+v", reconciled)
	}
}

// Every completed shadow run reports neutral whatever the task did, so the
// conclusion alone cannot tell two opposite observations apart. The title is
// where the observation lives and it is part of the match.
func TestAShadowRunIsMatchedOnWhatItObserved(t *testing.T) {
	run := intendedRun(t, ModeShadow, "failed")
	if run.Conclusion != "neutral" {
		t.Fatalf("shadow run posted %q", run.Conclusion)
	}
	settled, err := ReconcilePublish(run, publishedAs(run, 1, "completed", "neutral", run.Title))
	if err != nil || settled.Decision != PublishSettled {
		t.Fatalf("settled = %+v, err = %v", settled, err)
	}
	opposite := intendedRun(t, ModeShadow, "succeeded")
	if opposite.Title == run.Title {
		t.Fatal("two shadow observations rendered the same title")
	}
	conflict, err := ReconcilePublish(run, publishedAs(run, 1, "completed", "neutral", opposite.Title))
	if err != nil {
		t.Fatalf("ReconcilePublish: %v", err)
	}
	if conflict.Decision != PublishConflicts {
		t.Fatalf("conflict = %+v", conflict)
	}
}

// A run still executing is the ordinary shape of an uncertain write: the post
// landed and the answer did not. It is waited for, and it is reported before
// any disagreement, because what it will conclude is not known yet.
func TestAnUnfinishedRunIsWaitedForRatherThanArguedWith(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	for _, status := range []string{"queued", "in_progress", "waiting", "pending"} {
		published := publishedAs(run, 3, status, "", "")
		published.Runs = append(published.Runs, PublishedCheckRun{
			ID: 4, Name: run.Name, Mode: run.Mode, Status: "completed", Conclusion: "failure",
		})
		reconciled, err := ReconcilePublish(run, published)
		if err != nil {
			t.Fatalf("ReconcilePublish: %v", err)
		}
		if reconciled.Decision != PublishInFlight || reconciled.Repeat() {
			t.Fatalf("%s reconciled = %+v", status, reconciled)
		}
		if len(reconciled.Existing) != 2 {
			t.Fatalf("%s existing = %+v", status, reconciled.Existing)
		}
	}
}

// A finished run saying something else is reported, never posted over: nothing
// here can tell which of the two is right.
func TestADisagreeingRunIsReportedNotOverwritten(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	reconciled, err := ReconcilePublish(run, publishedAs(run, 7, "completed", "failure", "failed"))
	if err != nil {
		t.Fatalf("ReconcilePublish: %v", err)
	}
	if reconciled.Decision != PublishConflicts || reconciled.Repeat() {
		t.Fatalf("reconciled = %+v", reconciled)
	}
	if len(reconciled.Existing) != 1 || reconciled.Existing[0].Conclusion != "failure" {
		t.Fatalf("existing = %+v", reconciled.Existing)
	}
}

// A run under one of our names that this plane did not post in this mode is a
// collision, not a run to post over.
func TestARunUnderOurNameInAnotherModeConflicts(t *testing.T) {
	run := intendedRun(t, ModeShadow, "succeeded")
	published := publishedAs(run, 2, "completed", run.Conclusion, run.Title)
	published.Runs[0].Mode = ModeAuthoritative
	reconciled, err := ReconcilePublish(run, published)
	if err != nil {
		t.Fatalf("ReconcilePublish: %v", err)
	}
	if reconciled.Decision != PublishConflicts {
		t.Fatalf("reconciled = %+v", reconciled)
	}
}

// A listing about another commit answers nothing about this publish, and
// GitHub's own casing of a SHA is not a different commit.
func TestTheListingHasToBeAboutTheRunsCommit(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	elsewhere := publishedAs(run, 1, "completed", run.Conclusion, run.Title)
	elsewhere.HeadSHA = strings.Repeat("a", 40)
	if _, err := ReconcilePublish(run, elsewhere); !errors.Is(err, ErrReconcileCommitMismatch) {
		t.Fatalf("err = %v", err)
	}
	empty := PublishedCheckRuns{}
	if _, err := ReconcilePublish(run, empty); !errors.Is(err, ErrReconcileCommitMismatch) {
		t.Fatalf("empty listing err = %v", err)
	}
	upper := publishedAs(run, 1, "completed", run.Conclusion, run.Title)
	upper.HeadSHA = strings.ToUpper(run.HeadSHA)
	reconciled, err := ReconcilePublish(run, upper)
	if err != nil || reconciled.Decision != PublishSettled {
		t.Fatalf("reconciled = %+v, err = %v", reconciled, err)
	}
}

// A run this package would refuse to publish is refused here too, before any
// listing is read: deciding what to do about a run nothing may post is a
// question with no useful answer.
func TestAnIntendedRunTheseRulesWouldNotPublishIsRefused(t *testing.T) {
	valid := intendedRun(t, ModeAuthoritative, "succeeded")
	cases := map[string]TaskCheckRun{
		"no name":       {HeadSHA: valid.HeadSHA, Status: "completed", Conclusion: "success"},
		"short sha":     {Name: valid.Name, HeadSHA: "0123456", Status: "completed", Conclusion: "success"},
		"not completed": {Name: valid.Name, HeadSHA: valid.HeadSHA, Status: "in_progress", Conclusion: "success"},
		"no conclusion": {Name: valid.Name, HeadSHA: valid.HeadSHA, Status: "completed"},
	}
	listing := publishedAs(valid, 1, "completed", valid.Conclusion, valid.Title)
	for name, run := range cases {
		t.Run(name, func(t *testing.T) {
			if _, err := ReconcilePublish(run, listing); !errors.Is(err, ErrReconcileRunIncomplete) {
				t.Fatalf("err = %v", err)
			}
		})
	}
	unknownMode := valid
	unknownMode.Mode = CheckRunMode(7)
	if _, err := ReconcilePublish(unknownMode, listing); !errors.Is(err, ErrInvalidCheckRunMode) {
		t.Fatalf("unknown mode err = %v", err)
	}
}

// Exactly one decision lets a caller post, and each names itself.
func TestOnlyOneDecisionPosts(t *testing.T) {
	names := map[PublishDecision]string{
		PublishNeeded:    "needed",
		PublishSettled:   "settled",
		PublishInFlight:  "in flight",
		PublishConflicts: "conflicts",
	}
	posting := 0
	for decision, name := range names {
		if decision.String() != name {
			t.Fatalf("%d named %q", int(decision), decision.String())
		}
		if (ReconciledPublish{Decision: decision}).Repeat() {
			posting++
			if decision != PublishNeeded {
				t.Fatalf("%s permits a second post", name)
			}
		}
	}
	if posting != 1 {
		t.Fatalf("%d decisions permit a post", posting)
	}
	if got := CheckRunMode(9).String(); !strings.Contains(got, "9") {
		t.Fatalf("unknown mode named %q", got)
	}
	if got := PublishDecision(9).String(); !strings.Contains(got, "9") {
		t.Fatalf("unknown decision named %q", got)
	}
}
