// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

// standingSubject returns a check run name of this plane's and the commit the
// reconciler tests use, which is the pair every standing is asked about.
func standingSubject(t *testing.T) (string, string) {
	t.Helper()
	first, _ := reconcilerTaskNames(t)
	name, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("check run name: %v", err)
	}
	return name, reconcilerHead
}

// standingRun builds a published run under one name carrying one identifier.
func standingRun(name, identifier string) PublishedCheckRun {
	return PublishedCheckRun{
		ID: 1, Name: name, Mode: ModeAuthoritative,
		Status: "completed", Conclusion: "success", ExternalID: identifier,
	}
}

func standingOf(t *testing.T, run PublishedCheckRun, headSHA string) ExternalIDStanding {
	t.Helper()
	standing, err := ExternalIDStandingOf(run, headSHA)
	if err != nil {
		t.Fatalf("ExternalIDStandingOf(%q): %v", run.ExternalID, err)
	}
	return standing
}

// The match itself: the identifier this plane would post the run with.
func TestTheIdentifierThisPlaneComputesIsOurs(t *testing.T) {
	name, head := standingSubject(t)
	identifier, err := CheckRunExternalID(TaskCheckRun{Name: name, HeadSHA: head})
	if err != nil {
		t.Fatalf("CheckRunExternalID: %v", err)
	}
	standing := standingOf(t, standingRun(name, identifier), head)
	if standing != ExternalIDOurs || !standing.Ours() {
		t.Fatalf("standing = %v, want ours", standing)
	}
}

// The whole point of the file: the four ways a run is not ours are four
// different pieces of work and are kept apart, where every existing answer
// collapses them into one no.
func TestTheWaysARunIsNotOursAreKeptApart(t *testing.T) {
	name, head := standingSubject(t)
	ours, err := CheckRunExternalID(TaskCheckRun{Name: name, HeadSHA: head})
	if err != nil {
		t.Fatalf("CheckRunExternalID: %v", err)
	}
	otherCommit, err := CheckRunExternalID(TaskCheckRun{
		Name: name, HeadSHA: "fedcba9876543210fedcba9876543210fedcba98",
	})
	if err != nil {
		t.Fatalf("CheckRunExternalID(other commit): %v", err)
	}
	cases := []struct {
		what       string
		identifier string
		want       ExternalIDStanding
	}{
		{"no identifier at all", "", ExternalIDAbsent},
		{"somebody else's value", "jenkins-build-4417", ExternalIDForeign},
		{"a version this build does not derive", "ra8ci-2-" + strings.Repeat("a", externalIDDigits), ExternalIDSuperseded},
		{"our shape for another commit", otherCommit, ExternalIDOtherSubject},
		{"the identifier for this subject", ours, ExternalIDOurs},
	}
	for _, test := range cases {
		t.Run(test.what, func(t *testing.T) {
			if standing := standingOf(t, standingRun(name, test.identifier), head); standing != test.want {
				t.Fatalf("standing = %v, want %v", standing, test.want)
			}
		})
	}
}

// The case the existing yes/no answer hides: the run is this plane's, derived
// for another commit, and sitting on this one. It is not a stranger's run and
// reporting it as one would send an operator to argue with nobody.
func TestOurOwnIdentifierForAnotherCommitIsNotForeign(t *testing.T) {
	name, head := standingSubject(t)
	elsewhere, err := CheckRunExternalID(TaskCheckRun{
		Name: name, HeadSHA: "77777777aaaaaaaa55555555cccccccc99999999",
	})
	if err != nil {
		t.Fatalf("CheckRunExternalID: %v", err)
	}
	run := standingRun(name, elsewhere)
	if standing := standingOf(t, run, head); standing != ExternalIDOtherSubject {
		t.Fatalf("standing = %v, want other subject", standing)
	}
	if PublishedByThisPlane(run, head) {
		t.Fatal("PublishedByThisPlane said a run derived for another commit is ours")
	}
}

