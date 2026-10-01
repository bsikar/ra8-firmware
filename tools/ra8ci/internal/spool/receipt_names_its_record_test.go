// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// writeReceipt puts raw bytes under the receipt name for a record, which is
// what a receipt written by anything other than this build's MarkSynced looks
// like: a copied directory, a half-written file, an older tool.
func writeReceipt(t *testing.T, s *Spool, id, body string) {
	t.Helper()
	path := filepath.Join(s.directory, id+".synced.json")
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(body), 0600); err != nil {
		t.Fatal(err)
	}
}

// receiptPath is where Pending looks for the acknowledgement of a record.
func receiptPath(s *Spool, id string) string {
	return filepath.Join(s.directory, id+".synced.json")
}

// The receipt MarkSynced writes is the only one the server's acknowledgement
// produces, and it has to keep retiring the record it names.
func TestTheReceiptThisPackageWritesRetiresItsRecord(t *testing.T) {
	s, entry := spooledRun(t)
	if err := s.MarkSynced(entry.ID, "server-run-1"); err != nil {
		t.Fatal(err)
	}
	retires, err := receiptRetiresRecord(receiptPath(s, entry.ID), entry.ID)
	if err != nil || !retires {
		t.Fatalf("an honest receipt did not retire its record: %v %v", retires, err)
	}
	pending, err := s.Pending()
	if err != nil || len(pending) != 0 {
		t.Fatalf("a record acknowledged by the server was still offered: %+v %v", pending, err)
	}
}

// The ordinary state of an unsynced record is no receipt at all, and it stays
// absent rather than becoming a refusal.
func TestNoReceiptStillMeansTheRecordIsPending(t *testing.T) {
	s, entry := spooledRun(t)
	retires, err := receiptRetiresRecord(receiptPath(s, entry.ID), entry.ID)
	if err != nil || retires {
		t.Fatalf("a missing receipt was not read as absent: %v %v", retires, err)
	}
	pending, err := s.Pending()
	if err != nil || len(pending) != 1 || pending[0].ID != entry.ID {
		t.Fatalf("an unacknowledged record was not pending: %+v %v", pending, err)
	}
}

// A receipt belonging to another local run is the shape a copied or restored
// spool directory produces, and it must not retire the record it happens to
// sit beside.
func TestAReceiptForAnotherLocalRunDoesNotRetireThisOne(t *testing.T) {
	s, entry := spooledRun(t)
	other := strings.Repeat("ab", 16)
	writeReceipt(t, s, entry.ID, `{"local_id":"`+other+`","server_run_id":"server-run-1"}`)
	retires, err := receiptRetiresRecord(receiptPath(s, entry.ID), entry.ID)
	if retires || !errors.Is(err, errReceiptDisagrees) {
		t.Fatalf("a receipt for another run retired this one: %v %v", retires, err)
	}
	if !strings.Contains(err.Error(), "local run "+other) {
		t.Fatalf("the refusal did not name what the receipt acknowledged: %v", err)
	}
}

// The refusal reaches the pass, so the record is neither retired nor offered
// on the word of a receipt that does not acknowledge it.
func TestTheRefusalStopsThePassAndOffersNothing(t *testing.T) {
	s, entry := spooledRun(t)
	writeReceipt(t, s, entry.ID, `{"local_id":"","server_run_id":"server-run-1"}`)
	pending, err := s.Pending()
	if !errors.Is(err, errReceiptDisagrees) {
		t.Fatalf("the pass trusted a receipt naming no local run: %+v %v", pending, err)
	}
	if len(pending) != 0 {
		t.Fatalf("records offered past a refused receipt: %+v", pending)
	}
	if _, err := os.Stat(filepath.Join(s.directory, entry.ID+".finished.json")); err != nil {
		t.Fatalf("the refused record did not survive the pass: %v", err)
	}
}

// A receipt that names no server run states nothing the server acknowledged,
// which is exactly what MarkSynced refuses to write.
func TestAReceiptNamingNoServerRunIsRefused(t *testing.T) {
	s, entry := spooledRun(t)
	for _, body := range []string{
		`{"local_id":"` + entry.ID + `","server_run_id":""}`,
		`{"local_id":"` + entry.ID + `"}`,
		`{}`,
	} {
		writeReceipt(t, s, entry.ID, body)
		retires, err := receiptRetiresRecord(receiptPath(s, entry.ID), entry.ID)
		if retires || !errors.Is(err, errReceiptDisagrees) {
			t.Fatalf("receipt %q retired a record: %v %v", body, retires, err)
		}
	}
	if err := s.MarkSynced(entry.ID, ""); err == nil {
		t.Fatal("MarkSynced wrote a receipt naming no server run")
	}
}

