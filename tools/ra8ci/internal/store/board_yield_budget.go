package store

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/board"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/jackc/pgx/v5"
)

// Deriving the cohort and the declared bounds a yield is planned against.
//
// The estimator (#1526), the sample recorder (#1527), the plan (#1528), the
// endpoint (#1529), the recorded promise (#1530, #1531), the persisted
// samples (#1532), the history read (#1533) and the declared bounds in the
// catalog (#1534) are all in. The endpoint still answers 503 in production,
// because server.BoardYieldBudget has no implementation that reads any of it:
// the cohort had nowhere to come from.
//
// This is that derivation, and every field of it comes from state a holder
// cannot choose. The board names itself; the approved fixture profile names
// the revision; the run and the task name the catalog digest and the task;
// the reviewed HIL definition persisted with the task names the board model
// and what giving the board up costs. Nothing here is read from a request.

// heldYieldWork is the work a board's live lease holds, as one row. Separated
// from the query so the judgement below is testable without a database.
type heldYieldWork struct {
	// ApprovedRevision is the operator-registered fixture revision from
	// board_fixture_profiles. SessionRevision is what the live session
	// records. They must agree.
	ApprovedRevision string
	SessionRevision  string
	TaskName         string
	CatalogDigest    string
	ImageSHA256      string
	// HIL is the reviewed HIL definition persisted with the task, as raw
	// JSON exactly as tasks.arguments holds it.
	HIL json.RawMessage
}

// yieldWorkBudget judges one held-work row into a cohort and the bounds its
// task declares, or refuses it.
//
// A refusal here leaves the yield door closed for this board, which is the
// right answer: an ETA estimated over a cohort assembled from a value that
// does not describe the work would be worse than no ETA, because a person
// reads it as a promise either way.
func yieldWorkBudget(boardID string, work heldYieldWork) (board.YieldCohort, board.DeclaredHandoffBounds, error) {
	if !validBoardID(boardID) {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{}, fmt.Errorf("%w: yield budget board ID", ErrInvalid)
	}
	// The same rule neutralContext enforces before a neutral challenge: the
	// approved profile is the fixture identity, and a session claiming a
	// different revision is not evidence the board changed, it is evidence
	// the two disagree. Keying a cohort on the session's value would let a
	// re-flashed fixture quietly inherit the approved fixture's history.
	if work.ApprovedRevision == "" {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{}, fmt.Errorf("%w: board has no approved fixture profile", ErrDenied)
	}
	if work.SessionRevision != work.ApprovedRevision {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{}, fmt.Errorf("%w: fixture session differs from approved profile", ErrDenied)
	}
	var definition struct {
		HIL *catalog.HILTask `json:"hil"`
	}
	decoder := json.NewDecoder(bytes.NewReader(work.HIL))
	if err := decoder.Decode(&definition); err != nil || definition.HIL == nil {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{}, fmt.Errorf("%w: held task carries no reviewed HIL definition", ErrConflict)
	}
	hil := *definition.HIL
	if err := catalog.ValidateHILTaskMetadata(hil); err != nil {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{}, fmt.Errorf("%w: held HIL definition: %v", ErrConflict, err)
	}
	// The definition names the board it was reviewed for. A task whose
	// definition names another board is not this board's work, whatever the
	// lease says, and its history is not this board's history.
	if hil.BoardID != boardID {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{}, fmt.Errorf("%w: held HIL task is reviewed for board %q", ErrConflict, hil.BoardID)
	}
	cohort := board.YieldCohort{
		BoardID:         boardID,
		BoardModel:      hil.BoardModel,
		FixtureRevision: work.ApprovedRevision,
		TaskName:        work.TaskName,
		CatalogDigest:   work.CatalogDigest,
		ImageSHA256:     work.ImageSHA256,
	}
	if err := board.ValidateYieldCohort(cohort); err != nil {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{}, fmt.Errorf("%w: %v", ErrConflict, err)
	}
	// Undeclared bounds are returned as the zero value, NOT as a refusal.
	// board.PlanYield already draws that line: a person asking for the
	// board gets the plan with the ETA reported unknown, and only automatic
	// dispatch is refused. Refusing here would take that judgement away
	// from the place that states it.
	var bounds board.DeclaredHandoffBounds
	if hil.HandoffBoundsDeclared() {
		bounds = board.DeclaredHandoffBounds{
			SafeStepBound:     time.Duration(hil.HandoffSafeStepSeconds) * time.Second,
			RestoreProbeBound: time.Duration(hil.HandoffRestoreProbeSeconds) * time.Second,
		}
	}
	return cohort, bounds, nil
}

// HeldYieldWork derives the cohort and declared bounds for the work a board's
// live lease is holding right now.
//
// It takes the snapshot rather than a board ID because a board with no lease
// is holding no work, and a cohort assembled for an idle board would describe
// nothing. Such a board is refused here rather than given an empty cohort:
// there is no handoff to estimate when nobody holds the board.
func (s *Store) HeldYieldWork(ctx context.Context, snapshot board.Snapshot) (board.YieldCohort, board.DeclaredHandoffBounds, error) {
	if s == nil || s.pool == nil || ctx == nil || !validBoardID(snapshot.BoardID) {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{}, fmt.Errorf("%w: held yield work", ErrInvalid)
	}
	if snapshot.Lease == nil || snapshot.Lease.ID == "" {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{}, fmt.Errorf("%w: board holds no lease to yield", ErrConflict)
	}
	var work heldYieldWork
	err := s.pool.QueryRow(ctx, `SELECT p.fixture_revision, s.fixture_revision,
		t.name, r.catalog_sha256, COALESCE(s.current_image_sha256, ''), t.arguments
		FROM board_sessions s
		JOIN board_fixture_profiles p ON p.board_id = s.board_id
		JOIN task_attempts a ON a.board_lease_id = s.lease_id AND a.state = 'running'
		JOIN tasks t ON t.id = a.task_id
		JOIN runs r ON r.id = t.run_id
		WHERE s.board_id = $1 AND s.lease_id = $2 AND s.ended_at IS NULL
		ORDER BY a.attempt_no DESC LIMIT 1`, snapshot.BoardID, snapshot.Lease.ID).
		Scan(&work.ApprovedRevision, &work.SessionRevision, &work.TaskName,
			&work.CatalogDigest, &work.ImageSHA256, &work.HIL)
	if errors.Is(err, pgx.ErrNoRows) {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{},
			fmt.Errorf("%w: board lease holds no running task to estimate a handoff over", ErrConflict)
	}
	if err != nil {
		return board.YieldCohort{}, board.DeclaredHandoffBounds{},
			fmt.Errorf("%w: held yield work read: %v", ErrUnavailable, err)
	}
	return yieldWorkBudget(snapshot.BoardID, work)
}
