//go:build integration

package store

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Deriving a HIL workload from the session that enclosed it.
//
// A HIL timing observation is only comparable against history taken on the
// same fixture and the same approved profile, and neither of those is
// something the reporting agent gets to assert: they are read back from the
// board session that was open while the step ran. hilWorkloadForSession is
// that read, and everything it refuses is a cohort that would otherwise be
// filed against work it never did.

// hilBoardFor plants a board and an active lease, and answers the lease the
// sessions below hang off.
func hilBoardFor(t *testing.T, ctx context.Context, pool *pgxpool.Pool, from, until time.Time) (string, string) {
	t.Helper()
	boardID := "test-hilws-" + mustID(t)
	leaseID := mustID(t)
	if _, err := pool.Exec(ctx, `INSERT INTO boards (id,generation,state) VALUES ($1,1,'held')`, boardID); err != nil {
		t.Fatalf("planting board: %v", err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO board_leases
		(id,board_id,generation,holder_id,priority,reason,requested_duration_seconds,granted_at,expires_at,state)
		VALUES ($1,$2,1,'hil-test','ci','HIL workload derivation',300,$3,$4,'active')`,
		leaseID, boardID, from, until); err != nil {
		t.Fatalf("planting lease: %v", err)
	}
	return boardID, leaseID
}

// openSession plants one board session over the lease. A blank profile digest
// is written as SQL NULL, which is how a session that never recorded an
// approved profile reaches the table.
func openSession(t *testing.T, ctx context.Context, pool *pgxpool.Pool,
	boardID, leaseID, fixtureRevision, profileSHA256 string, startedAt time.Time, endedAt *time.Time) {
	t.Helper()
	var profile any
	if profileSHA256 != "" {
		profile = profileSHA256
	}
	if _, err := pool.Exec(ctx, `INSERT INTO board_sessions
		(id,board_id,lease_id,owner_id,fixture_revision,profile_sha256,phase,restore_policy,
		 metadata_version,started_at,ended_at)
		VALUES ($1,$2,$3,'hil-test',$4,$5,'held','restore-v1',1,$6,$7)`,
		mustID(t), boardID, leaseID, fixtureRevision, profile, startedAt, endedAt); err != nil {
		t.Fatalf("planting board session: %v", err)
	}
}

func hilTaskFor(boardID string) catalog.HILTask {
	return catalog.HILTask{BoardID: boardID, BoardModel: "EK-RA8D2",
		ManifestPath:    "examples/ek_ra8d2/hw_validated/hil/demo/hil.conf",
		ProgramFamily:   "uart-demo",
		Mode:            "uart_scrape",
		ObservationStep: "observe", FlashRestoreSeconds: 10}
}

// derivedWorkload runs the read the observation transaction runs.
func derivedWorkload(t *testing.T, ctx context.Context, pool *pgxpool.Pool, leaseID string,
	definition catalog.HILTask, startedAt, endedAt time.Time) (hilspec.Workload, error) {
	t.Helper()
	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	defer tx.Rollback(ctx)
	return hilWorkloadForSession(ctx, tx, leaseID, definition, startedAt, endedAt)
}

func TestIntegrationAHILWorkloadIsReadFromTheSessionThatEnclosedTheStep(t *testing.T) {
	_, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	now := time.Now().UTC().Truncate(time.Millisecond)
	boardID, leaseID := hilBoardFor(t, ctx, pool, now.Add(-time.Hour), now.Add(time.Hour))
	profileSHA := fmt.Sprintf("%x", sha256.Sum256([]byte(mustID(t))))
	openSession(t, ctx, pool, boardID, leaseID, "fixture-v1", profileSHA, now.Add(-30*time.Minute), nil)

	definition := hilTaskFor(boardID)
	workload, err := derivedWorkload(t, ctx, pool, leaseID, definition,
		now.Add(-10*time.Minute), now.Add(-9*time.Minute))
	if err != nil {
		t.Fatalf("deriving a workload from an enclosing session: %v", err)
	}
	// The fixture and the profile come from the session, not from the task:
	// the agent reporting the timing does not get to say which bench it ran
	// on. Everything else is the immutable task contract.
	if workload.FixtureRevision != "fixture-v1" || workload.ProfileSHA256 != profileSHA {
		t.Fatalf("workload took fixture %q profile %q from somewhere else",
			workload.FixtureRevision, workload.ProfileSHA256)
	}
	if workload.ManifestPath != definition.ManifestPath || workload.BoardModel != definition.BoardModel ||
		workload.ProgramFamily != definition.ProgramFamily || string(workload.Mode) != definition.Mode {
		t.Fatalf("workload contradicts its task contract: %+v", workload)
	}
}

func TestIntegrationAHILWorkloadRefusesAnIntervalItCannotEnclose(t *testing.T) {
	_, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	now := time.Now().UTC().Truncate(time.Millisecond)
	boardID, leaseID := hilBoardFor(t, ctx, pool, now.Add(-time.Hour), now.Add(time.Hour))
	profileSHA := fmt.Sprintf("%x", sha256.Sum256([]byte(mustID(t))))
	openSession(t, ctx, pool, boardID, leaseID, "fixture-v1", profileSHA, now.Add(-30*time.Minute), nil)
	definition := hilTaskFor(boardID)

	for _, one := range []struct {
		name           string
		started, ended time.Time
	}{
		{"no start", time.Time{}, now},
		{"no end", now, time.Time{}},
		{"an interval that runs backwards", now, now.Add(-time.Minute)},
	} {
		t.Run(one.name, func(t *testing.T) {
			// A step with no honest interval cannot be enclosed by anything,
			// and taking the newest session anyway would file the measurement
			// against whichever fixture happened to be loaded last.
			if _, err := derivedWorkload(t, ctx, pool, leaseID, definition, one.started, one.ended); !errors.Is(err, ErrConflict) {
				t.Fatalf("interval accepted: %v", err)
			}
		})
	}
}

func TestIntegrationAHILWorkloadRefusesASessionItCannotTrust(t *testing.T) {
	_, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	now := time.Now().UTC().Truncate(time.Millisecond)
	profileSHA := fmt.Sprintf("%x", sha256.Sum256([]byte(mustID(t))))
	step, stepEnd := now.Add(-10*time.Minute), now.Add(-9*time.Minute)

	t.Run("no session was open across the step", func(t *testing.T) {
		boardID, leaseID := hilBoardFor(t, ctx, pool, now.Add(-time.Hour), now.Add(time.Hour))
		// The session opened after the step had already finished.
		openSession(t, ctx, pool, boardID, leaseID, "fixture-v1", profileSHA, now.Add(-time.Minute), nil)
		if _, err := derivedWorkload(t, ctx, pool, leaseID, hilTaskFor(boardID), step, stepEnd); !errors.Is(err, ErrConflict) {
			t.Fatalf("a step outside every session was accepted: %v", err)
		}
	})

	t.Run("the enclosing session closed before the step ended", func(t *testing.T) {
		boardID, leaseID := hilBoardFor(t, ctx, pool, now.Add(-time.Hour), now.Add(time.Hour))
		closed := now.Add(-9*time.Minute - 30*time.Second)
		openSession(t, ctx, pool, boardID, leaseID, "fixture-v1", profileSHA, now.Add(-30*time.Minute), &closed)
		if _, err := derivedWorkload(t, ctx, pool, leaseID, hilTaskFor(boardID), step, stepEnd); !errors.Is(err, ErrConflict) {
			t.Fatalf("a session that closed mid-step was accepted: %v", err)
		}
	})

	t.Run("the session recorded no approved profile", func(t *testing.T) {
		boardID, leaseID := hilBoardFor(t, ctx, pool, now.Add(-time.Hour), now.Add(time.Hour))
		// A session with no profile digest cannot say which approved image
		// was on the board, so it is not an identity to file history under.
		openSession(t, ctx, pool, boardID, leaseID, "fixture-v1", "", now.Add(-30*time.Minute), nil)
		if _, err := derivedWorkload(t, ctx, pool, leaseID, hilTaskFor(boardID), step, stepEnd); !errors.Is(err, ErrConflict) {
			t.Fatalf("a session with no approved profile was accepted: %v", err)
		}
	})

	t.Run("two sessions enclose the step", func(t *testing.T) {
		boardID, leaseID := hilBoardFor(t, ctx, pool, now.Add(-time.Hour), now.Add(time.Hour))
		// A board holds at most one OPEN session, so the overlap that can
		// really happen is an earlier session closed after the step with a
		// later one still running. They name different fixtures, and
		// guessing between them would file the timing under whichever the
		// ordering happened to surface.
		closedAfterTheStep := now
		openSession(t, ctx, pool, boardID, leaseID, "fixture-v1", profileSHA,
			now.Add(-30*time.Minute), &closedAfterTheStep)
		openSession(t, ctx, pool, boardID, leaseID, "fixture-v2", profileSHA, now.Add(-20*time.Minute), nil)
		if _, err := derivedWorkload(t, ctx, pool, leaseID, hilTaskFor(boardID), step, stepEnd); !errors.Is(err, ErrConflict) {
			t.Fatalf("an ambiguous session was accepted: %v", err)
		}
	})

	t.Run("the session names an identity the cohort rules refuse", func(t *testing.T) {
		boardID, leaseID := hilBoardFor(t, ctx, pool, now.Add(-time.Hour), now.Add(time.Hour))
		// A padded fixture revision reads as a different cohort to every
		// comparison that trims, so it is refused rather than normalised.
		openSession(t, ctx, pool, boardID, leaseID, "fixture-v1 ", profileSHA, now.Add(-30*time.Minute), nil)
		if _, err := derivedWorkload(t, ctx, pool, leaseID, hilTaskFor(boardID), step, stepEnd); !errors.Is(err, ErrConflict) {
			t.Fatalf("an untrimmed fixture revision was accepted: %v", err)
		}
	})

	t.Run("the session belongs to another board", func(t *testing.T) {
		boardID, leaseID := hilBoardFor(t, ctx, pool, now.Add(-time.Hour), now.Add(time.Hour))
		openSession(t, ctx, pool, boardID, leaseID, "fixture-v1", profileSHA, now.Add(-30*time.Minute), nil)
		// The task contract names the board it is allowed to run on, and a
		// session on a different board is not evidence about this one.
		elsewhere := hilTaskFor("test-hilws-" + mustID(t))
		if _, err := derivedWorkload(t, ctx, pool, leaseID, elsewhere, step, stepEnd); !errors.Is(err, ErrConflict) {
			t.Fatalf("a session on another board was accepted: %v", err)
		}
	})
}
