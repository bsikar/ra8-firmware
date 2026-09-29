// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// A sweep runs on a host that has already done the work, so the one thing it
// must never do is lose evidence to an answer it could not read. Every
// failure below leaves the record pending, to be sent again.

// anOutboxHoldingOneRun is a spool with a single finished record waiting to
// be sent, the state a host is in after running a task offline.
func anOutboxHoldingOneRun(t *testing.T) *spool.Spool {
	t.Helper()
	directory := t.TempDir()
	// The spool refuses a directory other users can read, so the fixture
	// has to be as private as a real outbox is.
	if err := os.Chmod(directory, 0o700); err != nil {
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
	if _, err := outbox.Finish(started, executor.Result{TaskName: "format-check", ExitCode: 0}, nil); err != nil {
		t.Fatal(err)
	}
	return outbox
}

// stillPending is the assertion every failure in this file shares: the record
// was not marked synced, so the next sweep sends it again.
func stillPending(t *testing.T, outbox *spool.Spool) {
	t.Helper()
	pending, err := outbox.Pending()
	if err != nil {
		t.Fatal(err)
	}
	if len(pending) != 1 {
		t.Fatalf("pending records = %d, want the unsent record kept", len(pending))
	}
}

// answering runs a plane that replies however the test says, over TLS, and
// hands back the base URL and a client that trusts it.
func answering(t *testing.T, reply func(http.ResponseWriter)) (string, *http.Client) {
	t.Helper()
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		reply(w)
	}))
	t.Cleanup(server.Close)
	return server.URL, server.Client()
}

// A plane that cannot be reached at all is named in the failure, with the
// record it was carrying, rather than reported as a clean sweep of nothing.
func TestAPlaneThatCannotBeReachedKeepsTheRecord(t *testing.T) {
	outbox := anOutboxHoldingOneRun(t)
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	base, client := server.URL, server.Client()
	// Closed before the sweep runs, so the connection is refused rather
	// than answered: the host is offline again, which is the condition
	// this client exists for.
	server.Close()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	report, err := SyncPending(ctx, outbox, base, client)
	if err == nil {
		t.Fatal("a sweep that never reached the plane reported success")
	}
	if !strings.Contains(err.Error(), "upload local ") {
		t.Fatalf("error = %v, want the upload and its record named", err)
	}
	if report.Synced != 0 {
		t.Fatalf("synced = %d, want nothing counted as sent", report.Synced)
	}
	stillPending(t, outbox)
}

// A receipt too large to be one ends the sweep before the body is believed:
// the reader is bounded, so a plane flooding the client cannot spend the
// host's memory on the way to a decode.
func TestAReceiptTooLargeToBeOneEndsTheSweep(t *testing.T) {
	outbox := anOutboxHoldingOneRun(t)
	base, client := answering(t, func(w http.ResponseWriter) {
		_, _ = w.Write([]byte(strings.Repeat("a", 8192)))
	})

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	report, err := SyncPending(ctx, outbox, base, client)
	if err == nil {
		t.Fatal("an oversized receipt was read as a durable answer")
	}
	if !strings.Contains(err.Error(), "receipt") {
		t.Fatalf("error = %v, want the receipt named", err)
	}
	if report.Synced != 0 {
		t.Fatalf("synced = %d, want nothing counted as sent", report.Synced)
	}
	stillPending(t, outbox)
}

// A receipt the client cannot decode is refused by name, and so is one that
// carries a field this plane has no column for: an answer that is not the
// receipt asked for is not evidence the run is durable.
func TestAReceiptTheClientCannotDecodeIsRefused(t *testing.T) {
	for _, answered := range []struct {
		named string
		body  string
	}{
		{"not JSON at all", "definitely not a receipt"},
		{"a truncated object", `{"local_id":`},
		{"a field no column holds", `{"local_id":"x","unknown_field":1}`},
		{"an empty body", ""},
	} {
		t.Run(answered.named, func(t *testing.T) {
			outbox := anOutboxHoldingOneRun(t)
			base, client := answering(t, func(w http.ResponseWriter) {
				_, _ = w.Write([]byte(answered.body))
			})

			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			report, err := SyncPending(ctx, outbox, base, client)
			if err == nil {
				t.Fatal("an undecodable receipt marked the record synced")
			}
			if !strings.Contains(err.Error(), "receipt") {
				t.Fatalf("error = %v, want the receipt named", err)
			}
			if report.Synced != 0 {
				t.Fatalf("synced = %d, want nothing counted as sent", report.Synced)
			}
			stillPending(t, outbox)
		})
	}
}

// A plane that refuses the record says so in a status, and the sweep carries
// that status out rather than deciding for itself what it meant.
func TestARefusedUploadCarriesTheStatusOut(t *testing.T) {
	for _, refused := range []int{http.StatusBadRequest, http.StatusUnauthorized,
		http.StatusConflict, http.StatusInternalServerError} {
		outbox := anOutboxHoldingOneRun(t)
		status := refused
		base, client := answering(t, func(w http.ResponseWriter) {
			w.WriteHeader(status)
		})

		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		report, err := SyncPending(ctx, outbox, base, client)
		cancel()
		if err == nil {
			t.Fatalf("HTTP %d was read as a durable receipt", status)
		}
		if !strings.Contains(err.Error(), "HTTP") {
			t.Fatalf("error = %v, want the status carried out", err)
		}
		if report.Synced != 0 {
			t.Fatalf("synced = %d, want nothing counted as sent", report.Synced)
		}
		stillPending(t, outbox)
	}
}