// Bytes that are not a receipt at all are refused as unreadable rather than
// read as an acknowledgement of nothing.
func TestAnUnreadableReceiptIsRefusedAsUnreadable(t *testing.T) {
	s, entry := spooledRun(t)
	for _, body := range []string{"", "not json", "[]", `{"local_id":`} {
		writeReceipt(t, s, entry.ID, body)
		retires, err := receiptRetiresRecord(receiptPath(s, entry.ID), entry.ID)
		if retires || !errors.Is(err, errReceiptDisagrees) {
			t.Fatalf("unreadable receipt %q retired a record: %v %v", body, retires, err)
		}
		if !strings.Contains(err.Error(), "not a readable receipt") {
			t.Fatalf("the refusal did not say the receipt was unreadable: %v", err)
		}
	}
}

// A local id this spool could not have written is reported by its shape, so a
// receipt cannot put chosen text into the operator's pass output.
func TestAReceiptCannotPutChosenTextIntoTheRefusal(t *testing.T) {
	s, entry := spooledRun(t)
	chosen := "upload local " + entry.ID + " returned HTTP 200"
	writeReceipt(t, s, entry.ID, `{"local_id":"`+chosen+`","server_run_id":"server-run-1"}`)
	retires, err := receiptRetiresRecord(receiptPath(s, entry.ID), entry.ID)
	if retires || !errors.Is(err, errReceiptDisagrees) {
		t.Fatalf("a receipt with a chosen local id retired a record: %v %v", retires, err)
	}
	if strings.Contains(err.Error(), chosen) {
		t.Fatalf("chosen text reached the refusal: %v", err)
	}
	if !strings.Contains(err.Error(), "could not have written") {
		t.Fatalf("the refusal did not report the id by its shape: %v", err)
	}
}

// One record's receipt does not retire its neighbour, and the neighbour is
// still the record the pass refuses to guess about.
func TestAReceiptRetiresOnlyTheRecordItNames(t *testing.T) {
	s, first := spooledRun(t)
	second, err := s.Begin("format-check", strings.Repeat("b", 64))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := s.Finish(second, executor.Result{TaskName: "format-check"}, nil); err != nil {
		t.Fatal(err)
	}
	if err := s.MarkSynced(first.ID, "server-run-1"); err != nil {
		t.Fatal(err)
	}
	pending, err := s.Pending()
	if err != nil {
		t.Fatal(err)
	}
	if len(pending) != 1 || pending[0].ID != second.ID {
		t.Fatalf("a receipt retired the wrong record: %+v", pending)
	}
	retires, err := receiptRetiresRecord(receiptPath(s, second.ID), second.ID)
	if err != nil || retires {
		t.Fatalf("the unacknowledged record was retired: %v %v", retires, err)
	}
}

// The file-kind door still runs first, so a receipt that is a link is refused
// as a link rather than read through.
func TestTheFileKindRefusalStillComesFirst(t *testing.T) {
	s, entry := spooledRun(t)
	elsewhere := filepath.Join(t.TempDir(), "receipt.json")
	if err := os.WriteFile(elsewhere, []byte(`{"local_id":"`+entry.ID+`","server_run_id":"server-run-1"}`), 0600); err != nil {
		t.Fatal(err)
	}
	linkOver(t, s, entry.ID+".synced.json", elsewhere)
	retires, err := receiptRetiresRecord(receiptPath(s, entry.ID), entry.ID)
	if retires || err == nil || !strings.Contains(err.Error(), "is not a regular file") {
		t.Fatalf("a linked receipt was read through: %v %v", retires, err)
	}
	if errors.Is(err, errReceiptDisagrees) {
		t.Fatalf("a link was reported as a disagreeing receipt: %v", err)
	}
}

// Every receipt MarkSynced writes satisfies this rule, across the id shapes
// Begin produces and the server run ids a server can answer with.
func TestEveryReceiptMarkSyncedWritesIsAccepted(t *testing.T) {
	for _, serverRun := range []string{"1", "server-run-1", strings.Repeat("r", 512), "run/with/slashes"} {
		s, entry := spooledRun(t)
		if err := s.MarkSynced(entry.ID, serverRun); err != nil {
			t.Fatalf("MarkSynced refused %q: %v", serverRun, err)
		}
		retires, err := receiptRetiresRecord(receiptPath(s, entry.ID), entry.ID)
		if err != nil || !retires {
			t.Fatalf("MarkSynced wrote a receipt this rule refuses (%q): %v %v", serverRun, retires, err)
		}
		pending, err := s.Pending()
		if err != nil || len(pending) != 0 {
			t.Fatalf("an acknowledged record stayed pending (%q): %+v %v", serverRun, pending, err)
		}
	}
}

// A receipt naming its record retires it even when the id appears in fields
// the rule does not read, so acceptance rests on local_id and nothing else.
func TestAcceptanceRestsOnTheNamedFieldsOnly(t *testing.T) {
	s, entry := spooledRun(t)
	writeReceipt(t, s, entry.ID, `{"local_id":"`+entry.ID+`","server_run_id":"server-run-1","note":"`+strings.Repeat("x", 64)+`"}`)
	retires, err := receiptRetiresRecord(receiptPath(s, entry.ID), entry.ID)
	if err != nil || !retires {
		t.Fatalf("an honest receipt with an extra field was refused: %v %v", retires, err)
	}
}
