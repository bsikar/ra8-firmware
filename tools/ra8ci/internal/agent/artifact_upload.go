// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// artifactChunkPath and artifactManifestPath are the two plane endpoints an
// artifact travels over. They are derived from the grant's attempt, never
// from anything a task produced, so a declared output cannot steer a request.
func artifactChunkPath(attemptID string) string {
	return "/v1/attempts/" + attemptID + "/artifacts/chunk"
}

func artifactManifestPath(attemptID string) string {
	return "/v1/attempts/" + attemptID + "/artifacts/manifest"
}

// artifactUploader carries a step's collected outputs to the plane. It is the
// concrete SendArtifactChunk the collector takes, plus the close that ends
// each artifact, and it records the first failure so an attempt reports
// evidence it actually delivered.
type artifactUploader struct {
	agent      *Agent
	assignment protocol.Assignment
	uploaded   []protocol.ArtifactManifest
	err        error
}

func newArtifactUploader(agent *Agent, assignment protocol.Assignment) (*artifactUploader, error) {
	if agent == nil {
		return nil, fmt.Errorf("%w: uploader needs an agent", ErrUnsafeAssignment)
	}
	if err := assignment.Validate(); err != nil {
		return nil, err
	}
	return &artifactUploader{agent: agent, assignment: assignment}, nil
}

// send is the SendArtifactChunk the collector calls per chunk. The chunk is
// validated again here rather than trusted from the collector: this is the
// last place before the wire, and the plane refuses an invalid chunk anyway.
// A chunk the plane never answered for is offered again: a byte-identical
// replay is ArtifactDuplicate and the same 200 as the first, which is the
// outcome the store keeps precisely so an agent's retry is safe.
func (uploader *artifactUploader) send(ctx context.Context, chunk protocol.ArtifactChunk) error {
	if uploader == nil || uploader.agent == nil {
		return fmt.Errorf("%w: artifact uploader", ErrUnsafeAssignment)
	}
	if err := chunk.Validate(); err != nil {
		return fmt.Errorf("%w: artifact chunk", ErrUnsafeArtifact)
	}
	if err := uploader.sameGrant(chunk.AssignmentID, chunk.AttemptID,
		chunk.AssignmentVersion, chunk.FencingToken); err != nil {
		return err
	}
	return uploader.agent.acceptEvidence(ctx, uploader.assignment,
		artifactChunkPath(uploader.assignment.AttemptID), chunk)
}

// Close ends every artifact the collector produced, in the order it produced
// them. A manifest is only sent after its own chunks, so the plane never
// holds a close for bytes it has not seen. A close the plane never answered
// for is offered again: a duplicate manifest describing the same bytes is the
// same 200 as the first, so a retry never has to decide whether it landed.
func (uploader *artifactUploader) Close(ctx context.Context, manifests []protocol.ArtifactManifest) error {
	if uploader == nil || uploader.agent == nil {
		return fmt.Errorf("%w: artifact uploader", ErrUnsafeAssignment)
	}
	if err := protocol.ValidateArtifactSet(manifests); err != nil {
		return fmt.Errorf("%w: artifact set", ErrUnsafeArtifact)
	}
	for _, manifest := range manifests {
		if err := uploader.sameGrant(manifest.AssignmentID, manifest.AttemptID,
			manifest.AssignmentVersion, manifest.FencingToken); err != nil {
			return err
		}
		if err := uploader.agent.acceptEvidence(ctx, uploader.assignment,
			artifactManifestPath(uploader.assignment.AttemptID), manifest); err != nil {
			return err
		}
		uploader.uploaded = append(uploader.uploaded, manifest)
	}
	return nil
}

// sameGrant refuses to upload anything fenced to a different grant than the
// one this attempt holds, so a stale collector cannot write over a later
// attempt's evidence.
func (uploader *artifactUploader) sameGrant(assignmentID, attemptID string, version, fence int64) error {
	if assignmentID != uploader.assignment.AssignmentID ||
		attemptID != uploader.assignment.AttemptID ||
		version != uploader.assignment.AssignmentVersion ||
		fence != uploader.assignment.FencingToken {
		return fmt.Errorf("%w: artifact is fenced to another grant", ErrUnsafeArtifact)
	}
	return nil
}

// record keeps the first collection failure. Artifacts are evidence about a
// run that already happened, so a failure here fails the attempt's evidence,
// never the run: the terminal receipt still goes out.
func (uploader *artifactUploader) record(err error) {
	if err != nil && uploader.err == nil {
		uploader.err = err
	}
}

// status reports what the plane now holds and the first failure, if any.
func (uploader *artifactUploader) status() ([]protocol.ArtifactManifest, error) {
	if uploader == nil {
		return nil, nil
	}
	return uploader.uploaded, uploader.err
}

// collectStepArtifacts streams one step's declared outputs and closes them.
// Collection is best effort by design: a missing file is not an error (the
// collector skips it), and a genuine failure is recorded rather than
// returned, because losing the terminal receipt would cost more evidence
// than the artifact was worth.
func (agent *Agent) collectStepArtifacts(ctx context.Context, uploader *artifactUploader, collector *ArtifactCollector, stepName string, outputs []string) {
	if uploader == nil || collector == nil || len(outputs) == 0 {
		return
	}
	manifests, err := collector.Collect(ctx, stepName, outputs)
	if err != nil {
		uploader.record(err)
		return
	}
	uploader.record(uploader.Close(ctx, manifests))
}