// A check run's name can be edited after it is published and its identifier
// cannot, so a renamed run of ours reads as our shape for other work.
func TestARenamedRunOfOursIsOurShapeForOtherWork(t *testing.T) {
	first, second := reconcilerTaskNames(t)
	published, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("published name: %v", err)
	}
	renamed, err := CheckRunName(ModeAuthoritative, second)
	if err != nil {
		t.Fatalf("renamed name: %v", err)
	}
	identifier, err := CheckRunExternalID(TaskCheckRun{Name: published, HeadSHA: reconcilerHead})
	if err != nil {
		t.Fatalf("CheckRunExternalID: %v", err)
	}
	standing := standingOf(t, standingRun(renamed, identifier), reconcilerHead)
	if standing != ExternalIDOtherSubject {
		t.Fatalf("standing = %v, want other subject", standing)
	}
}

// A shadow run of ours answers the same way an authoritative one does: the
// identifier is derived from the name, and the name already carries the mode.
func TestAShadowRunOfOursIsOurs(t *testing.T) {
	_, second := reconcilerTaskNames(t)
	name, err := CheckRunName(ModeShadow, second)
	if err != nil {
		t.Fatalf("shadow name: %v", err)
	}
	identifier, err := CheckRunExternalID(TaskCheckRun{Name: name, HeadSHA: reconcilerHead})
	if err != nil {
		t.Fatalf("CheckRunExternalID: %v", err)
	}
	run := standingRun(name, identifier)
	run.Mode = ModeShadow
	if standing := standingOf(t, run, reconcilerHead); standing != ExternalIDOurs {
		t.Fatalf("standing = %v, want ours", standing)
	}
}

// The shape is exactly what the encoder writes. Anything else resembling it is
// somebody else's value, and reading a near miss as ours at another version
// would file a stranger's run under our own history.
func TestOnlyTheShapeThisPlaneWritesIsRecognised(t *testing.T) {
	name, head := standingSubject(t)
	digest := strings.Repeat("a", externalIDDigits)
	cases := []struct {
		what       string
		identifier string
		want       ExternalIDStanding
	}{
		{"upper case digest", "ra8ci-1-" + strings.ToUpper(digest), ExternalIDForeign},
		{"digest one short", "ra8ci-1-" + digest[:externalIDDigits-1], ExternalIDForeign},
		{"digest one long", "ra8ci-1-" + digest + "a", ExternalIDForeign},
		{"not hexadecimal", "ra8ci-1-" + strings.Repeat("z", externalIDDigits), ExternalIDForeign},
		{"no version segment", "ra8ci-" + digest, ExternalIDForeign},
		{"prefix only", externalIDPrefix, ExternalIDForeign},
		{"our prefix inside another value", "build-ra8ci-1-" + digest, ExternalIDForeign},
		{"a plausible sentence", "posted by ra8ci", ExternalIDForeign},
	}
	for _, test := range cases {
		t.Run(test.what, func(t *testing.T) {
			if standing := standingOf(t, standingRun(name, test.identifier), head); standing != test.want {
				t.Fatalf("standing = %v, want %v", standing, test.want)
			}
		})
	}
}

// A version this build does not derive is our own work from a deployment that
// computes identifiers differently, which is neither a match nor a stranger.
func TestAnotherVersionOfOurShapeIsSuperseded(t *testing.T) {
	name, head := standingSubject(t)
	digest := strings.Repeat("9", externalIDDigits)
	for _, version := range []string{"ra8ci-0", "ra8ci-2", "ra8ci-11"} {
		t.Run(version, func(t *testing.T) {
			standing := standingOf(t, standingRun(name, version+"-"+digest), head)
			if standing != ExternalIDSuperseded {
				t.Fatalf("standing = %v, want superseded", standing)
			}
		})
	}
}

// The subject is refused rather than guessed at. A run with no name or a
// commit that is not a commit is a caller's own mistake, and answering
// foreign would report it as somebody else's run.
func TestAnUnusableSubjectIsRefused(t *testing.T) {
	name, head := standingSubject(t)
	identifier, err := CheckRunExternalID(TaskCheckRun{Name: name, HeadSHA: head})
	if err != nil {
		t.Fatalf("CheckRunExternalID: %v", err)
	}
	cases := []struct {
		what string
		run  PublishedCheckRun
		head string
	}{
		{"no name", standingRun("", identifier), head},
		{"no commit", standingRun(name, identifier), ""},
		{"a short commit", standingRun(name, identifier), head[:7]},
		{"a commit that is not hexadecimal", standingRun(name, identifier), strings.Repeat("g", 40)},
		{"neither", standingRun("", identifier), ""},
	}
	for _, test := range cases {
		t.Run(test.what, func(t *testing.T) {
			if _, err := ExternalIDStandingOf(test.run, test.head); !errors.Is(err, ErrExternalIDSubjectUnusable) {
				t.Fatalf("err = %v, want ErrExternalIDSubjectUnusable", err)
			}
		})
	}
}

