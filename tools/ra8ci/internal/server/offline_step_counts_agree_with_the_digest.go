package server

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// digestOfNoBytes is SHA-256 over nothing at all. It is what the executor
// records for a stream a step never wrote to, and the one digest a record can
// state without any output having passed through the writer that measured it.
const digestOfNoBytes = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

// checkLocalStepCountsAgreeWithTheDigest holds a spooled step's two byte
// counts to the two digests beside them.
//
// checkLocalStepEvidenceIsMeasured judges each of the four numbers on its own:
// the digests for hex shape, the counts for not being negative. Nothing asked
// whether a pair agrees, and the pair is the whole of a step's log evidence,
// so a record could state 4096 stdout bytes beside the digest of nothing, or
// the digest of a megabyte beside zero bytes, and the plane filed both into
// local_run_steps as history.
//
// A run cannot produce that disagreement. Both numbers come out of one
// digestWriter and are written in the same branch of one Write:
// `writer.hash.Write(data[:n])` and `writer.bytes += int64(n)` sit under the
// same `if n > 0`, and digest() returns the sum and the count together
// (executor.go). Zero bytes through that writer therefore always hashes to
// the empty digest, and any byte through it always moves the hash off it. So
// a pair that disagrees was not measured by a run this plane would recognise,
// whatever shape each half has on its own.
//
// What that costs is the same thing the digest rule exists to protect. The
// digest is compared against a later run's; the count is what a reader quotes
// for how much the step printed. When they contradict each other, one of them
// is wrong and the row does not say which, so a comparison that matches is
// reported beside a size that cannot be true of the bytes it matched. A row
// that says nothing is better than a row that says two incompatible things
// while looking like evidence.
//
// This rule does NOT touch the silent step, which is the case the digest rule
// was careful about: nothing printed means zero bytes and the empty digest,
// the two agreeing, and it stays acceptable. Only the mismatch is refused.
// Nor is it a rule about truncation: an agent's upload may legitimately carry
// fewer bytes than a step produced, but a spooled step reports what the local
// writer saw and hashed, with no upload between the measurement and the
// record.
func checkLocalStepCountsAgreeWithTheDigest(step executor.StepResult) error {
	if err := countAgreesWithDigest(step.StdoutBytes, step.StdoutSHA256, step.Name, "stdout"); err != nil {
		return err
	}
	return countAgreesWithDigest(step.StderrBytes, step.StderrSHA256, step.Name, "stderr")
}

func countAgreesWithDigest(bytes int64, digest, name, stream string) error {
	switch {
	case bytes == 0 && digest != digestOfNoBytes:
		return fmt.Errorf("%w: step %q states no %s bytes beside a digest over some",
			store.ErrInvalid, name, stream)
	case bytes > 0 && digest == digestOfNoBytes:
		return fmt.Errorf("%w: step %q states %d %s bytes beside the digest of none",
			store.ErrInvalid, name, bytes, stream)
	}
	return nil
}
