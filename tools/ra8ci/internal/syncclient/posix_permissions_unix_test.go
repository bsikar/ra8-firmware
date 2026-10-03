//go:build unix

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	context "context"
	sha256 "crypto/sha256"
	hex "encoding/hex"
	json "encoding/json"
	catalog "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	store "github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
	io "io"
	http "net/http"
	httptest "net/http/httptest"
	os "os"
	strings "strings"
	testing "testing"
	time "time"
)

func TestAnUnreadableOutboxStopsTheSweep(t *testing.T) {
	outbox, directory := openOutbox(t)
	plantRaw(t, directory, measuredRecord())
	if err := os.Chmod(directory, 0o000); err != nil {
		t.Fatalf("seal outbox: %v", err)
	}
	t.Cleanup(func() { _ = os.Chmod(directory, 0o700) })
	if _, err := os.ReadDir(directory); err == nil {
		t.Skip("the outbox is readable while sealed, so this host cannot hold the refusal")
	}

	if _, err := SyncPending(context.Background(), outbox, "https://plane.example", http.DefaultClient); err == nil {
		t.Fatal("an unreadable outbox answered a clean sweep")
	}
}

// A durable receipt that cannot be written down is not a synced record: the
// marker is the only thing that stops the next pass sending the same record
// again, so a sweep that could not write it has to report the failure rather
// than count the record sent.
func TestAReceiptThatCannotBeWrittenDownIsNotASyncedRecord(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("root writes into a sealed directory regardless of its mode")
	}
	outbox, directory := openOutbox(t)
	entry := measuredRecord()
	plantRaw(t, directory, entry)
	t.Cleanup(func() { _ = os.Chmod(directory, 0o700) })

	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(r.Body)
		if err != nil {
			t.Errorf("read upload: %v", err)
			return
		}
		canonical, err := catalog.CanonicalJSON(body)
		if err != nil {
			t.Errorf("canonicalise upload: %v", err)
			return
		}
		sum := sha256.Sum256(canonical)
		// Sealed once the bytes are in hand and before the receipt is
		// answered: the plane has taken the record, and the host can no
		// longer write the marker that says so.
		if err := os.Chmod(directory, 0o500); err != nil {
			t.Errorf("seal outbox: %v", err)
			return
		}
		_ = json.NewEncoder(w).Encode(store.LocalRunReceipt{
			LocalRunID: durableRunID, LocalID: entry.ID, PayloadSHA256: hex.EncodeToString(sum[:]),
		})
	}))
	t.Cleanup(server.Close)

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	report, err := SyncPending(ctx, outbox, server.URL, server.Client())

	if err == nil {
		t.Fatal("a receipt that was never written down answered a clean sweep")
	}
	if !strings.Contains(err.Error(), "persist local "+entry.ID+" receipt") {
		t.Fatalf("err = %v, want the unwritten receipt and its record named", err)
	}
	if report.Synced != 0 {
		t.Errorf("report = %+v, want nothing counted as sent", report)
	}
}
