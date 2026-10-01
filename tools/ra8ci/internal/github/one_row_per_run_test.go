// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import "testing"

func publishedRow(id int64, name, status, conclusion string) PublishedCheckRun {
	return PublishedCheckRun{ID: id, Name: name, Mode: ModeShadow, Status: status, Conclusion: conclusion}
}

func publishedIDs(runs []PublishedCheckRun) []int64 {
	ids := make([]int64, 0, len(runs))
	for _, run := range runs {
		ids = append(ids, run.ID)
	}
	return ids
}

func workflowIDs(runs []CommitWorkflowRun) []int64 {
	ids := make([]int64, 0, len(runs))
	for _, run := range runs {
		ids = append(ids, run.ID)
	}
	return ids
}

func sameIDs(got, want []int64) bool {
	if len(got) != len(want) {
		return false
	}
	for index := range got {
		if got[index] != want[index] {
			return false
		}
	}
	return true
}

func TestOnePublishedCheckRunPerIDKeepsAListingWithoutRepeats(t *testing.T) {
	runs := []PublishedCheckRun{
		publishedRow(1, "ra8ci/shadow/build", "completed", "success"),
		publishedRow(2, "ra8ci/shadow/test", "completed", "failure"),
		publishedRow(3, "ra8ci/shadow/lint", "in_progress", ""),
	}
	kept := onePublishedCheckRunPerID(runs)
	if !sameIDs(publishedIDs(kept), []int64{1, 2, 3}) {
		t.Fatalf("kept = %v, want 1 2 3", publishedIDs(kept))
	}
	for index := range kept {
		if kept[index] != runs[index] {
			t.Fatalf("row %d = %+v, want %+v", index, kept[index], runs[index])
		}
	}
}

func TestOnePublishedCheckRunPerIDCollapsesARowServedTwice(t *testing.T) {
	kept := onePublishedCheckRunPerID([]PublishedCheckRun{
		publishedRow(7, "ra8ci/shadow/build", "completed", "success"),
		publishedRow(8, "ra8ci/shadow/test", "completed", "success"),
		publishedRow(7, "ra8ci/shadow/build", "completed", "success"),
	})
	if !sameIDs(publishedIDs(kept), []int64{7, 8}) {
		t.Fatalf("kept = %v, want 7 8", publishedIDs(kept))
	}
}

func TestOnePublishedCheckRunPerIDKeepsTheLaterSightingAtTheEarlierPosition(t *testing.T) {
	kept := onePublishedCheckRunPerID([]PublishedCheckRun{
		publishedRow(7, "ra8ci/shadow/build", "in_progress", ""),
		publishedRow(8, "ra8ci/shadow/test", "queued", ""),
		publishedRow(7, "ra8ci/shadow/build", "completed", "success"),
	})
	if !sameIDs(publishedIDs(kept), []int64{7, 8}) {
		t.Fatalf("kept = %v, want 7 8", publishedIDs(kept))
	}
	if kept[0].Status != "completed" || kept[0].Conclusion != "success" {
		t.Fatalf("kept the earlier sighting: %+v", kept[0])
	}
}

func TestOnePublishedCheckRunPerIDCollapsesThreeSightings(t *testing.T) {
	kept := onePublishedCheckRunPerID([]PublishedCheckRun{
		publishedRow(4, "ra8ci/shadow/build", "queued", ""),
		publishedRow(4, "ra8ci/shadow/build", "in_progress", ""),
		publishedRow(4, "ra8ci/shadow/build", "completed", "failure"),
	})
	if !sameIDs(publishedIDs(kept), []int64{4}) {
		t.Fatalf("kept = %v, want a single row", publishedIDs(kept))
	}
	if kept[0].Conclusion != "failure" {
		t.Fatalf("kept = %+v, want the last sighting", kept[0])
	}
}

func TestOnePublishedCheckRunPerIDKeepsTwoRunsUnderOneName(t *testing.T) {
	kept := onePublishedCheckRunPerID([]PublishedCheckRun{
		publishedRow(11, "ra8ci/shadow/build", "completed", "success"),
		publishedRow(12, "ra8ci/shadow/build", "completed", "failure"),
	})
	if !sameIDs(publishedIDs(kept), []int64{11, 12}) {
		t.Fatalf("kept = %v, want both posts", publishedIDs(kept))
	}
}

