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

// renderedCandidateSurvey is the page for one report, or a fatal test failure. Every
// test below reads the page as lines, because the page's whole argument is
// what is on which line and in what order.
func renderedCandidateSurvey(t *testing.T, report pullRequestSurveyReport) []string {
	t.Helper()
	page := &bytes.Buffer{}
	if err := RenderPullRequestSurvey(page, report); err != nil {
		t.Fatalf("render the survey: %v", err)
	}
	return strings.Split(strings.TrimSuffix(page.String(), "\n"), "\n")
}

// The page states the decision first. A reader who has to add the counts up
// to find out whether the set can be gathered is doing the work the page
// exists to do.
func TestTheCandidateSurveyPageStatesTheVerdictFirst(t *testing.T) {
	ready := renderedCandidateSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "failure"),
		selectableFor(1590, surveyHeadB, 772, 1, "success"),
	}))
	if !strings.HasPrefix(ready[0], "ready: ") || !strings.Contains(ready[0], "Checks") {
		t.Fatalf("the verdict line for a clean survey is %q", ready[0])
	}
	if ready[1] != "2 candidates considered, 2 selectable, 0 unselectable" {
		t.Fatalf("the counts line is %q", ready[1])
	}

	unready := renderedCandidateSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "failure"),
		unselectableFor(1590, surveyHeadB, github.CommitWorkflowRuns{
			HeadSHA: surveyHeadB,
			Runs: []github.CommitWorkflowRun{{
				ID: 772, Workflow: "Checks", Attempt: 1, Event: "pull_request",
				Status: "in_progress",
			}},
		}),
	}))
	if !strings.HasPrefix(unready[0], "not ready: ") {
		t.Fatalf("the verdict line for a survey with a refusal is %q", unready[0])
	}
}

// One candidate is "1 candidate", not "1 candidates". A line that reads as a
// template is one a reader stops believing was written about their set.
func TestTheCandidateSurveyPageCountsOneCandidateInWords(t *testing.T) {
	page := renderedCandidateSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "success"),
	}))
	if page[1] != "1 candidate considered, 1 selectable, 0 unselectable" {
		t.Fatalf("the counts line is %q", page[1])
	}
}

// A commit two candidates share leads the sections. Both pull requests are
// selectable and both are counted so, and the gather refuses the pair anyway:
// it is the only one of the three findings that is about the set rather than
// about a candidate, and it is the one that stops the whole gather.
func TestTheCandidateSurveyPageLeadsWithASharedHead(t *testing.T) {
	page := renderedCandidateSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "failure"),
		selectableFor(1590, surveyHeadA, 771, 1, "failure"),
		unselectableFor(1591, surveyHeadB, github.CommitWorkflowRuns{HeadSHA: surveyHeadB}),
	}))
	if page[0] != "not ready: this set cannot be gathered for Checks evidence as it stands" {
		t.Fatalf("a shared head left the verdict at %q", page[0])
	}
	if page[2] != "shared head "+surveyHeadA+": #1589, #1590" {
		t.Fatalf("the shared head line is %q", page[2])
	}
	if !strings.HasPrefix(page[3], "no evidence run: #1591 ") {
		t.Fatalf("the refusal does not follow the shared head: %q", page[3])
	}
	if !strings.HasPrefix(page[4], "selected: #1589 ") {
		t.Fatalf("the selections do not come last: %q", page[4])
	}
}

// A shared head is reported even when every candidate on it is otherwise
// fine, and the page says so rather than reading as ready.
func TestASharedHeadAloneIsNotAReadySet(t *testing.T) {
	page := renderedCandidateSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "success"),
		selectableFor(1590, surveyHeadA, 771, 1, "success"),
	}))
	if page[0] != "not ready: this set cannot be gathered for Checks evidence as it stands" {
		t.Fatalf("two candidates on one commit read as %q", page[0])
	}
	if page[1] != "2 candidates considered, 2 selectable, 0 unselectable" {
		t.Fatalf("the shared head moved a count: %q", page[1])
	}
}

// A candidate no run can be selected on carries the refusal in the words the
// selection returned. Naming the pull request without saying why sends the
// reader back to the document this page is meant to replace.
func TestTheCandidateSurveyPageCarriesTheRefusalInWords(t *testing.T) {
	listed := github.CommitWorkflowRuns{
		HeadSHA: surveyHeadB,
		Runs: []github.CommitWorkflowRun{{
			ID: 772, Workflow: "Checks", Attempt: 1, Event: "pull_request",
			Status: "in_progress",
		}},
	}
	_, refusal := github.SelectEvidenceRun(listed, "Checks")
	if refusal == nil {
		t.Fatal("an incomplete run was selected")
	}
	page := renderedCandidateSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		unselectableFor(1590, surveyHeadB, listed),
	}))
	want := "no evidence run: #1590 at " + surveyHeadB + " (" + refusal.Error() + ")"
	if page[2] != want {
		t.Fatalf("the refusal line is %q, want %q", page[2], want)
	}
}

