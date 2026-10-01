// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// artifactWindow bounds artifact collection on its own, separate from the
// window the terminal receipt travels in. A slow or large upload therefore
// cannot eat the receipt's budget: the plane learns how the attempt ended
// even when the evidence about what it produced did not arrive.
const artifactWindow = 30 * time.Second

// artifactStepName attributes a task's declared outputs to the last step that
// actually ran. Outputs are declared per task while the wire contract fences
// a chunk to a step, and the last step is the one whose failure or deadline
// the files explain. A task that executed nothing produced nothing, so the
// empty name is the signal to collect nothing rather than a name to invent.
func artifactStepName(result executor.Result) string {
	for index := len(result.Steps) - 1; index >= 0; index-- {
		if name := result.Steps[index].Name; name != "" {
			return name
		}
	}
	return ""
}

// artifactsWanted decides whether this attempt's outputs are still worth
// reading off the guest.
//
// A cancelled attempt is deliberately excluded: cancellation is the plane
// asking for the runner back, and spending the fence window pushing up to
// MaxArtifactBytes it never asked for holds a guest somebody is trying to
// reclaim. A deadline is the opposite case and still collects, because the
// files a step left behind are usually the only evidence that explains what
// it was doing when the clock ran out.
func artifactsWanted(result executor.Result, outputs []string) bool {
	return len(outputs) > 0 && !result.Cancelled && artifactStepName(result) != ""
}

// collectAttemptArtifacts carries this attempt's declared outputs to the
// plane and reports what the plane now holds. Nothing declared, nothing
// produced, or a cancelled attempt is a clean no-op, not a failure.
//
// The caller passes a context that outlives the run's own deadline: artifacts
// are evidence about work that has already finished, so collecting them on
// the expired run context would refuse exactly the timed-out attempt whose
// outputs matter most.
func (agent *Agent) collectAttemptArtifacts(ctx context.Context, assignment protocol.Assignment,
	task catalog.Task, result executor.Result, now func() time.Time) ([]protocol.ArtifactManifest, error) {
	if agent == nil {
		return nil, fmt.Errorf("%w: artifact collection needs an agent", ErrUnsafeAssignment)
	}
	if ctx == nil || now == nil {
		return nil, fmt.Errorf("%w: artifact collection needs a context and a clock", ErrUnsafeArtifact)
	}
	if !artifactsWanted(result, task.Outputs) {
		return nil, nil
	}
	uploader, err := newArtifactUploader(agent, assignment)
	if err != nil {
		return nil, err
	}
	collector, err := NewArtifactCollector(agent.root, assignment, uploader.send, now)
	if err != nil {
		return nil, err
	}
	agent.collectStepArtifacts(ctx, uploader, collector, artifactStepName(result), task.Outputs)
	return uploader.status()
}
