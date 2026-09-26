// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

func versioned(version int) spool.Entry {
	entry := verifiedEntry()
	entry.SchemaVersion = version
	return entry
}

func TestOnlyTheTwoVersionsTheSpoolWritesAreKnown(t *testing.T) {
	for _, version := range []int{legacySchemaVersion, uploadableSchemaVersion} {
		if err := checkSchemaVersionIsKnown(versioned(version)); err != nil {
			t.Fatalf("version %d refused: %v", version, err)
		}
	}
	for _, version := range []int{-7, -1, 0, 3, 4, 5, 12, 99, 1 << 20} {
		err := checkSchemaVersionIsKnown(versioned(version))
		if !errors.Is(err, ErrUnknownSchemaVersion) {
			t.Fatalf("version %d was accepted: %v", version, err)
		}
		if !strings.Contains(err.Error(), "schema version") {
			t.Fatalf("version %d refusal did not name the version: %v", version, err)
		}
	}
}

// The spool is the only writer of these records, so the set of versions this
// rule knows has to be the set the spool can produce. This test asks the spool
// itself rather than reading the constants a second time: a repository names a
// v2 record and its absence names a v1 one, and nothing else is reachable.
func TestTheKnownVersionsAreTheOnesTheSpoolCanWrite(t *testing.T) {
	outbox, _ := openOutbox(t)
	legacy, err := outbox.Begin("format-check", strings.Repeat("a", 64))
	if err != nil {
		t.Fatal(err)
	}
	stated, err := outbox.BeginWithMetadata("format-check", strings.Repeat("a", 64), spool.Metadata{
		Source: spool.SourceIdentity{Repository: "bsikar/ra8-firmware", CommitSHA: strings.Repeat("b", 40),
			Verification: "unverified"}, Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 900,
	})
	if err != nil {
		t.Fatal(err)
	}
	if legacy.SchemaVersion != legacySchemaVersion || stated.SchemaVersion != uploadableSchemaVersion {
		t.Fatalf("spool wrote versions %d and %d", legacy.SchemaVersion, stated.SchemaVersion)
	}
	for _, entry := range []spool.Entry{legacy, stated} {
		if err := checkSchemaVersionIsKnown(entry); err != nil {
			t.Fatalf("spool-written version %d refused: %v", entry.SchemaVersion, err)
		}
	}
}

// One version apart is the whole difference between a record this sweep leaves
// alone and one it refuses, so the verdict may not turn on anything else.
func TestOnlyTheVersionDecidesThisRule(t *testing.T) {
	base := versioned(uploadableSchemaVersion)
	if err := checkSchemaVersionIsKnown(base); err != nil {
		t.Fatalf("base entry refused: %v", err)
	}
	base.SchemaVersion = uploadableSchemaVersion + 1
	if !errors.Is(checkSchemaVersionIsKnown(base), ErrUnknownSchemaVersion) {
		t.Fatal("one version past the uploadable shape was accepted")
	}
	base.SchemaVersion = legacySchemaVersion - 1
	if !errors.Is(checkSchemaVersionIsKnown(base), ErrUnknownSchemaVersion) {
		t.Fatal("one version below the legacy shape was accepted")
	}
	for _, mutate := range []func(*spool.Entry){
		func(e *spool.Entry) { e.Source.Repository = "" },
		func(e *spool.Entry) { e.Source.CommitSHA = "nope" },
		func(e *spool.Entry) { e.Source.Verification = "trusted" },
		func(e *spool.Entry) { e.Source.SnapshotSHA256 = "" },
		func(e *spool.Entry) { e.ID = "" },
		func(e *spool.Entry) { e.Task = "" },
		func(e *spool.Entry) { e.SyncState = "running" },
	} {
		entry := versioned(uploadableSchemaVersion)
		mutate(&entry)
		if err := checkSchemaVersionIsKnown(entry); err != nil {
			t.Fatalf("a field that is not the version decided this rule: %v", err)
		}
	}
}

// A record this build cannot read may not be judged by a door that assumes it
// can, so the version is answered before the source identity is examined.
func TestTheVersionIsAnsweredBeforeTheIdentityIs(t *testing.T) {
	entry := versioned(9)
	entry.Source.Repository = ""
	entry.Source.CommitSHA = ""
	outbox, directory := openOutbox(t)
	plant(t, directory, entry)
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		t.Error("a record of an unknown version was uploaded")
	}))
	defer server.Close()
	_, err := SyncPending(context.Background(), outbox, server.URL, server.Client())
	if !errors.Is(err, ErrUnknownSchemaVersion) {
		t.Fatalf("sweep did not refuse on the version: %v", err)
	}
	if errors.Is(err, ErrUnstatedSourceIdentity) {
		t.Fatal("an unreadable record was judged by the identity door")
	}
}

