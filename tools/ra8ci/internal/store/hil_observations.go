// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"path"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/jackc/pgx/v5"
)

// RecordHILObservation derives workload identity from the immutable task
// definition and the board session that encloses its observation step.
func (s *Store) RecordHILObservation(ctx context.Context, in HILObservationInput) error {
	if s == nil || s.pool == nil || ctx == nil || !ValidID(in.AttemptID) ||
		strings.TrimSpace(in.ActorID) != in.ActorID || in.ActorID == "" || len(in.ActorID) > 256 ||
		in.StepKey == "" || len(in.StepKey) > 128 {
		return fmt.Errorf("%w: HIL observation input", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("%w: begin HIL observation: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	var runID string
	err = tx.QueryRow(ctx, `SELECT t.run_id::text FROM task_attempts a
		JOIN tasks t ON t.id=a.task_id JOIN task_steps st ON st.attempt_id=a.id AND st.step_key=$2
		WHERE a.id=$1`, in.AttemptID, in.StepKey).Scan(&runID)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("%w: locate HIL observation: %v", ErrUnavailable, err)
	}
	if err := withRunLock(ctx, tx, runID); err != nil {
		return fmt.Errorf("%w: lock HIL run: %v", ErrUnavailable, err)
	}
	var scope, attemptState, stepState, phase, boardLeaseID string
	var taskArguments []byte
	var evidenceComplete, hitDeadline bool
	var durationNS int64
	var stepStarted, stepEnded time.Time
	err = tx.QueryRow(ctx, `SELECT t.scope,t.arguments,a.state,a.evidence_complete,a.hit_deadline,
		COALESCE(a.board_lease_id::text,''),st.phase,st.state,st.duration_ns,st.started_at,st.ended_at
		FROM task_attempts a JOIN tasks t ON t.id=a.task_id
		JOIN task_steps st ON st.attempt_id=a.id AND st.step_key=$2
		WHERE a.id=$1 FOR UPDATE OF a`, in.AttemptID, in.StepKey).Scan(
		&scope, &taskArguments, &attemptState, &evidenceComplete, &hitDeadline, &boardLeaseID,
		&phase, &stepState, &durationNS, &stepStarted, &stepEnded)
	if errors.Is(err, pgx.ErrNoRows) {
		return ErrNotFound
	}
	if err != nil {
		return fmt.Errorf("%w: read completed HIL observation: %v", ErrUnavailable, err)
	}
	if scope != "hil" || boardLeaseID == "" || attemptState == "running" || attemptState == "issued" ||
		phase != "hil_observe" || durationNS <= 0 || durationNS > int64(time.Hour) {
		return fmt.Errorf("%w: HIL observation is not a completed observation step", ErrConflict)
	}
	var arguments struct {
		Arguments []string         `json:"argv"`
		HIL       *catalog.HILTask `json:"hil"`
	}
	decoder := json.NewDecoder(bytes.NewReader(taskArguments))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&arguments); err != nil || arguments.HIL == nil ||
		catalog.ValidateHILTaskMetadata(*arguments.HIL) != nil ||
		arguments.HIL.ObservationStep != in.StepKey {
		return fmt.Errorf("%w: persisted HIL task contract is invalid", ErrConflict)
	}
	workload, err := hilWorkloadForSession(ctx, tx, boardLeaseID, *arguments.HIL, stepStarted, stepEnded)
	if err != nil {
		return err
	}
	id, err := NewID()
	if err != nil {
		return fmt.Errorf("%w: %v", errEntropy, err)
	}
	succeeded := attemptState == "succeeded" && stepState == "succeeded" && evidenceComplete && !hitDeadline
	timedOut := attemptState == "timed_out" || hitDeadline || stepState == "timed_out"
	tag, err := tx.Exec(ctx, `INSERT INTO hil_observations
		(id,attempt_id,manifest_path,board_model,fixture_revision,profile_sha256,program_family,mode,
		 step_key,duration_ns,succeeded,evidence_complete,timed_out)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13)
		ON CONFLICT (attempt_id,manifest_path,board_model,fixture_revision,profile_sha256,program_family,mode)
		DO NOTHING`, id, in.AttemptID, workload.ManifestPath, workload.BoardModel,
		workload.FixtureRevision, workload.ProfileSHA256, workload.ProgramFamily,
		string(workload.Mode), in.StepKey, durationNS, succeeded, evidenceComplete, timedOut)
	if err != nil {
		return fmt.Errorf("%w: insert HIL observation: %v", ErrUnavailable, err)
	}
	if tag.RowsAffected() == 0 {
		var existingStep string
		var existingDuration int64
		var existingSucceeded, existingEvidence, existingTimedOut bool
		err := tx.QueryRow(ctx, `SELECT step_key,duration_ns,succeeded,evidence_complete,timed_out
			FROM hil_observations WHERE attempt_id=$1 AND manifest_path=$2 AND board_model=$3
			AND fixture_revision=$4 AND profile_sha256=$5 AND program_family=$6 AND mode=$7`,
			in.AttemptID, workload.ManifestPath, workload.BoardModel, workload.FixtureRevision,
			workload.ProfileSHA256, workload.ProgramFamily, string(workload.Mode)).
			Scan(&existingStep, &existingDuration, &existingSucceeded, &existingEvidence, &existingTimedOut)
		if err != nil || existingStep != in.StepKey || existingDuration != durationNS ||
			existingSucceeded != succeeded || existingEvidence != evidenceComplete || existingTimedOut != timedOut {
			return fmt.Errorf("%w: HIL observation retry changed its evidence", ErrConflict)
		}
		return tx.Commit(ctx)
	}
	reason := map[string]any{"attempt_id": in.AttemptID, "step_key": in.StepKey,
		"workload": workload, "duration_ns": durationNS, "succeeded": succeeded,
		"evidence_complete": evidenceComplete, "timed_out": timedOut}
	if err := appendAudit(ctx, tx, in.ActorID, "hil.observation.recorded",
		"hil_observation", id, "ok", "", "recorded", runID, reason); err != nil {
		return fmt.Errorf("%w: audit HIL observation: %v", ErrUnavailable, err)
	}
	if err := appendEvent(ctx, tx, runID, "hil.observation.recorded", reason); err != nil {
		return fmt.Errorf("%w: event HIL observation: %v", ErrUnavailable, err)
	}
	return tx.Commit(ctx)
}

func hilWorkloadForSession(ctx context.Context, tx pgx.Tx, leaseID string,
	definition catalog.HILTask, startedAt, endedAt time.Time) (hilspec.Workload, error) {
	if startedAt.IsZero() || endedAt.IsZero() || endedAt.Before(startedAt) {
		return hilspec.Workload{}, fmt.Errorf("%w: HIL observation interval", ErrConflict)
	}
	rows, err := tx.Query(ctx, `SELECT fixture_revision,profile_sha256 FROM board_sessions
		WHERE lease_id=$1 AND board_id=$2 AND started_at<=$3 AND (ended_at IS NULL OR ended_at>=$4) AND profile_sha256 IS NOT NULL
		ORDER BY started_at DESC LIMIT 2`, leaseID, definition.BoardID, startedAt, endedAt)
	if err != nil {
		return hilspec.Workload{}, fmt.Errorf("%w: find HIL board session: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	if !rows.Next() {
		if err := rows.Err(); err != nil {
			return hilspec.Workload{}, fmt.Errorf("%w: read HIL board session: %v", ErrUnavailable, err)
		}
		return hilspec.Workload{}, fmt.Errorf("%w: no board session encloses the HIL observation", ErrConflict)
	}
	var fixtureRevision, profileSHA256 string
	if err := rows.Scan(&fixtureRevision, &profileSHA256); err != nil {
		return hilspec.Workload{}, fmt.Errorf("%w: scan HIL board session: %v", ErrUnavailable, err)
	}
	if rows.Next() {
		return hilspec.Workload{}, fmt.Errorf("%w: ambiguous board session for HIL observation", ErrConflict)
	}
	if err := rows.Err(); err != nil {
		return hilspec.Workload{}, fmt.Errorf("%w: iterate HIL board sessions: %v", ErrUnavailable, err)
	}
	workload := hilspec.Workload{ManifestPath: definition.ManifestPath, BoardModel: definition.BoardModel,
		FixtureRevision: fixtureRevision, ProfileSHA256: profileSHA256,
		ProgramFamily: definition.ProgramFamily, Mode: hilspec.Mode(definition.Mode)}
	if !validHILWorkload(workload) {
		return hilspec.Workload{}, fmt.Errorf("%w: invalid HIL board session identity", ErrConflict)
	}
	return workload, nil
}

// Observations implements hilspec.ObservationSource with an exact cohort match.
func (s *Store) Observations(ctx context.Context, workload hilspec.Workload) ([]hilspec.HistoricalObservation, error) {
	if s == nil || s.pool == nil || ctx == nil || !validHILWorkload(workload) {
		return nil, fmt.Errorf("%w: HIL workload", ErrInvalid)
	}
	rows, err := s.pool.Query(ctx, `SELECT duration_ns,succeeded,evidence_complete,timed_out
		FROM hil_observations WHERE manifest_path=$1 AND board_model=$2 AND fixture_revision=$3
		AND profile_sha256=$4 AND program_family=$5 AND mode=$6
		ORDER BY observed_at DESC,id DESC LIMIT 10000`, workload.ManifestPath, workload.BoardModel,
		workload.FixtureRevision, workload.ProfileSHA256, workload.ProgramFamily, string(workload.Mode))
	if err != nil {
		return nil, fmt.Errorf("%w: query HIL observations: %v", ErrUnavailable, err)
	}
	defer rows.Close()
	result := make([]hilspec.HistoricalObservation, 0)
	for rows.Next() {
		var durationNS int64
		var row hilspec.HistoricalObservation
		if err := rows.Scan(&durationNS, &row.Succeeded, &row.EvidenceComplete, &row.TimedOut); err != nil {
			return nil, fmt.Errorf("%w: scan HIL observations: %v", ErrUnavailable, err)
		}
		row.Workload = workload
		row.Duration = time.Duration(durationNS)
		result = append(result, row)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("%w: iterate HIL observations: %v", ErrUnavailable, err)
	}
	return result, nil
}

// BoardHILObservations exposes only timing cohorts selected from a validated
// server catalog task and the operator-approved profile for this board.
func (s *Store) BoardHILObservations(ctx context.Context, actor BoardActor,
	definition catalog.HILTask) (hilspec.Workload, []hilspec.HistoricalObservation, error) {
	if s == nil || s.pool == nil || ctx == nil || actor.kind != "board_agent" ||
		actor.role != "board_agent" || !validBoardID(actor.boardID) ||
		catalog.ValidateHILTaskMetadata(definition) != nil || definition.BoardID != actor.boardID {
		return hilspec.Workload{}, nil, fmt.Errorf("%w: board HIL history arguments", ErrInvalid)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return hilspec.Workload{}, nil, fmt.Errorf("%w: begin board HIL history: %v", ErrUnavailable, err)
	}
	defer func() { _ = tx.Rollback(ctx) }()
	if err := revalidateBoardActor(ctx, tx, actor); err != nil {
		return hilspec.Workload{}, nil, err
	}
	var fixtureRevision, profileSHA256 string
	err = tx.QueryRow(ctx, `SELECT fixture_revision,profile_sha256
		FROM board_fixture_profiles WHERE board_id=$1`, actor.boardID).Scan(&fixtureRevision, &profileSHA256)
	if errors.Is(err, pgx.ErrNoRows) {
		return hilspec.Workload{}, nil, ErrNotFound
	}
	if err != nil {
		return hilspec.Workload{}, nil, fmt.Errorf("%w: read approved HIL fixture profile: %v", ErrUnavailable, err)
	}
	if err := tx.Commit(ctx); err != nil {
		return hilspec.Workload{}, nil, fmt.Errorf("%w: commit board HIL history identity: %v", ErrUnavailable, err)
	}
	workload := hilspec.Workload{ManifestPath: definition.ManifestPath, BoardModel: definition.BoardModel,
		FixtureRevision: fixtureRevision, ProfileSHA256: profileSHA256,
		ProgramFamily: definition.ProgramFamily, Mode: hilspec.Mode(definition.Mode)}
	if !validHILWorkload(workload) {
		return hilspec.Workload{}, nil, fmt.Errorf("%w: approved HIL workload identity is invalid", ErrConflict)
	}
	observations, err := s.Observations(ctx, workload)
	return workload, observations, err
}

func validateHILTimingEvidence(evidence HILTimingEvidence, definition catalog.HILTask, taskMaximumSeconds int) bool {
	workload := evidence.Workload
	decision := evidence.Decision
	if !validHILWorkload(workload) || workload.ManifestPath != definition.ManifestPath ||
		workload.BoardModel != definition.BoardModel || workload.ProgramFamily != definition.ProgramFamily ||
		workload.Mode != hilspec.Mode(definition.Mode) || taskMaximumSeconds < 1 ||
		decision.ValidityWindow <= 0 || decision.ValidityWindow%time.Second != 0 ||
		decision.ValidityWindow > time.Duration(taskMaximumSeconds)*time.Second ||
		decision.FlashRestoreBound != time.Duration(definition.FlashRestoreSeconds)*time.Second ||
		decision.SafetyMaximum < decision.ValidityWindow || decision.SafetyMaximum > time.Hour ||
		decision.Samples < 0 || decision.Samples > 10000 || decision.RejectedRows < 0 ||
		decision.RejectedRows > 10000 {
		return false
	}
	switch decision.Source {
	case "default", "hil.conf":
		return decision.Samples < 5
	case "observed", "observed-capped":
		return decision.Samples >= 5
	default:
		return false
	}
}

func validHILWorkload(workload hilspec.Workload) bool {
	if workload.ManifestPath == "" || path.Clean(workload.ManifestPath) != workload.ManifestPath ||
		path.Base(workload.ManifestPath) != "hil.conf" || !strings.HasPrefix(workload.ManifestPath, "examples/") ||
		strings.Contains(workload.ManifestPath, "..") || strings.Contains(workload.ManifestPath, "\\") ||
		len(workload.ManifestPath) > 512 || workload.BoardModel == "" ||
		strings.TrimSpace(workload.BoardModel) != workload.BoardModel || len(workload.BoardModel) > 128 ||
		workload.FixtureRevision == "" || strings.TrimSpace(workload.FixtureRevision) != workload.FixtureRevision ||
		len(workload.FixtureRevision) > 128 || !hexSHA.MatchString(workload.ProfileSHA256) ||
		workload.ProgramFamily == "" || strings.TrimSpace(workload.ProgramFamily) != workload.ProgramFamily ||
		len(workload.ProgramFamily) > 128 {
		return false
	}
	switch workload.Mode {
	case hilspec.ModeAlive, hilspec.ModeUARTScrape, hilspec.ModeRTTScrape,
		hilspec.ModeJLinkMemprobe, hilspec.ModeEthernetTCP, hilspec.ModeC6CameraLivestream:
		return true
	default:
		return false
	}
}

var _ hilspec.ObservationSource = (*Store)(nil)
