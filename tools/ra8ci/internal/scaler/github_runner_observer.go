// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package scaler

import (
	"context"
	"errors"
	"fmt"
	"strconv"
	"time"

	"github.com/actions/scaleset"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/github"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

type runnerAdmin interface {
	RunnerByID(context.Context, int) (github.RunnerIdentity, bool, error)
	RemoveRunner(context.Context, int) error
}

// GitHubRunnerObserver uses the durable terminal job event as proof that the
// runner has no active job, then independently deregisters and verifies its
// absence through GitHub's scale-set-scoped runner administration API.
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

func (o *GitHubRunnerObserver) DrainAndDeregister(ctx context.Context, vm store.RunnerVM, job github.Job) (RunnerObservation, error) {
	if o == nil || o.admin == nil || ctx == nil || ctx.Err() != nil ||
		vm.ScaleSetID != o.scaleSetID || vm.VMID < 9000 || vm.VMID > 9099 ||
		(vm.State != "draining" && vm.State != "stopped") || !vm.CleanupRequested || vm.UnknownOutcome ||
		!store.ValidID(vm.ID) || vm.Name != fmt.Sprintf("ra8-lab-ci-%d", vm.VMID) ||
		vm.ExternalRunnerID <= 0 || vm.ExternalRunnerName != "runner-"+strconv.Itoa(vm.VMID) {
		return RunnerObservation{}, errors.New("invalid durable runner identity for drain")
	}
	if job.Kind != scaleset.MessageTypeJobCompleted || job.JobID != vm.JobID || job.RunnerRequestID != vm.RunnerRequestID ||
		job.WorkflowRunID != vm.WorkflowRunID || job.Repository != vm.Repository || job.WorkflowRef != vm.WorkflowRef ||
		job.RunnerID != int(vm.ExternalRunnerID) || job.RunnerName != vm.ExternalRunnerName || job.Result == "" ||
		job.FinishTime.IsZero() || job.FinishTime.After(time.Now().Add(time.Second)) {
		return RunnerObservation{}, errors.New("terminal job event does not prove completion for this runner")
	}
	identity, exists, err := o.admin.RunnerByID(ctx, job.RunnerID)
	if err != nil {
		return RunnerObservation{}, fmt.Errorf("verify runner before deregistration: %w", err)
	}
	if exists && (identity.ID != job.RunnerID || identity.Name != vm.ExternalRunnerName) {
		return RunnerObservation{}, errors.New("refusing to deregister a foreign GitHub runner")
	}
	if exists {
		if err := o.admin.RemoveRunner(ctx, job.RunnerID); err != nil {
			return RunnerObservation{}, fmt.Errorf("deregister completed GitHub runner: %w", err)
		}
	}
	_, remains, err := o.admin.RunnerByID(ctx, job.RunnerID)
	if err != nil {
		return RunnerObservation{}, fmt.Errorf("verify runner deregistration: %w", err)
	}
	if remains {
		return RunnerObservation{}, errors.New("GitHub runner remains registered after deregistration")
	}
	evidenceID, err := store.NewID()
	if err != nil {
		return RunnerObservation{}, errors.New("create runner drain evidence ID")
	}
	return RunnerObservation{RunnerID: vm.ExternalRunnerID, RunnerName: vm.ExternalRunnerName,
		EvidenceID: evidenceID, ObservedAt: time.Now().UTC(), Drained: true, NoActiveJob: true,
		RunnerDeregistered: true}, nil
}

var _ RunnerObserver = (*GitHubRunnerObserver)(nil)
var _ runnerAdmin = (*github.Session)(nil)
