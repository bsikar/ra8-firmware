// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// checkLogSequenceCoversSteps holds a receipt claiming complete evidence to a
// final log sequence its own step byte counts could have produced. The two
// numbers are written by the same agent from the same stream: every byte a step
// wrote went through the uploader's writer (agent.go, streamWriter forwarding
// into logUploader.write), which cuts the bytes into chunks of at most
// MaxLogBytes and advances the sequence by exactly one per chunk the plane
// accepted. So the sequence is not free-standing metadata, it counts the chunks
// that carried the bytes the steps report.
//
// Nothing before this compared them. Validate judged FinalLogSequence only
// against zero and judged a step's digest against its byte count
// (receipt_log_evidence.go), never one against the other, and the only place
// the sequence is held to the chunks actually stored is the database path
// (store/dispatch.go, refusing a receipt whose final sequence is not the last
// chunk it holds), which no unit test on a box without Postgres can reach. A
// receipt claiming a step wrote megabytes while stating a final sequence of
// zero was admitted everywhere the database was not.
//
// That pair is what a later reader uses to know whether the logs it can fetch
// are the logs the step produced. A receipt that says the evidence is complete
// while stating fewer chunks than its own bytes need is claiming a complete
// record of output the plane was never offered, and a reader has no way to tell
// that from a step that genuinely printed nothing.
//
// The rule applies only to a receipt with EvidenceComplete set, which is the
// receipt asserting that every piece of evidence landed. An incomplete receipt
// reports exactly the opposite (agent.go sets log_upload_error and leaves the
// sequence at the last chunk that did land), so a shortfall there is the
// failure being reported rather than a contradiction.
//
// It refuses one shape, too few chunks for the bytes, and leaves a sequence
// above what the bytes need alone: the uploader cuts a chunk per write, so a
// step printing a line at a time spends a chunk a line, and a receipt stating
// chunks beside a byte count it never captured is the same absent-evidence case
// checkStepLogEvidence already leaves alone.
func checkLogSequenceCoversSteps(receipt TerminalReceipt) error {
	if !receipt.EvidenceComplete {
		return nil
	}
	needed := int64(0)
	for _, step := range receipt.Steps {
		needed = addChunks(needed, chunksFor(step.StdoutBytes))
		needed = addChunks(needed, chunksFor(step.StderrBytes))
	}
	if receipt.FinalLogSequence < needed {
		return fmt.Errorf("%w: receipt states complete evidence with %d log chunks, fewer than the %d its step bytes need",
			ErrInvalid, receipt.FinalLogSequence, needed)
	}
	return nil
}

// chunksFor is the fewest chunks that can carry this many bytes of one stream
// of one step. A chunk carries at least one byte and at most MaxLogBytes
// (LogChunk.Validate refuses both an empty chunk and an oversized one), and the
// uploader never mixes two streams or two steps into one chunk, so each pair is
// counted on its own rather than from a total.
func chunksFor(bytes int64) int64 {
	if bytes <= 0 {
		return 0
	}
	chunks := bytes / MaxLogBytes
	if bytes%MaxLogBytes != 0 {
		chunks++
	}
	return chunks
}

// addChunks saturates instead of wrapping. A receipt is client JSON and may
// state byte counts near the width of the type; a negative total from overflow
// would turn this refusal into an acceptance, which is the one outcome worth
// spending a branch to prevent.
func addChunks(total, add int64) int64 {
	const maxInt64 = int64(^uint64(0) >> 1)
	if add > maxInt64-total {
		return maxInt64
	}
	return total + add
}
