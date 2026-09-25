// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"
)

// shadow_report_render.go prints ONE commit's comparison for the operator who
// has to read it. shadow_evidence.go accumulates many of those commits into the
// answer #1481 actually defers the required-check move on, and nothing prints
// that. `ra8ci github shadow-evidence` writes a machine document: per-task
// counts, three name lists, a shortfall table and two commit lists per task.
// That is the right shape for a pipe and the wrong shape for the decision,
// because the decision is made by a person reading it once and it is spread
// across five keys that have to be joined by hand.
//
// This is that page. It is pure, like the evidence it renders: the decision to
// move a required check is worth being re-makeable later from the same inputs,
// and a renderer that read anything would make the page a report of the moment
// it was printed rather than of the evidence it was given.

// maxRenderedEvidenceTasks bounds a rendered evidence page. The catalog is tens
// of tasks, not thousands, and a page long enough to scroll past is one nobody
// reads to the end of. It is the same argument maxRenderedComparisons makes for
// one commit's page.
const maxRenderedEvidenceTasks = 500

// maxRenderedEvidenceCommits bounds the commits named for one task. Naming the
// pull requests to go back to is the point of ConflictingCommits and
// IndeterminateCommits; a list past this length has stopped being a list of
// places to look.
const maxRenderedEvidenceCommits = 200

var (
	// ErrShadowEvidenceTooLarge is returned for an evidence page with more
	// tasks, or more named commits on one task, than a person would read.
	// It is a refusal rather than a truncation, for shadow_report_render.go's
	// reason: a page cut short in the middle of its conflicts is worse than
	// no page.
	ErrShadowEvidenceTooLarge = errors.New("shadow evidence has too much to render")
	// ErrShadowEvidenceMismatch is returned when the readiness answer does
	// not partition exactly the tasks the evidence covers. The two are
	// separate arguments, so a caller can hand over a readiness read from
	// some other accumulation, and a page that rendered it would attribute
	// one set of commits to another set of verdicts.
	ErrShadowEvidenceMismatch = errors.New("readiness does not answer for this evidence")
)

