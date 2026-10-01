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
// asked about: the commits two candidates share, then the bases the set is
// spread across, then the candidates that can carry no evidence, then the
// selections worth reading before they are gathered, then the runs the
// gather would actually use. That is the order the work is in, from the
// clash that refuses the whole gather, through the shape of the set and the
// candidates to drop and the ones to look at twice, to the set to hand on.
//
// A shared head leads because it is the one section that refuses the gather:
// both pull requests are selectable, both are counted so, and
// `pull-request-evidence` refuses the pair anyway because the readiness
// threshold counts commits. The bases follow it because they are the other
// fact about the SET rather than about a candidate, and a reader who has
// just been told two candidates clash is the reader who wants to know the
// set is not all aimed at one branch either.
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
	if err := checkSurveyedCandidates(report); err != nil {
		return err
	}
	if err := checkSurveyedAnswers(report); err != nil {
		return err
	}
	if err := checkSurveyedSubject(report); err != nil {
		return err
	}
	if err := checkSurveyedHeadState(report); err != nil {
		return err
	}
	if err := checkSurveyedBase(report); err != nil {
		return err
	}
	if err := checkSurveySharedHeads(report); err != nil {
		return err
	}
	if err := checkSurveyedSharedCommits(report); err != nil {
		return err
	}

	page := &bytes.Buffer{}
	// The readiness question is answered once, by candidateSetIsReady,
	// and this line states that answer. Writing the expression out again
	// here is how a page that says a set is ready ends up over a command
	// that refuses it.
	if candidateSetIsReady(report) {
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
	for _, base := range surveyedBases(selectable) {
		fmt.Fprintf(page, "base %s: %s\n", base.BaseRef, numberedPullRequests(base.PullRequests))
	}
	for _, candidate := range unselectable {
		fmt.Fprintf(page, "no evidence run: #%d at %s (%s)\n",
			candidate.Number, candidate.HeadSHA, candidate.Reason)
	}
	for _, caveat := range caveatedSelections(selectable) {
		fmt.Fprintf(page, "read before gathering: #%d at %s (%s)\n",
			caveat.Number, caveat.HeadSHA, caveat.Reason)
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

// checkSurveyedCandidates refuses a listing that does not answer for each
// candidate exactly once, by a number a reader can open.
//
// The counts are checked against this listing, and the shared heads are
// checked against it too, so it is the one slice of the document everything
// else on the page is read through. Nothing checked it. A survey answering
// for one pull request twice passes both count checks, because the counts
// are derived from the listing's own length and its own selectable bits, and
// then prints that candidate twice: twice under its base, twice in the
// caveats, twice in the selections. A reader counting the selected lines to
// decide how much evidence the gather will carry counts one pull request as
// two.
//
// The repeat is worse than a doubled line, and this is why the check runs
// before the shared heads rather than beside them. checkSurveySharedHeads
// looks a candidate up BY NUMBER to decide whether it is really at the
// commit it is grouped under. With one number answered for twice, that
// lookup reads whichever of the two came last: a real clash can be refused
// as "#1589 shares abc and is at def", naming a head the reader will not
// find on the line the page prints, or pass because the second answer
// happened to agree. A refusal about the wrong candidate is the one thing
// this page must never produce.
//
// An unnumbered candidate is refused for the reason the page names every
// candidate by number: every line an operator acts on is "#1589", and their
// next move is to open it. "#0" is not a pull request, and a listing that
// carries one is not a survey of ours: the number comes straight off the
// head GitHub answered with, so a missing one means the document was
// assembled somewhere else. It is named by the commit it sits on, because
// that is the only thing left to identify it with.
//
// Both refusals are the page's existing invalid sentinel. Neither is a size
// bound: the listing is already bounded, and a repeat is not a long page, it
// is a wrong one.
func checkSurveyedCandidates(report pullRequestSurveyReport) error {
	answeredFor := make(map[int]struct{}, len(report.PullRequests))
	for _, candidate := range report.PullRequests {
		if candidate.Number <= 0 {
			return fmt.Errorf("%w: a candidate at %s is numbered %d",
				ErrPullRequestSurveyPageInvalid,
				statedSurveyHead(candidate.HeadSHA), candidate.Number)
		}
		if _, twice := answeredFor[candidate.Number]; twice {
			return fmt.Errorf("%w: #%d is answered for more than once",
				ErrPullRequestSurveyPageInvalid, candidate.Number)
		}
		answeredFor[candidate.Number] = struct{}{}
	}
	return nil
}

// checkSurveyedAnswers refuses a candidate whose answer is not the one its
// own selectable bit says it is.
//
// The listing is checked for who it answers for, and nothing checked WHAT it
// answers. Both halves of it are printed with fields nothing reads first,
// and each half has one field the reader's next move depends on.
//
// An unselectable candidate's line is "no evidence run: #1589 at abc123
// (reason)", and the reason is the only thing on it that says why the
// candidate is being dropped: the refusal text appears nowhere else in the
// document, and the page is read to decide which pull requests to leave out
// of the gather. Without it the line reads "#1589 at abc123 ()", which asks
// the operator to drop a candidate and tells them nothing they could argue
// with.
//
// A selectable candidate's line names a run, and naming a run is the whole
// point of the section: the gather opens run 771 on that commit. A selection
// carrying no run prints "run 0 attempt 0", which is a line an operator acts
// on pointing at a run that does not exist. The run identifier comes straight
// off the run GitHub answered with, so a selection without one was assembled
// somewhere other than a survey of ours.
//
// A selectable candidate carrying a refusal is the same contradiction from
// the other side, and it is read first because it explains the other two: the
// survey writes a reason exactly when it refuses a candidate, and it refuses
// and selects in the same breath. The page would print such a candidate among
// the selections with its refusal dropped on the floor, which is the one
// direction this page must never round in.
//
// The other run fields are deliberately not checked. An attempt of zero, an
// unnamed event and an empty conclusion are all things a real run can come
// back with, and refusing a survey over them would refuse a page that is
// perfectly readable.
//
// All three are the page's existing invalid sentinel, and none is a bound:
// the listing is already bounded, and a candidate answered for wrongly is not
// a long page, it is a wrong one.
func checkSurveyedAnswers(report pullRequestSurveyReport) error {
	for _, candidate := range report.PullRequests {
		refusal := strings.TrimSpace(candidate.Reason)
		if candidate.Selectable {
			if refusal != "" {
				return fmt.Errorf("%w: #%d is selectable and is refused as %s",
					ErrPullRequestSurveyPageInvalid, candidate.Number, refusal)
			}
			if candidate.RunID <= 0 {
				return fmt.Errorf("%w: #%d is selectable and names no run",
					ErrPullRequestSurveyPageInvalid, candidate.Number)
			}
			continue
		}
		if refusal == "" {
			return fmt.Errorf("%w: #%d is unselectable and no reason is given",
				ErrPullRequestSurveyPageInvalid, candidate.Number)
		}
	}
	return nil
}

// checkSurveyedSubject refuses a survey that does not say what it is about.
//
// The listing is checked for who it answers for and for what it answers, and
// neither reads the two words the page prints about the work itself. The
// workflow is on the line read first, in both readings of it: a survey
// without one renders "ready: every candidate can carry  evidence on a
// commit of its own", which is a verdict about nothing over a set an operator
// is about to gather. The head is on every candidate's line in both
// listings, and it is the commit the gather opens the run on: "selected:
// #1589 at , run 771 attempt 1" names a run and no commit to find it on, and
// "no evidence run: #1591 at  (no run for Checks on )" asks an operator to
// drop a candidate over a commit it does not name.
//
// Every candidate is read, unlike the reconcile page's conflicting tasks,
// because this page prints every one of them: a selectable candidate on a
// selected line, an unselectable one on a no-evidence-run line. There is no
// candidate here whose head a reader would never have seen.
//
// It is read AFTER the listing and the answers and BEFORE the shared heads,
// and both halves of that matter. A candidate numbered zero at no commit is
// still refused as unnumbered, naming the unstated commit, because the
// number is what a reader opens. A candidate at no commit grouped under a
// shared head would otherwise be refused as "#1589 shares abc123 and is at",
// a sentence with a gap where the answer goes.
//
// Whitespace is not a statement, the rule the other checks on this page keep
// for a commit: a head of three spaces would print as a blank one.
//
// Nothing a real survey writes is refused: the workflow is the argument the
// command was given, and the head comes straight off the pull request GitHub
// answered with.
func checkSurveyedSubject(report pullRequestSurveyReport) error {
	if strings.TrimSpace(report.Workflow) == "" {
		return fmt.Errorf("%w: the survey names no workflow",
			ErrPullRequestSurveyPageInvalid)
	}
	for _, candidate := range report.PullRequests {
		if strings.TrimSpace(candidate.HeadSHA) == "" {
			return fmt.Errorf("%w: #%d is at no commit",
				ErrPullRequestSurveyPageInvalid, candidate.Number)
		}
	}
	return nil
}

// checkSurveyedHeadState refuses a selection whose head state the page
// cannot state.
//
// The listing is checked for who it answers for, for what it answers, and
// for the two words the page prints about the work. The head facts are the
// last of it unread, and they are the ones the caveat section is made of:
// caveatedSelections turns State, Merged and FromFork into the "read before
// gathering" line, the section that exists precisely because a merged pull
// request and a fork's head read like an ordinary selection on every other
// line of this page.
//
// A selection stating no head state is the first shape refused. The page
// says nothing at all about a candidate with no caveat, so a survey that
// could not answer for a state renders a candidate that reads open, ours and
// ordinary. That is worse than a gap in a sentence, which is what #1648
// refuses a blank commit for: there is no gap to see. GitHub answers with a
// state for every pull request and the head reader refuses a pull request
// without one, so no survey of ours carries a blank.
//
// A selection that is merged and stated open is the second. Those two cannot
// both be true, and caveatedSelections says "already merged" and drops the
// state on the floor, so the contradiction never reaches the page either
// way. It is read after the blank because a state that was never stated
// cannot contradict anything.
//
// Only the selectable candidates are read, the rule #1647 keeps on the
// reconcile page and the deliberate contrast with #1648 here: this page
// prints every candidate's number and commit, so those are read for every
// one of them, but the caveats are derived from the selections alone. An
// unselectable candidate is named on its own line, is not going to be
// gathered, and its head facts are printed nowhere.
//
// Both are the page's existing invalid sentinel, and neither is a bound.
// A candidate answered for wrongly is not a long page, it is a wrong one.
func checkSurveyedHeadState(report pullRequestSurveyReport) error {
	for _, candidate := range report.PullRequests {
		if !candidate.Selectable {
			continue
		}
		state := strings.TrimSpace(candidate.State)
		if state == "" {
			return fmt.Errorf("%w: #%d is selectable and is in no state",
				ErrPullRequestSurveyPageInvalid, candidate.Number)
		}
		if candidate.Merged && strings.EqualFold(state, "open") {
			return fmt.Errorf("%w: #%d is merged and is stated %s",
				ErrPullRequestSurveyPageInvalid, candidate.Number, state)
		}
	}
	return nil
}

// checkSurveyedBase refuses a selection aimed at no base.
//
// The listing is now read for who it answers for, for what it answers, for
// the two words the page prints about the work and for the head facts the
// caveats are made of. The base is the last document field a selection
// carries onto this page, and it is the one the set-shaped section is built
// from: surveyedBases groups the selections by it, and that section exists
// because a pull request aimed at a release branch reads, on every other
// line of this page, exactly like the rest of the set.
//
// A blank base does not render as a gap in a sentence. surveyedBases skips
// it, by the key it groups on, so the candidate is simply absent from the
// grouping: the page names the bases of the rest of the set and says nothing
// at all about where this one is aimed. Worse is the shape that hides the
// section outright. Two selections on one base and a third aimed at nothing
// leave one key, the section is omitted for being a set aimed at one base,
// and a page that exists to say the set is spread says the opposite by
// saying nothing. That is the reading this check refuses: a survey that
// could not answer for a base must not be stated as a set aimed at one.
//
// The skip in surveyedBases stays where it is. It is what keeps the grouping
// from carrying an empty key, and it is reached on a page this check has
// already refused.
//
// Only the selectable candidates are read, the rule checkSurveyedHeadState
// keeps for the head facts and for the same reason: the grouping is derived
// from the selections alone, and an unselectable candidate is dropped from
// it deliberately, so where it was aimed is printed nowhere. It is the
// deliberate contrast with checkSurveyedSubject, which reads every candidate
// because every candidate's number and commit is printed.
//
// It is read AFTER the head state, so a selection that is merged and aimed
// at nothing is refused as the contradiction the caveat section would have
// stated, and BEFORE the shared heads, so a grouping refusal never names a
// candidate this page could not have placed.
//
// Whitespace is not a statement, the rule every other check on this page
// keeps: surveyedBases trims before it groups, so a base of three spaces is
// skipped exactly like a blank one.
//
// Nothing a real survey writes is refused. The base comes straight off the
// pull request GitHub answered with, and the head reader refuses a pull
// request whose base repository is not ours before it ever reaches here.
//
// The refusal is the page's existing invalid sentinel and is not a bound. A
// candidate answered for wrongly is not a long page, it is a wrong one.
func checkSurveyedBase(report pullRequestSurveyReport) error {
	for _, candidate := range report.PullRequests {
		if !candidate.Selectable {
			continue
		}
		if strings.TrimSpace(candidate.BaseRef) == "" {
			return fmt.Errorf("%w: #%d is selectable and is aimed at no base",
				ErrPullRequestSurveyPageInvalid, candidate.Number)
		}
	}
	return nil
}

// statedSurveyHead names the commit a candidate sits on for a refusal that
// cannot name the candidate. A survey that answered for neither is refused
// saying so rather than with an empty gap in the sentence.
func statedSurveyHead(head string) string {
	head = strings.TrimSpace(head)
	if head == "" {
		return "an unstated commit"
	}
	return head
}

// checkSurveySharedHeads refuses a survey whose shared heads are not about
// its own candidates.
//
// The counts are already checked against the candidates, because a page that
// says "3 selectable" over two selections is a page an operator would act
// on. The shared heads were not, and they are the section that leads the
// page and the one that spends the verdict: everything below them is read in
// the light of "these two candidates clash". They were printed exactly as
// the document stated them, so a document from another build, or one put
// together by hand, could put a clash on the page between pull requests this
// survey never surveyed.
//
// Four things make a shared head a fact about this survey, and `sharedHeads`
// produces all four by construction: more than one candidate, every candidate
// surveyed here, every one of them actually at that commit, and one group per
// commit. A document that fails any of them is refused by name rather than
// rendered, the treatment its counts already get.
//
// The fourth is what bounds this section. Everything else on the page is
// bounded by the candidate count: the bases, the caveats and the two listings
// are all derived from candidates the survey answered for, and there are at
// most `maxRenderedSurveyCandidates` of those. The shared heads are their own
// slice in the document, and while each group is bounded by the candidates it
// may name, nothing stopped a document carrying the same clash a thousand
// times over under a thousand spellings of one commit. One group per commit
// makes the section as long as the candidates allow and no longer, which is a
// better answer than a bound: a page refused for its size tells a reader
// nothing about which line was wrong.
//
// A commit is matched the way `sharedHeads` grouped it, without its casing or
// surrounding space: one commit written two ways is one commit, and refusing
// over the spelling would refuse a survey that is perfectly well formed.
//
// The commit the group is about is read before any of that, because every
// refusal in here names it: "%s is shared by 1 candidate(s)", "%s is shared
// in more than one group", "#1589 shares %s with itself", "... and was not
// surveyed", "... and is at abc123". A group carrying no commit turns all
// five into a sentence with a gap where the subject goes, and turns the line
// the page leads with into "shared head : #1589, #1590". One check first
// answers for all of them, the same reason #1648 reads a candidate's head
// before the group it is grouped under.
//
// The candidates are not named in that refusal. A group that names no commit
// may name no candidates either, and "a shared head grouping  names no
// commit" is the gap this check exists to close. The count says which group
// it is without ever being blank.
//
// A candidate is read for its number before it is looked up, the answer the
// reconcile page's grouping already gives a run of its own. The listing
// refuses an unnumbered candidate, so a group naming #0 could only ever be
// looked up and missed, and "#0 shares abc123 and was not surveyed" sends a
// reader off to find a pull request the survey was never asked about. The
// document is not wrong about which candidates were surveyed; it is carrying
// a number nobody can open, and it is refused saying so.
func checkSurveySharedHeads(report pullRequestSurveyReport) error {
	at := make(map[int]string, len(report.PullRequests))
	for _, candidate := range report.PullRequests {
		at[candidate.Number] = strings.ToLower(strings.TrimSpace(candidate.HeadSHA))
	}
	grouped := make(map[string]struct{}, len(report.SharedHeads))
	for _, shared := range report.SharedHeads {
		if strings.TrimSpace(shared.HeadSHA) == "" {
			return fmt.Errorf("%w: a shared head over %d candidate(s) names no commit",
				ErrPullRequestSurveyPageInvalid, len(shared.PullRequests))
		}
		if len(shared.PullRequests) < 2 {
			return fmt.Errorf("%w: %s is shared by %d candidate(s)",
				ErrPullRequestSurveyPageInvalid, shared.HeadSHA, len(shared.PullRequests))
		}
		commit := strings.ToLower(strings.TrimSpace(shared.HeadSHA))
		if _, twice := grouped[commit]; twice {
			return fmt.Errorf("%w: %s is shared in more than one group",
				ErrPullRequestSurveyPageInvalid, shared.HeadSHA)
		}
		grouped[commit] = struct{}{}
		named := make(map[int]struct{}, len(shared.PullRequests))
		for _, number := range shared.PullRequests {
			if number <= 0 {
				return fmt.Errorf("%w: a candidate sharing %s is numbered %d",
					ErrPullRequestSurveyPageInvalid, shared.HeadSHA, number)
			}
			if _, repeated := named[number]; repeated {
				return fmt.Errorf("%w: #%d shares %s with itself",
					ErrPullRequestSurveyPageInvalid, number, shared.HeadSHA)
			}
			named[number] = struct{}{}
			head, surveyed := at[number]
			if !surveyed {
				return fmt.Errorf("%w: #%d shares %s and was not surveyed",
					ErrPullRequestSurveyPageInvalid, number, shared.HeadSHA)
			}
			if head != commit {
				return fmt.Errorf("%w: #%d shares %s and is at %s",
					ErrPullRequestSurveyPageInvalid, number, shared.HeadSHA, head)
			}
		}
	}
	return nil
}

// checkSurveyedSharedCommits refuses a survey whose candidates share a commit
// the grouping does not name.
//
// checkSurveySharedHeads reads every group against the listing: the commit is
// stated, the group is not empty, no commit is grouped twice, and every
// candidate it names really is at that commit. All five refusals are about a
// group that is there. Nothing read the other direction, and that is the one
// the page's first line depends on.
//
// candidateSetIsReady answers the readiness question with
// `len(report.SharedHeads) == 0`, so a document carrying two candidates at one
// commit and no group for it prints "ready: every candidate can carry Checks
// evidence on a commit of its own" and then lists both candidates as selected.
// `pull-request-evidence` refuses that set, because the readiness threshold
// counts commits and this one is counted twice. An operator who read this page
// first was told the opposite by the page whose job is to tell them. A missing
// group is worse than a wrong one: a wrong group is a line to check, while a
// missing group is a clean verdict over a set that cannot be gathered.
//
// The grouping is derived here the way `sharedHeads` derives it, and that
// agreement is the whole point: matched without casing or surrounding space,
// because one commit written two ways is one commit; blanks skipped, because
// two unanswered heads are not a shared commit; and every candidate the survey
// answered for is read, selectable or not, because `sharedHeads` walks the
// surveyed heads rather than the selections.
//
// A group that names only some of the candidates at its commit is refused too.
// `sharedHeads` names all of them, the page prints the group verbatim, and
// "shared head abc123: #1589, #1590" over a third candidate also at abc123
// understates the clash on the one line a reader uses to size it.
//
// The groups are read before this, so a group that is itself malformed is
// refused as that rather than as a candidate missing from it.
func checkSurveyedSharedCommits(report pullRequestSurveyReport) error {
	grouped := make(map[string]map[int]struct{}, len(report.SharedHeads))
	for _, shared := range report.SharedHeads {
		named := make(map[int]struct{}, len(shared.PullRequests))
		for _, number := range shared.PullRequests {
			named[number] = struct{}{}
		}
		grouped[strings.ToLower(strings.TrimSpace(shared.HeadSHA))] = named
	}
	order := make([]string, 0, len(report.PullRequests))
	sharing := make(map[string][]int, len(report.PullRequests))
	stated := make(map[string]string, len(report.PullRequests))
	for _, candidate := range report.PullRequests {
		commit := strings.ToLower(strings.TrimSpace(candidate.HeadSHA))
		if commit == "" {
			continue
		}
		if _, met := sharing[commit]; !met {
			order = append(order, commit)
			// Reported as the first candidate stated it, the
			// rule sharedHeads keeps for a commit.
			stated[commit] = candidate.HeadSHA
		}
		sharing[commit] = append(sharing[commit], candidate.Number)
	}
	for _, commit := range order {
		at := sharing[commit]
		if len(at) < 2 {
			continue
		}
		named, isGrouped := grouped[commit]
		if !isGrouped {
			return fmt.Errorf("%w: #%d and #%d are both at %s and it is not grouped as a shared head",
				ErrPullRequestSurveyPageInvalid, at[0], at[1], stated[commit])
		}
		for _, number := range at {
			if _, ok := named[number]; !ok {
				return fmt.Errorf("%w: #%d is at %s and is not named among the candidates sharing it",
					ErrPullRequestSurveyPageInvalid, number, stated[commit])
			}
		}
	}
	return nil
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

// surveyBase is one base branch the set is aimed at, with the candidates
// aimed at it.
type surveyBase struct {
	BaseRef      string
	PullRequests []int
}

// surveyedBases names the base branches the selections are spread across,
// and says nothing at all when they are all aimed at one.
//
// The survey reads every candidate's base and writes it into the document,
// and the page dropped it. A candidate set is gathered as one body of
// evidence against one readiness threshold, so which branch each candidate
// is aimed at is a fact about whether the set is one set. A pull request
// aimed at a release branch sitting in a set gathered for the development
// branch reads, on every other line of this page, exactly like the rest of
// them: open, ours, a run behind it, selectable.
//
// It states and never refuses, the rule #1625 settled and #1635 kept. A set
// spread across two bases is gatherable and may well be deliberate; being
// deliberate is the operator's to decide, and they cannot decide it from a
// page that does not mention it.
//
// Nothing is said when every selection is aimed at one base, which is the
// ordinary survey. "1 base" on every page is the line that teaches a reader
// to skip the line that matters, the reason the other sections are omitted
// when empty.
//
// Only the selectable candidates are read. An unselectable one is not going
// to be gathered, so where it was aimed cannot make the gathered set span
// two branches; naming it here would report a spread that the gather does
// not have.
//
// A base is matched without its casing or surrounding space and reported as
// the first candidate stated it, the rule sharedHeads keeps for a commit.
// A candidate whose base the survey could not answer for is not a base of
// its own: two unanswered bases are not two branches, and reporting a blank
// one as a base would turn an unread head into a spread.
func surveyedBases(selectable []surveyedPullRequest) []surveyBase {
	order := make([]string, 0, len(selectable))
	at := make(map[string]*surveyBase, len(selectable))
	for _, candidate := range selectable {
		key := strings.ToLower(strings.TrimSpace(candidate.BaseRef))
		if key == "" {
			continue
		}
		base := at[key]
		if base == nil {
			base = &surveyBase{BaseRef: candidate.BaseRef}
			at[key] = base
			order = append(order, key)
		}
		base.PullRequests = append(base.PullRequests, candidate.Number)
	}
	if len(order) < 2 {
		return nil
	}
	bases := make([]surveyBase, 0, len(order))
	for _, key := range order {
		bases = append(bases, *at[key])
	}
	return bases
}

// surveyCaveat is one selectable candidate whose head is not the ordinary
// case the rest of the page reads as: a pull request that is no longer open,
// or one whose head lives in a fork.
type surveyCaveat struct {
	Number  int
	HeadSHA string
	Reason  string
}

// caveatedSelections names the selections an operator should look at twice
// before handing the set on.
//
// The survey already reads a candidate's state, whether it merged, and
// whether its head is in a fork, and it writes all three into the document.
// The page dropped them, so the two selections that are worth a second
// thought read exactly like the ordinary ones. A merged pull request still
// has a run and is still selectable, but the evidence it would carry is
// about a branch that is already in the base; and a fork's run is somebody
// else's branch under a workflow of ours. Neither is wrong to gather, which
// is why this states them rather than refusing them: the page's job is to
// put the fact where it is read, and #1625 settled that naming something
// moves no verdict.
//
// Only selectable candidates are read here. An unselectable one is named on
// its own line already and is not going to be gathered, so a second line
// about its head is noise on the section that matters.
//
// A candidate that is both merged and forked is read once, with both
// reasons, rather than on two lines: two lines about one pull request read
// as two pull requests.
func caveatedSelections(selectable []surveyedPullRequest) []surveyCaveat {
	caveats := make([]surveyCaveat, 0, len(selectable))
	for _, candidate := range selectable {
		reasons := make([]string, 0, 2)
		// Merged is said instead of the state, never beside it: a
		// merged pull request is closed too, and "already merged;
		// no longer open (closed)" states one fact twice.
		state := strings.TrimSpace(candidate.State)
		switch {
		case candidate.Merged:
			reasons = append(reasons, "already merged")
		case state != "" && !strings.EqualFold(state, "open"):
			reasons = append(reasons, "no longer open ("+state+")")
		}
		if candidate.FromFork {
			reasons = append(reasons, forkedHead(candidate.HeadRepository))
		}
		if len(reasons) == 0 {
			continue
		}
		caveats = append(caveats, surveyCaveat{
			Number:  candidate.Number,
			HeadSHA: candidate.HeadSHA,
			Reason:  strings.Join(reasons, "; "),
		})
	}
	return caveats
}

// forkedHead names the fork a head sits in, and says the head is forked even
// when the survey could not name the repository: which fork it is matters
// less than that it is one.
func forkedHead(repository string) string {
	repository = strings.TrimSpace(repository)
	if repository == "" {
		return "from a fork"
	}
	return "from the fork " + repository
}
