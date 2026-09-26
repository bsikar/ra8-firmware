// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import "fmt"

// checkStepNamesAttributeEvidence holds a terminal receipt's step names to the
// shape every other message about those steps must carry. A step name is not a
// label on a summary, it is the key the attempt's evidence is filed under:
// LogChunk.StepName carries it for every line a step printed
// (LogChunk.Validate), ArtifactChunk and ArtifactManifest carry it for every
// file a step produced (artifact.go, both through validStepName), and the store
// attributes an uploaded artifact to the step by comparing that name against
// the row it already holds (store/agent_artifacts.go, refusing a chunk whose
// step key differs). The catalog states the same rule at the other end, where
// the steps are declared: a step needs a valid name and no task may name two
// steps alike (catalog.go, validName plus seenSteps).
//
// Only the receipt was exempt. Validate refused an empty name and nothing else,
// so a receipt could state a step named with trailing whitespace or past the
// 128-byte bound, which is a name no chunk and no manifest for that step could
// ever be admitted under, or state two steps sharing one name, which leaves
// every chunk and artifact carrying it attributable to either.
//
// Both shapes break the same reading. A later reader answers "what did this
// step print" and "what did this step produce" by matching that name, and the
// quiet half is the worse one: an unattributable name does not fail anything at
// upload time, it just means the evidence for a step is filed where no one
// looking at that step will find it, and a duplicated name means the evidence
// of two steps is read as the evidence of one.
//
// The rule refuses the shapes rather than repairing them: trimming a name here
// would make the receipt disagree with the chunks the agent already uploaded
// under the untrimmed one.
func checkStepNamesAttributeEvidence(receipt TerminalReceipt) error {
	named := make(map[string]bool, len(receipt.Steps))
	for _, step := range receipt.Steps {
		if !validStepName(step.Name) {
			return fmt.Errorf("%w: step %q is named in a shape its own log chunks cannot carry", ErrInvalid, step.Name)
		}
		if named[step.Name] {
			return fmt.Errorf("%w: receipt names two steps %q, so their evidence cannot be told apart", ErrInvalid, step.Name)
		}
		named[step.Name] = true
	}
	return nil
}
