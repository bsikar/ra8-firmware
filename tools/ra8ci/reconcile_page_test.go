// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// surveyOf builds a survey the way the command builds one, from real plans
// and a real listing, so the page is tested against what it will actually be
// handed rather than a hand-assembled value.
func surveyOf(t *testing.T, planned []plannedCheckRun, published github.PublishedCheckRuns) reconcileReport {
	t.Helper()
	report, err := surveyCheckRunPlan(planned, published)
	if err != nil {
		t.Fatalf("survey: %v", err)
	}
	return report
}

func renderedSurvey(t *testing.T, report reconcileReport) string {
	t.Helper()
	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("render: %v", err)
	}
	return page.String()
}

// The verdict is the first line and it states the decision, not the counts.
// A reader who reads one line of this page has to come away with the answer.
func TestTheSurveyPageStatesTheVerdictFirst(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	settled := publishedAs(plan, 101, "completed", plan.Run.Conclusion, plan.Run.Title)

	page := renderedSurvey(t, surveyOf(t, []plannedCheckRun{plan}, listing(settled)))
	first := strings.SplitN(page, "\n", 2)[0]
	if !strings.HasPrefix(first, "settled:") || !strings.Contains(first, plan.Run.HeadSHA) {
		t.Fatalf("first line %q does not state the verdict for the commit", first)
	}
}

// A publish with work left says so in the same place, and never by leaving
// the reader to add the counts up.
func TestTheSurveyPageSaysWhenAPublishIsNotSettled(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")

	page := renderedSurvey(t, surveyOf(t, []plannedCheckRun{plan}, listing()))
	first := strings.SplitN(page, "\n", 2)[0]
	if !strings.HasPrefix(first, "not settled:") {
		t.Fatalf("first line %q does not state the verdict", first)
	}
	if !strings.Contains(page, "1 task, 1 to post, 0 in flight, 0 conflicting") {
		t.Fatalf("page %q does not carry the counts", page)
	}
}

// The sections are in the order the work is in: the argument a person has to
// settle today, then a run under a name we plan, then a leftover that can
// wait. Alphabetical would put the leftover first.
func TestTheSurveyPageIsInDecisionOrder(t *testing.T) {
	first, second := twoCatalogTasks(t)
	contested := plannedRun(t, github.ModeAuthoritative, first, "failed")
	stranger := publishedAs(contested, 102, "completed", contested.Run.Conclusion, contested.Run.Title)
	stranger.ExternalID = ""
	leftover := plannedRun(t, github.ModeAuthoritative, second, "failed")
	retired := publishedAs(leftover, 103, "completed", leftover.Run.Conclusion, leftover.Run.Title)
	retired.Name = leftover.Run.Name + " (retired)"

	report := surveyOf(t, []plannedCheckRun{contested}, listing(stranger, retired))
	page := renderedSurvey(t, report)
	conflicting := strings.Index(page, "conflicting: ")
	planned := strings.Index(page, "under a name we plan, ")
	unplanned := strings.Index(page, "no task plans, ")
	if conflicting < 0 || planned < 0 || unplanned < 0 {
		t.Fatalf("page %q is missing a section", page)
	}
	if !(conflicting < planned && planned < unplanned) {
		t.Fatalf("page %q is not in decision order", page)
	}
}

// A clean survey says nothing about conflicts, contested names or leftovers.
// A zero on every line teaches a reader to skip the lines that matter.
func TestACleanSurveyPageCarriesNoEmptySections(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	settled := publishedAs(plan, 104, "completed", plan.Run.Conclusion, plan.Run.Title)

	page := renderedSurvey(t, surveyOf(t, []plannedCheckRun{plan}, listing(settled)))
	for _, unwanted := range []string{"conflicting: ", "under a name we plan", "no task plans"} {
		if strings.Contains(page, unwanted) {
			t.Fatalf("page %q carries an empty section %q", page, unwanted)
		}
	}
}

// A contested run is named with the name it sits under, because the name is
// what an operator has to do something about.
func TestTheSurveyPageNamesAContestedRunWithItsName(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	stranger := publishedAs(plan, 105, "completed", plan.Run.Conclusion, plan.Run.Title)
	stranger.ExternalID = ""

	page := renderedSurvey(t, surveyOf(t, []plannedCheckRun{plan}, listing(stranger)))
	if !strings.Contains(page, "#105 under "+plan.Run.Name) {
		t.Fatalf("page %q does not name the run under its own name", page)
	}
	if !strings.Contains(page, github.ExternalIDAbsent.String()) {
		t.Fatalf("page %q does not say what the identifier says", page)
	}
}

// A survey whose counts disagree with its tasks is refused by name rather
// than printed. The page is read to decide whether a publish needs a person.
func TestASurveyThatContradictsItselfIsRefused(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	report := surveyOf(t, []plannedCheckRun{plan}, listing())
	report.Conflict = 2

	page := &bytes.Buffer{}
	err := RenderReconcileSurvey(page, report)
	if !errors.Is(err, ErrReconcilePageInvalid) {
		t.Fatalf("err = %v, want a refusal", err)
	}
	if page.Len() != 0 {
		t.Fatalf("page %q was written above the refusal", page)
	}
}

// Past the bounds the answer is refused, never cut: a page that stops
// halfway reports a commit as clearer than it is.
func TestAnOversizeSurveyIsRefusedRatherThanCut(t *testing.T) {
	report := reconcileReport{Commit: strings.Repeat("a", 40), Mode: "authoritative"}
	report.Tasks = make([]reconcileSurvey, maxRenderedSurveyTasks+1)

	page := &bytes.Buffer{}
	err := RenderReconcileSurvey(page, report)
	if !errors.Is(err, ErrReconcilePageTooLarge) {
		t.Fatalf("err = %v, want a refusal", err)
	}
	if page.Len() != 0 {
		t.Fatalf("page %q was written above the refusal", page)
	}
}

// One task is "1 task". A line that reads as a template is one a reader
// stops believing was written about their commit.
func TestTheSurveyPageCountsInWords(t *testing.T) {
	if surveyPlural(1, "task", "tasks") != "task" || surveyPlural(0, "task", "tasks") != "tasks" {
		t.Fatal("the count is not written as a person writes it")
	}
}
