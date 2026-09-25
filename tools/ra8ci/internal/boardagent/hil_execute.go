// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/boardclient"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// HILStepRunner runs one catalog-bound step and must stop and reap every child
// before returning. It receives a deadline context and cannot choose commands.
type HILStepRunner func(context.Context, string, catalog.Task, catalog.Step) (int, error)

// RunHILAttempt executes one already-claimed assignment as serialized,
// deadline-bounded board segments, records each step, and persists a terminal
// result even when the caller is cancelled. The runner is responsible for
// implementing only the reviewed operations named by the task.
func (a *Agent) RunHILAttempt(ctx context.Context, token boardclient.LeaseToken, checkoutRoot string,
	assignment store.BoardHILAssignment, safetyMaximum, recoveryMargin time.Duration,
	runner HILStepRunner) (store.BoardHILCompletion, error) {
	if a == nil || ctx == nil || runner == nil || token.BoardID != a.boardID ||
		!store.ValidID(token.LeaseID) || token.Generation == 0 ||
		assignment.Attempt.State != "running" || !store.ValidID(assignment.Attempt.ID) ||
		assignment.Attempt.StartedAt.IsZero() || assignment.Attempt.DeadlineAt.IsZero() ||
		!assignment.Attempt.DeadlineAt.After(assignment.Attempt.StartedAt) ||
		assignment.Task.Scope != "hil" || assignment.Task.HIL == nil ||
		assignment.Task.HIL.BoardID != a.boardID || !assignment.Task.SupportsOS("linux") || catalog.ValidateTask(assignment.Task) != nil ||
		assignment.CatalogSHA256 == "" || safetyMaximum < 0 || safetyMaximum > time.Hour ||
		recoveryMargin < 0 || recoveryMargin > maxBoardOperation {
		return store.BoardHILCompletion{}, ErrInvalidAgent
	}
	attemptCtx, cancel := context.WithDeadline(ctx, assignment.Attempt.DeadlineAt)
	defer cancel()
	// Report this holder alive for as long as the attempt runs. A beat is
	// not a deadline command in either direction, so its outcome never
	// decides anything here: a lease that stopped being ours is already
	// answered by the fence and by every segment call, and a client that
	// cannot beat simply goes unseen. The attempt never outlives its own
	// reporting, so nothing keeps beating at a board this call has left.
	beatCtx, stopBeats := context.WithCancel(attemptCtx)
	beatsStopped := make(chan struct{})
	go func() {
		defer close(beatsStopped)
		_ = a.KeepAlive(beatCtx, token)
	}()
	defer func() {
		stopBeats()
		<-beatsStopped
	}()
	completion := store.BoardHILCompletion{AttemptID: assignment.Attempt.ID, LeaseID: token.LeaseID,
		Generation: token.Generation, Result: "failed", Reason: "HIL task did not complete"}
	steps := make([]store.HILStep, 0, len(assignment.Task.Steps))
	var terminalExitCode *int
	decision := hilspec.Decision{}
	executionErr := error(nil)
	if assignment.HILTiming == nil {
		executionErr = fmt.Errorf("%w: server did not pin HIL timing evidence", ErrInvalidAgent)
	} else {
		decision = assignment.HILTiming.Decision
		spec, err := hilspec.Load(checkoutRoot, assignment.Task.HIL.ManifestPath)
		if err != nil {
			executionErr = err
		} else if spec.Mode != hilspec.Mode(assignment.Task.HIL.Mode) || spec.TimeoutDeclared != assignment.Task.HIL.TimeoutDeclared || spec.TimeoutSeconds != assignment.Task.HIL.TimeoutSeconds || spec.SafetyMaximumSeconds != assignment.Task.HIL.SafetyMaximumSeconds {
			executionErr = fmt.Errorf("%w: pinned HIL timing differs from manifest", catalog.ErrInvalidCatalog)
		} else if assignment.HILTiming.Workload.ManifestPath != assignment.Task.HIL.ManifestPath || assignment.HILTiming.Workload.BoardModel != assignment.Task.HIL.BoardModel || assignment.HILTiming.Workload.ProgramFamily != assignment.Task.HIL.ProgramFamily || assignment.HILTiming.Workload.Mode != hilspec.Mode(assignment.Task.HIL.Mode) {
			executionErr = hilspec.ErrInvalidHistory
		}
	}
	if executionErr == nil {
		executionErr = validateHILSafetyMaximum(decision, safetyMaximum)
	}
	if executionErr == nil && decision.ValidityWindow > assignment.Attempt.DeadlineAt.Sub(assignment.Attempt.StartedAt) {
		executionErr = fmt.Errorf("%w: HIL observation budget exceeds persisted attempt deadline", ErrInvalidAgent)
	}
	if executionErr != nil {
		now := time.Now().UTC()
		if now.Before(assignment.Attempt.StartedAt) {
			now = assignment.Attempt.StartedAt
		}
		state := "failed"
		if errors.Is(executionErr, context.DeadlineExceeded) || errors.Is(attemptCtx.Err(), context.DeadlineExceeded) {
			state = "timed_out"
		} else if errors.Is(attemptCtx.Err(), context.Canceled) {
			state = "cancelled"
		}
		steps = append(steps, store.HILStep{Key: assignment.Task.Steps[0].Name, StartedAt: now,
			EndedAt: now.Add(time.Nanosecond), DurationNS: int64(time.Nanosecond), State: state})
	}
	for stepIndex, step := range assignment.Task.Steps {
		if executionErr != nil {
			break
		}
		if err := attemptCtx.Err(); err != nil {
			executionErr = err
			break
		}
		bound := time.Duration(assignment.Task.DeadlineSeconds) * time.Second
		segmentRecoveryMargin := recoveryMargin
		if step.Name == assignment.Task.HIL.ObservationStep {
			bound = decision.ValidityWindow
			segmentRecoveryMargin += decision.FlashRestoreBound
		}
		remaining := time.Until(assignment.Attempt.DeadlineAt)
		if bound > remaining {
			bound = remaining
		}
		stepStarted := time.Now().UTC()
		childCode := -1
		runErr := error(nil)
		_, segmentErr := a.RunSegment(attemptCtx, token, assignment.Attempt.ID, step.Name, bound,
			segmentRecoveryMargin, func(stepCtx context.Context) error {
				var err error
				childCode, err = runner(stepCtx, checkoutRoot, assignment.Task, step)
				if childCode >= 0 {
					codeCopy := childCode
					terminalExitCode = &codeCopy
				}
				if err != nil {
					return err
				}
				if childCode != 0 {
					return fmt.Errorf("HIL step %s exited with status %d", step.Name, childCode)
				}
				return nil
			})
		runErr = segmentErr
		stepEnded := time.Now().UTC()
		if stepEnded.Before(stepStarted) {
			stepEnded = stepStarted
		}
		state := "succeeded"
		if errors.Is(runErr, context.DeadlineExceeded) || errors.Is(attemptCtx.Err(), context.DeadlineExceeded) {
			state = "timed_out"
		} else if errors.Is(attemptCtx.Err(), context.Canceled) {
			state = "cancelled"
		} else if runErr != nil || childCode != 0 {
			state = "failed"
		}
		codeCopy := childCode
		var childExit *int
		if childCode >= 0 {
			childExit = &codeCopy
		}
		duration := stepEnded.Sub(stepStarted)
		if duration <= 0 {
			duration = time.Nanosecond
		}
		steps = append(steps, store.HILStep{Key: step.Name, StartedAt: stepStarted,
			EndedAt: stepEnded, DurationNS: duration.Nanoseconds(), State: state, ChildExitCode: childExit})
		if runErr != nil {
			executionErr = runErr
			break
		}
		snapshot, err := a.Reconcile(attemptCtx)
		if err != nil {
			executionErr = err
			break
		}
		if stepIndex+1 < len(assignment.Task.Steps) && (snapshot.Phase == board.YieldRequested || snapshot.Phase == board.Draining) {
			executionErr = errors.New("board lease yielded at HIL checkpoint")
			completion.Result = "preempted"
			completion.Reason = executionErr.Error()
			break
		}
	}
	completion.Steps = steps
	if executionErr == nil && len(steps) == len(assignment.Task.Steps) {
		completion.Result, completion.Reason, completion.EvidenceComplete = "succeeded", "", true
		zero := 0
		completion.ChildExitCode = &zero
	} else if completion.Result != "preempted" {
		completion.ChildExitCode = terminalExitCode
		completion.Reason = "HIL attempt failed"
		if executionErr != nil {
			completion.Reason = executionErr.Error()
			if len(completion.Reason) > 1024 {
				completion.Reason = completion.Reason[:1024]
			}
		}
		if errors.Is(executionErr, context.DeadlineExceeded) ||
			errors.Is(attemptCtx.Err(), context.DeadlineExceeded) {
			completion.Result, completion.HitDeadline = "timed_out", true
		} else if errors.Is(attemptCtx.Err(), context.Canceled) {
			completion.Result = "cancelled"
		}
	}
	completeCtx, completeCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer completeCancel()
	completeErr := a.CompleteHILAttempt(completeCtx, token, assignment, completion)
	return completion, completeErr
}

func validateHILSafetyMaximum(decision hilspec.Decision, safetyMaximum time.Duration) error {
	if safetyMaximum < 0 || safetyMaximum > time.Hour {
		return ErrInvalidAgent
	}
	if safetyMaximum > 0 && decision.ValidityWindow > safetyMaximum {
		return fmt.Errorf("%w: server HIL validity window exceeds the local safety maximum", ErrInvalidAgent)
	}
	return nil
}
