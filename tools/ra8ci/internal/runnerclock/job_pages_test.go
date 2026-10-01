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
)

// jobPages answers a run's job list from a fixed supply of jobs, paging it
// the way the GitHub API does, and records the per_page/page pairs it was
// asked for. The job at skewedIndex carries a step that finished before it
// started, so a test can put the fault anywhere in the list.
type jobPages struct {
	supply      int
	skewedIndex int
	alwaysFull  bool

	mu       sync.Mutex
	requests [][2]int
}

func (pages *jobPages) ServeHTTP(w http.ResponseWriter, r *http.Request) {
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
	last := first + perPage
	if !pages.alwaysFull && last > pages.supply {
		last = pages.supply
	}
	var body strings.Builder
	body.WriteString(`{"jobs":[`)
	for index := first; index < last; index++ {
		if index != first {
			body.WriteString(",")
		}
		completed := "2026-07-28T06:00:09Z"
		if index == pages.skewedIndex {
			completed = "2026-07-28T05:54:09Z"
		}
		fmt.Fprintf(&body, `{"name":"job-%d","runner_name":"runner-%d","steps":[{"name":"gate","started_at":"2026-07-28T06:00:04Z","completed_at":%q}]}`,
			index+1, index+1, completed)
	}
	body.WriteString("]}")
	_, _ = w.Write([]byte(body.String()))
}

func (pages *jobPages) asked() [][2]int {
	pages.mu.Lock()
	defer pages.mu.Unlock()
	return append([][2]int(nil), pages.requests...)
}

func walkJobs(t *testing.T, pages *jobPages) ([]job, [][2]int, error) {
	t.Helper()
	server := httptest.NewTLSServer(pages)
	defer server.Close()
	api, err := newActionsAPIAt(server.URL, "test-token", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	collected, walkErr := runJobs(context.Background(), api, "owner/repo", 1)
	return collected, pages.asked(), walkErr
}

func TestARunWithMoreJobsThanOnePageIsReadWhole(t *testing.T) {
	collected, requests, err := walkJobs(t, &jobPages{supply: 250, skewedIndex: -1})
	if err != nil {
		t.Fatalf("a 250-job run failed to walk: %v", err)
	}
	if len(collected) != 250 {
		t.Fatalf("a 250-job run gave %d jobs", len(collected))
	}
	if len(requests) != 3 {
		t.Fatalf("expected three pages, got %v", requests)
	}
	for index, request := range requests {
		if request[0] != maxJobPage || request[1] != index+1 {
			t.Fatalf("page %d was requested as per_page=%d page=%d", index+1, request[0], request[1])
		}
	}
	for index, item := range collected {
		if item.Name != fmt.Sprintf("job-%d", index+1) {
			t.Fatalf("job %d of the walk is %q; the pages overlap or skip", index, item.Name)
		}
	}
}

func TestAJobPageTheAPICouldNotFillEndsTheWalk(t *testing.T) {
	collected, requests, err := walkJobs(t, &jobPages{supply: 40, skewedIndex: -1})
	if err != nil {
		t.Fatalf("a 40-job run failed to walk: %v", err)
	}
	if len(collected) != 40 || len(requests) != 1 {
		t.Fatalf("a short page did not end the walk: %d jobs, %v", len(collected), requests)
	}
}

func TestARunWhoseLastPageIsExactlyFullIsStillFinished(t *testing.T) {
	collected, requests, err := walkJobs(t, &jobPages{supply: maxJobPage, skewedIndex: -1})
	if err != nil {
		t.Fatalf("an exactly-full run failed to walk: %v", err)
	}
	if len(collected) != maxJobPage {
		t.Fatalf("an exactly-full run gave %d jobs", len(collected))
	}
	if len(requests) != 2 {
		t.Fatalf("a full last page was assumed to be the end: %v", requests)
	}
}

func TestARunWithNoJobsIsNotAFailure(t *testing.T) {
	collected, requests, err := walkJobs(t, &jobPages{supply: 0, skewedIndex: -1})
	if err != nil || len(collected) != 0 || len(requests) != 1 {
		t.Fatalf("an empty job list: %d jobs, %v, err=%v", len(collected), requests, err)
	}
}

func TestAJobListThatNeverEndsIsRefusedRatherThanTruncated(t *testing.T) {
	_, requests, err := walkJobs(t, &jobPages{supply: maxRunJobs * 2, skewedIndex: -1, alwaysFull: true})
	if err == nil {
		t.Fatal("a job list past the walk bound was accepted")
	}
	if len(requests) != maxRunJobs/maxJobPage {
		t.Fatalf("the runaway walk did not stop where it says it does: %d requests", len(requests))
	}
}

// scanServer answers both endpoints the scan uses: one completed run, and
// that run's job list from the supplied pages.
type scanServer struct {
	jobs *jobPages
}

func (s *scanServer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if strings.HasSuffix(r.URL.Path, "/jobs") {
		s.jobs.ServeHTTP(w, r)
		return
	}
	// The run is dispatched before any step of it begins, so the only
	// finding a test can produce is the one it planted.
	fmt.Fprint(w, `{"workflow_runs":[{"id":1,"name":"ci","run_started_at":"2026-07-28T05:59:00Z"}]}`)
}

// TestAFaultOnAJobPastTheFirstPageReachesTheReport is the point of the rule:
// before it, a skewed runner sitting on job 137 of a matrix was never read,
// and the scan said every step on every runner was time-ordered.
func TestAFaultOnAJobPastTheFirstPageReachesTheReport(t *testing.T) {
	server := httptest.NewTLSServer(&scanServer{jobs: &jobPages{supply: 250, skewedIndex: 136}})
	defer server.Close()
	api, err := newActionsAPIAt(server.URL, "test-token", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	var out strings.Builder
	code, err := scan(context.Background(), api, "owner/repo", 1, nil, &out)
	if err != nil {
		t.Fatalf("the scan failed: %v", err)
	}
	if code != 1 {
		t.Fatalf("a skewed runner on job 137 left the scan green: code=%d\n%s", code, out.String())
	}
	if !strings.Contains(out.String(), "runner-137") {
		t.Fatalf("the report does not name the runner that carries the fault:\n%s", out.String())
	}
	if !strings.Contains(out.String(), "250 jobs") {
		t.Fatalf("the scan counted something other than the whole run:\n%s", out.String())
	}
}

func TestACleanMatrixRunIsStillClean(t *testing.T) {
	server := httptest.NewTLSServer(&scanServer{jobs: &jobPages{supply: 250, skewedIndex: -1}})
	defer server.Close()
	api, err := newActionsAPIAt(server.URL, "test-token", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	var out strings.Builder
	code, err := scan(context.Background(), api, "owner/repo", 1, nil, &out)
	if err != nil || code != 0 {
		t.Fatalf("a clean 250-job run was not clean: code=%d err=%v\n%s", code, err, out.String())
	}
	if !strings.Contains(out.String(), "250 steps") {
		t.Fatalf("the scan read something other than every step:\n%s", out.String())
	}
}
