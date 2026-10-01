// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package runnerclock

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func TestScanJobOrderingRules(t *testing.T) {
	tests := []struct {
		name string
		job  job
		want int
	}{
		{"healthy", job{Steps: []step{{Name: "setup", StartedAt: "2026-07-28T06:00:00Z", CompletedAt: "2026-07-28T06:00:04Z"}, {Name: "checkout", StartedAt: "2026-07-28T06:00:04Z", CompletedAt: "2026-07-28T06:00:09Z"}}}, 0},
		{"finished before start", job{Steps: []step{{Name: "gate", StartedAt: "2026-07-28T06:23:40Z", CompletedAt: "2026-07-28T06:21:12Z"}}}, 1},
		{"overlap beyond tolerance", job{Steps: []step{{Name: "a", StartedAt: "2026-07-28T06:00:00Z", CompletedAt: "2026-07-28T06:05:00Z"}, {Name: "b", StartedAt: "2026-07-28T06:04:54Z", CompletedAt: "2026-07-28T06:06:00Z"}}}, 1},
		{"five seconds rounding", job{Steps: []step{{Name: "a", StartedAt: "2026-07-28T06:00:00Z", CompletedAt: "2026-07-28T06:00:05Z"}, {Name: "b", StartedAt: "2026-07-28T06:00:00Z", CompletedAt: "2026-07-28T06:00:09Z"}}}, 0},
		{"skipped step", job{Steps: []step{{Name: "skip"}}}, 0},
		{"malformed timestamps are absent", job{Steps: []step{{Name: "bad", StartedAt: "not a timestamp", CompletedAt: "also bad"}}}, 0},
		{"no steps", job{}, 0},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := len(scanJob(test.job)); got != test.want {
				t.Fatalf("scanJob findings = %d, want %d", got, test.want)
			}
		})
	}
}

func TestParseTimestampAcceptsOffsetsAndFractions(t *testing.T) {
	got, ok := parseTimestamp("2026-07-28T06:00:00.125-04:00")
	if !ok || got.UTC().Format(time.RFC3339Nano) != "2026-07-28T10:00:00.125Z" {
		t.Fatalf("timestamp = %v, %t", got, ok)
	}
	if _, ok := parseTimestamp("invalid"); ok {
		t.Fatal("invalid timestamp parsed")
	}
}

func TestSelfTestAndCIRunLimit(t *testing.T) {
	var output bytes.Buffer
	if !selfTest(&output) || !strings.Contains(output.String(), "7/7 cases") {
		t.Fatalf("selftest failed: %s", output.String())
	}
	for _, test := range []struct {
		raw  string
		want int
		bad  bool
	}{{"", 60, false}, {"12", 12, false}, {"0", 0, true}, {"abc", 0, true}} {
		got, err := ciRunLimit(test.raw)
		if (err != nil) != test.bad || got != test.want {
			t.Fatalf("ciRunLimit(%q) = %d, %v", test.raw, got, err)
		}
	}
}

func TestActionsAPISendsScopedHeadersAndScans(t *testing.T) {
	var requests atomic.Int32
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests.Add(1)
		if r.Header.Get("Authorization") != "Bearer test-token" ||
			r.Header.Get("Accept") != "application/vnd.github+json" ||
			r.Header.Get("X-GitHub-Api-Version") != apiVersion ||
			r.Header.Get("User-Agent") != "ra8-firmware-runner-clock" {
			t.Errorf("unexpected API headers: %#v", r.Header)
		}
		switch r.URL.Path {
		case "/repos/owner/repo/actions/runs":
			if r.URL.Query().Get("status") != "completed" || r.URL.Query().Get("per_page") != "3" {
				t.Errorf("unexpected run query: %s", r.URL.RawQuery)
			}
			_, _ = fmt.Fprint(w, `{"workflow_runs":[{"id":17,"name":"workflow","run_started_at":"2026-07-28T06:00:00Z"}]}`)
		case "/repos/owner/repo/actions/runs/17/jobs":
			if r.URL.Query().Get("per_page") != "100" {
				t.Errorf("unexpected jobs query: %s", r.URL.RawQuery)
			}
			_, _ = fmt.Fprint(w, `{"jobs":[{"name":"job","runner_name":"linux-1","steps":[{"name":"setup","started_at":"2026-07-28T06:00:00Z","completed_at":"2026-07-28T06:00:04Z"}]}]}`)
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	api, err := newActionsAPIAt(server.URL, "test-token", server.Client())
	if err != nil {
		t.Fatal(err)
	}
	var output, stderr bytes.Buffer
	code, err := scan(context.Background(), api, "owner/repo", 3, nil, &output)
	if err != nil || code != 0 || requests.Load() != 2 {
		t.Fatalf("scan = %d, %v, requests=%d output=%q stderr=%q", code, err, requests.Load(), output.String(), stderr.String())
	}
	if !strings.Contains(output.String(), "1 completed runs, 1 jobs, 1 steps") ||
		!strings.Contains(output.String(), "every step on every runner is time-ordered") {
		t.Fatalf("unexpected clean report: %q", output.String())
	}
}

func TestActionsAPIRejectsCrossOriginRedirect(t *testing.T) {
	targetCalls := atomic.Int32{}
	target := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		targetCalls.Add(1)
	}))
	defer target.Close()
	source := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, target.URL+"/stolen", http.StatusFound)
	}))
	defer source.Close()
	api, err := newActionsAPIAt(source.URL, "sensitive-token", source.Client())
	if err != nil {
		t.Fatal(err)
	}
	var payload map[string]any
	err = api.get(context.Background(), "repos/owner/repo/actions/runs", url.Values{}, &payload)
	if err == nil || !strings.Contains(err.Error(), "cross-origin") || targetCalls.Load() != 0 {
		t.Fatalf("redirect result = %v, target calls=%d", err, targetCalls.Load())
	}
}

func TestRunArgumentAndRepoValidation(t *testing.T) {
	for _, args := range [][]string{{"--runs", "0"}, {"--repo", "../attacker"}, {"--ci-scan", "--runs", "5"}} {
		var output, stderr bytes.Buffer
		if code := Run(context.Background(), args, &output, &stderr); code != 2 {
			t.Fatalf("Run(%q) = %d, output=%q stderr=%q", args, code, output.String(), stderr.String())
		}
	}
	var output, stderr bytes.Buffer
	if code := Run(context.Background(), []string{"--selftest"}, &output, &stderr); code != 0 {
		t.Fatalf("selftest exit = %d, output=%q stderr=%q", code, output.String(), stderr.String())
	}
}

func TestRunListDecodesGitHubPayload(t *testing.T) {
	var payload runList
	if err := json.Unmarshal([]byte(`{"workflow_runs":[{"id":1,"run_started_at":"2026-07-28T06:00:00Z"}]}`), &payload); err != nil {
		t.Fatal(err)
	}
	if len(payload.Runs) != 1 || payload.Runs[0].ID != 1 {
		t.Fatalf("decoded runs = %+v", payload.Runs)
	}
}
