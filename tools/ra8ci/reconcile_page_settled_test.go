// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// readsAsSettled says whether the page opened with the settled verdict.
func readsAsSettled(t *testing.T, page string) bool {
	t.Helper()
	if strings.HasPrefix(page, "settled: ") {
		return true
	}
	if strings.HasPrefix(page, "not settled: ") {
		return false
	}
	t.Fatalf("the page opens with neither verdict: %q", page)
	return false
}

// The page states the answer the survey already reached. This walks the
// shapes a publish actually takes and asserts the two never disagree: a
// third reason a publish is unsettled goes in reconcileIsSettled and gets a
// row here, and is written out in neither caller.
func TestThePageAndTheSurveyAgreeOnSettled(t *testing.T) {
	for _, shape := range []struct {
		as        string
		decisions []github.PublishDecision
	}{
		{"nothing planned", nil},
		{"all settled", []github.PublishDecision{github.PublishSettled, github.PublishSettled}},
		{"one to post", []github.PublishDecision{github.PublishNeeded, github.PublishSettled}},
		{"one in flight", []github.PublishDecision{github.PublishInFlight}},
		{"one conflicting", []github.PublishDecision{github.PublishConflicts}},
		{"one of each", []github.PublishDecision{
			github.PublishNeeded, github.PublishInFlight,
			github.PublishConflicts, github.PublishSettled,
		}},
	} {
		t.Run(shape.as, func(t *testing.T) {
			report := countedSurveyOf(shape.decisions...)

			page := &bytes.Buffer{}
			if err := RenderReconcileSurvey(page, report); err != nil {
				t.Fatalf("a well formed survey was refused: %v", err)
			}
			if readsAsSettled(t, page.String()) != reconcileIsSettled(report) {
				t.Fatalf("the page and the survey disagree over %q: %q", shape.as, page)
			}
		})
	}
}

// A survey stating the verdict its own counts refuse is the page's existing
// refusal, and it still names the counts that decided it.
func TestASurveyStatedSettledOverWorkIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishNeeded)
	report.Settled = true

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "settled is true") {
		t.Fatalf("the refusal does not state the verdict: %v", err)
	}
	if !strings.Contains(err.Error(), "1 to post") {
		t.Fatalf("the refusal does not name the counts that decided it: %v", err)
	}
}

// The other direction is refused too: a publish with nothing left is
// settled, and a survey saying otherwise is not a page to print.
func TestASurveyStatedUnsettledOverNothingIsRefused(t *testing.T) {
	report := countedSurveyOf(github.PublishSettled)
	report.Settled = false

	err := refusedCountedPage(t, report)
	if !strings.Contains(err.Error(), "settled is false") {
		t.Fatalf("the refusal does not state the verdict: %v", err)
	}
}

// A leftover run is deliberately outside the answer. A commit whose every
// planned task is accounted for is settled while an operator still has a
// run under a name nothing plans to go and look at.
func TestALeftoverRunDoesNotUnsettleThePage(t *testing.T) {
	report := listedSurveyOf(
		[]reconcileUnplannedRun{unplannedRunOf(7, "a stranger")},
		[]reconcileUnplannedStanding{unplannedGroupOf("a stranger", 7)},
		nil, nil,
	)

	if !reconcileIsSettled(report) {
		t.Fatal("a leftover run unsettled the publish")
	}
	page := &bytes.Buffer{}
	if err := RenderReconcileSurvey(page, report); err != nil {
		t.Fatalf("a well formed survey was refused: %v", err)
	}
	if !readsAsSettled(t, page.String()) {
		t.Fatalf("a leftover run unsettled the page: %q", page)
	}
	if !strings.Contains(page.String(), "no task plans, a stranger: #7") {
		t.Fatalf("the leftover run is not on the page: %q", page)
	}
}
