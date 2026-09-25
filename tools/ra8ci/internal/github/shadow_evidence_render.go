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
	accumulated := make(map[string]bool, len(evidence.Commits))
	for _, commit := range evidence.Commits {
		accumulated[commit] = true
	}
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
