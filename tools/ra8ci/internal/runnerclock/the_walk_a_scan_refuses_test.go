// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"bytes"
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// A scan walks completed runs, then each run's jobs, then each job's
// steps. Everything it refuses along that walk is an exit 2 with a reason,
// because a scan that quietly reported "every step is time-ordered" after
// reading nothing would be worse than one that failed.

// scanning serves the run list and the job list from the bodies given and
// hands back the scan's exit code, its output and its error.
func scanning(t *testing.T, runsBody, jobsBody string, onRuns func()) (int, string, error) {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body := runsBody
		if strings.HasSuffix(r.URL.Path, "/jobs") {
			body = jobsBody
		} else if onRuns != nil {
			onRuns()
		}
		if body == "" {
			http.Error(w, "upstream is unwell", http.StatusBadGateway)
			return
		}
		_, _ = w.Write([]byte(body))
	}))
	t.Cleanup(server.Close)
	api, err := newActionsAPIAt(server.URL, "token", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	var stdout bytes.Buffer
	code, scanErr := scan(context.Background(), api, "owner/repo", 1, nil, &stdout)
	return code, stdout.String(), scanErr
}

const oneRun = `{"workflow_runs":[{"id":1,"name":"ci","run_started_at":"2026-07-28T06:00:00Z"}]}`

// A run list that cannot be read is a failed scan, not an empty one.
func TestAScanThatCannotReadTheRunListFails(t *testing.T) {
	code, out, err := scanning(t, "", "", nil)
	if code != 2 || err == nil {
		t.Fatalf("code %d, err %v, want the scan refused", code, err)
	}
	if out != "" {
		t.Fatalf("a failed scan reported: %q", out)
	}
}

// Nor is a run without an id something to walk: the jobs URL would be
// built from it.
func TestAScanRefusesARunWithoutAnID(t *testing.T) {
	code, _, err := scanning(t, `{"workflow_runs":[{"id":0,"name":"ci"}]}`, "{}", nil)
	if code != 2 || err == nil || !strings.Contains(err.Error(), "valid id") {
		t.Fatalf("code %d, err %v, want the run refused", code, err)
	}
}

// A job list that cannot be read fails the scan too, rather than counting
// the run as clean.
func TestAScanThatCannotReadAJobListFails(t *testing.T) {
	code, _, err := scanning(t, oneRun, "", nil)
	if code != 2 || err == nil {
		t.Fatalf("code %d, err %v, want the scan refused", code, err)
	}
}

// A scan that saw no steps at all has proved nothing, so it says so
// instead of passing. A silent pass here is exactly how a broken token or
// too narrow a window would look like a healthy fleet.
func TestAScanThatSawNoStepsRefusesToPass(t *testing.T) {
	code, out, err := scanning(t, oneRun, `{"jobs":[]}`, nil)
	if code != 2 || err == nil || !strings.Contains(err.Error(), "zero steps") {
		t.Fatalf("code %d, err %v, want the empty scan refused", code, err)
	}
	if strings.Contains(out, "time-ordered") {
		t.Fatalf("an empty scan reported a clean fleet: %q", out)
	}
}

// A caller that walks away mid-walk stops the scan at the next run rather
// than spending the rest of the run list on nobody.
func TestAScanStopsWhenTheCallerWalksAway(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		cancel()
		_, _ = w.Write([]byte(oneRun))
	}))
	defer server.Close()
	api, err := newActionsAPIAt(server.URL, "token", server.Client())
	if err != nil {
		t.Fatal(err)
	}

	var stdout bytes.Buffer
	code, scanErr := scan(ctx, api, "owner/repo", 1, nil, &stdout)
	if code != 2 || scanErr == nil {
		t.Fatalf("code %d, err %v, want the abandoned scan stopped", code, scanErr)
	}
	if stdout.Len() != 0 {
		t.Fatalf("an abandoned scan reported: %q", stdout.String())
	}
}

// GitHub can hand back a job with no runner, no name and a run with no
// workflow name. A finding still has to say where it was seen, so each
// gets a stand-in rather than an empty column.
func TestAFindingWithoutNamesStillSaysWhereItWasSeen(t *testing.T) {
	jobs := `{"jobs":[{"id":9,"name":"","runner_name":"","steps":[` +
		`{"number":1,"name":"gate","started_at":"2026-07-28T06:23:40Z","completed_at":"2026-07-28T06:21:12Z"}]}]}`
	code, out, err := scanning(t, `{"workflow_runs":[{"id":1,"name":"","run_started_at":"2026-07-28T06:00:00Z"}]}`, jobs, nil)
	if err != nil {
		t.Fatal(err)
	}
	if code != 1 {
		t.Fatalf("code %d, want the skew reported", code)
	}
	if !strings.Contains(out, "runner=(unknown runner)") || !strings.Contains(out, "? / ?") {
		t.Fatalf("a nameless finding reported as:\n%s", out)
	}
}

// The report ranks runners by how many skewed steps each has, and breaks a
// tie by name, so the same fleet always reads the same way twice running.
func TestTheReportRanksRunnersByCountThenName(t *testing.T) {
	findings := []finding{
		{runner: "win-ci-9", workflow: "ci", job: "build", step: "a", kind: "ran backwards", detail: "d"},
		{runner: "win-ci-3", workflow: "ci", job: "build", step: "b", kind: "ran backwards", detail: "d"},
		{runner: "win-ci-3", workflow: "ci", job: "build", step: "c", kind: "ran backwards", detail: "d"},
		{runner: "lin-ci-1", workflow: "ci", job: "build", step: "d", kind: "ran backwards", detail: "d"},
	}
	var stdout bytes.Buffer
	report(&stdout, findings, 2, 3, 40)

	out := stdout.String()
	ranked := out[strings.Index(out, "per runner:"):]
	if strings.Index(ranked, "win-ci-3") > strings.Index(ranked, "lin-ci-1") ||
		strings.Index(ranked, "lin-ci-1") > strings.Index(ranked, "win-ci-9") {
		t.Fatalf("runners ranked as:\n%s", ranked)
	}
	if !strings.Contains(out, "4 step(s) on 3 runner(s)") {
		t.Fatalf("the failure line read:\n%s", out)
	}
	if !strings.Contains(out, "2 completed runs, 3 jobs, 40 steps") {
		t.Fatalf("the header read:\n%s", out)
	}
}

// A clean fleet says so once, and says nothing about runners.
func TestTheReportOfACleanFleetNamesNoRunners(t *testing.T) {
	var stdout bytes.Buffer
	report(&stdout, nil, 5, 9, 120)

	out := stdout.String()
	if !strings.Contains(out, "every step on every runner is time-ordered.") {
		t.Fatalf("a clean report read:\n%s", out)
	}
	if strings.Contains(out, "per runner:") || strings.Contains(out, "FAIL") {
		t.Fatalf("a clean report carried skew sections:\n%s", out)
	}
}
