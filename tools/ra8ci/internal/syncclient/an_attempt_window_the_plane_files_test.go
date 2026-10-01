// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// attempted builds a record whose envelope runs from second 100 to second 200
// and whose execution window is placed inside, on the edges, or outside it.
func attempted(from, to int64) spool.Entry {
	finished := time.Unix(200, 0).UTC()
	return spool.Entry{
		ID:         "0123456789abcdef0123456789abcdef",
		StartedAt:  time.Unix(100, 0).UTC(),
		FinishedAt: &finished,
		Result: &executor.Result{
			StartedAt: time.Unix(from, 0).UTC(),
			EndedAt:   time.Unix(to, 0).UTC(),
		},
	}
}

func TestAnAttemptInsideItsRecordIsUploaded(t *testing.T) {
	if err := checkUploadedAttemptWindowFitsTheRecord(attempted(110, 190)); err != nil {
		t.Fatalf("an ordinary execution window refused: %v", err)
	}
}

func TestAnAttemptFillingItsRecordIsUploaded(t *testing.T) {
	if err := checkUploadedAttemptWindowFitsTheRecord(attempted(100, 200)); err != nil {
		t.Fatalf("an execution on the record's own edges refused: %v", err)
	}
}

func TestAnAttemptBeginningBeforeItsRecordStopsTheSweep(t *testing.T) {
	err := checkUploadedAttemptWindowFitsTheRecord(attempted(90, 190))
	if !errors.Is(err, ErrUnfilableAttemptWindow) {
		t.Fatalf("an execution beginning before its record accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "before the record's start stamp") {
		t.Fatalf("the refusal did not say which stamp: %v", err)
	}
}

func TestAnAttemptEndingAfterItsRecordStopsTheSweep(t *testing.T) {
	err := checkUploadedAttemptWindowFitsTheRecord(attempted(110, 260))
	if !errors.Is(err, ErrUnfilableAttemptWindow) {
		t.Fatalf("an execution ending after its record accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "after the record's finish stamp") {
		t.Fatalf("the refusal did not say which stamp: %v", err)
	}
}

func TestAnAttemptStatingNoStartIsLeftToTheDoorThatClassifies(t *testing.T) {
	// offlineInput files a record whose execution states no start as
	// incomplete_evidence rather than refusing it, so refusing it here would
	// throw away the only account of an attempt that came apart early.
	entry := attempted(110, 190)
	entry.Result.StartedAt = time.Time{}
	entry.Result.EndedAt = time.Time{}
	if err := checkUploadedAttemptWindowFitsTheRecord(entry); err != nil {
		t.Fatalf("an unmeasured execution refused: %v", err)
	}
}

func TestAnAttemptStatingNoEndIsStillUploaded(t *testing.T) {
	// A zero end is the same classification case, and it is never after the
	// record's finish stamp.
	entry := attempted(110, 190)
	entry.Result.EndedAt = time.Time{}
	if err := checkUploadedAttemptWindowFitsTheRecord(entry); err != nil {
		t.Fatalf("an execution stating no end refused: %v", err)
	}
}

func TestARecordWithNoResultStatesNoAttemptWindow(t *testing.T) {
	entry := attempted(110, 190)
	entry.Result = nil
	if err := checkUploadedAttemptWindowFitsTheRecord(entry); err != nil {
		t.Fatalf("a record without a result was judged for its execution window: %v", err)
	}
}

func TestARecordWithNoFinishStampStatesNoAttemptWindow(t *testing.T) {
	// The envelope door refuses this record ahead of here, and it names the
	// stamp; this door has no envelope to judge an execution against.
	entry := attempted(110, 190)
	entry.FinishedAt = nil
	if err := checkUploadedAttemptWindowFitsTheRecord(entry); err != nil {
		t.Fatalf("a record without a finish stamp was judged for its execution window: %v", err)
	}
}
