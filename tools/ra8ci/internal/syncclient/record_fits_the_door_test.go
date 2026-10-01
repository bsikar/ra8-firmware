// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

func bodyOf(size int) []byte { return []byte(strings.Repeat("x", size)) }

func TestABodyTheDoorReadsIsAccepted(t *testing.T) {
	for name, size := range map[string]int{
		"empty":            0,
		"one byte":         1,
		"a kilobyte":       1024,
		"one under":        maxUploadedRecordBytes - 1,
		"exactly the door": maxUploadedRecordBytes,
	} {
		if err := checkUploadedRecordFitsTheOfflineDoor(bodyOf(size)); err != nil {
			t.Fatalf("%s: refused a body the server reads: %v", name, err)
		}
	}
}

func TestABodyLargerThanTheDoorIsRefused(t *testing.T) {
	for name, size := range map[string]int{
		"one over":        maxUploadedRecordBytes + 1,
		"a kilobyte over": maxUploadedRecordBytes + 1024,
		"twice the door":  maxUploadedRecordBytes * 2,
	} {
		err := checkUploadedRecordFitsTheOfflineDoor(bodyOf(size))
		if !errors.Is(err, ErrRecordExceedsOfflineDoor) {
			t.Fatalf("%s: sent a body the server cannot read: %v", name, err)
		}
		if !strings.Contains(err.Error(), "262144") {
			t.Fatalf("%s: refusal did not name the bound: %v", name, err)
		}
	}
}

func TestTheRefusalNamesTheSizeThatMissed(t *testing.T) {
	err := checkUploadedRecordFitsTheOfflineDoor(bodyOf(300000))
	if !errors.Is(err, ErrRecordExceedsOfflineDoor) {
		t.Fatalf("oversized body accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "300000") {
		t.Fatalf("refusal did not name the size: %v", err)
	}
}

func TestTheBoundIsTheServersOwnBound(t *testing.T) {
	if maxUploadedRecordBytes != 256<<10 {
		t.Fatalf("client bound %d is not the server's 256 KiB door", maxUploadedRecordBytes)
	}
}

// The claim the rule rests on is that a body over this bound is a body the
// server's own reader cannot finish, so drive that reader rather than assert it.
func TestTheServersReaderRefusesExactlyWhatThisRuleRefuses(t *testing.T) {
	var readErr error
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		r.Body = http.MaxBytesReader(w, r.Body, maxUploadedRecordBytes)
		_, readErr = io.ReadAll(r.Body)
	}))
	defer server.Close()
	for name, size := range map[string]int{
		"exactly the door": maxUploadedRecordBytes,
		"one over":         maxUploadedRecordBytes + 1,
	} {
		readErr = nil
		request, err := http.NewRequest(http.MethodPost, server.URL, strings.NewReader(strings.Repeat("x", size)))
		if err != nil {
			t.Fatal(err)
		}
		response, err := server.Client().Do(request)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		_ = response.Body.Close()
		refusedHere := checkUploadedRecordFitsTheOfflineDoor(bodyOf(size)) != nil
		if refusedHere != (readErr != nil) {
			t.Fatalf("%s: rule says refused=%v, the server's reader says %v", name, refusedHere, readErr)
		}
	}
}

func TestEveryLengthAroundTheBoundIsJudgedByOneComparison(t *testing.T) {
	for size := maxUploadedRecordBytes - 16; size <= maxUploadedRecordBytes+16; size++ {
		err := checkUploadedRecordFitsTheOfflineDoor(bodyOf(size))
		if (err != nil) != (size > maxUploadedRecordBytes) {
			t.Fatalf("size %d judged %v", size, err)
		}
	}
}

func TestTheRuleCountsBytesNotRunes(t *testing.T) {
	// Well under the bound in runes, over it in bytes, which is what is sent.
	body := []byte(strings.Repeat("\u20ac", maxUploadedRecordBytes/3+1))
	if len(body) <= maxUploadedRecordBytes {
		t.Fatalf("fixture is only %d bytes", len(body))
	}
	if err := checkUploadedRecordFitsTheOfflineDoor(body); !errors.Is(err, ErrRecordExceedsOfflineDoor) {
		t.Fatalf("a multibyte body over the bound was accepted: %v", err)
	}
}

func oversizedOutbox(t *testing.T) *spool.Spool {
	t.Helper()
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
	huge := errors.New(strings.Repeat("e", 300<<10))
	if _, err := outbox.Finish(started, executor.Result{TaskName: "format-check", ExitCode: 1}, huge); err != nil {
		t.Fatal(err)
	}
	return outbox
}

func TestAnOversizedRecordStopsTheSweepBeforeTheRequest(t *testing.T) {
	outbox := oversizedOutbox(t)
	pending, err := outbox.Pending()
	if err != nil || len(pending) != 1 {
		t.Fatalf("fixture outbox: %d %v", len(pending), err)
	}
	marshalled, err := json.Marshal(pending[0])
	if err != nil {
		t.Fatal(err)
	}
	if len(marshalled) <= maxUploadedRecordBytes {
		t.Fatalf("fixture record is only %d bytes", len(marshalled))
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		t.Error("an unreadable record was posted to the server")
	}))
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	report, err := SyncPending(ctx, outbox, server.URL, server.Client())
	if !errors.Is(err, ErrRecordExceedsOfflineDoor) {
		t.Fatalf("sweep did not refuse the oversized record: %+v %v", report, err)
	}
	if !strings.Contains(err.Error(), pending[0].ID) {
		t.Fatalf("refusal did not name the record: %v", err)
	}
	if report.Synced != 0 || report.Quarantined != 0 {
		t.Fatalf("oversized record was counted: %+v", report)
	}
}

func TestTheOversizedRecordIsLeftInTheOutbox(t *testing.T) {
	outbox := oversizedOutbox(t)
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		t.Error("an unreadable record was posted to the server")
	}))
	defer server.Close()
	if _, err := SyncPending(context.Background(), outbox, server.URL, server.Client()); err == nil {
		t.Fatal("oversized record was accepted")
	}
	pending, err := outbox.Pending()
	if err != nil || len(pending) != 1 {
		t.Fatalf("refused record left the outbox: %d %v", len(pending), err)
	}
}

func TestAnOrdinaryRecordIsNowhereNearTheDoor(t *testing.T) {
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
	if _, err := outbox.Finish(started, executor.Result{TaskName: "format-check", ExitCode: 0}, nil); err != nil {
		t.Fatal(err)
	}
	pending, err := outbox.Pending()
	if err != nil || len(pending) != 1 {
		t.Fatalf("fixture outbox: %d %v", len(pending), err)
	}
	marshalled, err := json.Marshal(pending[0])
	if err != nil {
		t.Fatal(err)
	}
	if err := checkUploadedRecordFitsTheOfflineDoor(marshalled); err != nil {
		t.Fatalf("an ordinary %d-byte record was refused: %v", len(marshalled), err)
	}
}
