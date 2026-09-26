// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package github

import (
	"context"
	"fmt"
	"net/http"
	"strings"
	"testing"
)

// A page the API filled is never the end of the walk, whatever count came
// back beside it. This is the half the listing's own arithmetic got wrong: a
// count answered from a commit that is still gaining runs can be smaller than
// the rows already served, and the walk it ended left runs unread.
func TestAFullPageIsNeverTheEndOfTheWalk(t *testing.T) {
	for _, row := range []struct {
		name   string
		rows   int
		stated int
		page   int
	}{
		{"count says this page is all there is", publishedCheckRunPageSize, publishedCheckRunPageSize, 1},
		{"count says less than this page carried", publishedCheckRunPageSize, 3, 1},
		{"count says nothing is there at all", publishedCheckRunPageSize, 0, 1},
		{"deep in the walk", publishedCheckRunPageSize, 200, 2},
		{"count agrees there is more", publishedCheckRunPageSize, 1000, 1},
	} {
		t.Run(row.name, func(t *testing.T) {
			if commitCheckRunPageEndsTheWalk(row.rows, row.stated, row.page) {
				t.Fatalf("full page ended the walk: rows %d stated %d page %d", row.rows, row.stated, row.page)
			}
		})
	}
}

// An empty page ends the walk outright. There is nothing on it to argue with
// and nothing beyond it to fetch, and a count still claiming more at that
// point is describing runs the API is not serving.
func TestAnEmptyPageEndsTheWalkWhateverTheCountSays(t *testing.T) {
	for _, stated := range []int{0, 1, publishedCheckRunPageSize, 100000} {
		t.Run(fmt.Sprint(stated), func(t *testing.T) {
			if !commitCheckRunPageEndsTheWalk(0, stated, 1) {
				t.Fatalf("empty page continued the walk: stated %d", stated)
			}
		})
	}
}

// A page the API could not fill still defers to a count that says there is
// more, which is the shape the listing takes while runs are being added
// underneath it.
func TestAShortPageDefersToACountThatSaysThereIsMore(t *testing.T) {
	for _, row := range []struct {
		name   string
		rows   int
		stated int
		page   int
		ends   bool
	}{
		{"short page, count satisfied", 3, 3, 1, true},
		{"short page, count says more", 1, 101, 1, false},
		{"short page, count satisfied on page two", 1, 101, 2, true},
		{"short page, count one beyond this page", 99, 101, 1, false},
		{"last row of a short page ends it", publishedCheckRunPageSize - 1, publishedCheckRunPageSize, 1, true},
	} {
		t.Run(row.name, func(t *testing.T) {
			if got := commitCheckRunPageEndsTheWalk(row.rows, row.stated, row.page); got != row.ends {
				t.Fatalf("ends = %v, want %v", got, row.ends)
			}
		})
	}
}

// The regression this file exists for. A commit whose first page comes back
// full under a count that says the page is everything is a commit with runs on
// a page nothing read, and a run the reader does not report is a run
// ReconcilePublish answers PublishNeeded for and the plane posts twice.
func TestAnUnderstatedCountDoesNotTruncateTheListing(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	first, second := reconcilerTaskNames(t)
	fullName, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	beyondName, err := CheckRunName(ModeAuthoritative, second)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	server.serveListing(http.StatusOK,
		fmt.Sprintf(`{"total_count":%d,"check_runs":[%s]}`,
			publishedCheckRunPageSize, filledPage(fullName)),
		`{"total_count":101,"check_runs":[`+
			publishedRunJSON(9001, beyondName, reconcilerHead, "completed", "failure", "")+`]}`,
		`{"total_count":101,"check_runs":[]}`)
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	if len(published.Runs) != publishedCheckRunPageSize+1 {
		t.Fatalf("published = %d runs", len(published.Runs))
	}
	if len(published.Named(beyondName)) != 1 {
		t.Fatalf("the run beyond the first page was not read: %+v", published.Runs)
	}
	if requests := countListingRequests(server); requests != 2 {
		t.Fatalf("listing requests = %d", requests)
	}
}

