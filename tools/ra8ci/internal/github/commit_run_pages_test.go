// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"testing"
)

// commitRunJSON is one workflow run as GitHub serves it.
func commitRunJSON(id int64, workflow, status, conclusion string) string {
	return fmt.Sprintf(`{"id":%d,"name":%q,"run_attempt":1,"head_sha":%q,"event":"push","status":%q,"conclusion":%q}`,
		id, workflow, commitRunsHead, status, conclusion)
}

// commitRunPage is one page of the listing, stating whatever count the caller
// wants it to state.
func commitRunPage(stated int, runs ...string) string {
	return fmt.Sprintf(`{"total_count":%d,"workflow_runs":[%s]}`, stated, strings.Join(runs, ","))
}

// filledCommitRunPage is a page the API could fill, numbered from first.
func filledCommitRunPage(first int64, stated int) string {
	runs := make([]string, 0, commitRunPageSize)
	for i := 0; i < commitRunPageSize; i++ {
		runs = append(runs, commitRunJSON(first+int64(i), "docs", "completed", "success"))
	}
	return commitRunPage(stated, runs...)
}

// A page the API filled is never the end of the walk, whatever count came back
// beside it. This is the half the listing's own arithmetic got wrong: a count
// answered from a commit that is still gaining runs can be smaller than the
// rows already served, and the walk it ended left runs unread.
func TestAFullCommitRunPageIsNeverTheEndOfTheWalk(t *testing.T) {
	for _, row := range []struct {
		name   string
		stated int
		page   int
	}{
		{"count says this page is all there is", commitRunPageSize, 1},
		{"count says less than this page carried", 3, 1},
		{"count says nothing is there at all", 0, 1},
		{"deep in the walk", 200, 2},
		{"count agrees there is more", 1000, 1},
	} {
		t.Run(row.name, func(t *testing.T) {
			if commitRunPageEndsTheWalk(commitRunPageSize, row.stated, row.page) {
				t.Fatalf("full page ended the walk: stated %d page %d", row.stated, row.page)
			}
		})
	}
}

// An empty page ends the walk outright. There is nothing on it to argue with
// and nothing beyond it to fetch.
func TestAnEmptyCommitRunPageEndsTheWalkWhateverTheCountSays(t *testing.T) {
	for _, stated := range []int{0, 1, commitRunPageSize, 100000} {
		t.Run(fmt.Sprint(stated), func(t *testing.T) {
			if !commitRunPageEndsTheWalk(0, stated, 1) {
				t.Fatalf("empty page continued the walk: stated %d", stated)
			}
		})
	}
}

// A page the API could not fill still defers to a count that says there is
// more, which is the shape the listing takes while runs are being added
// underneath it.
func TestAShortCommitRunPageDefersToACountThatSaysThereIsMore(t *testing.T) {
	for _, row := range []struct {
		rows   int
		stated int
		page   int
	}{
		{1, commitRunPageSize + 1, 1},
		{99, 101, 1},
		{50, 1000, 1},
		{1, 201, 2},
		{99, 100000, 9},
	} {
		t.Run(fmt.Sprintf("rows %d stated %d page %d", row.rows, row.stated, row.page), func(t *testing.T) {
			if commitRunPageEndsTheWalk(row.rows, row.stated, row.page) {
				t.Fatalf("short page ended a walk the count said continues")
			}
		})
	}
}

// A short page whose count is already satisfied is the ordinary end of the
// walk, and costs no confirming request.
func TestAShortCommitRunPageEndsTheWalkWhenTheCountIsSatisfied(t *testing.T) {
	for _, row := range []struct {
		rows   int
		stated int
		page   int
	}{
		{1, 1, 1},
		{99, 99, 1},
		{1, 0, 1},
		{50, commitRunPageSize, 1},
		{1, 101, 2},
		{99, 200, 2},
	} {
		t.Run(fmt.Sprintf("rows %d stated %d page %d", row.rows, row.stated, row.page), func(t *testing.T) {
			if !commitRunPageEndsTheWalk(row.rows, row.stated, row.page) {
				t.Fatalf("short page continued a walk nothing said continues")
			}
		})
	}
}

// The gap itself: a first page the API filled, beside a count small enough to
// have ended the old walk, is read past rather than trusted.
func TestRunsOnReadsPastAFullPageTheCountUnderstated(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	server.serveList(http.StatusOK,
		filledCommitRunPage(1, commitRunPageSize),
		commitRunPage(commitRunPageSize+1, commitRunJSON(9001, "checks", "completed", "failure")),
	)

	listed, err := reader.RunsOn(context.Background(), commitRunsHead)
	if err != nil {
		t.Fatalf("RunsOn: %v", err)
	}
	if len(listed.Runs) != commitRunPageSize+1 {
		t.Fatalf("listed %d runs, want %d", len(listed.Runs), commitRunPageSize+1)
	}
	last := listed.Runs[len(listed.Runs)-1]
	if last.ID != 9001 || last.Workflow != "checks" {
		t.Fatalf("the second page's run was not read: %+v", last)
	}
}

