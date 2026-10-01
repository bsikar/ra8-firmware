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
	return PublishedCheckRuns{HeadSHA: run.HeadSHA, Runs: []PublishedCheckRun{ourRun(run, id, status, conclusion, title)}}
}

// ourRun renders one intended run as a run this plane already published,
// carrying the external identifier the publisher posts. A listing built
// without it is a listing of somebody else's runs, which is a different case
// and has its own tests below.
func ourRun(run TaskCheckRun, id int64, status, conclusion, title string) PublishedCheckRun {
	identifier, _ := CheckRunExternalID(run)
	return PublishedCheckRun{
		ID: id, Name: run.Name, Mode: run.Mode,
		Status: status, Conclusion: conclusion, Title: title, ExternalID: identifier,
	}
}

// foreignRun renders a run left under one of our names by something else.
func foreignRun(run TaskCheckRun, id int64, status, conclusion, title, identifier string) PublishedCheckRun {
	published := ourRun(run, id, status, conclusion, title)
	published.ExternalID = identifier
	return published
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
	published.Runs = append(published.Runs, ourRun(run, 9, "completed", run.Conclusion, run.Title))
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
		published.Runs = append(published.Runs, ourRun(run, 4, "completed", "failure", ""))
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

// A name is public. Anything holding a checks:write token on the repository
// can post under one of ours, and the external identifier is what tells the
// two apart: a run carrying somebody else's identifier is a collision to
// report even when it says exactly what this plane was about to say.
func TestARunWeDidNotPostIsAConflictHoweverWellItAgrees(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	elsewhere := foreignRun(run, 11, "completed", run.Conclusion, run.Title, "ra8ci-1-"+strings.Repeat("0", 32))
	reconciled, err := ReconcilePublish(run, PublishedCheckRuns{HeadSHA: run.HeadSHA, Runs: []PublishedCheckRun{elsewhere}})
	if err != nil {
		t.Fatalf("ReconcilePublish: %v", err)
	}
	if reconciled.Decision != PublishConflicts || reconciled.Repeat() {
		t.Fatalf("reconciled = %+v", reconciled)
	}
	if len(reconciled.Unclaimed) != 1 || reconciled.Unclaimed[0].ID != 11 {
		t.Fatalf("unclaimed = %+v", reconciled.Unclaimed)
	}
	if len(reconciled.Existing) != 1 {
		t.Fatalf("existing = %+v", reconciled.Existing)
	}
}

// A run with no identifier at all is either one posted before this plane wrote
// the field or one posted by something else. Nothing here can tell which, and
// answering for an operator in either direction is worse than reporting it.
func TestARunCarryingNoIdentifierIsNotClaimed(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	before := foreignRun(run, 12, "completed", run.Conclusion, run.Title, "")
	reconciled, err := ReconcilePublish(run, PublishedCheckRuns{HeadSHA: run.HeadSHA, Runs: []PublishedCheckRun{before}})
	if err != nil {
		t.Fatalf("ReconcilePublish: %v", err)
	}
	if reconciled.Decision != PublishConflicts || len(reconciled.Unclaimed) != 1 {
		t.Fatalf("reconciled = %+v", reconciled)
	}
}

// Somebody else's run is reported whatever state it is in. Waiting for it is
// waiting for an answer to a question it was never asked, so the identity is
// settled ahead of the status the in-flight answer reads.
func TestSomebodyElsesRunIsNeverWaitedFor(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	for _, status := range []string{"queued", "in_progress", "waiting", "pending"} {
		theirs := foreignRun(run, 13, status, "", "", "ra8ci-1-"+strings.Repeat("f", 32))
		reconciled, err := ReconcilePublish(run, PublishedCheckRuns{HeadSHA: run.HeadSHA, Runs: []PublishedCheckRun{theirs}})
		if err != nil {
			t.Fatalf("%s ReconcilePublish: %v", status, err)
		}
		if reconciled.Decision != PublishConflicts {
			t.Fatalf("%s reconciled = %+v", status, reconciled)
		}
	}
}

// Every run the decision was made from is reported, and the ones this plane
// did not post are named among them: an operator opening the commit has to
// know which of several runs under one name is the one that does not belong.
func TestTheRunsWeDidNotPostAreNamedAmongTheRest(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	published := publishedAs(run, 14, "completed", run.Conclusion, run.Title)
	published.Runs = append(published.Runs,
		foreignRun(run, 15, "completed", run.Conclusion, run.Title, "ra8ci-1-"+strings.Repeat("a", 32)))
	reconciled, err := ReconcilePublish(run, published)
	if err != nil {
		t.Fatalf("ReconcilePublish: %v", err)
	}
	if reconciled.Decision != PublishConflicts || len(reconciled.Existing) != 2 {
		t.Fatalf("reconciled = %+v", reconciled)
	}
	if len(reconciled.Unclaimed) != 1 || reconciled.Unclaimed[0].ID != 15 {
		t.Fatalf("unclaimed = %+v", reconciled.Unclaimed)
	}
}

// The two conflicts are different work: our own runs disagreeing about one
// commit is a question about this deployment, and a stranger under our name is
// a question about who holds a token. A disagreement among our own runs
// therefore names nothing unclaimed.
func TestOurOwnDisagreementNamesNothingUnclaimed(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	reconciled, err := ReconcilePublish(run, publishedAs(run, 16, "completed", "failure", "failed"))
	if err != nil {
		t.Fatalf("ReconcilePublish: %v", err)
	}
	if reconciled.Decision != PublishConflicts {
		t.Fatalf("reconciled = %+v", reconciled)
	}
	if len(reconciled.Unclaimed) != 0 {
		t.Fatalf("unclaimed = %+v", reconciled.Unclaimed)
	}
}

// Nothing short of a conflict carries unclaimed runs, so a caller can read the
// list as the reason for the refusal rather than as a warning beside an answer
// that was fine.
func TestOnlyAConflictCarriesUnclaimedRuns(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	listings := map[PublishDecision]PublishedCheckRuns{
		PublishNeeded:   {HeadSHA: run.HeadSHA},
		PublishSettled:  publishedAs(run, 17, "completed", run.Conclusion, run.Title),
		PublishInFlight: publishedAs(run, 18, "queued", "", ""),
	}
	for want, published := range listings {
		reconciled, err := ReconcilePublish(run, published)
		if err != nil {
			t.Fatalf("%s ReconcilePublish: %v", want, err)
		}
		if reconciled.Decision != want {
			t.Fatalf("decision %s, want %s", reconciled.Decision, want)
		}
		if len(reconciled.Unclaimed) != 0 {
			t.Fatalf("%s unclaimed = %+v", want, reconciled.Unclaimed)
		}
	}
}

// A summary is carried through the decision and never decides it. The
// intended run has no summary to compare against, and the field is free text
// a duration or a re-wording changes without the outcome changing, so
// matching on it would turn an ordinary republish into a conflict somebody
// has to clear by hand. It is reported so the person reading the listing can
// see a run whose title agrees while its summary describes other work.
func TestASummaryIsReportedAndNeverMatchedOn(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	published := publishedAs(run, 21, "completed", run.Conclusion, run.Title)
	published.Runs[0].Summary = "ran the wrong board and said so at length"

	reconciled, err := ReconcilePublish(run, published)
	if err != nil {
		t.Fatalf("reconcile: %v", err)
	}
	if reconciled.Decision != PublishSettled {
		t.Fatalf("decision = %s, want settled: the summary is not part of the match", reconciled.Decision)
	}
	if len(reconciled.Existing) != 1 || reconciled.Existing[0].Summary != "ran the wrong board and said so at length" {
		t.Fatalf("existing = %+v, want the summary carried through verbatim", reconciled.Existing)
	}
}

// Two of our runs differing only in their summaries are still settled, and
// both summaries survive into the answer. This is the case the reporting
// exists for: nothing here can say which of the two is right.
func TestTwoRunsDifferingOnlyInSummaryAreStillSettled(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	published := publishedAs(run, 22, "completed", run.Conclusion, run.Title)
	published.Runs[0].Summary = "41 cases, 0 failures"
	second := ourRun(run, 23, "completed", run.Conclusion, run.Title)
	second.Summary = "12 cases, 0 failures"
	published.Runs = append(published.Runs, second)

	reconciled, err := ReconcilePublish(run, published)
	if err != nil {
		t.Fatalf("reconcile: %v", err)
	}
	if reconciled.Decision != PublishSettled {
		t.Fatalf("decision = %s, want settled", reconciled.Decision)
	}
	if len(reconciled.Existing) != 2 ||
		reconciled.Existing[0].Summary != "41 cases, 0 failures" ||
		reconciled.Existing[1].Summary != "12 cases, 0 failures" {
		t.Fatalf("existing = %+v, want both summaries reported", reconciled.Existing)
	}
}

// A disagreement is still decided on what the run concluded, whatever its
// summary says. A matching summary does not rescue a conflicting conclusion.
func TestAnAgreeingSummaryDoesNotSettleADisagreeingRun(t *testing.T) {
	run := intendedRun(t, ModeAuthoritative, "succeeded")
	published := publishedAs(run, 24, "completed", "failure", "failed")
	published.Runs[0].Summary = run.Title

	reconciled, err := ReconcilePublish(run, published)
	if err != nil {
		t.Fatalf("reconcile: %v", err)
	}
	if reconciled.Decision != PublishConflicts {
		t.Fatalf("decision = %s, want conflicts", reconciled.Decision)
	}
}