// A commit whose runs end exactly on a page boundary costs one confirming
// request, because a full page cannot say whether it is the last.
func TestAnExactlyFullLastPageCostsAConfirmingRequest(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	first, _ := reconcilerTaskNames(t)
	name, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	server.serveListing(http.StatusOK,
		fmt.Sprintf(`{"total_count":%d,"check_runs":[%s]}`, publishedCheckRunPageSize, filledPage(name)),
		fmt.Sprintf(`{"total_count":%d,"check_runs":[]}`, publishedCheckRunPageSize))
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	if len(published.Runs) != publishedCheckRunPageSize {
		t.Fatalf("published = %d runs", len(published.Runs))
	}
	if requests := countListingRequests(server); requests != 2 {
		t.Fatalf("listing requests = %d", requests)
	}
}

// The ordinary commit is unchanged: a page the API could not fill, under a
// count that agrees, is the whole listing and costs one request.
func TestAShortFirstPageIsTheWholeListing(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	first, _ := reconcilerTaskNames(t)
	name, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	server.serveListing(http.StatusOK, `{"total_count":2,"check_runs":[`+
		publishedRunJSON(1, name, reconcilerHead, "completed", "success", "")+`,`+
		publishedRunJSON(2, name, reconcilerHead, "completed", "success", "")+`]}`)
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	if len(published.Runs) != 2 {
		t.Fatalf("published = %+v", published.Runs)
	}
	if requests := countListingRequests(server); requests != 1 {
		t.Fatalf("listing requests = %d", requests)
	}
}

// A page full of names this plane does not publish keeps nothing and is still
// a full page. The rows the API served decide the walk; the runs kept from
// them decide nothing, or a commit busy with Actions checks would hide the
// plane's own runs behind them.
func TestAPageOfForeignNamesStillContinuesTheWalk(t *testing.T) {
	reconciler, server := newCheckRunReconciler(t)
	first, _ := reconcilerTaskNames(t)
	name, err := CheckRunName(ModeAuthoritative, first)
	if err != nil {
		t.Fatalf("name: %v", err)
	}
	foreign := make([]string, 0, publishedCheckRunPageSize)
	for index := 0; index < publishedCheckRunPageSize; index++ {
		foreign = append(foreign, publishedRunJSON(int64(index+1),
			fmt.Sprintf("build (%d)", index), reconcilerHead, "completed", "success", ""))
	}
	server.serveListing(http.StatusOK,
		fmt.Sprintf(`{"total_count":%d,"check_runs":[%s]}`,
			publishedCheckRunPageSize, strings.Join(foreign, ",")),
		`{"total_count":101,"check_runs":[`+
			publishedRunJSON(9002, name, reconcilerHead, "completed", "success", "")+`]}`,
		`{"total_count":101,"check_runs":[]}`)
	published, err := reconciler.PublishedRuns(context.Background(), reconcilerHead)
	if err != nil {
		t.Fatalf("PublishedRuns: %v", err)
	}
	if len(published.Runs) != 1 || published.Runs[0].ID != 9002 {
		t.Fatalf("published = %+v", published.Runs)
	}
}

// filledPage is one full page of runs under a single name. GitHub keeps every
// post as its own run, so a name repeated with distinct identifiers is a shape
// the listing really takes.
func filledPage(name string) string {
	rows := make([]string, 0, publishedCheckRunPageSize)
	for index := 0; index < publishedCheckRunPageSize; index++ {
		rows = append(rows, publishedRunJSON(int64(index+1), name, reconcilerHead, "completed", "success", ""))
	}
	return strings.Join(rows, ",")
}

// countListingRequests counts the requests made against the commit's check run
// listing, leaving out the installation token mints.
func countListingRequests(server *commitCheckRunServer) int {
	_, paths, _ := server.observed()
	listings := 0
	for _, path := range paths {
		if strings.HasSuffix(path, "/check-runs") {
			listings++
		}
	}
	return listings
}
