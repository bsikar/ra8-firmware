// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"

	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// The doors held here are the ones that judge a record's WINDOWS and its
// steps, which the sweep asks after the doors pinned beside them. Each is
// held on its own elsewhere; what is held here is that the sweep asks it in
// front of the network, and the invocation it refuses before it asks
// anything at all.

var recordStart = time.Date(2026, 9, 26, 11, 59, 0, 0, time.UTC)
var recordEnd = recordStart.Add(time.Minute)

// plantRaw writes the started/terminal pair with the caller's own stamps, so
// a record can be wrong about exactly one window.
func plantRaw(t *testing.T, directory string, entry spool.Entry) {
	t.Helper()
	entry.SchemaVersion = uploadableSchemaVersion
	started := entry
	started.SyncState = "running"
	started.FinishedAt = nil
	started.Result = nil
	entry.SyncState = "unsynced"
	writeRecord(t, filepath.Join(directory, entry.ID+".started.json"), started)
	writeRecord(t, filepath.Join(directory, entry.ID+".finished.json"), entry)
}

func soundStep(name string) executor.StepResult {
	return executor.StepResult{
		Name:         name,
		StartedAt:    recordStart,
		EndedAt:      recordStart.Add(30 * time.Second),
		Duration:     30 * time.Second,
		StdoutSHA256: strings.Repeat("a1b2c3d4", 8),
		StderrSHA256: strings.Repeat("d4c3b2a1", 8),
	}
}

// measuredRecord is a record every window and step door accepts.
func measuredRecord() spool.Entry {
	entry := filable()
	entry.StartedAt = recordStart
	end := recordEnd
	entry.FinishedAt = &end
	entry.Result = &executor.Result{
		TaskName:  entry.Task,
		StartedAt: recordStart,
		EndedAt:   recordEnd,
		Duration:  time.Minute,
		Steps:     []executor.StepResult{soundStep("build")},
	}
	return entry
}

// The step-window and attempt-window doors are held by their own tests
// rather than through the sweep: the spool's own reader refuses a record
// whose stamps are out of order, so such a record can never reach Pending
// and the sweep's copy of those doors is a second line behind the first.
func TestTheWindowAndStepDoorsStopTheSweepBeforeTheNetwork(t *testing.T) {
	for _, refused := range []struct {
		name   string
		door   error
		broken func(spool.Entry) spool.Entry
	}{
		{"an envelope wider than the door reads", ErrUnreadableEnvelope, func(e spool.Entry) spool.Entry {
			e.StartedAt = recordEnd.Add(-26 * time.Hour)
			return e
		}},
		{"log evidence no run measured", ErrUnmeasuredLogEvidence, func(e spool.Entry) spool.Entry {
			e.Result.Steps[0].StdoutSHA256 = "beef"
			return e
		}},
		{"two steps of one name", ErrUnfilableSteps, func(e spool.Entry) spool.Entry {
			e.Result.Steps = append(e.Result.Steps, soundStep("build"))
			return e
		}},
		{"a step that ended two ways at once", ErrContradictoryEnding, func(e spool.Entry) spool.Entry {
			e.Result.Steps[0].TimedOut = true
			e.Result.Steps[0].Cancelled = true
			return e
		}},
		{"a source name no column holds", ErrUnfilableSource, func(e spool.Entry) spool.Entry {
			e.Source.Repository = "bsikar/" + strings.Repeat("r", 510)
			return e
		}},
	} {
		outbox, directory := openOutbox(t)
		plantRaw(t, directory, refused.broken(measuredRecord()))

		asked := false
		server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
			asked = true
		}))

		report, err := SyncPending(context.Background(), outbox, server.URL, server.Client())
		server.Close()

		if !errors.Is(err, refused.door) {
			t.Errorf("a record with %s answered %v", refused.name, err)
			continue
		}
		if asked {
			t.Errorf("a record with %s was sent to the plane anyway", refused.name)
		}
		if !strings.Contains(err.Error(), "local "+measuredRecord().ID) {
			t.Errorf("a record with %s was refused without being named: %v", refused.name, err)
		}
		if report.Synced != 0 {
			t.Errorf("a record with %s answered %+v", refused.name, report)
		}
	}
}

func TestASweepWithoutAnOutboxOrAClientIsRefused(t *testing.T) {
	outbox, _ := openOutbox(t)
	for name, sweep := range map[string]func() (Report, error){
		"no outbox": func() (Report, error) {
			return SyncPending(context.Background(), nil, "https://plane.example", http.DefaultClient)
		},
		"no client": func() (Report, error) {
			return SyncPending(context.Background(), outbox, "https://plane.example", nil)
		},
	} {
		report, err := sweep()
		if err == nil {
			t.Fatalf("a sweep with %s was accepted", name)
		}
		if !strings.Contains(err.Error(), "outbox and HTTP client") {
			t.Errorf("a sweep with %s was refused without saying what it needs: %v", name, err)
		}
		if report != (Report{}) {
			t.Errorf("a sweep with %s answered %+v", name, report)
		}
	}
}

// The origin is judged before the outbox is read, so a record is never
// marshalled toward a server the sweep would not send it to.
func TestAnOriginTheSweepWillNotSendAnOutboxToIsRefused(t *testing.T) {
	outbox, directory := openOutbox(t)
	plantRaw(t, directory, measuredRecord())

	for _, origin := range []string{
		"",
		"plane.example",
		"http://plane.example",
		"https://",
		"https://user@plane.example",
		"https://plane.example/sync",
		"https://plane.example?tenant=2",
		"https://plane.example#now",
	} {
		report, err := SyncPending(context.Background(), outbox, origin, http.DefaultClient)
		if err == nil {
			t.Errorf("origin %q was accepted", origin)
			continue
		}
		if !strings.Contains(err.Error(), "HTTPS origin") {
			t.Errorf("origin %q was refused without naming the shape: %v", origin, err)
		}
		if report.Synced != 0 || report.Quarantined != 0 {
			t.Errorf("origin %q answered %+v", origin, report)
		}
	}
}

// A redirect is an answer, not an instruction: the sweep reports the status
// rather than carrying the outbox to whatever origin the header names.
func TestARedirectIsReportedRatherThanFollowed(t *testing.T) {
	elsewhere := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		t.Error("the outbox was carried to the redirect target")
	}))
	defer elsewhere.Close()

	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, elsewhere.URL+SyncPath, http.StatusFound)
	}))
	defer server.Close()

	outbox, directory := openOutbox(t)
	plantRaw(t, directory, measuredRecord())

	report, err := SyncPending(context.Background(), outbox, server.URL, server.Client())
	if err == nil {
		t.Fatal("a redirecting plane answered a clean sweep")
	}
	if !strings.Contains(err.Error(), "HTTP 302") {
		t.Errorf("the redirect was not reported as its status: %v", err)
	}
	if report.Synced != 0 {
		t.Errorf("a redirected record was counted as synced: %+v", report)
	}
}
