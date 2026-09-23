// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"fmt"
	"strconv"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

var ErrGuestDrainProtocolUnavailable = errors.New("authenticated runner guest drain protocol is unavailable")

type runnerAdmin interface {
	RunnerByID(context.Context, int) (github.RunnerIdentity, bool, error)
}

// GitHubRunnerObserver verifies runner registration independently. It never
// derives guest health, idleness, or deregistration from a job message.
type GitHubRunnerObserver struct {
	scaleSetID int64
	admin      runnerAdmin
}

func NewGitHubRunnerObserver(scaleSetID int64, admin runnerAdmin) (*GitHubRunnerObserver, error) {
	if scaleSetID <= 0 || admin == nil {
		return nil, errors.New("GitHub runner observer requires a positive scale-set ID and admin client")
	}
	return &GitHubRunnerObserver{scaleSetID: scaleSetID, admin: admin}, nil
}

func (o *GitHubRunnerObserver) Registered(ctx context.Context, vm store.RunnerVM, job github.Job) (RunnerObservation, error) {
	if o == nil || o.admin == nil || ctx == nil || ctx.Err() != nil ||
		vm.ScaleSetID != o.scaleSetID || vm.VMID < 9000 || vm.VMID > 9099 ||
		vm.State != "running" || vm.Generation <= 0 || vm.UnknownOutcome || vm.CleanupRequested ||
		!store.ValidID(vm.ID) || vm.Name != fmt.Sprintf("ra8-lab-ci-%d", vm.VMID) {
		return RunnerObservation{}, errors.New("invalid or mismatched runner registration observation")
	}
	if job.JobID == "" || job.JobID != vm.JobID || job.RunnerRequestID <= 0 ||
		job.RunnerRequestID != vm.RunnerRequestID || job.WorkflowRunID <= 0 ||
		job.WorkflowRunID != vm.WorkflowRunID || job.Repository == "" ||
		job.Repository != vm.Repository || job.WorkflowRef == "" || job.WorkflowRef != vm.WorkflowRef {
		return RunnerObservation{}, errors.New("GitHub job does not match the durable VM reservation")
	}
	expectedName := "runner-" + strconv.Itoa(vm.VMID)
	if job.RunnerID <= 0 || job.RunnerName != expectedName {
		return RunnerObservation{}, errors.New("GitHub job runner does not match the reserved VM identity")
	}
	identity, exists, err := o.admin.RunnerByID(ctx, job.RunnerID)
	if err != nil {
		return RunnerObservation{}, fmt.Errorf("query GitHub runner registration: %w", err)
	}
	if !exists {
		return RunnerObservation{}, errors.New("GitHub runner is not independently registered")
	}
	if identity.ID != job.RunnerID || identity.Name != expectedName {
		return RunnerObservation{}, errors.New("GitHub runner registration differs from the reserved VM identity")
	}
	evidenceID, err := store.NewID()
	if err != nil {
		return RunnerObservation{}, errors.New("create runner registration evidence ID")
	}
	return RunnerObservation{RunnerID: int64(identity.ID), RunnerName: identity.Name,
		EvidenceID: evidenceID, ObservedAt: time.Now().UTC()}, nil
}

func (o *GitHubRunnerObserver) DrainAndDeregister(context.Context, store.RunnerVM) (RunnerObservation, error) {
	return RunnerObservation{}, ErrGuestDrainProtocolUnavailable
}

var _ RunnerObserver = (*GitHubRunnerObserver)(nil)