// RenderShadowEvidence writes the accumulated evidence as the page the
// required-check decision is read from.
//
// The verdict line is first and states the decision rather than the counts, the
// shadow_report_render.go convention: this page is either a reason to hold the
// required checks where they are or it is not. Then the sections, in the order
// the decision is made in: the tasks where ra8ci and Actions disagreed about a
// merge, then the tasks nobody has gathered enough evidence on, then the tasks
// that may move. A page ordered by task name would read as a list of facts.
func RenderShadowEvidence(out io.Writer, evidence ShadowEvidence, readiness ShadowReadiness) error {
	if out == nil {
		return errors.New("no writer for shadow evidence")
	}
	if len(evidence.Commits) == 0 || len(evidence.Tasks) == 0 {
		return fmt.Errorf("%w: nothing to render", ErrShadowEvidenceEmpty)
	}
	if len(evidence.Tasks) > maxRenderedEvidenceTasks {
		return fmt.Errorf("%w: %d tasks", ErrShadowEvidenceTooLarge, len(evidence.Tasks))
	}
	if readiness.Threshold < 1 {
		return fmt.Errorf("%w: %d", ErrShadowEvidenceThresholdInvalid, readiness.Threshold)
	}
	byTask, err := evidenceByTask(evidence)
	if err != nil {
		return err
	}
	if err := checkReadinessCovers(byTask, readiness); err != nil {
		return err
	}
	shortfall, err := shortfallByTask(readiness)
	if err != nil {
		return err
	}
	if err := checkShortfallAddsUp(byTask, readiness, shortfall); err != nil {
		return err
	}
	if err := checkCountedVerdicts(byTask, readiness); err != nil {
		return err
	}
	if err := checkNamedCommits(byTask, readiness, accumulatedCommits(evidence)); err != nil {
		return err
	}
	if err := checkUngradedCommits(evidence, byTask, readiness); err != nil {
		return err
	}

	var page strings.Builder
	fmt.Fprintf(&page, "shadow evidence over %d commit%s, threshold %d\n",
		len(evidence.Commits), plural(len(evidence.Commits)), readiness.Threshold)
	if readiness.Settled() {
		fmt.Fprintf(&page, "every compared task is ready: %d graded on at least %d commit%s with no disagreement\n",
			len(readiness.Ready), readiness.Threshold, plural(readiness.Threshold))
	} else {
		page.WriteString("holds the required checks: ")
		page.WriteString(evidenceHoldReason(readiness))
		page.WriteString("\n")
	}
	fmt.Fprintf(&page, "%d ready, %d conflicting, %d insufficient\n",
		len(readiness.Ready), len(readiness.Conflicting), len(readiness.Insufficient))
	if err := writeUngradedLine(&page, evidence); err != nil {
		return err
	}

	if len(readiness.Conflicting) > 0 {
		page.WriteString("\nconflicting (ra8ci and Actions disagreed about a merge)\n")
		for _, name := range readiness.Conflicting {
			task := byTask[name]
			fmt.Fprintf(&page, "  %s: %d of %d graded commit%s disagreed\n",
				name, task.Conflicting, task.Graded, plural(task.Graded))
			if err := writeCommitLine(&page, "disagreed on", task.ConflictingCommits); err != nil {
				return err
			}
		}
	}
	if len(readiness.Insufficient) > 0 {
		page.WriteString("\ninsufficient (no disagreement, not yet graded often enough)\n")
		for _, name := range readiness.Insufficient {
			task := byTask[name]
			short := shortfall[name]
			fmt.Fprintf(&page, "  %s: graded on %d, %d more needed (paired on %d, %d never judged)\n",
				name, short.Graded, short.Remaining, task.Observed, task.Indeterminate)
			if err := writeCommitLine(&page, "never judged on", task.IndeterminateCommits); err != nil {
				return err
			}
		}
	}
	if len(readiness.Ready) > 0 {
		page.WriteString("\nready (may move)\n")
		for _, name := range readiness.Ready {
			task := byTask[name]
			fmt.Fprintf(&page, "  %s: %d graded (%d agreed, %d divergent)\n",
				name, task.Graded, task.Agreed, task.Divergent)
		}
	}

	if _, err := io.WriteString(out, page.String()); err != nil {
		return fmt.Errorf("write shadow evidence: %w", err)
	}
	return nil
}

// writeUngradedLine says how much of the evidence counted for nothing, and is
// written directly under the count line rather than in a section of its own.
// The first line of this page says the evidence covers so many commits, and a
// reader takes that as the breadth the threshold was met across; a commit every
// task came back indeterminate on is part of that number and moved nothing. The
// line is left out entirely when there are none, because a page that says "0
// graded nothing" on every clean run trains a reader to skip the place the
// warning appears.
//
// The commits are named, not counted, for writeCommitLine's reason: they are
// the pull requests to go back to.
func writeUngradedLine(page *strings.Builder, evidence ShadowEvidence) error {
	if len(evidence.UngradedCommits) == 0 {
		return nil
	}
	if len(evidence.UngradedCommits) > maxRenderedEvidenceCommits {
		return fmt.Errorf("%w: %d commits graded nothing", ErrShadowEvidenceTooLarge, len(evidence.UngradedCommits))
	}
	// An ungraded commit the accumulation does not carry is refused
	// rather than printed. AccumulateShadowEvidence cannot produce one,
	// so an evidence value carrying one was assembled by hand, and the
	// line would send an operator to a pull request this evidence never
	// looked at.
	accumulated := accumulatedCommits(evidence)
	for _, commit := range evidence.UngradedCommits {
		if !accumulated[commit] {
			return fmt.Errorf("%w: %s graded nothing and is not one of the accumulated commits",
				ErrShadowEvidenceReportInvalid, commit)
		}
	}
	fmt.Fprintf(page, "%d of them graded nothing (every pairing indeterminate): %s\n",
		len(evidence.UngradedCommits), strings.Join(evidence.UngradedCommits, ", "))
	return nil
}

