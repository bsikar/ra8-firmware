// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// checkArtifactSetNamesOneAttempt holds every manifest in a set to the grant
// the set's first entry names.
//
// ValidateArtifactSet answers three questions that only exist across a set,
// and all three are questions about ONE attempt. The count is bounded by
// MaxArtifactsPerAttempt. The byte total is, in that function's own words, the
// total the attempt is allowed to upload. And the one-entry-per-path rule is
// the store's identity for an artifact, which is (attempt_id, path) and
// nothing else (store/agent_artifacts.go, where a close is filed under
// manifest.AttemptID and manifest.Path). Nothing in the function confirmed
// that the entries it was accumulating name one attempt, so all three were
// answered against whatever mixture the caller happened to pass.
//
// The two directions are not the same mistake. A mixed set inflates the count
// and the byte total, so the budgets refuse work that was within every
// attempt's own allowance; and the path rule, keyed on the path alone,
// reports two attempts that each produced build/ra8.elf.map as a duplicate,
// which is the ordinary case for two runs of the same task rather than an
// error. The function does not become stricter by learning this. It becomes
// answerable: a budget is meaningless until the denominator is known.
//
// No caller in this tree passes a mixed set today. The collector builds every
// manifest from one assignment (agent/artifact.go, stream) and the uploader
// refuses anything fenced elsewhere (agent/artifact_upload.go, sameGrant).
// But ValidateArtifactSet is exported and is the set-level rule this package
// offers, and sameGrant runs AFTER it, so the contract the doc comment states
// was one the caller had to know to uphold and the function never checked.
// The rule is stated here so the function enforces its own documented
// contract rather than inheriting it from whoever calls it.
//
// The whole grant is compared, not just the attempt. The four fields travel
// together on every artifact message and validGrant already judges them as a
// unit, so two manifests agreeing on the attempt while disagreeing on the
// fence are two different grants over one attempt, which is exactly what
// fencing exists to tell apart.
func checkArtifactSetNamesOneAttempt(first, manifest ArtifactManifest) error {
	if manifest.AssignmentID != first.AssignmentID ||
		manifest.AttemptID != first.AttemptID ||
		manifest.AssignmentVersion != first.AssignmentVersion ||
		manifest.FencingToken != first.FencingToken {
		return fmt.Errorf("%w: artifact set names more than one grant", ErrInvalid)
	}
	return nil
}
