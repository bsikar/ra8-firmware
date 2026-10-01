// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// unplannedRunOf is one run no task plans, as the listing carries it.
func unplannedRunOf(id int64, identifier string) reconcileUnplannedRun {
	return reconcileUnplannedRun{
		ID: id, Name: "other / build", Mode: "authoritative",
		Status: "completed", Conclusion: "success", Identifier: identifier,
	}
}

// The standings are checked against this listing, and the listing itself was
// not checked: a run listed twice collapses into whichever answer came last.
func TestARunListedTwiceAsUnplannedIsRefused(t *testing.T) {
	report := countedSurveyOf()
	report.UnplannedRun = []reconcileUnplannedRun{
		unplannedRunOf(7, "foreign"),
		unplannedRunOf(8, "foreign"),
		unplannedRunOf(7, "superseded"),
	}
	report.Unplanned = len(report.UnplannedRun)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 7 is listed more than once") {
		t.Fatalf("the refusal does not name the repeated run: %v", err)
	}
}

// The two listings are meant to be disjoint: the unplanned runs are what is
// left once every planned name is kept back, so a run in both is a document
// contradicting itself about whether any task plans it.
func TestARunListedAsUnplannedAndUnderATaskIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishConflicts)
	report.Tasks[0].Published = []reconcileSurveyedRun{{
		ID: 11, Identifier: "foreign",
	}}
	report.UnplannedRun = []reconcileUnplannedRun{unplannedRunOf(11, "foreign")}
	report.Unplanned = len(report.UnplannedRun)

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "run 11 is listed as one no task plans and under ra8ci / build") {
		t.Fatalf("the refusal does not say where the run stands: %v", err)
	}
}

// Two runs under one identifier are the ordinary leftover, not a repeat: the
// page states them on one line and the standing groups them.
func TestTwoUnplannedRunsUnderOneIdentifierAreStated(t *testing.T) {
	report := countedSurveyOf()
	report.UnplannedRun = []reconcileUnplannedRun{
		unplannedRunOf(7, "foreign"),
		unplannedRunOf(8, "foreign"),
	}
	report.Unplanned = len(report.UnplannedRun)
	report.UnplannedStanding = []reconcileUnplannedStanding{{
		Identifier: "foreign", Runs: []int64{7, 8},
	}}

	page := renderedSurvey(t, report)
	if !strings.Contains(page, "no task plans, foreign: #7, #8") {
		t.Fatalf("an ordinary leftover was not stated:\n%s", page)
	}
}

// The listing is read before the counts, because a repeat lengthens the
// listing and the count check would answer for the wrong thing.
func TestTheRunsAreReadBeforeTheCounts(t *testing.T) {
	report := countedSurveyOf()
	report.UnplannedRun = []reconcileUnplannedRun{
		unplannedRunOf(7, "foreign"),
		unplannedRunOf(7, "foreign"),
	}
	report.Unplanned = 99

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "listed more than once") {
		t.Fatalf("the counts were read first: %v", err)
	}
}

// A survey the command itself made is never refused. The checks are about a
// document that came from somewhere else.
func TestASurveysOwnRunsAreNotRefused(t *testing.T) {
	page := renderedSurvey(t, standingSurveyOf(
		[]reconcileContestedStanding{contestedStandingOf("foreign", 2)},
		[]reconcileUnplannedStanding{unplannedStandingOf("superseded", 2)},
	))
	if !strings.Contains(page, "no task plans, superseded: ") {
		t.Fatalf("an ordinary survey was not stated in full:\n%s", page)
	}
}
