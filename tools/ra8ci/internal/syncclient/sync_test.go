package syncclient

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

func TestSyncPendingNeedsMatchingDurableReceipt(t *testing.T) {
	directory := t.TempDir()
	if err := os.Chmod(directory, 0700); err != nil {
		t.Fatal(err)
	}
	outbox, err := spool.Open(directory)
	if err != nil {
		t.Fatal(err)
	}
	started, err := outbox.BeginWithMetadata("format-check", strings.Repeat("a", 64), spool.Metadata{
		Source: spool.SourceIdentity{Repository: "bsikar/ra8-firmware", CommitSHA: strings.Repeat("b", 40),
			Verification: "unverified"}, Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 900,
	})
	if err != nil {
		t.Fatal(err)
	}
	finished, err := outbox.Finish(started, executor.Result{TaskName: "format-check", ExitCode: 0}, nil)
	if err != nil {
		t.Fatal(err)
	}
	serverID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	matching := false
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/v1/local-runs/sync" || r.Method != http.MethodPost {
			t.Errorf("unexpected request %s %s", r.Method, r.URL.Path)
			return
		}
		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Error(err)
			return
		}
		canonical, err := catalog.CanonicalJSON(body)
		if err != nil {
			t.Error(err)
			return
		}
		sum := sha256.Sum256(canonical)
		hash := hex.EncodeToString(sum[:])
		if !matching {
			hash = strings.Repeat("0", 64)
		}
		_ = json.NewEncoder(w).Encode(store.LocalRunReceipt{
			LocalRunID: serverID, LocalID: finished.ID, PayloadSHA256: hash,
		})
	}))
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if _, err := SyncPending(ctx, outbox, server.URL, server.Client()); err == nil {
		t.Fatal("mismatched receipt marked record synced")
	}
	if pending, err := outbox.Pending(); err != nil || len(pending) != 1 {
		t.Fatalf("mismatch lost pending record: %d %v", len(pending), err)
	}
	matching = true
	report, err := SyncPending(ctx, outbox, server.URL, server.Client())
	if err != nil || report.Synced != 1 || report.Quarantined != 0 {
		t.Fatalf("sync with durable receipt: %+v %v", report, err)
	}
	if pending, err := outbox.Pending(); err != nil || len(pending) != 0 {
		t.Fatalf("synced record remained pending: %d %v", len(pending), err)
	}
}

func TestSyncPendingLeavesLegacyOutboxUnverified(t *testing.T) {
	directory := t.TempDir()
	if err := os.Chmod(directory, 0700); err != nil {
		t.Fatal(err)
	}
	outbox, err := spool.Open(directory)
	if err != nil {
		t.Fatal(err)
	}
	started, err := outbox.Begin("format-check", strings.Repeat("a", 64))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := outbox.Finish(started, executor.Result{}, nil); err != nil {
		t.Fatal(err)
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		t.Error("legacy record was uploaded")
	}))
	defer server.Close()
	report, err := SyncPending(context.Background(), outbox, server.URL, server.Client())
	if err != nil || report.Synced != 0 || report.Quarantined != 1 {
		t.Fatalf("legacy report: %+v %v", report, err)
	}
	if pending, err := outbox.Pending(); err != nil || len(pending) != 1 {
		t.Fatalf("legacy record was removed: %d %v", len(pending), err)
	}
}
