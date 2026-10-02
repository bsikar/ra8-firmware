// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"strings"
	"testing"
)

// planeRan is the smallest well-formed plane side, so each case below is the
// only thing wrong with the set it is handed to.
func planeRan(task, head string) []PlaneOutcome {
	return []PlaneOutcome{{Task: task, HeadSHA: head, Observed: "success"}}
}

func TestAnActionsJobThatNamesNoCommitIsRefusedBeforeItIsPaired(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"format": "lint-format", "unit-tests": "lint-tidy"})
	for _, c := range []struct {
		name string
		head string
	}{
		{"no commit at all", ""},
		{"an abbreviated commit", "0123456"},
		{"one character short", headA[:39]},
		{"one character long", headA + "0"},
		{"not hexadecimal", strings.Repeat("z", 40)},
	} {
		t.Run(c.name, func(t *testing.T) {
			collection, err := correspondence.Collect(
				planeRan("format", headA),
				[]ActionsOutcome{{Job: "lint-format", HeadSHA: c.head, Conclusion: "success"}},
			)
			if !errors.Is(err, ErrShadowObservationInvalid) {
				t.Fatalf("want ErrShadowObservationInvalid, got %v", err)
			}
			if !strings.Contains(err.Error(), "lint-format") {
				t.Fatalf("refusal does not name the job: %v", err)
			}
			if len(collection.Observations) != 0 || len(collection.NotRun) != 0 {
				t.Fatalf("collection returned beside a refusal: %+v", collection)
			}
		})
	}
}

// The Actions side is read before the plane side, so a set that spans two
// commits within Actions alone must be caught there rather than surviving to
// be blamed on the plane.
func TestActionsJobsThatJudgedTwoCommitsAreRefusedOnTheActionsSideAlone(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"format": "lint-format", "unit-tests": "lint-tidy"})
	collection, err := correspondence.Collect(
		planeRan("format", headA),
		[]ActionsOutcome{
			{Job: "lint-format", HeadSHA: headA, Conclusion: "success"},
			{Job: "lint-tidy", HeadSHA: headB, Conclusion: "success"},
		},
	)
	if !errors.Is(err, ErrShadowHeadMismatch) {
		t.Fatalf("want ErrShadowHeadMismatch, got %v", err)
	}
	if len(collection.Observations) != 0 {
		t.Fatalf("collection returned beside a refusal: %+v", collection)
	}

	agreeing, err := correspondence.Collect(
		planeRan("format", headA),
		[]ActionsOutcome{
			{Job: "lint-format", HeadSHA: headA, Conclusion: "success"},
			{Job: "lint-tidy", HeadSHA: headA, Conclusion: "failure"},
		},
	)
	if err != nil {
		t.Fatalf("two jobs on one commit refused: %v", err)
	}
	if len(agreeing.Observations) != 1 || agreeing.Observations[0].ActionsConclusion != "success" {
		t.Fatalf("observations=%+v", agreeing.Observations)
	}
}

func TestAPlaneOutcomeForAnUnusableTaskNameIsRefused(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"format": "lint-format", "unit-tests": "lint-tidy"})
	for _, c := range []struct {
		name string
		task string
	}{
		{"no task at all", ""},
		{"a shouted task", "FORMAT"},
		{"an underscore", "format_check"},
		{"a path", "checks/format"},
		{"a space", "format check"},
		{"past the length bound", strings.Repeat("a", 101)},
	} {
		t.Run(c.name, func(t *testing.T) {
			collection, err := correspondence.Collect(planeRan(c.task, headA), nil)
			if !errors.Is(err, ErrShadowObservationInvalid) {
				t.Fatalf("want ErrShadowObservationInvalid, got %v", err)
			}
			if errors.Is(err, ErrShadowTaskNotCorrespondent) {
				t.Fatalf("an unusable name was reported as merely uncovered: %v", err)
			}
			if len(collection.Observations) != 0 {
				t.Fatalf("collection returned beside a refusal: %+v", collection)
			}
		})
	}
	if _, err := correspondence.Collect(planeRan(strings.Repeat("a", 100), headA), nil); !errors.Is(err, ErrShadowTaskNotCorrespondent) {
		t.Fatalf("a name exactly at the bound was refused as unusable: %v", err)
	}
}

// A refusal anywhere in the set discards the whole set. Nothing paired before
// the bad observation may reach a caller, or a report could rest on a prefix
// of the evidence it claims to cover.
func TestARefusalLateInTheSetDiscardsWhatWasAlreadyPaired(t *testing.T) {
	correspondence := testCorrespondence(t, map[string]string{"format": "lint-format", "unit-tests": "lint-tidy"})
	collection, err := correspondence.Collect(
		[]PlaneOutcome{
			{Task: "format", HeadSHA: headA, Observed: "success"},
			{Task: "TIDY", HeadSHA: headA, Observed: "success"},
		},
		[]ActionsOutcome{{Job: "lint-format", HeadSHA: headA, Conclusion: "success"}},
	)
	if !errors.Is(err, ErrShadowObservationInvalid) {
		t.Fatalf("want ErrShadowObservationInvalid, got %v", err)
	}
	if len(collection.Observations) != 0 || len(collection.NotRun) != 0 || len(collection.NotRunJudged) != 0 {
		t.Fatalf("partial collection handed back: %+v", collection)
	}
}
