// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// The plane's per-attempt log budget, stated here so the message boundary can
// judge a receipt with no database behind it. The store enforces the same two
// numbers on the way in: it refuses a chunk whose sequence is past
// maxAgentLogChunks, and one whose bytes would push the attempt's stored total
// past maxAgentLogBytes (store/dispatch.go, AcceptAgentLogChunk). Restated
// rather than imported because internal/store depends on this package, and
// because a boundary that can only refuse what a live pool would refuse is not
// a boundary.
const (
	maxAttemptLogBytes  = 64 << 20
	maxAttemptLogChunks = 4096
)

// checkCompleteEvidenceFitsTheAttemptsLogBudget holds a receipt claiming
// complete evidence to an upload the plane could actually have accepted.
//
// checkLogSequenceCoversSteps bounds the same two numbers from BELOW: a receipt
// may not state fewer chunks than its own bytes need. Nothing bounded either
// from above. FinalLogSequence was judged only against zero, and a step's byte
// counts only against zero and their own digests, so a receipt could claim
// complete evidence for a gigabyte of output across a hundred thousand chunks
// and pass every rule in the chain.
//
// Such a receipt is not merely large, it is impossible. Every byte it reports
// went through the uploader and was offered to the plane one chunk at a time
// (agent.go, logUploader.write), and the plane turns away the chunk that would
// carry the attempt past either bound. An agent that hit the ceiling gets an
// error back and reports it: the receipt it sends has EvidenceComplete false
// and log_upload_error, which is the honest shape and the one this rule leaves
// alone. A receipt claiming the bytes AND that all of them landed is claiming
// an upload that was refused.
//
// It matters because the flag is what a later reader trusts. Complete evidence
// means the logs the plane holds are the logs the steps produced, and a reader
// who fetches them finds whatever fraction was actually stored with nothing
// saying the rest was never offered.
//
// The byte bound is the binding one in the ordinary case: a full chunk is
// MaxLogBytes, so 64 MiB fills 2048 of the 4096 chunks. The chunk bound is not
// redundant, because the uploader cuts a chunk per write rather than per full
// buffer, so a step printing a line at a time spends chunks far faster than
// bytes.
func checkCompleteEvidenceFitsTheAttemptsLogBudget(receipt TerminalReceipt) error {
	if !receipt.EvidenceComplete {
		return nil
	}
	if receipt.FinalLogSequence > maxAttemptLogChunks {
		return fmt.Errorf("%w: receipt states complete evidence in %d log chunks, more than the %d an attempt may upload",
			ErrInvalid, receipt.FinalLogSequence, maxAttemptLogChunks)
	}
	// Each step's counts are already non-negative by the time this runs, and
	// the total never passes the budget, so the remaining headroom is what
	// the next count is measured against: the comparison cannot overflow the
	// way a running sum would.
	var total int64
	for _, step := range receipt.Steps {
		for _, bytes := range [2]int64{step.StdoutBytes, step.StderrBytes} {
			if bytes > maxAttemptLogBytes-total {
				return fmt.Errorf("%w: receipt states complete evidence for more log bytes than the %d an attempt may upload",
					ErrInvalid, int64(maxAttemptLogBytes))
			}
			total += bytes
		}
	}
	return nil
}
