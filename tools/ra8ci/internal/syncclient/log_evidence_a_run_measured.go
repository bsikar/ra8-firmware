// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

// ErrUnmeasuredLogEvidence is the refusal of a local record whose per-step log
// evidence no run on this host could have measured.
var ErrUnmeasuredLogEvidence = errors.New("local record states log evidence no run could have measured")

// A spooled step carries no logs, by design: it states two digests and two
// byte counts, and those four fields are the whole local evidence that the
// step printed what it printed. This client uploaded all four unexamined.
//
// They come from one place. executor.runStep takes them from digestWriter's
// digest(), which returns a lowercase hex SHA-256 over everything written
// beside the count of those bytes, so a digest of another shape, or a negative
// count, was not produced by any run this plane would recognise. A record can
// still carry one: the fields come back off disk through json.Unmarshal, which
// fills whatever the file holds, and spool.Pending judges the terminal record
// against the start record rather than the shape of what it says.
//
// The far end refuses both shapes (server.checkLocalStepEvidenceIsMeasured,
// and store.validateLocalRun again before the insert), so nothing unmeasured
// was reaching local_run_steps.
//
// *** HONESTY: this refusal changes nothing about what the database holds. It
// buys where the sweep stops and what it says, the same thing the record-only
// doors above it buy. SyncPending turns any non-200 into a returned error that
// ends the whole sweep; Pending hands a record back until a synced marker sits
// beside it, so the same record is read, posted and refused again on every
// pass, and every unsynced record behind it in the outbox waits behind it on
// every pass too. The sentence the operator reads is "upload local <id>
// returned HTTP 400", which is also what a server that is merely unwell says.
// Refused here, they are told which record, which step and which field, before
// the bytes leave the host that wrote them.
//
// The empty output is NOT an exception: the digest of nothing is still a
// digest (e3b0c442...), and that is what a silent step records. Nothing here
// compares a count with a digest; that pair is the server's own rule and needs
// the empty digest spelled out, which this door deliberately does not restate.
func checkUploadedLogEvidenceWasMeasured(entry spool.Entry) error {
	if entry.Result == nil {
		return nil
	}
	for _, step := range entry.Result.Steps {
		if err := logEvidenceWasMeasured(step); err != nil {
			return err
		}
	}
	return nil
}

func logEvidenceWasMeasured(step executor.StepResult) error {
	if !hexOfLength(step.StdoutSHA256, 64) {
		return fmt.Errorf("%w: step %q states stdout digest %q, not a 64-hex SHA",
			ErrUnmeasuredLogEvidence, step.Name, step.StdoutSHA256)
	}
	if !hexOfLength(step.StderrSHA256, 64) {
		return fmt.Errorf("%w: step %q states stderr digest %q, not a 64-hex SHA",
			ErrUnmeasuredLogEvidence, step.Name, step.StderrSHA256)
	}
	if step.StdoutBytes < 0 || step.StderrBytes < 0 {
		return fmt.Errorf("%w: step %q states %d stdout and %d stderr bytes",
			ErrUnmeasuredLogEvidence, step.Name, step.StdoutBytes, step.StderrBytes)
	}
	return nil
}
