// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

// Both of this package's commit listings are walked by page number:
// PublishedRuns asks for page 1, page 2, page 3 of a commit's check runs, and
// RunsOn does the same for its workflow runs. Every row every page carried is
// appended, and the walk stops on the arithmetic the two page rules state.
//
// Page numbers address positions in a listing, not rows. That is the same
// property both page rules are written about, read from the other end: a run
// posted between two requests moves every row after it, and a row that moves
// from the end of page 1 to the start of page 2 is served twice. The page
// rules make sure a walk does not END early when the listing grows underneath
// it; nothing made sure the walk does not COLLECT the same run twice while it
// does.
//
// A repeated row is not a harmless duplicate here, because both listings are
// read as statements about how many runs a commit carries.
// PublishedCheckRuns.Named answers with every run under a name and says in
// writing that more than one means a repeated write left two runs on the
// commit, which is a state an operator is meant to go and look at; a row the
// paging served twice tells them to go and look at a run that was posted once.
// UnplannedRuns reports a stale run once per row, so the same retired run is
// argued about twice. On the workflow side SelectEvidenceRun refuses a commit
// carrying two decided runs of one workflow, precisely because a re-run and
// the original disagree: a row served twice is one run made to disagree with
// itself, and the evidence selection an operator cannot resolve is over a
// second run that does not exist.
//
// So a run the listing served more than once is collapsed to one row. It is
// not refused: a commit gaining a run mid-walk is the ordinary state both page
// rules already accommodate, and failing the reconciliation over it would turn
// a benign concurrent post into the blind republish the reconciliation exists
// to prevent.
//
// Sameness is the run's own identifier and nothing else. Two runs can share a
// name, a status and a conclusion and be two posts an operator has to see;
// only the identifier says they are one run. A row whose identifier is not a
// positive number names no run to be the same as, so it is kept as it came:
// dropping it would be this file deciding a listing's malformed row is a
// duplicate, which it cannot know.
//
// The row kept is the LAST sighting, at the position of the FIRST. A run's
// state moves forward while a walk is in progress (queued, then in progress,
// then completed with a conclusion), so the later read is the current answer
// about it, and keeping the earlier position leaves both listings in the order
// their own documentation promises: newest first for workflow runs, and the
// deterministic order PublishedRuns sorts into afterwards.

// onePublishedCheckRunPerID collapses check run rows the listing served on
// more than one page.
func onePublishedCheckRunPerID(runs []PublishedCheckRun) []PublishedCheckRun {
	kept := make([]PublishedCheckRun, 0, len(runs))
	at := make(map[int64]int, len(runs))
	for _, run := range runs {
		if run.ID <= 0 {
			kept = append(kept, run)
			continue
		}
		if index, seen := at[run.ID]; seen {
			kept[index] = run
			continue
		}
		at[run.ID] = len(kept)
		kept = append(kept, run)
	}
	return kept
}

// oneCommitWorkflowRunPerID collapses workflow run rows the listing served on
// more than one page.
func oneCommitWorkflowRunPerID(runs []CommitWorkflowRun) []CommitWorkflowRun {
	kept := make([]CommitWorkflowRun, 0, len(runs))
	at := make(map[int64]int, len(runs))
	for _, run := range runs {
		if run.ID <= 0 {
			kept = append(kept, run)
			continue
		}
		if index, seen := at[run.ID]; seen {
			kept[index] = run
			continue
		}
		at[run.ID] = len(kept)
		kept = append(kept, run)
	}
	return kept
}
