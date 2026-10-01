// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

// runPages answers the completed-run list from a fixed supply of runs, paging
// it the way the GitHub API does, and records the per_page/page pairs it was
// asked for.
type runPages struct {
	supply  int
	started string

	mu       sync.Mutex
	requests [][2]int
}

func (pages *runPages) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	perPage, _ := strconv.Atoi(r.URL.Query().Get("per_page"))
	page, _ := strconv.Atoi(r.URL.Query().Get("page"))
	if perPage < 1 || page < 1 {
		http.Error(w, "bad paging", http.StatusBadRequest)
		return
	}
	pages.mu.Lock()
	pages.requests = append(pages.requests, [2]int{perPage, page})
	pages.mu.Unlock()
	first := (page - 1) * perPage
	var body strings.Builder
	body.WriteString(`{"workflow_runs":[`)
	for index := first; index < first+perPage && index < pages.supply; index++ {
		if index != first {
			body.WriteString(",")
		}
		fmt.Fprintf(&body, `{"id":%d,"name":"workflow","run_started_at":%q}`, index+1, pages.started)
	}
	body.WriteString("]}")
	_, _ = w.Write([]byte(body.String()))
}

func (pages *runPages) seen() [][2]int {
	pages.mu.Lock()
	defer pages.mu.Unlock()
	return append([][2]int(nil), pages.requests...)
}

func walk(t *testing.T, supply, limit int, hours *int, started string) ([]run, [][2]int) {
	t.Helper()
	pages := &runPages{supply: supply, started: started}
	server := httptest.NewTLSServer(pages)
	defer server.Close()
	api, err := newActionsAPIAt(server.URL, "test-token", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	collected, err := runs(context.Background(), api, "owner/repo", limit, hours)
	if err != nil {
		t.Fatalf("runs(limit=%d) failed: %v", limit, err)
	}
	return collected, pages.seen()
}

func TestAScanWiderThanOnePageReadsThePagesBehindIt(t *testing.T) {
	recent := time.Now().UTC().Format(time.RFC3339)
	collected, requests := walk(t, 400, 250, nil, recent)
	if len(collected) != 250 {
		t.Fatalf("a 250-run scan read %d runs", len(collected))
	}
	if len(requests) != 3 {
		t.Fatalf("expected three pages, got %v", requests)
	}
	for index, request := range requests {
		if request[0] != maxRunPage || request[1] != index+1 {
			t.Fatalf("page %d was requested as per_page=%d page=%d", index+1, request[0], request[1])
		}
	}
	for index, item := range collected {
		if item.ID != int64(index+1) {
			t.Fatalf("run %d of the walk is id %d; the pages overlap or skip", index, item.ID)
		}
	}
}

func TestAPageTheAPICouldNotFillEndsTheWalk(t *testing.T) {
	recent := time.Now().UTC().Format(time.RFC3339)
	collected, requests := walk(t, 40, 250, nil, recent)
	if len(collected) != 40 {
		t.Fatalf("a repository with 40 completed runs gave %d", len(collected))
	}
	if len(requests) != 1 {
		t.Fatalf("a short page did not end the walk: %v", requests)
	}
}

func TestAScanInsideOnePageStillAsksForOnePage(t *testing.T) {
	recent := time.Now().UTC().Format(time.RFC3339)
	collected, requests := walk(t, 400, 40, nil, recent)
	if len(collected) != 40 {
		t.Fatalf("a 40-run scan read %d runs", len(collected))
	}
	if len(requests) != 1 || requests[0][0] != 40 {
		t.Fatalf("a scan inside one page did not ask for exactly it: %v", requests)
	}
}

func TestTheHoursWindowBoundsWhatIsKeptNotHowFarTheWalkReaches(t *testing.T) {
	hours := 1
	old := time.Now().UTC().Add(-48 * time.Hour).Format(time.RFC3339)
	collected, requests := walk(t, 400, 250, &hours, old)
	if len(collected) != 0 {
		t.Fatalf("runs outside the window were kept: %d", len(collected))
	}
	if len(requests) != 3 {
		t.Fatalf("dropped runs changed how far the walk reached: %v", requests)
	}
	recent := time.Now().UTC().Format(time.RFC3339)
	inside, _ := walk(t, 400, 150, &hours, recent)
	if len(inside) != 150 {
		t.Fatalf("runs inside the window were dropped: %d", len(inside))
	}
}

func TestAScanPastTheEndOfPagingIsRefusedRatherThanTruncated(t *testing.T) {
	pages := &runPages{supply: 10, started: time.Now().UTC().Format(time.RFC3339)}
	server := httptest.NewTLSServer(pages)
	defer server.Close()
	api, err := newActionsAPIAt(server.URL, "test-token", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	if _, err := runs(context.Background(), api, "owner/repo", maxScanRuns+1, nil); err == nil {
		t.Fatal("a scan wider than the API will page was accepted")
	}
	if requests := pages.seen(); len(requests) != 0 {
		t.Fatalf("a refused scan still spent requests: %v", requests)
	}
	if _, err := runs(context.Background(), api, "owner/repo", maxScanRuns, nil); err != nil {
		t.Fatalf("the widest supported scan was refused: %v", err)
	}
}

func TestAnEmptyPageEndsTheWalkWithoutAnotherRequest(t *testing.T) {
	collected, requests := walk(t, 0, 250, nil, time.Now().UTC().Format(time.RFC3339))
	if len(collected) != 0 {
		t.Fatalf("an empty repository gave %d runs", len(collected))
	}
	if len(requests) != 1 {
		t.Fatalf("an empty first page did not end the walk: %v", requests)
	}
}
