// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// What a scan does with a page it can read but cannot trust: a body too
// large to hold, a run whose start stamp is unreadable, a step with no name,
// and a walk called off between one run and the next.

// A body is read under a bound and judged after it, so an endpoint cannot
// spend the tool's memory by answering at length.
func TestAResponseLargerThanTheBoundIsRefusedRatherThanHeld(t *testing.T) {
	api := servedBy(t, func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte(`{"workflow_runs":[{"id":1,"name":"` + strings.Repeat("a", maxBody) + `"}]}`))
	})

	var payload runList
	err := api.get(context.Background(), "repos/o/r/actions/runs", nil, &payload)
	if err == nil {
		t.Fatal("an oversized page was accepted")
	}
	if !strings.Contains(err.Error(), fmt.Sprintf("exceeds %d bytes", maxBody)) {
		t.Fatalf("the refusal did not name the bound: %v", err)
	}
	if len(payload.Runs) != 0 {
		t.Fatalf("an oversized page was decoded anyway: %d runs", len(payload.Runs))
	}
}

// An ordinary page of the same shape is still read, so the bound is not the
// tool refusing pages in general.
func TestAPageUnderTheBoundIsStillRead(t *testing.T) {
	api := servedBy(t, func(w http.ResponseWriter, _ *http.Request) {
		_, _ = w.Write([]byte(`{"workflow_runs":[{"id":7,"name":"workflow"}]}`))
	})

	var payload runList
	if err := api.get(context.Background(), "repos/o/r/actions/runs", nil, &payload); err != nil {
		t.Fatalf("an ordinary page was refused: %v", err)
	}
	if len(payload.Runs) != 1 || payload.Runs[0].ID != 7 {
		t.Fatalf("the page decoded as %+v", payload.Runs)
	}
}

// GitHub states two moments for a run and the first is sometimes absent or
// unreadable. The window is judged on the one that can be read rather than
// dropping a run the clock could have scanned.
func TestARunWithAnUnreadableStartIsJudgedOnWhenItWasCreated(t *testing.T) {
	recent := time.Now().UTC().Add(-time.Hour).Format(time.RFC3339)
	old := time.Now().UTC().Add(-72 * time.Hour).Format(time.RFC3339)
	window := 24

	for _, item := range []struct {
		name    string
		started string
		created string
		kept    bool
	}{
		{"an unreadable start and a recent creation", "not a timestamp", recent, true},
		{"no start at all and a recent creation", "", recent, true},
		{"an unreadable start and an old creation", "not a timestamp", old, false},
		{"neither stamp readable", "not a timestamp", "also not one", false},
		{"a readable start inside the window", recent, old, true},
	} {
		api := servedBy(t, func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Query().Get("page") != "1" {
				_, _ = w.Write([]byte(`{"workflow_runs":[]}`))
				return
			}
			fmt.Fprintf(w, `{"workflow_runs":[{"id":11,"name":"workflow","run_started_at":%q,"created_at":%q}]}`,
				item.started, item.created)
		})

		collected, err := runs(context.Background(), api, "owner/repo", 5, &window)
		if err != nil {
			t.Fatalf("%s: the walk failed: %v", item.name, err)
		}
		if kept := len(collected) == 1; kept != item.kept {
			t.Errorf("%s: the walk kept %d runs", item.name, len(collected))
		}
	}
}

// A step the runner reported without a name is still evidence about a
// clock, so it is reported under a placeholder rather than dropped or
// reported as the empty string.
func TestAStepWithNoNameIsReportedUnderAPlaceholder(t *testing.T) {
	start := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	findings := scanJob(job{
		Name:       "build",
		RunnerName: "ra8-lab-1",
		Steps: []step{{
			Number:      1,
			StartedAt:   start.Format(time.RFC3339),
			CompletedAt: start.Add(-time.Minute).Format(time.RFC3339),
		}},
	})

	if len(findings) != 1 {
		t.Fatalf("an unnamed step out of order gave %d findings", len(findings))
	}
	if findings[0].step != "?" {
		t.Fatalf("the unnamed step was reported as %q", findings[0].step)
	}
	if findings[0].kind != "finished-before-it-started" {
		t.Fatalf("the finding was %q", findings[0].kind)
	}
}

// A walk called off part way through stops at the next run rather than
// reading the jobs of runs nobody is waiting for any more.
func TestAScanCalledOffBetweenRunsStopsThere(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	jobsAsked := false
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.Contains(r.URL.Path, "/jobs") {
			jobsAsked = true
			_, _ = w.Write([]byte(`{"jobs":[]}`))
			return
		}
		if r.URL.Query().Get("page") == "1" {
			started := time.Now().UTC().Format(time.RFC3339)
			fmt.Fprintf(w, `{"workflow_runs":[{"id":21,"name":"workflow","run_started_at":%q}]}`, started)
			// The list is in hand; everything after this point is work the
			// caller has already said it no longer wants.
			cancel()
			return
		}
		_, _ = w.Write([]byte(`{"workflow_runs":[]}`))
	}))
	defer server.Close()

	api, err := newActionsAPIAt(server.URL, "test-token", server.Client())
	if err != nil {
		t.Fatal(err)
	}

	var stdout strings.Builder
	code, err := scan(ctx, api, "owner/repo", 1, nil, &stdout)
	if code != 2 || err == nil {
		t.Fatalf("a called-off scan answered code %d, err %v", code, err)
	}
	if jobsAsked {
		t.Error("a called-off scan read a run's jobs anyway")
	}
}

// A step with one stamp is judged on the one it has: a completion with no
// start is still a moment the runner's clock reported.
func TestAStepStatingOnlyWhenItFinishedIsJudgedOnThatStamp(t *testing.T) {
	end := time.Date(2026, 9, 26, 12, 30, 0, 0, time.UTC)

	stamp, ok := lastStampOf(step{Name: "upload", CompletedAt: end.Format(time.RFC3339)})
	if !ok || !stamp.Equal(end) {
		t.Fatalf("a completion-only step gave %s (%t)", stamp, ok)
	}

	stamp, ok = lastStampOf(step{Name: "checkout", StartedAt: end.Format(time.RFC3339)})
	if !ok || !stamp.Equal(end) {
		t.Fatalf("a start-only step gave %s (%t)", stamp, ok)
	}

	if _, ok := lastStampOf(step{Name: "skipped"}); ok {
		t.Fatal("a step with neither stamp was taken as evidence about a clock")
	}
}