// A full last page costs one confirming request, and the confirmation adds
// nothing to what was already read.
func TestRunsOnConfirmsAFullLastPageWithOneMoreRequest(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	server.serveList(http.StatusOK,
		filledCommitRunPage(1, commitRunPageSize),
		commitRunPage(commitRunPageSize),
	)

	listed, err := reader.RunsOn(context.Background(), commitRunsHead)
	if err != nil {
		t.Fatalf("RunsOn: %v", err)
	}
	if len(listed.Runs) != commitRunPageSize {
		t.Fatalf("listed %d runs, want %d", len(listed.Runs), commitRunPageSize)
	}
	seen := map[int64]bool{}
	for _, run := range listed.Runs {
		if seen[run.ID] {
			t.Fatalf("run %d was listed twice", run.ID)
		}
		seen[run.ID] = true
	}
	if pages := commitRunListPages(t, server); pages != 2 {
		t.Fatalf("asked for %d pages, want 2", pages)
	}
}

// A short first page whose count is satisfied is read once and no more.
func TestRunsOnStopsOnAShortFirstPage(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	server.serveList(http.StatusOK, commitRunPage(2,
		commitRunJSON(11, "checks", "completed", "success"),
		commitRunJSON(12, "docs", "completed", "success"),
	))

	listed, err := reader.RunsOn(context.Background(), commitRunsHead)
	if err != nil {
		t.Fatalf("RunsOn: %v", err)
	}
	if len(listed.Runs) != 2 {
		t.Fatalf("listed %d runs, want 2", len(listed.Runs))
	}
	if pages := commitRunListPages(t, server); pages != 1 {
		t.Fatalf("asked for %d pages, want 1", pages)
	}
}

// A listing that fills every page up to the ceiling is refused, not reported.
// A full last page and a listing that continues are the same page, and
// reporting one as the other is the partial answer the rule exists to prevent.
func TestRunsOnRefusesACommitWhoseEveryPageIsFull(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	pages := make([]string, 0, maxCommitRunPages)
	for page := 0; page < maxCommitRunPages; page++ {
		pages = append(pages, filledCommitRunPage(int64(page*commitRunPageSize+1), commitRunPageSize))
	}
	server.serveList(http.StatusOK, pages...)

	if _, err := reader.RunsOn(context.Background(), commitRunsHead); !errors.Is(err, ErrCommitRunsUnreadable) {
		t.Fatalf("error %v, want ErrCommitRunsUnreadable", err)
	}
}

// What the short read cost, end to end: the run dropped behind an understated
// count is the second decided run of the named workflow, so the selection that
// used to answer confidently now refuses the ambiguity it exists to catch.
func TestTheDroppedRunWasTheAmbiguityTheSelectionRefuses(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	first := make([]string, 0, commitRunPageSize)
	first = append(first, commitRunJSON(4001, "checks", "completed", "success"))
	for i := 1; i < commitRunPageSize; i++ {
		first = append(first, commitRunJSON(int64(5000+i), "docs", "completed", "success"))
	}
	server.serveList(http.StatusOK,
		commitRunPage(commitRunPageSize, first...),
		commitRunPage(commitRunPageSize+1, commitRunJSON(4002, "checks", "completed", "failure")),
	)

	listed, err := reader.RunsOn(context.Background(), commitRunsHead)
	if err != nil {
		t.Fatalf("RunsOn: %v", err)
	}
	if _, err := SelectEvidenceRun(listed, "checks"); !errors.Is(err, ErrEvidenceRunAmbiguous) {
		t.Fatalf("selection answered %v, want ErrEvidenceRunAmbiguous", err)
	}
}

// Every request asks for a whole page, and asks for the pages in order.
func TestRunsOnAsksForWholePagesInOrder(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	server.serveList(http.StatusOK,
		filledCommitRunPage(1, 100000),
		commitRunPage(commitRunPageSize+1, commitRunJSON(7001, "docs", "completed", "success")),
	)

	if _, err := reader.RunsOn(context.Background(), commitRunsHead); err != nil {
		t.Fatalf("RunsOn: %v", err)
	}
	_, paths, queries := server.seenRequests()
	asked := []string{}
	for i, path := range paths {
		if strings.HasSuffix(path, "/actions/runs") {
			asked = append(asked, queries[i])
		}
	}
	if len(asked) != 2 {
		t.Fatalf("asked for %d pages, want 2", len(asked))
	}
	for i, query := range asked {
		if !strings.Contains(query, fmt.Sprintf("per_page=%d", commitRunPageSize)) {
			t.Fatalf("page %d did not ask for a whole page: %s", i+1, query)
		}
		if !strings.Contains(query, fmt.Sprintf("page=%d", i+1)) {
			t.Fatalf("page %d asked out of order: %s", i+1, query)
		}
	}
}

// The page size this reader asks for is the page size it judges a page by. A
// walk that asked for fifty and called a hundred full would never end.
func TestTheCommitRunPageSizeIsTheOneAskedFor(t *testing.T) {
	reader, server := newCommitRunsReader(t)
	server.serveList(http.StatusOK, commitRunPage(0))

	if _, err := reader.RunsOn(context.Background(), commitRunsHead); err != nil {
		t.Fatalf("RunsOn: %v", err)
	}
	_, paths, queries := server.seenRequests()
	for i, path := range paths {
		if !strings.HasSuffix(path, "/actions/runs") {
			continue
		}
		if !strings.Contains(queries[i], fmt.Sprintf("per_page=%d", commitRunPageSize)) {
			t.Fatalf("asked for a page other than %d: %s", commitRunPageSize, queries[i])
		}
	}
}

// commitRunListPages counts the listing requests the reader made.
func commitRunListPages(t *testing.T, server *commitRunsServer) int {
	t.Helper()
	_, paths, _ := server.seenRequests()
	pages := 0
	for _, path := range paths {
		if strings.HasSuffix(path, "/actions/runs") {
			pages++
		}
	}
	return pages
}
