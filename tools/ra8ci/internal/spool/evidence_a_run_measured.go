// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

// errUnmeasuredEvidence names the one thing this rule refuses: a result
// handed to Finish whose per-step log evidence no run of this build could
// have measured.
var errUnmeasuredEvidence = errors.New("execution result states log evidence no run measured")

// checkTheEvidenceWasMeasured holds each step's log evidence at the freeze.
//
// A step carries no logs, only two digests and two byte counts, and they are
// the whole of what durable history can say about what a step printed:
// server.offlineInput copies StdoutSHA256, StderrSHA256, StdoutBytes and
// StderrBytes into store.LocalStepInput verbatim and re-derives none of them.
// Finish took the executor's result by value, stored its address and wrote it
// out without looking at any of the four.
//
// They come from one place, executor.runStep's digestWriter: a lowercase hex
// SHA-256 beside the count of the bytes it covered, both written for every
// step including a silent one (the digest of nothing is still a digest). So a
// digest of another shape, or a negative count, was produced by no run this
// build performed, and writing it into the outbox freezes a claim about a
// step's output that nothing downstream can repair.
//
// THE RULE, and it is the store's rather than a new one
// (store.validateLocalRun, local_sync.go:232-233): 64 lowercase hex per
// stream, both counts at or above zero.
//
// This is the FREEZE, not the sweep, and the two doors are not the same door.
// checkUploadedLogEvidenceWasMeasured judges a record read back OFF DISK,
// where an older build's file or an edited one can say anything at all; this
// one judges what the executor in this process just handed over, before the
// bytes are written, so the outbox never holds the claim in the first place.
// A record this build wrote cannot be refused by either: an honest step
// carries a digest and a count by construction.
//
// DELIBERATELY NOT the count-against-digest comparison: a count of zero
// beside the digest of nothing is the only pair this package could check
// without the bytes, and the server states that rule in full
// (checkLocalStepCountsAgreeWithTheDigest) where the whole record is in hand.
// Restating half of it here would refuse nothing the shape rule does not
// already catch.
func checkTheEvidenceWasMeasured(result executor.Result) error {
	for i, step := range result.Steps {
		named := namedStepOf(result, i)
		for _, stream := range []struct {
			name   string
			digest string
			count  int64
		}{
			{"stdout", step.StdoutSHA256, step.StdoutBytes},
			{"stderr", step.StderrSHA256, step.StderrBytes},
		} {
			if !hexDigest(stream.digest, 64) {
				return fmt.Errorf("%w: %s states a %s digest %q that is not a 64-hex SHA-256",
					errUnmeasuredEvidence, named, stream.name, stream.digest)
			}
			if stream.count < 0 {
				return fmt.Errorf("%w: %s states %d bytes of %s",
					errUnmeasuredEvidence, named, stream.count, stream.name)
			}
		}
	}
	return nil
}

// namedStepOf names a step of a result that is not yet part of a record, so
// the refusal above can point at the same step the operator sees in the log.
func namedStepOf(result executor.Result, i int) string {
	if i < len(result.Steps) && result.Steps[i].Name != "" {
		return fmt.Sprintf("step %d %q", i, result.Steps[i].Name)
	}
	return fmt.Sprintf("step %d", i)
}
