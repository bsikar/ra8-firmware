// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

// digestOfNothing is SHA-256 over no bytes at all. It is a well-formed digest
// and it is the one digest anybody can state without having measured a thing.
const digestOfNothing = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

// evidenceWasMeasured holds a receipt's evidence digest to a reading that was
// actually taken.
//
// The verifier judged EvidenceSHA256 for hex shape alone. Both sides below it
// already refuse an empty reading: the observer marshals its evidence document
// and returns ErrObservationAbsent when the encoding is empty
// (profile_linux.go), and the producer refuses an observation carrying no
// evidence at all (validObservation: len(o.Evidence) > 0). The verifier is the
// side that decides, and it accepted a receipt saying "the fixture is neutral,
// and here is my evidence: nothing".
//
// The evidence bytes never travel; the digest is all the verifier ever sees of
// them. So there is exactly one shape left that it can still recognise as
// unmeasured, and it is the shape that matters: the digest a signer reaches for
// when it computes over an empty buffer, which is what a producer that skipped
// the observation and still had a key would hash.
//
// This tree treats the empty digest the other way round for logs, and the
// difference is the point. A step may legitimately print nothing, so
// server/offline_step_evidence.go says the digest of nothing is still a digest
// and accepts it. A neutral observation may not legitimately be nothing: its
// evidence is a JSON document this package builds itself, over a profile that
// must carry at least two identity signals, three state signals and one sensor,
// so a real reading never encodes to zero bytes. The empty digest is not a
// sparse reading here, it is the absence of one.
func evidenceWasMeasured(digest string) bool {
	return validSHA256(digest) && digest != digestOfNothing
}