// checkUngradedCommits reads the ungraded line against the sections printed
// under it. The line says "N of them graded nothing (every pairing
// indeterminate)" and then names those commits, and that sentence is a claim
// about the same commits the conflicting section names underneath it. Nothing
// read the two together: writeUngradedLine holds the list to the accumulation,
// and checkNamedCommits holds each task's printed list to the accumulation, so
// both lists can name the same commit and each is right on its own. The page
// then tells an operator a commit moved no task one step closer to its
// threshold, and four lines lower tells them a task disagreed with Actions on
// exactly that commit, which is the strongest evidence this page ever carries.
//
// Only ConflictingCommits is read against it. A commit that graded nothing came
// back indeterminate on every task it paired, so its appearance in an
// insufficient task's "never judged on" list is the two lines agreeing, not
// contradicting: refusing that would refuse every page with an ungraded commit
// on it.
//
// The blank is read here too, for the line's own sake: a blank the
// accumulation happens to carry answers to writeUngradedLine's check and
// renders "3 of them graded nothing: aaa1, , ccc3".
//
// *** A COMMIT NAMED TWICE IS DELIBERATELY NOT REFUSED, AND THE GAP IS PINNED
// OPEN BY TestACommitNamedTwiceAsUngradedStillRenders. The printed count is
// len(UngradedCommits), so a duplicate does overstate how much of the evidence
// counted for nothing, but the list's size bound is enforced inside
// writeUngradedLine while the page is being written, i.e. after every check
// here, and the bound is exercised by a fixture that repeats one commit past
// maxRenderedEvidenceCommits. A duplicate check in this position refuses an
// over-long list as a duplicate instead of as a page too long to read. That is
// the trap #1665 hit with the commit-list lengths, and the fix is the same one:
// hold the list to its length where the length is read, not here. ***
//
// writeUngradedLine's accumulation check stays where it is for the same
// reason.
func checkUngradedCommits(evidence ShadowEvidence, byTask map[string]TaskEvidence, readiness ShadowReadiness) error {
	if len(evidence.UngradedCommits) == 0 {
		return nil
	}
	ungraded := make(map[string]bool, len(evidence.UngradedCommits))
	for _, commit := range evidence.UngradedCommits {
		if strings.TrimSpace(commit) == "" {
			return fmt.Errorf("%w: a commit that graded nothing is unnamed",
				ErrShadowEvidenceReportInvalid)
		}
		ungraded[commit] = true
	}
	// Walked in section order, so a page with two contradictions refuses
	// for the one the reader reaches first.
	for _, name := range readiness.Conflicting {
		for _, commit := range byTask[name].ConflictingCommits {
			if ungraded[commit] {
				return fmt.Errorf("%w: %s graded nothing, and %q disagreed on it",
					ErrShadowEvidenceReportInvalid, commit, name)
			}
		}
	}
	return nil
}

// evidenceHoldReason says which of the two unclean conditions is present, both
// when both are, because they are different jobs: a disagreement is argued
// about and an insufficiency is waited out. holdReason makes the same argument
// for one commit's page.
func evidenceHoldReason(readiness ShadowReadiness) string {
	switch {
	case len(readiness.Conflicting) > 0 && len(readiness.Insufficient) > 0:
		return fmt.Sprintf("%d disagreed with Actions, %d short of the threshold",
			len(readiness.Conflicting), len(readiness.Insufficient))
	case len(readiness.Conflicting) > 0:
		return fmt.Sprintf("%d disagreed with Actions", len(readiness.Conflicting))
	case len(readiness.Insufficient) > 0:
		return fmt.Sprintf("%d short of the threshold", len(readiness.Insufficient))
	default:
		// Settled() also requires at least one ready task, so a readiness
		// with no conflict and no insufficiency can still be unsettled:
		// it covered nothing. Saying so is the whole reason this branch
		// exists rather than being folded into the insufficient case.
		return "no task was compared at all"
	}
}

