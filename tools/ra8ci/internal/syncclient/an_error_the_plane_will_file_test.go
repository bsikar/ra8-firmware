// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

func failedWith(message string) spool.Entry {
	return spool.Entry{
		SchemaVersion: uploadableSchemaVersion,
		ID:            "0123456789abcdef0123456789abcdef",
		Task:          "unit-tests",
		Error:         message,
	}
}

func TestAnOrdinaryFailureMessageIsUploaded(t *testing.T) {
	if err := checkUploadedExecutorErrorIsOneThePlaneWillFile(
		failedWith("exit status 1: go test ./internal/... failed")); err != nil {
		t.Fatalf("an ordinary failure message was refused: %v", err)
	}
}

func TestAnAttemptStatingNoErrorIsUploaded(t *testing.T) {
	if err := checkUploadedExecutorErrorIsOneThePlaneWillFile(failedWith("")); err != nil {
		t.Fatalf("a succeeded attempt was refused: %v", err)
	}
}

func TestAMessageExactlyTheColumnsWidthIsUploaded(t *testing.T) {
	if err := checkUploadedExecutorErrorIsOneThePlaneWillFile(
		failedWith(strings.Repeat("e", maxFilableExecutorErrorBytes))); err != nil {
		t.Fatalf("a message exactly the column's width was refused: %v", err)
	}
}

func TestAMessageWiderThanTheColumnIsRefused(t *testing.T) {
	err := checkUploadedExecutorErrorIsOneThePlaneWillFile(
		failedWith(strings.Repeat("e", maxFilableExecutorErrorBytes+1)))
	if !errors.Is(err, ErrUnfilableExecutorError) {
		t.Fatalf("an oversized message was accepted: %v", err)
	}
}

func TestTheExecutorErrorBoundIsBytesNotRunes(t *testing.T) {
	// 512 two-byte runes are 1024 bytes, the column's whole width, and one
	// more rune is over it even though the string is 513 characters long.
	if err := checkUploadedExecutorErrorIsOneThePlaneWillFile(
		failedWith(strings.Repeat("é", maxFilableExecutorErrorBytes/2))); err != nil {
		t.Fatalf("a message exactly the column's width was refused: %v", err)
	}
	err := checkUploadedExecutorErrorIsOneThePlaneWillFile(
		failedWith(strings.Repeat("é", maxFilableExecutorErrorBytes/2+1)))
	if !errors.Is(err, ErrUnfilableExecutorError) {
		t.Fatalf("a message one rune over the column was accepted: %v", err)
	}
}

func TestAMessageHoldingANULIsRefused(t *testing.T) {
	err := checkUploadedExecutorErrorIsOneThePlaneWillFile(failedWith("exit status 1\x00truncated"))
	if !errors.Is(err, ErrUnfilableExecutorError) {
		t.Fatalf("a message holding a NUL was accepted: %v", err)
	}
}

func TestInvalidUTF8InTheMessageIsRefused(t *testing.T) {
	err := checkUploadedExecutorErrorIsOneThePlaneWillFile(failedWith("child said \xff\xfe"))
	if !errors.Is(err, ErrUnfilableExecutorError) {
		t.Fatalf("a message carrying invalid UTF-8 was accepted: %v", err)
	}
}

func TestAMessageCarryingAnEscapeSequenceIsUploaded(t *testing.T) {
	// The column holds it and an operator reading a child's own output
	// expects colour codes in it. This door bounds what the column cannot
	// hold, not what a terminal renders.
	if err := checkUploadedExecutorErrorIsOneThePlaneWillFile(
		failedWith("exit status 1: \x1b[31mFAIL\x1b[0m")); err != nil {
		t.Fatalf("a message carrying an escape sequence was refused: %v", err)
	}
}

func TestAnErrorBesideASuccessIsLeftToTheDoorThatClassifies(t *testing.T) {
	// The store refuses the pair, but the server never files it: an
	// attempt stating an error is classified incomplete_evidence before
	// the success is read, which is how a run that came apart reaches
	// history at all.
	entry := failedWith("context deadline exceeded")
	entry.Result = &executor.Result{TaskName: "unit-tests", ExitCode: 0}
	if err := checkUploadedExecutorErrorIsOneThePlaneWillFile(entry); err != nil {
		t.Fatalf("an error beside a success was refused here: %v", err)
	}
}

func TestSyncPendingRefusesAMessageTheFreezeWillinglyWrote(t *testing.T) {
	// The wiring test, and the point of the slice: the freeze door
	// deliberately does not bound the length, so this record is frozen
	// through the ordinary path and must be stopped at the sweep.
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
	long := errors.New(strings.Repeat("e", maxFilableExecutorErrorBytes+1))
	if _, err := outbox.Finish(started, executor.Result{TaskName: "format-check", ExitCode: 1}, long); err != nil {
		t.Fatalf("the freeze refused a long but readable message: %v", err)
	}
	reached := false
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		reached = true
	}))
	defer server.Close()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	report, err := SyncPending(ctx, outbox, server.URL, server.Client())
	if !errors.Is(err, ErrUnfilableExecutorError) {
		t.Fatalf("the sweep uploaded an unfilable message: %+v %v", report, err)
	}
	if reached {
		t.Fatal("the record reached the plane")
	}
	if pending, err := outbox.Pending(); err != nil || len(pending) != 1 {
		t.Fatalf("the refused record left the outbox: %d %v", len(pending), err)
	}
}