// GitHub renders a commit in either case and the identifier is derived from
// the lowered value, so the same run read twice answers the same way.
func TestTheCommitsCasingIsNotADifferentSubject(t *testing.T) {
	name, head := standingSubject(t)
	identifier, err := CheckRunExternalID(TaskCheckRun{Name: name, HeadSHA: head})
	if err != nil {
		t.Fatalf("CheckRunExternalID: %v", err)
	}
	standing := standingOf(t, standingRun(name, identifier), strings.ToUpper(head))
	if standing != ExternalIDOurs {
		t.Fatalf("standing = %v, want ours", standing)
	}
}

// One derivation serves both answers: the yes/no question is the long answer
// asked loosely, never a second opinion beside it.
func TestTheYesNoAnswerIsThisStanding(t *testing.T) {
	name, head := standingSubject(t)
	ours, err := CheckRunExternalID(TaskCheckRun{Name: name, HeadSHA: head})
	if err != nil {
		t.Fatalf("CheckRunExternalID: %v", err)
	}
	for _, identifier := range []string{
		"", ours, "jenkins-4417",
		"ra8ci-2-" + strings.Repeat("b", externalIDDigits),
		"ra8ci-1-" + strings.Repeat("b", externalIDDigits),
	} {
		run := standingRun(name, identifier)
		standing := standingOf(t, run, head)
		if standing.Ours() != PublishedByThisPlane(run, head) {
			t.Fatalf("%q: standing.Ours() = %v, PublishedByThisPlane = %v",
				identifier, standing.Ours(), PublishedByThisPlane(run, head))
		}
	}
}

// The standing is about the identifier and nothing else: a run of ours that
// disagrees is still ours, and a stranger's run that agrees is still not.
func TestTheStandingIsAboutTheIdentifierAlone(t *testing.T) {
	name, head := standingSubject(t)
	identifier, err := CheckRunExternalID(TaskCheckRun{Name: name, HeadSHA: head})
	if err != nil {
		t.Fatalf("CheckRunExternalID: %v", err)
	}
	failing := standingRun(name, identifier)
	failing.Conclusion = "failure"
	failing.Status = "in_progress"
	if standing := standingOf(t, failing, head); standing != ExternalIDOurs {
		t.Fatalf("a failing unfinished run of ours: standing = %v, want ours", standing)
	}
	stranger := standingRun(name, "buildkite-2231")
	stranger.Conclusion = "success"
	if standing := standingOf(t, stranger, head); standing != ExternalIDForeign {
		t.Fatalf("an agreeing stranger: standing = %v, want foreign", standing)
	}
}

// Every standing names itself, including a value outside the set, so a report
// built from the name never carries a bare integer.
func TestEveryStandingNamesItself(t *testing.T) {
	named := map[ExternalIDStanding]string{
		ExternalIDAbsent:       "absent",
		ExternalIDForeign:      "foreign",
		ExternalIDSuperseded:   "superseded",
		ExternalIDOtherSubject: "other subject",
		ExternalIDOurs:         "ours",
	}
	seen := map[string]bool{}
	for standing, want := range named {
		if standing.String() != want {
			t.Fatalf("String() = %q, want %q", standing.String(), want)
		}
		if seen[want] {
			t.Fatalf("two standings named %q", want)
		}
		seen[want] = true
	}
	if outside := ExternalIDStanding(42).String(); !strings.Contains(outside, "42") {
		t.Fatalf("String() of an unknown standing = %q, want it to carry the value", outside)
	}
	if ExternalIDStanding(42).Ours() {
		t.Fatal("a value outside the set answered ours")
	}
}
