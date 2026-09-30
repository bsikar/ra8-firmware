// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
)

// A page that cannot be written is an error, and it must not be reported as
// the survey's verdict.
//
// The page command writes before it returns the conflict, so the two answers
// are easy to confuse from the outside: both leave a non-zero exit status
// over an incomplete page. They call for opposite moves. A conflict means a
// check run exists on the commit that this publish cannot account for, which
// somebody has to go and look at on GitHub. A write failure means the pipe
// gave out and the survey itself is still unread.
func TestTheReconcilePageCommandReportsAPageItCouldNotWrite(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	stranger := publishedAs(plan, 301, "completed", plan.Run.Conclusion, plan.Run.Title)
	stranger.ExternalID = ""
	report := surveyOf(t, []plannedCheckRun{plan}, listing(stranger))
	document := documentOf(t, report)

	// The verdict line is the first thing written, so cutting at nothing
	// and cutting partway through it are the two ways a reader can go
	// away: before the page starts, and once it has begun.
	whole := renderedSurvey(t, report)
	for name, budget := range map[string]int{
		"nothing written": 0,
		"mid verdict":     len(strings.SplitN(whole, "\n", 2)[0]) / 2,
	} {
		t.Run(name, func(t *testing.T) {
			out := &halting{budget: budget}
			err := githubReconcilePage(strings.NewReader(document), out)
			if err == nil {
				t.Fatal("a page that could not be written was reported clean")
			}
			if !strings.Contains(err.Error(), "render reconcile survey") {
				t.Fatalf("err = %v; want the render named", err)
			}
			if strings.Contains(err.Error(), "cannot account for") {
				t.Fatalf("a write failure was reported as a conflict: %v", err)
			}
			if out.taken.Len() > budget {
				t.Fatalf("wrote %d bytes past the budget of %d", out.taken.Len()-budget, budget)
			}
		})
	}
}

// A settled survey whose page cannot be written is still an error, so a
// caller reading only the exit status cannot take a clean one from a page
// nobody received. This is the case the conflict test cannot cover: with no
// conflict to return, the write failure is the only thing left to report.
func TestASettledSurveyPageThatCouldNotBeWrittenIsStillRefused(t *testing.T) {
	task := firstCatalogTask(t)
	plan := plannedRun(t, github.ModeAuthoritative, task, "failed")
	settled := publishedAs(plan, 302, "completed", plan.Run.Conclusion, plan.Run.Title)
	report := surveyOf(t, []plannedCheckRun{plan}, listing(settled))
	if report.Conflict != 0 {
		t.Fatalf("the fixture is not settled: %d conflicting", report.Conflict)
	}

	out := &halting{budget: 0}
	err := githubReconcilePage(strings.NewReader(documentOf(t, report)), out)
	if err == nil {
		t.Fatal("a settled survey nobody could read was reported clean")
	}
	if !strings.Contains(err.Error(), "render reconcile survey") {
		t.Fatalf("err = %v; want the render named", err)
	}
	if out.taken.Len() != 0 {
		t.Fatalf("wrote %q against a budget of nothing", out.taken.String())
	}
}
