// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// The refusals already pinned beside this one are all about material sync was
// handed before it reaches the plane. These are the other half: what sync does
// with an answer the plane actually gave it. A local record is the only
// evidence a run ever happened, so a record the plane did not receipt has to
// stay pending and the command has to say so, and a record it did receipt has
// to stop being offered a second time.

// pendingLocalRun plants one finished, uploadable record in the state
// directory sync will read, and returns that directory.
func pendingLocalRun(t *testing.T) string {
	t.Helper()
	directory := privateStateDirectory(t)
	outbox, err := spool.Open(directory)
	if err != nil {
		t.Fatalf("open the local spool: %v", err)
	}
	started, err := outbox.BeginWithMetadata("format-check", strings.Repeat("a", 64), spool.Metadata{
		Source: spool.SourceIdentity{Repository: "bsikar/ra8-firmware",
			CommitSHA: strings.Repeat("b", 40), Verification: "unverified"},
		Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 900,
	})
	if err != nil {
		t.Fatalf("begin a local run: %v", err)
	}
	if _, err := outbox.Finish(started, executor.Result{TaskName: "format-check"}, nil); err != nil {
		t.Fatalf("finish a local run: %v", err)
	}
	t.Setenv("RA8CI_STATE_DIR", directory)
	return directory
}

// stillPending reports how many records the outbox would still offer a plane.
func stillPending(t *testing.T, directory string) int {
	t.Helper()
	outbox, err := spool.Open(directory)
	if err != nil {
		t.Fatalf("reopen the local spool: %v", err)
	}
	entries, err := outbox.Pending()
	if err != nil {
		t.Fatalf("read the local spool: %v", err)
	}
	return len(entries)
}

func TestSyncKeepsARecordThePlaneWouldNotTake(t *testing.T) {
	material := mintReportMaterial(t)
	asked := 0
	servingReport(t, material, func(writer http.ResponseWriter, request *http.Request) {
		asked++
		writer.WriteHeader(http.StatusInternalServerError)
	})
	directory := pendingLocalRun(t)

	if err := syncLocalRuns(context.Background()); err == nil {
		t.Fatal("a plane that took nothing was reported as a clean sync")
	}
	if asked == 0 {
		t.Fatal("the record was never offered to the plane")
	}
	// The record is the only evidence the run happened. A plane that refused
	// it must leave it here to offer again, not consume it.
	if held := stillPending(t, directory); held != 1 {
		t.Fatalf("pending=%d; want the refused record still offered", held)
	}
}

// The counterpart, and the reason the refusal above is about the plane's
// answer rather than sync declining to upload at all: the same record against
// a plane that receipts it is taken, and is not offered a second time.
func TestSyncStopsOfferingARecordThePlaneReceipted(t *testing.T) {
	material := mintReportMaterial(t)
	serverID, err := store.NewID()
	if err != nil {
		t.Fatal(err)
	}
	servingReport(t, material, func(writer http.ResponseWriter, request *http.Request) {
		body, err := io.ReadAll(request.Body)
		if err != nil {
			t.Errorf("read the uploaded record: %v", err)
			return
		}
		canonical, err := catalog.CanonicalJSON(body)
		if err != nil {
			t.Errorf("canonicalise the uploaded record: %v", err)
			return
		}
		var uploaded spool.Entry
		if err := json.Unmarshal(body, &uploaded); err != nil {
			t.Errorf("decode the uploaded record: %v", err)
			return
		}
		sum := sha256.Sum256(canonical)
		_ = json.NewEncoder(writer).Encode(store.LocalRunReceipt{
			LocalRunID: serverID, LocalID: uploaded.ID, PayloadSHA256: hex.EncodeToString(sum[:]),
		})
	})
	directory := pendingLocalRun(t)

	if err := syncLocalRuns(context.Background()); err != nil {
		t.Fatalf("a receipted record was reported as a failed sync: %v", err)
	}
	if held := stillPending(t, directory); held != 0 {
		t.Fatalf("pending=%d; want the receipted record no longer offered", held)
	}
}