// writeCommitLine names the commits to go back to. The list is indented under
// its task and carried in the order the reports were given, which is the order
// the pull requests were observed in: re-sorting would invent an order that
// means nothing, the argument ShadowEvidence.Commits already makes.
func writeCommitLine(page *strings.Builder, label string, commits []string) error {
	if len(commits) == 0 {
		return nil
	}
	if len(commits) > maxRenderedEvidenceCommits {
		return fmt.Errorf("%w: %d commits %s one task", ErrShadowEvidenceTooLarge, len(commits), label)
	}
	fmt.Fprintf(page, "    %s: %s\n", label, strings.Join(commits, ", "))
	return nil
}

// checkShortfallAddsUp reads the numbers on an insufficient line against the
// two things the page already states around them. The line is
// "graded on N, M more needed (paired on P, Q never judged)": N and M come off
// the shortfall, P and Q off the evidence, and the threshold M counts toward is
// printed on the first line of the page. Nothing read them together, so a
// shortfall assembled beside an accumulation rather than from it renders a line
// that contradicts the header it sits under and the pairing counts it sits
// beside, with no number on the page wrong on its own.
//
// shortfallByTask already holds the shortfall to the insufficient NAMES. This
// holds it to their numbers.
func checkShortfallAddsUp(byTask map[string]TaskEvidence, readiness ShadowReadiness, shortfall map[string]TaskShortfall) error {
	for _, name := range readiness.Insufficient {
		short := shortfall[name]
		if short.Graded != byTask[name].Graded {
			return fmt.Errorf("%w: %q is graded on %d in the evidence and on %d in its shortfall",
				ErrShadowEvidenceMismatch, name, byTask[name].Graded, short.Graded)
		}
		if short.Remaining < 1 {
			return fmt.Errorf("%w: %q is held short with %d more needed",
				ErrShadowEvidenceMismatch, name, short.Remaining)
		}
		if short.Graded+short.Remaining != readiness.Threshold {
			return fmt.Errorf("%w: %q is graded on %d with %d more needed, which is not the threshold %d this page was read at",
				ErrShadowEvidenceMismatch, name, short.Graded, short.Remaining, readiness.Threshold)
		}
	}
	return nil
}

// checkCountedVerdicts refuses a task whose counted verdicts do not add up to
// the number printed beside them.
//
// Every section of this page prints this task's integers and nothing else.
// The conflicting line is "%d of %d graded commit(s) disagreed" (Conflicting
// out of Graded), the insufficient line carries "(paired on %d, %d never
// judged)" (Observed and Indeterminate), and the ready line is "%d graded (%d
// agreed, %d divergent)". They are four counters and two totals written by one
// walk in AccumulateShadowEvidence, and until now nothing read them against
// each other: checkReadinessCovers reads the readiness against the tasks,
// checkShortfallAddsUp reads the shortfall against Graded, checkNamedCommits
// reads the commit lists against the accumulation. The task's own arithmetic
// was the last thing on this page nobody checked.
//
// The worst reading is on the ready line, the section headed "ready (may
// move)". "4 graded (1 agreed, 1 divergent)" is a task whose page says it
// cleared a threshold of four and accounts for two, and the two numbers that
// would say what the other two were are the ones printed. An operator moving a
// required check on this page has no other source for them. The conflicting
// line's is "0 of 3 graded commits disagreed" under a heading that says this
// task disagreed, which is a task in the section for tasks to argue about with
// nothing to argue about.
//
// The COUNTS are read here and the LENGTHS of the commit lists beside them are
// deliberately not, though "3 of 5 graded commits disagreed" over a "disagreed
// on:" line naming two commits is the same family of wrong. The size bounds on
// those lists (maxRenderedEvidenceCommits) are enforced by writeCommitLine
// while the page is being written, which is after every check in this
// function, so a check here would refuse an over-long list as a disagreement
// between a count and a list rather than as a page too long to read. Holding
// the lists to their counts means moving that bound forward first, and that is
// an argument about where a size bound belongs, not about arithmetic.
//
// It is read AFTER the readiness checks and BEFORE checkNamedCommits. A
// readiness assembled beside some other accumulation is not this page's
// arithmetic going wrong, it is the wrong pair of arguments, and a reader told
// that first is told the more useful thing. Within a task, the numbers on a
// line are read before the commits that line names, the order #1663 settled
// for the insufficient line.
//
// It is the report sentinel rather than the mismatch one: the readiness is not
// in the wrong here, the accumulation is, and nothing AccumulateShadowEvidence
// writes can fail it. Every verdict increments exactly one counter and Graded
// with it, Observed is incremented once per pairing, and a conflicting or
// indeterminate pairing appends exactly one commit as it counts it.
func checkCountedVerdicts(byTask map[string]TaskEvidence, readiness ShadowReadiness) error {
	for _, name := range evidenceSections(readiness) {
		task := byTask[name]
		if task.Agreed+task.Divergent+task.Conflicting != task.Graded {
			return fmt.Errorf("%w: %q is graded on %d and counts %d agreed, %d divergent, %d disagreeing",
				ErrShadowEvidenceReportInvalid, name, task.Graded,
				task.Agreed, task.Divergent, task.Conflicting)
		}
		if task.Graded+task.Indeterminate != task.Observed {
			return fmt.Errorf("%w: %q is paired on %d and counts %d graded with %d never judged",
				ErrShadowEvidenceReportInvalid, name, task.Observed,
				task.Graded, task.Indeterminate)
		}
	}
	for _, name := range readiness.Conflicting {
		task := byTask[name]
		if task.Conflicting < 1 {
			return fmt.Errorf("%w: %q is named as disagreeing and counts %d disagreeing commits",
				ErrShadowEvidenceReportInvalid, name, task.Conflicting)
		}
	}
	return nil
}