func TestOnePublishedCheckRunPerIDKeepsRowsThatNameNoRun(t *testing.T) {
	kept := onePublishedCheckRunPerID([]PublishedCheckRun{
		publishedRow(0, "ra8ci/shadow/build", "completed", "success"),
		publishedRow(0, "ra8ci/shadow/test", "completed", "success"),
		publishedRow(-1, "ra8ci/shadow/lint", "completed", "success"),
	})
	if !sameIDs(publishedIDs(kept), []int64{0, 0, -1}) {
		t.Fatalf("kept = %v, want every row", publishedIDs(kept))
	}
}

func TestOnePublishedCheckRunPerIDOnNothing(t *testing.T) {
	if kept := onePublishedCheckRunPerID(nil); len(kept) != 0 {
		t.Fatalf("kept = %v, want none", publishedIDs(kept))
	}
	if kept := onePublishedCheckRunPerID([]PublishedCheckRun{}); len(kept) != 0 {
		t.Fatalf("kept = %v, want none", publishedIDs(kept))
	}
}

func TestOnePublishedCheckRunPerIDDoesNotAlterTheInput(t *testing.T) {
	runs := []PublishedCheckRun{
		publishedRow(5, "ra8ci/shadow/build", "in_progress", ""),
		publishedRow(5, "ra8ci/shadow/build", "completed", "success"),
	}
	onePublishedCheckRunPerID(runs)
	if runs[0].Status != "in_progress" || runs[1].Status != "completed" {
		t.Fatalf("input was altered: %+v", runs)
	}
}

func workflowRow(id int64, workflow, status, conclusion string) CommitWorkflowRun {
	return CommitWorkflowRun{ID: id, Workflow: workflow, Attempt: 1, Event: "push", Status: status, Conclusion: conclusion}
}

func TestOneCommitWorkflowRunPerIDKeepsAListingWithoutRepeats(t *testing.T) {
	runs := []CommitWorkflowRun{
		workflowRow(1, "ci", "completed", "success"),
		workflowRow(2, "docs", "completed", "success"),
	}
	kept := oneCommitWorkflowRunPerID(runs)
	if !sameIDs(workflowIDs(kept), []int64{1, 2}) {
		t.Fatalf("kept = %v, want 1 2", workflowIDs(kept))
	}
	for index := range kept {
		if kept[index] != runs[index] {
			t.Fatalf("row %d = %+v, want %+v", index, kept[index], runs[index])
		}
	}
}

func TestOneCommitWorkflowRunPerIDCollapsesARowServedTwice(t *testing.T) {
	kept := oneCommitWorkflowRunPerID([]CommitWorkflowRun{
		workflowRow(20, "ci", "in_progress", ""),
		workflowRow(21, "docs", "completed", "success"),
		workflowRow(20, "ci", "completed", "failure"),
	})
	if !sameIDs(workflowIDs(kept), []int64{20, 21}) {
		t.Fatalf("kept = %v, want 20 21", workflowIDs(kept))
	}
	if kept[0].Conclusion != "failure" {
		t.Fatalf("kept the earlier sighting: %+v", kept[0])
	}
}

func TestOneCommitWorkflowRunPerIDKeepsTwoRunsOfOneWorkflow(t *testing.T) {
	kept := oneCommitWorkflowRunPerID([]CommitWorkflowRun{
		workflowRow(30, "ci", "completed", "failure"),
		workflowRow(31, "ci", "completed", "success"),
	})
	if !sameIDs(workflowIDs(kept), []int64{30, 31}) {
		t.Fatalf("kept = %v, want both runs", workflowIDs(kept))
	}
}

func TestOneCommitWorkflowRunPerIDKeepsRowsThatNameNoRun(t *testing.T) {
	kept := oneCommitWorkflowRunPerID([]CommitWorkflowRun{
		workflowRow(0, "ci", "completed", "success"),
		workflowRow(0, "docs", "completed", "success"),
	})
	if !sameIDs(workflowIDs(kept), []int64{0, 0}) {
		t.Fatalf("kept = %v, want every row", workflowIDs(kept))
	}
}

func TestOneCommitWorkflowRunPerIDOnNothing(t *testing.T) {
	if kept := oneCommitWorkflowRunPerID(nil); len(kept) != 0 {
		t.Fatalf("kept = %v, want none", workflowIDs(kept))
	}
}
