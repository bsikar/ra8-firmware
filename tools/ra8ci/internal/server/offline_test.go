package server

import (
	"crypto/sha256"
	"encoding/hex"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

func offlineTestEntry(t *testing.T) (spool.Entry, *catalog.Catalog) {
	t.Helper()
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	definition, ok := cat.Task("format-check")
	if !ok {
		t.Fatal("missing reviewed task")
	}
	started := time.Now().UTC().Add(-2 * time.Second)
	finished := started.Add(time.Second)
	empty := sha256.Sum256(nil)
	result := executor.Result{TaskName: definition.Name, StartedAt: started.Add(time.Millisecond),
		EndedAt: finished.Add(-time.Millisecond), ExitCode: 0,
		Steps: []executor.StepResult{{Name: definition.Steps[0].Name,
			StartedAt: started.Add(2 * time.Millisecond), EndedAt: finished.Add(-2 * time.Millisecond),
			Duration: 996 * time.Millisecond, ExitCode: 0,
			StdoutSHA256: hex.EncodeToString(empty[:]), StderrSHA256: hex.EncodeToString(empty[:])}},
	}
	return spool.Entry{
		SchemaVersion: 2, ID: strings.Repeat("a", 32), Task: definition.Name,
		CatalogDigest: cat.Digest(), Source: spool.SourceIdentity{
			Repository: "bsikar/ra8-firmware", Branch: "offline-test",
			CommitSHA: strings.Repeat("b", 40), Verification: "unverified",
		}, Tier: definition.Tier, Scope: definition.Scope,
		DeadlineSeconds: definition.DeadlineSeconds, StartedAt: started,
		FinishedAt: &finished, Result: &result, SyncState: "unsynced",
	}, cat
}

func TestOfflineInputQuarantinesLegacyAndPreservesUnverifiedSource(t *testing.T) {
	entry, cat := offlineTestEntry(t)
	in, err := offlineInput(entry, cat)
	if err != nil || in.SourceVerification != "unverified" || in.SnapshotSHA256 != "" || in.Result != "succeeded" || len(in.Steps) != 1 {
		t.Fatalf("offline mapping: %+v %v", in, err)
	}
	entry.SchemaVersion = 1
	if _, err := offlineInput(entry, cat); err == nil {
		t.Fatal("legacy record with no source metadata was accepted")
	}
	entry.SchemaVersion = 2
	entry.Result.Steps = nil
	if incomplete, err := offlineInput(entry, cat); err != nil || incomplete.Result != "incomplete_evidence" {
		t.Fatalf("missing successful step was not downgraded: %+v %v", incomplete, err)
	}
	entry, _ = offlineTestEntry(t)
	entry.Source.SnapshotSHA256 = strings.Repeat("c", 64)
	if _, err := offlineInput(entry, cat); err == nil {
		t.Fatal("unverified source claimed a snapshot digest")
	}
	entry.Source.SnapshotSHA256 = ""
	entry.Tier = "optional"
	if _, err := offlineInput(entry, cat); err == nil {
		t.Fatal("task metadata was not matched to reviewed catalog")
	}
}