// evidenceSections walks the readiness in the order the page prints it, so a
// page with two tasks wrong is refused for the one a reader reaches first.
func evidenceSections(readiness ShadowReadiness) []string {
	names := make([]string, 0, len(readiness.Conflicting)+len(readiness.Insufficient)+len(readiness.Ready))
	names = append(names, readiness.Conflicting...)
	names = append(names, readiness.Insufficient...)
	names = append(names, readiness.Ready...)
	return names
}

// checkNamedCommits holds the commit lists this page prints to the commits the
// evidence was accumulated from. writeUngradedLine already makes this argument
// for the ungraded line: a commit the accumulation does not carry sends an
// operator to a pull request this evidence never looked at. The two per-task
// lists are the same sentence in the same page and are the ones writeCommitLine
// calls the point of the answer, so they are read the same way.
//
// Only the lists the page PRINTS are read: ConflictingCommits for a conflicting
// task and IndeterminateCommits for an insufficient one. A ready task's
// conflicting commits are printed nowhere and are counted nowhere (every count
// on this page comes off the integer fields), so refusing a whole page over a
// list nobody would have seen takes a readable page away from an operator for
// nothing. That is the rule the reconcile page's subject check settled, applied
// here.
//
// The readiness lists are walked rather than byTask, so the refusals come in
// the order the sections are printed in rather than in map order.
func checkNamedCommits(byTask map[string]TaskEvidence, readiness ShadowReadiness, accumulated map[string]bool) error {
	for _, name := range readiness.Conflicting {
		if err := checkCommitList(name, "disagreed on", byTask[name].ConflictingCommits, accumulated); err != nil {
			return err
		}
	}
	for _, name := range readiness.Insufficient {
		if err := checkCommitList(name, "never judged on", byTask[name].IndeterminateCommits, accumulated); err != nil {
			return err
		}
	}
	return nil
}

// checkCommitList reads one printed commit list. The label is the one the page
// prints for that list, so the refusal says which line the operator would have
// read rather than naming the field it came from.
//
// A blank commit is read before an unaccumulated one, and not only because an
// unnamed commit is the worse line: an accumulation carrying a blank of its own
// would make a blank commit answer to the accumulated set and render a list
// with a gap in it.
func checkCommitList(name, label string, commits []string, accumulated map[string]bool) error {
	for _, commit := range commits {
		if strings.TrimSpace(commit) == "" {
			return fmt.Errorf("%w: %q %s an unnamed commit", ErrShadowEvidenceReportInvalid, name, label)
		}
		if !accumulated[commit] {
			return fmt.Errorf("%w: %q %s %s, which is not one of the accumulated commits",
				ErrShadowEvidenceReportInvalid, name, label, commit)
		}
	}
	return nil
}

