// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"strings"
	"testing"
)

// evidenceOverOneJudgedCommit is an evidence document over the single
// comparison that produces every part of the page: a covered task that ran
// and agreed, and a second covered task that did not run here and was judged
// by Actions anyway. That second task is in no commit's evidence, so it lands
// in the never-exercised set as well as its own commit's tail.
func evidenceOverOneJudgedCommit(t *testing.T) string {
	t.Helper()
	return `{"threshold":1,"commits":[` + judgedWithoutARunInput(t) + `]}`
}

// A page that cannot be written is an error at each of the four places the
// evidence page writes, the githubShadowCompare rule one level up: the page
// is written before the verdict is returned, so a caller reading only the
// exit status must never get a clean one from a page nobody could read.
//
// The two tails matter as much as the accumulation above them. A task no
// commit exercised, and a task Actions judged without a run on this side, are
// facts a reader cannot recover from the accumulated evidence, because
// neither is in any task's evidence at all. Losing them silently would leave
// an operator reading a clean page over a collection with holes in it.
func TestTheEvidencePageReportsAPageItCouldNotFinishWriting(t *testing.T) {
	// The verdict is not what this is about: whether one agreeing commit
	// settles the evidence at a threshold of one is the accumulation's
	// business. What matters here is that the whole page was written, so
	// the offsets below cut it in the right places.
	var whole bytes.Buffer
	if err := githubEvidencePage(strings.NewReader(evidenceOverOneJudgedCommit(t)), &whole); err != nil &&
		!strings.Contains(err.Error(), "not settled") {
		t.Fatalf("the page was refused for something other than its verdict: %v", err)
	}
	page := whole.String()

	never := strings.Index(page, "\nnever exercised on any commit: ")
	if never < 0 {
		t.Fatalf("the page names no never-exercised set to cut at:\n%s", page)
	}
	notExercised := strings.Index(page, "\nnot exercised on ")
	if notExercised < 0 {
		t.Fatalf("the page carries no unexercised tail to cut at:\n%s", page)
	}
	judged := strings.Index(page, "judged by Actions without a run on ")
	if judged < 0 {
		t.Fatalf("the page carries no judged line to cut at:\n%s", page)
	}

	for name, budget := range map[string]int{
		"nothing written":           0,
		"accumulation only":         never,
		"through the never set":     notExercised,
		"through the commit's tail": judged,
	} {
		t.Run(name, func(t *testing.T) {
			out := &halting{budget: budget}
			err := githubEvidencePage(strings.NewReader(evidenceOverOneJudgedCommit(t)), out)
			if err == nil {
				t.Fatal("a page that could not be written was reported clean")
			}
			if !strings.Contains(err.Error(), "render shadow evidence") {
				t.Fatalf("err = %v; want the render named", err)
			}
			// The refusal must not read as a verdict about the
			// evidence. An operator acts on "not settled" by
			// collecting more pull requests, which would be the
			// wrong move entirely when the real fault is the pipe.
			if strings.Contains(err.Error(), "not settled") {
				t.Fatalf("a write failure was reported as unsettled evidence: %v", err)
			}
			if out.taken.Len() > budget {
				t.Fatalf("wrote %d bytes past the budget of %d", out.taken.Len()-budget, budget)
			}
		})
	}
}

// The never-exercised line is omitted when there is nothing to name, so a
// clean collection does not teach its reader to skip a line that only ever
// says "none". The whole page still has to be written for the omission to
// mean anything, so this asserts the page arrived without it.
func TestTheEvidencePageOmitsAnEmptyNeverExercisedLine(t *testing.T) {
	document := `{"threshold":1,"commits":[` + agreeingCommit(t) + `]}`

	var out bytes.Buffer
	if err := githubEvidencePage(strings.NewReader(document), &out); err != nil {
		t.Fatalf("a settled collection was refused: %v", err)
	}
	page := out.String()
	if strings.Contains(page, "never exercised") {
		t.Fatalf("a collection with nothing left out named a never-exercised set:\n%s", page)
	}
	if strings.Contains(page, "not exercised on ") {
		t.Fatalf("a commit that exercised its covered task carried a tail:\n%s", page)
	}
	if page == "" {
		t.Fatal("a settled collection wrote no page")
	}
}
