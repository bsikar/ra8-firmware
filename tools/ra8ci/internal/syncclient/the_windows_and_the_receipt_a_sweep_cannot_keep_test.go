// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
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

// Three places a sweep can lose a record: the step window the spool's own
// freeze lets through, the attempt window it does not, and the marker that
// says a record the plane has already taken must not be sent again.

// durableRunID is the shape the sweep requires of a durable run identifier
// before it believes a receipt: version 7, with a variant nibble.
const durableRunID = "0189abcd-1234-7abc-89de-0123456789ab"

// A step reporting longer than the stamps it arrives with is the one window
// fault the spool's freeze does not catch: its rule is the step's duration
// against the record's whole envelope, which 36 seconds inside a minute sits
// comfortably within, while the sweep holds the duration to the span of the
// step's own two stamps. So this record reaches the sweep having passed the
// freeze, which is exactly the case the sweep's copy of the door exists for.
func TestAStepReportingLongerThanItsStampsAllowStopsTheSweep(t *testing.T) {
	outbox, directory := openOutbox(t)
	entry := measuredRecord()
	entry.Result.Steps[0].Duration = 36 * time.Second
	plantRaw(t, directory, entry)

	asked := false
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		asked = true
	}))
	t.Cleanup(server.Close)

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	report, err := SyncPending(ctx, outbox, server.URL, server.Client())

	if !errors.Is(err, ErrUnmeasuredStepWindow) {
		t.Fatalf("err = %v, want the unmeasured window refused", err)
	}
	if asked {
		t.Error("the record was sent to the plane anyway")
	}
	if !strings.Contains(err.Error(), "36s between stamps 30s apart") {
		t.Errorf("err = %v, want the two measurements named", err)
	}
	if !strings.Contains(err.Error(), `step 0 ("build")`) || !strings.Contains(err.Error(), "local "+entry.ID) {
		t.Errorf("err = %v, want the step and the record named", err)
	}
	if report.Synced != 0 {
		t.Errorf("report = %+v, want nothing counted as sent", report)
	}
	if pending, err := outbox.Pending(); err != nil || len(pending) != 1 {
		t.Errorf("pending = %d (%v), want the refused record kept", len(pending), err)
	}
}

// The execution's own window is the pair the executor wrote rather than the
// pair the spool did. Held directly: the spool refuses both of these at the
// freeze as stamps out of order, so a record breaking them cannot reach a
// sweep through the outbox, and this door is the second line behind the
// first, for a file restored from a backup or edited by hand.
func TestAnExecutionOutsideTheRecordsOwnWindowIsRefused(t *testing.T) {
	for _, one := range []struct {
		name   string
		broken func(spool.Entry) spool.Entry
		says   string
	}{
		{"began before the record", func(e spool.Entry) spool.Entry {
			e.Result.StartedAt = recordStart.Add(-time.Second)
			return e
		}, "before the record's start stamp"},
		{"ended after the record", func(e spool.Entry) spool.Entry {
			e.Result.EndedAt = recordEnd.Add(time.Second)
			return e
		}, "after the record's finish stamp"},
	} {
		t.Run(one.name, func(t *testing.T) {
			err := checkUploadedAttemptWindowFitsTheRecord(one.broken(measuredRecord()))

			if !errors.Is(err, ErrUnfilableAttemptWindow) {
				t.Fatalf("err = %v, want the attempt window refused", err)
			}
			if !strings.Contains(err.Error(), one.says) {
				t.Errorf("err = %v, want it to say %q", err, one.says)
			}
		})
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

// A record edited into a state these doors exist for is exactly the one whose
// step names may be missing, so a step is named by the ordinal the upload and
// durable history key it by whether or not it states a name.
func TestAStepWithNoNameIsStillNamedByItsOrdinal(t *testing.T) {
	entry := measuredRecord()
	unnamed := soundStep("")
	unnamed.Duration = 36 * time.Second
	entry.Result.Steps = []executor.StepResult{soundStep("build"), unnamed}

	err := checkUploadedStepWindowsWereMeasured(entry)

	if !errors.Is(err, ErrUnmeasuredStepWindow) {
		t.Fatalf("err = %v, want the unmeasured window refused", err)
	}
	if !strings.Contains(err.Error(), "step 1 reports") {
		t.Fatalf("err = %v, want the nameless step named by its ordinal alone", err)
	}
	if strings.Contains(err.Error(), `step 1 (`) {
		t.Errorf("err = %v, want no empty name quoted beside the ordinal", err)
	}
}
