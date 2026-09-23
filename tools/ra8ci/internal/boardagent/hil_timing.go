// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

type HILHistoryClient interface {
	HILObservations(context.Context, string, catalog.Task) (hilspec.Workload, []hilspec.HistoricalObservation, error)
}

type observationRows []hilspec.HistoricalObservation

func (rows observationRows) Observations(context.Context, hilspec.Workload) ([]hilspec.HistoricalObservation, error) {
	return rows, nil
}

// HILTimingDecision applies the manifest fallback and evidence-backed timing
// estimator to the board's currently approved fixture cohort. The caller must
// still reserve the returned validity and restore bounds under a live lease
// before touching hardware.
func (a *Agent) HILTimingDecision(ctx context.Context, checkoutRoot string, task catalog.Task,
	safetyMaximum time.Duration) (hilspec.Decision, error) {
	if a == nil || ctx == nil || checkoutRoot == "" || task.Scope != "hil" ||
		task.HIL == nil || task.BoardPolicy != "exclusive" || task.HIL.BoardID != a.boardID ||
		catalog.ValidateTask(task) != nil || task.HIL.FlashRestoreSeconds < 1 {
		return hilspec.Decision{}, ErrInvalidAgent
	}
	client, ok := a.client.(HILHistoryClient)
	if !ok {
		return hilspec.Decision{}, ErrInvalidAgent
	}
	spec, err := hilspec.Load(checkoutRoot, task.HIL.ManifestPath)
	if err != nil {
		return hilspec.Decision{}, err
	}
	if spec.Mode != hilspec.Mode(task.HIL.Mode) {
		return hilspec.Decision{}, hilspec.ErrInvalidManifest
	}
	workload, rows, err := client.HILObservations(ctx, a.boardID, task)
	if err != nil {
		return hilspec.Decision{}, err
	}
	if workload.ManifestPath != task.HIL.ManifestPath || workload.BoardModel != task.HIL.BoardModel ||
		workload.ProgramFamily != task.HIL.ProgramFamily || workload.Mode != hilspec.Mode(task.HIL.Mode) ||
		workload.FixtureRevision == "" || workload.ProfileSHA256 == "" {
		return hilspec.Decision{}, hilspec.ErrInvalidHistory
	}
	return hilspec.Decide(ctx, spec, workload, observationRows(rows), hilspec.Options{
		FlashRestoreBound: time.Duration(task.HIL.FlashRestoreSeconds) * time.Second,
		SafetyMaximum:     safetyMaximum,
	})
}