// The selection names the run the gather would use, with its attempt and its
// outcome. An operator reads this page to hand the set on, and a selection
// without its run number is one they have to look up again.
func TestTheCandidateSurveyPageNamesTheSelectedRun(t *testing.T) {
	page := renderedCandidateSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 3, "failure"),
	}))
	want := "selected: #1589 at " + surveyHeadA + ", run 771 attempt 3 (pull_request: failure)"
	if page[2] != want {
		t.Fatalf("the selection line is %q, want %q", page[2], want)
	}
}

// An ordinary survey carries no shared-head line and no refusal line. "0
// shared heads" on every clean page teaches a reader to skip the line that
// matters.
func TestAnOrdinaryCandidateSurveyPageOmitsTheEmptySections(t *testing.T) {
	page := renderedCandidateSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1589, surveyHeadA, 771, 1, "success"),
		selectableFor(1590, surveyHeadB, 772, 1, "success"),
	}))
	if len(page) != 4 {
		t.Fatalf("a clean survey rendered %d lines: %#v", len(page), page)
	}
	for _, line := range page {
		if strings.HasPrefix(line, "shared head ") || strings.HasPrefix(line, "no evidence run: ") {
			t.Fatalf("a clean survey carries %q", line)
		}
	}
}

// A survey with nothing in it is still a survey, and the page says so rather
// than refusing: asking about no pull requests is an empty answer, not a
// contradiction.
func TestAnEmptyCandidateSurveyRendersAsReady(t *testing.T) {
	page := renderedCandidateSurvey(t, pullRequestSurveyFrom("Checks", nil))
	if len(page) != 2 {
		t.Fatalf("an empty survey rendered %d lines: %#v", len(page), page)
	}
	if page[1] != "0 candidates considered, 0 selectable, 0 unselectable" {
		t.Fatalf("the counts line is %q", page[1])
	}
}

// A survey whose counts disagree with its candidates is refused by name. The
// page is read to decide which pull requests go into the evidence, so a value
// that contradicts itself is worse than no page.
func TestACandidateSurveyThatDisagreesWithItselfIsRefused(t *testing.T) {
	for _, one := range []struct {
		name   string
		report pullRequestSurveyReport
	}{{
		name: "considered counts a candidate that is not answered for",
		report: pullRequestSurveyReport{
			Workflow: "Checks", Considered: 2, Selectable: 1,
			SharedHeads: []surveySharedHead{},
			PullRequests: []surveyedPullRequest{{
				Number: 1589, HeadSHA: surveyHeadA, Selectable: true, RunID: 771,
			}},
		},
	}, {
		name: "selectable counts a candidate the listing calls refused",
		report: pullRequestSurveyReport{
			Workflow: "Checks", Considered: 1, Selectable: 1,
			SharedHeads: []surveySharedHead{},
			PullRequests: []surveyedPullRequest{{
				Number: 1589, HeadSHA: surveyHeadA, Reason: "no run",
			}},
		},
	}, {
		name: "unselectable counts a candidate the listing calls selected",
		report: pullRequestSurveyReport{
			Workflow: "Checks", Considered: 1, Unselectable: 1,
			SharedHeads: []surveySharedHead{},
			PullRequests: []surveyedPullRequest{{
				Number: 1589, HeadSHA: surveyHeadA, Selectable: true, RunID: 771,
			}},
		},
	}} {
		t.Run(one.name, func(t *testing.T) {
			page := &bytes.Buffer{}
			err := RenderPullRequestSurvey(page, one.report)
			if !errors.Is(err, ErrPullRequestSurveyPageInvalid) {
				t.Fatalf("the survey was answered with %v", err)
			}
			if page.Len() != 0 {
				t.Fatalf("a refusal wrote %q", page.String())
			}
		})
	}
}

// A survey past the bound is refused rather than cut. A page that quietly
// stops halfway is the one way this could report a set as cleaner than it is.
func TestACandidateSurveyTooLargeToRenderIsRefused(t *testing.T) {
	report := pullRequestSurveyReport{
		Workflow: "Checks", SharedHeads: []surveySharedHead{},
	}
	for number := 0; number <= maxRenderedSurveyCandidates; number++ {
		report.PullRequests = append(report.PullRequests, surveyedPullRequest{
			Number: 1589 + number, HeadSHA: surveyHeadA, Selectable: true, RunID: 771,
		})
	}
	report.Considered = len(report.PullRequests)
	report.Selectable = len(report.PullRequests)

	page := &bytes.Buffer{}
	err := RenderPullRequestSurvey(page, report)
	if !errors.Is(err, ErrPullRequestSurveyPageTooLarge) {
		t.Fatalf("an oversized survey was answered with %v", err)
	}
	if page.Len() != 0 {
		t.Fatalf("a refusal wrote %q", page.String())
	}
}

// The page keeps the survey's own order for the candidates it lists, within
// each section. Sorting them here would make the page and the document
// disagree about which pull request came first.
func TestTheCandidateSurveyPageKeepsTheSurveysOwnOrder(t *testing.T) {
	page := renderedCandidateSurvey(t, pullRequestSurveyFrom("Checks", []surveyedHead{
		selectableFor(1591, surveyHeadB, 773, 1, "success"),
		selectableFor(1589, surveyHeadA, 771, 1, "success"),
	}))
	if !strings.HasPrefix(page[2], "selected: #1591 ") || !strings.HasPrefix(page[3], "selected: #1589 ") {
		t.Fatalf("the page reordered the selections: %#v", page)
	}
}