// accumulatedCommits is the set of commits this evidence looked at. It is the
// one definition of that, read by every line on the page that names a commit.
func accumulatedCommits(evidence ShadowEvidence) map[string]bool {
	accumulated := make(map[string]bool, len(evidence.Commits))
	for _, commit := range evidence.Commits {
		accumulated[commit] = true
	}
	return accumulated
}

// evidenceByTask indexes the accumulated tasks. A task named twice is refused
// rather than resolved: AccumulateShadowEvidence cannot produce one, so an
// evidence value carrying one was assembled by hand, and picking a winner would
// render counts nobody accumulated.
func evidenceByTask(evidence ShadowEvidence) (map[string]TaskEvidence, error) {
	byTask := make(map[string]TaskEvidence, len(evidence.Tasks))
	for _, task := range evidence.Tasks {
		if task.Task == "" {
			return nil, fmt.Errorf("%w: evidence covers an unnamed task", ErrShadowEvidenceReportInvalid)
		}
		if _, seen := byTask[task.Task]; seen {
			return nil, fmt.Errorf("%w: %q", ErrShadowSetAmbiguous, task.Task)
		}
		byTask[task.Task] = task
	}
	return byTask, nil
}

// checkReadinessCovers holds the readiness to exactly the tasks the evidence
// covers, in both directions. A name the evidence does not carry would be
// printed with another task's counts beside it, and a covered task missing from
// all three lists would be silently left off a page whose whole purpose is to
// say where every task stands.
func checkReadinessCovers(byTask map[string]TaskEvidence, readiness ShadowReadiness) error {
	answered := make(map[string]bool, len(byTask))
	for _, list := range [][]string{readiness.Ready, readiness.Conflicting, readiness.Insufficient} {
		for _, name := range list {
			if _, covered := byTask[name]; !covered {
				return fmt.Errorf("%w: %q is not in the evidence", ErrShadowEvidenceMismatch, name)
			}
			if answered[name] {
				return fmt.Errorf("%w: %q is answered for twice", ErrShadowEvidenceMismatch, name)
			}
			answered[name] = true
		}
	}
	if len(answered) != len(byTask) {
		return fmt.Errorf("%w: %s", ErrShadowEvidenceMismatch, unanswered(byTask, answered))
	}
	return nil
}

// unanswered names the covered tasks the readiness said nothing about, so the
// refusal tells the caller which they were rather than only that the counts
// differed.
func unanswered(byTask map[string]TaskEvidence, answered map[string]bool) string {
	missing := make([]string, 0, len(byTask))
	for name := range byTask {
		if !answered[name] {
			missing = append(missing, name)
		}
	}
	sort.Strings(missing)
	return fmt.Sprintf("%s unanswered", strings.Join(missing, ", "))
}

// shortfallByTask indexes the shortfall entries and holds them to the
// insufficient list. Readiness builds the two together; a hand-assembled
// readiness whose shortfall names a different set would put one task's
// remaining count under another task's name.
func shortfallByTask(readiness ShadowReadiness) (map[string]TaskShortfall, error) {
	byTask := make(map[string]TaskShortfall, len(readiness.Shortfall))
	for _, short := range readiness.Shortfall {
		if _, seen := byTask[short.Task]; seen {
			return nil, fmt.Errorf("%w: shortfall names %q twice", ErrShadowEvidenceMismatch, short.Task)
		}
		byTask[short.Task] = short
	}
	for _, name := range readiness.Insufficient {
		if _, stated := byTask[name]; !stated {
			return nil, fmt.Errorf("%w: %q is insufficient with no shortfall", ErrShadowEvidenceMismatch, name)
		}
	}
	if len(byTask) != len(readiness.Insufficient) {
		return nil, fmt.Errorf("%w: shortfall covers %d task(s), %d are insufficient",
			ErrShadowEvidenceMismatch, len(byTask), len(readiness.Insufficient))
	}
	return byTask, nil
}

// plural is the "s" on a counted noun. A page that says "1 commits" reads as
// generated rather than written, and this page is asking a person to make a
// decision on what it says.
func plural(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}
