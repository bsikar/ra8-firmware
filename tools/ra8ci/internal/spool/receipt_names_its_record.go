// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"encoding/json"
	"errors"
	"fmt"
)

// errReceiptDisagrees names the one thing this rule refuses: a receipt that
// retires a record without acknowledging it.
var errReceiptDisagrees = errors.New("sync receipt does not acknowledge the record it retires")

// syncReceipt is what MarkSynced writes, and the only shape this package has
// ever put under a .synced.json name: the local run that was uploaded and the
// server run the server durably created for it.
type syncReceipt struct {
	LocalID     string `json:"local_id"`
	ServerRunID string `json:"server_run_id"`
}

// receiptRetiresRecord reports whether the receipt at path acknowledges the
// local run named by id.
//
// A receipt is the only thing that retires evidence. Pending skips a terminal
// record when one sits beside it, before the start record is read and before
// any of this package's other rules are applied to it, and nothing ever looks
// at that record again: the spool is append-only, so the pass simply stops
// offering it and the outbox has delivered what it kept. Until this rule, the
// whole of that decision was that a name existed and was a regular file. The
// two fields inside it, the ones MarkSynced writes precisely so a receipt says
// what it acknowledges, were never read by anything.
//
// So any regular file at <id>.synced.json retired the run, whatever it said.
// The shapes that actually occur are not attacks. A spool directory copied or
// restored onto another host carries receipts whose local_id belongs to the
// host it came from, and the copy's own records are retired by them. A
// half-written receipt, or one whose server_run_id is empty because the
// upload's answer was read before it carried an id, states no server run at
// all and still retires the record. A tool writing into the directory under a
// name this package owns is the case pending_matches_start and
// record_is_a_real_file already exist to catch one name over.
//
// What that costs is the one loss this outbox exists to prevent. The record
// the server never received is dropped silently: no pass names it, the
// operator reads a clean sweep, and the evidence of a run that happened on a
// disconnected host is gone from everywhere except a local file nobody reads.
// That is the direction the package has already chosen against twice, in
// syncReceiptPresent ("read as present it drops one the server does not") and
// in checkTerminalRecordCarriesItsRun ("skipping it silently would retire
// evidence the server never received").
//
// Refused, it costs the sweep: the pass stops and names the receipt and what
// is wrong with it, the record stays pending, and an operator who genuinely
// did upload it can restate the receipt. A receipt the server's own
// acknowledgement produced satisfies this rule by construction, because
// MarkSynced writes both fields from the id it was handed and the server run
// it was given, and refuses an empty server run id at that door.
func receiptRetiresRecord(path, id string) (bool, error) {
	present, err := syncReceiptPresent(path)
	if err != nil || !present {
		return false, err
	}
	raw, err := readRegularFile(path)
	if err != nil {
		return false, err
	}
	var receipt syncReceipt
	if err := json.Unmarshal(raw, &receipt); err != nil {
		return false, fmt.Errorf("%w: %q is not a readable receipt: %w", errReceiptDisagrees, path, err)
	}
	if receipt.LocalID != id {
		return false, fmt.Errorf("%w: %q acknowledges %s", errReceiptDisagrees, path, acknowledgedRun(receipt.LocalID))
	}
	if receipt.ServerRunID == "" {
		return false, fmt.Errorf("%w: %q names no server run", errReceiptDisagrees, path)
	}
	return true, nil
}

// acknowledgedRun says what a receipt claims to acknowledge without putting
// whatever bytes it holds into the pass's output. A local run id this spool
// could have written is worth naming, because the ordinary cause of a
// mismatch is a directory carried over from another host and the id is how an
// operator recognises that. Anything else is reported by its shape only.
func acknowledgedRun(id string) string {
	switch {
	case validID(id):
		return "local run " + id
	case id == "":
		return "no local run"
	default:
		return "a local run id this spool could not have written"
	}
}
