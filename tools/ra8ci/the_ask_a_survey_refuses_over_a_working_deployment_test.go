// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"strings"
	"testing"
)

// The survey's ask refusals were reached only over a half-configured
// deployment, where the command refuses at the missing repository long before
// the ask is read. Those tests passed because they asked only for some error,
// so the refusal they were pinning was never the one they named.
//
// publishCheckRunEnv configures a complete publishing environment whose App
// key file does not exist, which is what makes the ordering readable: over it,
// a refusal naming the ask is the ask's own, and reaching the absent key means
// the ask was accepted.

// askRefusal surveys one document over a whole configuration and returns the
// refusal, failing if the survey answered or wrote anything.
func askRefusal(t *testing.T, document string) string {
	t.Helper()
	publishCheckRunEnv(t)
	wrote, err := surveyed(t, document)
	if err == nil {
		t.Fatalf("an unsurveyable ask answered: %q", wrote)
	}
	if wrote != "" {
		t.Fatalf("a refused survey wrote %q", wrote)
	}
	return err.Error()
}

// Each way an ask cannot be surveyed is refused in its own words, over a
// deployment that could have surveyed a good one. The App key is absent, so a
// refusal that names it would mean the ask had been accepted.
func TestEachUnsurveyableAskIsRefusedInItsOwnWords(t *testing.T) {
	for label, document := range map[string]struct{ ask, want string }{
		"no workflow":         {`{"pull_requests":[1589]}`, "no workflow named"},
		"a blank workflow":    {`{"workflow":"   ","pull_requests":[1589]}`, "no workflow named"},
		"no candidates":       {`{"workflow":"Checks","pull_requests":[]}`, "no pull requests to survey"},
		"a zero number":       {`{"workflow":"Checks","pull_requests":[0]}`, "a pull request with no number"},
		"a number below zero": {`{"workflow":"Checks","pull_requests":[-1589]}`, "a pull request with no number"},
		"one named twice":     {`{"workflow":"Checks","pull_requests":[1589,1590,1589]}`, "pull request 1589 named twice"},
	} {
		t.Run(label, func(t *testing.T) {
			refusal := askRefusal(t, document.ask)
			if !strings.Contains(refusal, document.want) {
				t.Fatalf("refusal=%q; want %q", refusal, document.want)
			}
			if !strings.Contains(refusal, "read the pull requests to survey") {
				t.Fatalf("refusal=%q; want the ask named as the ask", refusal)
			}
			if strings.Contains(refusal, "private key") {
				t.Fatalf("refusal=%q; the App key was opened over an unsurveyable ask", refusal)
			}
		})
	}
}

// A workflow stated in surrounding space is the workflow, so an ask is not
// refused for how it was typed. The absent key is what says it was accepted.
func TestAWorkflowInSurroundingSpaceIsStillTheWorkflow(t *testing.T) {
	publishCheckRunEnv(t)
	wrote, err := surveyed(t, `{"workflow":"  Checks  ","pull_requests":[1589]}`)
	if err == nil {
		t.Fatalf("a survey answered with no key on disk: %q", wrote)
	}
	if !strings.Contains(err.Error(), "GitHub App private key") {
		t.Fatalf("refusal=%v; a padded workflow was not accepted", err)
	}
}

// The document shape is judged before the ask is, so a document that is not a
// survey request at all is reported as unreadable rather than as an ask with
// nothing in it.
func TestADocumentThatIsNotAnAskIsRefusedAsUnreadable(t *testing.T) {
	for label, document := range map[string]struct{ ask, want string }{
		"an unknown field":    {`{"workflow":"Checks","pull_requests":[1589],"threshold":2}`, "unknown field"},
		"a second document":   {`{"workflow":"Checks","pull_requests":[1589]}{"workflow":"Checks","pull_requests":[1590]}`, "trailing content after the document"},
		"a list":              {`[]`, "read the pull requests to survey"},
		"a number for a name": {`{"workflow":7,"pull_requests":[1589]}`, "read the pull requests to survey"},
		"half a document":     {`{"workflow":"Checks",`, "read the pull requests to survey"},
	} {
		t.Run(label, func(t *testing.T) {
			refusal := askRefusal(t, document.ask)
			if !strings.Contains(refusal, document.want) {
				t.Fatalf("refusal=%q; want %q", refusal, document.want)
			}
		})
	}
}

// The page writes before it judges, so a reader always has the finding in
// front of them. When the writing itself fails there is no finding to carry,
// and the refusal says so rather than handing back the survey's verdict as
// though the page had been written.
func TestAPageThatCouldNotBeWrittenSaysSo(t *testing.T) {
	survey := `{"workflow":"Checks","considered":1,"selectable":1,"unselectable":0,` +
		`"shared_heads":[],"pull_requests":[{"number":1589,"head_sha":"` + shadowCompareHead +
		`","base_ref":"main","state":"open","selectable":true,"run_id":7,"attempt":1,` +
		`"event":"pull_request","conclusion":"success"}]}`

	// The same survey over a writer that accepts it is ready, so the
	// refusal below is the writing rather than the verdict.
	var accepted strings.Builder
	if err := githubPullRequestSurveyPage(strings.NewReader(survey), &accepted); err != nil {
		t.Fatalf("a ready survey was not rendered: %v", err)
	}
	if accepted.String() == "" {
		t.Fatal("a ready survey rendered nothing")
	}

	err := githubPullRequestSurveyPage(strings.NewReader(survey), &halting{})
	if err == nil {
		t.Fatal("a page that could not be written answered ready")
	}
	if !strings.Contains(err.Error(), "render pull request survey") {
		t.Fatalf("refusal=%v; want the rendering named", err)
	}
}