// The refusal has to stop the sweep rather than leave the record counted as
// legacy, and it has to name which record it was.
func TestAnUnknownVersionStopsTheSweepAndNamesTheRecord(t *testing.T) {
	outbox, directory := openOutbox(t)
	entry := versioned(3)
	plant(t, directory, entry)
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		t.Error("a record of an unknown version was uploaded")
	}))
	defer server.Close()
	report, err := SyncPending(context.Background(), outbox, server.URL, server.Client())
	if !errors.Is(err, ErrUnknownSchemaVersion) {
		t.Fatalf("sweep continued past an unknown version: %v", err)
	}
	if !strings.Contains(err.Error(), entry.ID) {
		t.Fatalf("refusal did not name the record: %v", err)
	}
	if report.Quarantined != 0 || report.Synced != 0 {
		t.Fatalf("unknown version was counted: %+v", report)
	}
	pending, err := outbox.Pending()
	if err != nil || len(pending) != 1 {
		t.Fatalf("refused record did not stay pending: %d %v", len(pending), err)
	}
}

// The pile the Quarantined count describes is version 1 and nothing else, so a
// legacy record still passes this rule and is still counted, on the same sweep
// that refuses an unreadable one.
func TestLegacyIsStillQuarantinedAndTheUnknownIsNot(t *testing.T) {
	outbox, directory := openOutbox(t)
	legacy, err := outbox.Begin("format-check", strings.Repeat("a", 64))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := outbox.Finish(legacy, executor.Result{}, nil); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		t.Error("a quarantined record was uploaded")
	}))
	defer server.Close()
	report, err := SyncPending(context.Background(), outbox, server.URL, server.Client())
	if err != nil || report.Quarantined != 1 || report.Synced != 0 {
		t.Fatalf("legacy sweep: %+v %v", report, err)
	}
	plant(t, directory, versioned(4))
	report, err = SyncPending(context.Background(), outbox, server.URL, server.Client())
	if !errors.Is(err, ErrUnknownSchemaVersion) {
		t.Fatalf("mixed sweep did not refuse: %v", err)
	}
	if report.Synced != 0 {
		t.Fatalf("mixed sweep uploaded something: %+v", report)
	}
	pending, err := outbox.Pending()
	if err != nil || len(pending) != 2 {
		t.Fatalf("mixed sweep lost a record: %d %v", len(pending), err)
	}
}

// A skip is permanent: Pending returns a record until a synced marker sits
// beside it, so the old behaviour re-read and re-counted the same unreadable
// record on every pass. This pins that the new answer is stable instead.
func TestTheRefusalIsTheSameOnEveryPass(t *testing.T) {
	outbox, directory := openOutbox(t)
	plant(t, directory, versioned(7))
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		t.Error("a record of an unknown version was uploaded")
	}))
	defer server.Close()
	for pass := 0; pass < 3; pass++ {
		report, err := SyncPending(context.Background(), outbox, server.URL, server.Client())
		if !errors.Is(err, ErrUnknownSchemaVersion) || report.Quarantined != 0 {
			t.Fatalf("pass %d: %+v %v", pass, report, err)
		}
	}
}

func openOutbox(t *testing.T) (*spool.Spool, string) {
	t.Helper()
	directory := t.TempDir()
	if err := os.Chmod(directory, 0700); err != nil {
		t.Fatal(err)
	}
	outbox, err := spool.Open(directory)
	if err != nil {
		t.Fatal(err)
	}
	return outbox, directory
}

// plant writes a terminal record the spool's own API cannot produce, which is
// exactly the case this rule exists for: a file in the outbox whose version
// did not come from this build. Pending reads the pair, so both the start and
// the terminal record are written and they agree field for field.
func plant(t *testing.T, directory string, entry spool.Entry) {
	t.Helper()
	finished := time.Date(2026, 9, 26, 12, 0, 0, 0, time.UTC)
	entry.StartedAt = finished.Add(-time.Minute)
	entry.Task = "format-check"
	entry.CatalogDigest = strings.Repeat("a", 64)
	entry.Tier = "required"
	entry.Scope = "safe-local-read-only"
	entry.DeadlineSeconds = 900
	started := entry
	started.SyncState = "running"
	started.FinishedAt = nil
	started.Result = nil
	entry.SyncState = "unsynced"
	entry.FinishedAt = &finished
	entry.Result = &executor.Result{TaskName: "format-check", ExitCode: 0}
	writeRecord(t, filepath.Join(directory, entry.ID+".started.json"), started)
	writeRecord(t, filepath.Join(directory, entry.ID+".finished.json"), entry)
}

func writeRecord(t *testing.T, path string, value any) {
	t.Helper()
	raw, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, append(raw, '\n'), 0600); err != nil {
		t.Fatal(err)
	}
}
