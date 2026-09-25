// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"
)

// emptyStreamDigest is the SHA-256 of no bytes at all, which is exactly what
// the executor reports for a step that wrote nothing: digestWriter hashes only
// what it forwards and returns hash.Sum on whatever it received
// (executor.go, digestWriter.Write and digest), so an empty stream still
// carries a well-formed digest rather than an empty string.
var emptyStreamDigest = hex.EncodeToString(func() []byte {
	sum := sha256.Sum256(nil)
	return sum[:]
}())

// checkStepLogEvidence holds a step's stdout and stderr digests to the byte
// counts stated beside them. The two travel as separate JSON fields and nothing
// before this compared them, nor judged the digests' shape at all: Validate
// never looked at either field, and the store compares a digest against the log
// chunks actually uploaded only on the database path
// (store/dispatch.go), which no unit test on a box without Postgres can reach.
// So a step claiming zero bytes alongside the digest of real output, or real
// output alongside the digest of an empty stream, was admitted everywhere the
// database was not.
//
// The pair is the whole evidence that a step's logs are the logs it ran with.
// A digest that disagrees with its own byte count cannot be checked against
// anything later: whichever half is wrong, the receipt is claiming something
// about output it did not produce.
//
// The rule refuses only the three shapes that contradict themselves and leaves
// an absent digest alone, because a receipt may legitimately state no digest
// for a stream it never captured, and the store's own comparison remains the
// place where a present digest is held to the bytes that were uploaded.
func checkStepLogEvidence(receipt TerminalReceipt) error {
	for _, step := range receipt.Steps {
		if err := checkStreamEvidence(step.Name, "stdout", step.StdoutSHA256, step.StdoutBytes); err != nil {
			return err
		}
		if err := checkStreamEvidence(step.Name, "stderr", step.StderrSHA256, step.StderrBytes); err != nil {
			return err
		}
	}
	return nil
}

func checkStreamEvidence(stepName, stream, digest string, bytes int64) error {
	if digest == "" {
		return nil
	}
	if !ValidSHA256(digest) {
		return fmt.Errorf("%w: step %s states a malformed %s digest", ErrInvalid, stepName, stream)
	}
	if bytes == 0 && digest != emptyStreamDigest {
		return fmt.Errorf("%w: step %s states no %s bytes and the digest of some", ErrInvalid, stepName, stream)
	}
	if bytes > 0 && digest == emptyStreamDigest {
		return fmt.Errorf("%w: step %s states %d %s bytes and the digest of none", ErrInvalid, stepName, bytes, stream)
	}
	return nil
}
