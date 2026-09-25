// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"strings"
)

// The pull-request survey is the read an operator makes BEFORE gathering
// evidence, and it is the last of the three surveys with no page beside its
// document. The shadow comparison, the shadow evidence and the reconcile
// survey each have one, for the reason this one needs one: the answer is
// assembled from three counts, a per-candidate listing whose refusals only
// appear inside it, and a shared-head grouping that decides whether the set
// can be gathered at all. Joining those by eye, over a set chosen precisely
// because it is too big to hold in the head, is the work the reader came to
// have done.
//
// Nothing here decides anything. The page states what the survey already
// decided, in the order the decisions matter, and every refusal below is
// about the page being unreadable rather than the survey being wrong.

var (
	// ErrPullRequestSurveyPageTooLarge is returned rather than a cut
	// page. A survey past this bound is a document to read with a
	// machine, and a page that quietly stops halfway is the one way this
	// could report a candidate set as cleaner than it is.
	ErrPullRequestSurveyPageTooLarge = errors.New("pull request survey is too large to render")
	// ErrPullRequestSurveyPageInvalid is returned for a survey that
	// cannot be stated: its own counts disagree with its candidates. The
	// page is read to decide which pull requests go into the evidence,
	// so an assembled value that contradicts itself is refused by name
	// instead of printed.
	ErrPullRequestSurveyPageInvalid = errors.New("pull request survey does not describe itself")
)

// maxRenderedSurveyCandidates bounds the candidate listing.
const maxRenderedSurveyCandidates = 200

// RenderPullRequestSurvey writes one candidate set's survey as the page it is
// read from.
//
// The verdict is first and states the decision rather than the counts, the
// convention the other three pages keep. The sections that follow are in
// decision order, never alphabetical and never the order the candidates were
// asked about: the commits two candidates share, then the candidates that can
// carry no evidence, then the runs the gather would actually use. That is the
// order the work is in, from the clash that refuses the whole gather, through
// the candidates to drop, to the set to hand on.
//
// A shared head leads because it is the only one of the three that is a fact
// about the SET rather than about a candidate: both pull requests are
// selectable, both are counted so, and `pull-request-evidence` refuses the
// pair anyway because the readiness threshold counts commits.
//
// A section with nothing in it is left out. "0 shared heads" on every
// ordinary survey teaches a reader to skip the line that matters.
//
// Nothing is written on a refusal: a caller that hands the page straight to
// a terminal should not be left with half of one above the error.
func RenderPullRequestSurvey(out io.Writer, report pullRequestSurveyReport) error {
	if len(report.PullRequests) > maxRenderedSurveyCandidates {
		return fmt.Errorf("%w: %d candidates, %d at most",
			ErrPullRequestSurveyPageTooLarge, len(report.PullRequests), maxRenderedSurveyCandidates)
	}
	if report.Considered != len(report.PullRequests) {
		return fmt.Errorf("%w: %d considered, %d answered for",
			ErrPullRequestSurveyPageInvalid, report.Considered, len(report.PullRequests))
	}
	selectable, unselectable := partitionSurveyedPullRequests(report)
	if len(selectable) != report.Selectable || len(unselectable) != report.Unselectable {
		return fmt.Errorf("%w: %d selectable and %d unselectable counted, %d and %d answered for",
			ErrPullRequestSurveyPageInvalid,
			report.Selectable, report.Unselectable, len(selectable), len(unselectable))
	}

	page := &bytes.Buffer{}
	if report.Unselectable == 0 && len(report.SharedHeads) == 0 {
		fmt.Fprintf(page, "ready: every candidate can carry %s evidence on a commit of its own\n",
			report.Workflow)
	} else {
		fmt.Fprintf(page, "not ready: this set cannot be gathered for %s evidence as it stands\n",
			report.Workflow)
	}
	fmt.Fprintf(page, "%d %s considered, %d selectable, %d unselectable\n",
		report.Considered, surveyPlural(report.Considered, "candidate", "candidates"),
		report.Selectable, report.Unselectable)
	for _, shared := range report.SharedHeads {
		fmt.Fprintf(page, "shared head %s: %s\n", shared.HeadSHA, numberedPullRequests(shared.PullRequests))
	}
	for _, candidate := range unselectable {
		fmt.Fprintf(page, "no evidence run: #%d at %s (%s)\n",
			candidate.Number, candidate.HeadSHA, candidate.Reason)
	}
	for _, candidate := range selectable {
		fmt.Fprintf(page, "selected: #%d at %s, run %d attempt %d (%s: %s)\n",
			candidate.Number, candidate.HeadSHA, candidate.RunID, candidate.Attempt,
			candidate.Event, candidate.Conclusion)
	}
	_, err := out.Write(page.Bytes())
	return err
}

// partitionSurveyedPullRequests splits the candidates the way the survey
// already split them, in the order the survey walked them. It reads the
// Selectable bit the survey wrote rather than re-deriving selectability from
// the run fields or the refusal text: two answers to one question in one
// command is how they drift apart, and the counts beside the report are
// written off that same bit.
func partitionSurveyedPullRequests(report pullRequestSurveyReport) (selectable, unselectable []surveyedPullRequest) {
	selectable = make([]surveyedPullRequest, 0, report.Selectable)
	unselectable = make([]surveyedPullRequest, 0, report.Unselectable)
	for _, candidate := range report.PullRequests {
		if candidate.Selectable {
			selectable = append(selectable, candidate)
			continue
		}
		unselectable = append(unselectable, candidate)
	}
	return selectable, unselectable
}

// numberedPullRequests writes a shared head's candidates the way a person
// refers to them. The numbers are the survey's own order, which is the order
// they were asked about: sorting them here would make two pages of one set
// disagree about which pull request came first.
func numberedPullRequests(numbers []int) string {
	named := make([]string, 0, len(numbers))
	for _, number := range numbers {
		named = append(named, fmt.Sprintf("#%d", number))
	}
	return strings.Join(named, ", ")
}
