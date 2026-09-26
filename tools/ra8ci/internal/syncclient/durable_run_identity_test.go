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

func TestASecondReceiptForOneDurableRunIsRefused(t *testing.T) {
	claimed := map[string]string{}
	first := store.LocalRunReceipt{LocalRunID: "019235f1-0a2b-7c3d-9e4f-5a6b7c8d9e0f", LocalID: "aaaa"}
	if err := checkDurableRunIsUnclaimed(claimed, "aaaa", first); err != nil {
		t.Fatalf("first receipt refused: %v", err)
	}
	second := store.LocalRunReceipt{LocalRunID: first.LocalRunID, LocalID: "bbbb"}
	err := checkDurableRunIsUnclaimed(claimed, "bbbb", second)
	if err == nil {
		t.Fatal("two local records took one durable run")
	}
	if !strings.Contains(err.Error(), first.LocalRunID) || !strings.Contains(err.Error(), "aaaa") {
		t.Fatalf("refusal did not name the run and the earlier record: %v", err)
	}
}

func TestDistinctDurableRunsAreAllClaimed(t *testing.T) {
	claimed := map[string]string{}
	ids := []string{
		"019235f1-0a2b-7c3d-9e4f-5a6b7c8d9e0f",
		"019235f1-0a2b-7c3d-9e4f-5a6b7c8d9e10",
		"019235f1-0a2b-7c3d-9e4f-5a6b7c8d9e11",
	}
	for i, id := range ids {
		if err := checkDurableRunIsUnclaimed(claimed, string(rune('a'+i)), store.LocalRunReceipt{LocalRunID: id}); err != nil {
			t.Fatalf("distinct run %s refused: %v", id, err)
		}
	}
	if len(claimed) != len(ids) {
		t.Fatalf("claimed %d runs, wanted %d", len(claimed), len(ids))
	}
}

// The rule is scoped to one sweep. A nil map is the shape a caller that is not
// tracking passes, and it must not start refusing on its own.
func TestAnAbsentClaimSetRefusesNothing(t *testing.T) {
	receipt := store.LocalRunReceipt{LocalRunID: "019235f1-0a2b-7c3d-9e4f-5a6b7c8d9e0f"}
	if err := checkDurableRunIsUnclaimed(nil, "aaaa", receipt); err != nil {
		t.Fatalf("nil claim set refused a receipt: %v", err)
	}
	if err := checkDurableRunIsUnclaimed(nil, "bbbb", receipt); err != nil {
		t.Fatalf("nil claim set refused a repeat: %v", err)
	}
}

// The second record must still be pending afterwards: the refusal happens
// before MarkSynced, so nothing durable records the collision.
func TestOneDurableRunForTwoRecordsStopsTheSweepBeforeTheSecondMarker(t *testing.T) {
	directory := t.TempDir()
	if err := os.Chmod(directory, 0700); err != nil {
		t.Fatal(err)
	}
	outbox, err := spool.Open(directory)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		started, err := outbox.BeginWithMetadata("format-check", strings.Repeat("a", 64), spool.Metadata{
			Source: spool.SourceIdentity{Repository: "bsikar/ra8-firmware", CommitSHA: strings.Repeat("b", 40),
				Verification: "unverified"}, Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 900,
		})
		if err != nil {
			t.Fatal(err)
		}
		if _, err := outbox.Finish(started, executor.Result{TaskName: "format-check", ExitCode: 0}, nil); err != nil {
			t.Fatal(err)
		}
	}
	serverID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
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
		var entry spool.Entry
		if err := json.Unmarshal(body, &entry); err != nil {
			t.Error(err)
			return
		}
		sum := sha256.Sum256(canonical)
		_ = json.NewEncoder(w).Encode(store.LocalRunReceipt{
			LocalRunID: serverID, LocalID: entry.ID, PayloadSHA256: hex.EncodeToString(sum[:]),
		})
	}))
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	report, err := SyncPending(ctx, outbox, server.URL, server.Client())
	if err == nil {
		t.Fatal("one durable run receipted two local records")
	}
	if !strings.Contains(err.Error(), serverID) {
		t.Fatalf("refusal did not name the durable run: %v", err)
	}
	if report.Synced != 1 {
		t.Fatalf("synced %d records, wanted the first only", report.Synced)
	}
	pending, err := outbox.Pending()
	if err != nil {
		t.Fatal(err)
	}
	if len(pending) != 1 {
		t.Fatalf("pending %d records after the refusal, wanted the second only", len(pending))
	}
}
