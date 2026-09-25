// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"encoding/json"
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

// documentOf writes a survey the way the surveying command writes it, so the
// page command is tested against the bytes it will actually be handed rather
// than a value passed in memory.
func documentOf(t *testing.T, report reconcileReport) string {
	t.Helper()
	document := &bytes.Buffer{}
	encoder := json.NewEncoder(document)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(report); err != nil {
		t.Fatalf("encode survey: %v", err)
	}
	return document.String()
}

// The command's whole job: a survey document in, the page out. It is the same
// page the renderer writes, because a second rendering path is a second page
// to keep in step.
func TestTheReconcilePageCommandWritesThePage(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	report := surveyOf(t, []plannedCheckRun{plan}, listing())

	page := &bytes.Buffer{}
	if err := githubReconcilePage(strings.NewReader(documentOf(t, report)), page); err != nil {
		t.Fatalf("reconcile page: %v", err)
	}
	if page.String() != renderedSurvey(t, report) {
		t.Fatalf("page %q is not the survey's own page", page)
	}
}

// A conflict is the answer, so it reaches the exit status here exactly as it
// does from the surveying command, and the page is written first: a verdict
// carried only by an exit status is one nobody can read.
func TestTheReconcilePageCommandCarriesTheConflictVerdict(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	stranger := publishedAs(plan, 201, "completed", plan.Run.Conclusion, plan.Run.Title)
	stranger.ExternalID = ""
	report := surveyOf(t, []plannedCheckRun{plan}, listing(stranger))

	page := &bytes.Buffer{}
	err := githubReconcilePage(strings.NewReader(documentOf(t, report)), page)
	if err == nil {
		t.Fatal("a conflicting survey came back without a verdict")
	}
	if !strings.Contains(err.Error(), plan.Task) || !strings.Contains(err.Error(), report.Commit) {
		t.Fatalf("verdict %q does not name the task on its commit", err)
	}
	if !strings.Contains(page.String(), "conflicting: "+plan.Task) {
		t.Fatalf("page %q was not written above the verdict", page)
	}
}

// The verdict names every conflicting task, and it is the survey's own
// decision that picks them rather than a second reading of the runs.
func TestTheConflictVerdictNamesEveryConflictingTask(t *testing.T) {
	first, second := twoCatalogTasks(t)
	one := plannedRun(t, github.ModeAuthoritative, first, "failed")
	other := plannedRun(t, github.ModeAuthoritative, second, "failed")
	strangerOne := publishedAs(one, 202, "completed", one.Run.Conclusion, one.Run.Title)
	strangerOne.ExternalID = ""
	strangerOther := publishedAs(other, 203, "completed", other.Run.Conclusion, other.Run.Title)
	strangerOther.ExternalID = ""

	report := surveyOf(t, []plannedCheckRun{one, other}, listing(strangerOne, strangerOther))
	err := surveyConflictVerdict(report)
	if err == nil {
		t.Fatal("two conflicting tasks came back without a verdict")
	}
	for _, task := range []string{one.Task, other.Task} {
		if !strings.Contains(err.Error(), task) {
			t.Fatalf("verdict %q does not name %s", err, task)
		}
	}
}

// A settled survey returns nothing at all: the page is the whole answer.
func TestASettledSurveyCarriesNoVerdict(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	settled := publishedAs(plan, 204, "completed", plan.Run.Conclusion, plan.Run.Title)
	report := surveyOf(t, []plannedCheckRun{plan}, listing(settled))

	if err := surveyConflictVerdict(report); err != nil {
		t.Fatalf("settled survey carried a verdict: %v", err)
	}
}

// A field this build cannot state is refused rather than ignored. A newer
// build's survey rendered by an older page would read as a commit with
// nothing on it but the parts this one happens to know.
func TestAFieldThePageCannotStateIsRefused(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	document := documentOf(t, surveyOf(t, []plannedCheckRun{plan}, listing()))
	document = strings.Replace(document, "{\n", "{\n  \"disputed_standing\": [],\n", 1)

	page := &bytes.Buffer{}
	if err := githubReconcilePage(strings.NewReader(document), page); err == nil {
		t.Fatal("a document with an unknown field was rendered")
	}
	if page.Len() != 0 {
		t.Fatalf("page %q was written above the refusal", page)
	}
}

// Neither an empty document nor a commit with no tasks is a survey, and a
// "settled" line over nothing at all is the one reading this must never
// produce.
func TestADocumentThatIsNotASurveyIsRefused(t *testing.T) {
	for _, document := range []string{
		"{}",
		`{"commit":"","tasks":[]}`,
		`{"commit":"0d9ab2e5f01c4b4b8a1b6d7f6b2b9c0f1a2b3c4d","tasks":[]}`,
	} {
		page := &bytes.Buffer{}
		if err := githubReconcilePage(strings.NewReader(document), page); err == nil {
			t.Fatalf("document %q was rendered as a survey", document)
		}
		if page.Len() != 0 {
			t.Fatalf("page %q was written above the refusal", page)
		}
	}
}

// Two surveys in one stream are two answers about two commits, and rendering
// the first silently would state one of them as the whole reading.
func TestASecondSurveyInTheStreamIsRefused(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	document := documentOf(t, surveyOf(t, []plannedCheckRun{plan}, listing()))

	page := &bytes.Buffer{}
	err := githubReconcilePage(strings.NewReader(document+document), page)
	if err == nil || !strings.Contains(err.Error(), "trailing content") {
		t.Fatalf("err = %v, want a refusal of the trailing survey", err)
	}
}
