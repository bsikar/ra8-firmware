// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// closedArtifactMatchesTheManifest judges a manifest that arrives after the
// artifact it closes is already closed.
//
// An agent retries, so the same manifest arrives more than once and the second
// arrival has to be answered without changing what the plane holds. That answer
// is ArtifactDuplicate, and the word means one thing in this package: the far
// end re-presented evidence already on file, unchanged. Saying it about a
// manifest that states something else tells an agent its statement was filed
// when a different one is on file, and the agent stops retrying on the strength
// of it.
//
// The closed path compared the digest, the byte total and the truncation flag,
// and said nothing about the chunk count. The open path refuses a manifest
// whose FinalSequence is not the number of chunks uploaded (artifactClose,
// beside the total and the digest), so the same disagreement was refused or
// blessed depending only on whether the close had already happened. Arrival
// order is not evidence, and a rule that changes verdict with it is not a rule.
//
// The count is held against chunk_count on file rather than against the
// manifest's own arithmetic, because that column is what the plane stored and
// what a reassembly reads. A digest match does not cover it: the digest is over
// the bytes, and the same bytes can be claimed as a different number of chunks.
func closedArtifactMatchesTheManifest(manifest protocol.ArtifactManifest, artifact heldArtifact) error {
	if artifact.SHA256 != manifest.SHA256 || artifact.TotalBytes != manifest.TotalBytes ||
		artifact.Truncated != manifest.Truncated {
		return fmt.Errorf("%w: artifact was closed on other evidence", ErrConflict)
	}
	if artifact.Chunks != manifest.FinalSequence {
		return fmt.Errorf("%w: artifact was closed on another chunk count", ErrConflict)
	}
	return nil
}
