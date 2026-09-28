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

// The record-only doors all sit inside one sweep, and each has the same two
// obligations: stop the sweep rather than send a record the plane will refuse
// with an opaque 400, and name the local record so an operator knows which
// file in the outbox to look at. The doors themselves are held one by one
// elsewhere; what is held here is that SyncPending actually asks them, in
// front of the network, and leaves the evidence where it was.

// plantOwn writes a terminal record the spool's own API would not produce,
// without forcing the fields the door under test needs to be wrong. Pending
// reads the pair, so the start and terminal records are written and agree.
func plantOwn(t *testing.T, directory string, entry spool.Entry) {
	t.Helper()
	finished := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	entry.SchemaVersion = uploadableSchemaVersion
	entry.StartedAt = finished.Add(-time.Minute)
	started := entry
	started.SyncState = "running"
	started.FinishedAt = nil
	started.Result = nil
	entry.SyncState = "unsynced"
	entry.FinishedAt = &finished
	if entry.Result == nil {
		entry.Result = &executor.Result{TaskName: entry.Task, ExitCode: 0}
	}
	writeRecord(t, filepath.Join(directory, entry.ID+".started.json"), started)
	writeRecord(t, filepath.Join(directory, entry.ID+".finished.json"), entry)
}

// filable is a record every door accepts, the control the broken ones are
// built from.
func filable() spool.Entry {
	entry := verifiedEntry()
	entry.CatalogDigest = strings.Repeat("a", 64)
	entry.Tier = "required"
	entry.Scope = "safe-local-read-only"
	entry.DeadlineSeconds = 900
	return entry
}

func TestEveryRecordOnlyDoorStopsTheSweepBeforeTheNetwork(t *testing.T) {
	for _, refused := range []struct {
		name   string
		door   error
		broken func(spool.Entry) spool.Entry
	}{
		{"no source identity", ErrUnstatedSourceIdentity, func(e spool.Entry) spool.Entry {
			e.Source.Repository = ""
			return e
		}},
		{"a result naming another task", ErrResultNamesAnotherTask, func(e spool.Entry) spool.Entry {
			e.Result = &executor.Result{TaskName: "some-other-task", ExitCode: 0}
			return e
		}},
		{"an exit no runner could report", ErrUnreportableExit, func(e spool.Entry) spool.Entry {
			e.Result = &executor.Result{TaskName: e.Task, ExitCode: -3}
			return e
		}},
		{"no catalog digest", ErrUnstatedCatalogDigest, func(e spool.Entry) spool.Entry {
			e.CatalogDigest = "not-a-digest"
			return e
		}},
		{"a classification the plane cannot file", ErrUnfilableClassification, func(e spool.Entry) spool.Entry {
			e.Tier = "whenever"
			return e
		}},
		{"a task name the plane will not file", ErrUnfilableTaskName, func(e spool.Entry) spool.Entry {
			e.Task = " clang-format"
			e.Result = &executor.Result{TaskName: " clang-format", ExitCode: 0}
			return e
		}},
		{"arguments the plane will not file", ErrUnfilableArguments, func(e spool.Entry) spool.Entry {
			e.Args = []string{"--mode=\x00check"}
			return e
		}},
	} {
		outbox, directory := openOutbox(t)
		plantOwn(t, directory, refused.broken(filable()))

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
		if !strings.Contains(err.Error(), "local "+filable().ID) {
			t.Errorf("a record with %s was refused without being named: %v", refused.name, err)
		}
		if report.Synced != 0 || report.Quarantined != 0 {
			t.Errorf("a record with %s answered %+v", refused.name, report)
		}
		// The evidence stays where it was: a refusal is a reason to look,
		// never a reason to drop the only copy of a finished run.
		if pending, err := outbox.Pending(); err != nil || len(pending) != 1 {
			t.Errorf("a record with %s left %d pending (%v)", refused.name, len(pending), err)
		}
	}
}

// A sound record still goes out beside those refusals, so the doors are not
// simply refusing everything.
func TestASoundRecordStillReachesThePlane(t *testing.T) {
	outbox, directory := openOutbox(t)
	plantOwn(t, directory, filable())

	asked := 0
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		asked++
		w.WriteHeader(http.StatusInternalServerError)
	}))
	defer server.Close()

	if _, err := SyncPending(context.Background(), outbox, server.URL, server.Client()); err == nil {
		t.Fatal("a refusing plane answered a clean sync")
	}
	if asked != 1 {
		t.Fatalf("a sound record reached the plane %d times", asked)
	}
}
