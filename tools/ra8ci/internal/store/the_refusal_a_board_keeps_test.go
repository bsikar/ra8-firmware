//go:build integration

package store

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// Keeping the record of a refusal.
//
// Every board command that is turned down rolls its work back, and the
// refusal is the one thing that must not roll back with it: a board that
// denies a stranger and leaves no trace is indistinguishable from a board
// nobody asked. auditBoardDenial is the commit that keeps the refusal, and
// it hands the original denial back so the caller still fails for the
// reason it decided on rather than for anything that happened here.

// deniedAudit is one recorded refusal, read back after the transaction that
// wrote it has gone.
type deniedAudit struct {
	actor      string
	action     string
	targetType string
	targetID   string
	outcome    string
	reason     string
}

func refusalsRecordedFor(t *testing.T, ctx context.Context, pool *pgxpool.Pool, boardID string) []deniedAudit {
	t.Helper()
	rows, err := pool.Query(ctx, `SELECT actor_id,action,target_type,target_id,outcome,reason::text
		FROM audit WHERE action='board.command.denied' AND target_id=$1 ORDER BY id`, boardID)
	if err != nil {
		t.Fatalf("reading recorded refusals: %v", err)
	}
	defer rows.Close()
	var recorded []deniedAudit
	for rows.Next() {
		var one deniedAudit
		if err := rows.Scan(&one.actor, &one.action, &one.targetType, &one.targetID,
			&one.outcome, &one.reason); err != nil {
			t.Fatalf("scanning recorded refusal: %v", err)
		}
		recorded = append(recorded, one)
	}
	return recorded
}

func TestIntegrationARefusedBoardCommandLeavesItsRefusalBehind(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	boardID, actor, _, _ := leasedUnder(t, ctx, st, pool, time.Now().UTC().Truncate(time.Second))

	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	defer tx.Rollback(ctx)
	denial := errors.New("denied: not the holder")
	// The caller must still fail for the reason the command decided on, not
	// for anything the recording did.
	if err := auditBoardDenial(ctx, tx, actor, denial); !errors.Is(err, denial) {
		t.Fatalf("the denial handed back was %v, want the original", err)
	}

	// The refusal survives its own transaction: auditBoardDenial commits,
	// so a reader with no part in that transaction can see it.
	recorded := refusalsRecordedFor(t, ctx, pool, boardID)
	if len(recorded) != 1 {
		t.Fatalf("want one recorded refusal, got %d", len(recorded))
	}
	one := recorded[0]
	if one.actor != actor.ID() {
		t.Fatalf("refusal recorded against actor %q, want %q", one.actor, actor.ID())
	}
	if one.targetType != "board" || one.targetID != boardID {
		t.Fatalf("refusal recorded against %s %q", one.targetType, one.targetID)
	}
	if one.outcome != "denied" {
		t.Fatalf("refusal recorded with outcome %q", one.outcome)
	}
	// The reason is what tells an operator reading the log later why the
	// board said no, so it carries the denial's own words.
	if !strings.Contains(one.reason, "not the holder") {
		t.Fatalf("refusal recorded with reason %q", one.reason)
	}
}

func TestIntegrationARefusalThatCannotBeRecordedIsNotReportedAsAPlainDenial(t *testing.T) {
	st, pool := integrationStore(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	boardID, actor, _, _ := leasedUnder(t, ctx, st, pool, time.Now().UTC().Truncate(time.Second))

	tx, err := pool.Begin(ctx)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	defer tx.Rollback(ctx)
	// A statement that fails leaves the transaction unable to accept any
	// further work, which is the state a denial can genuinely arrive in:
	// the command failed partway and then decided to refuse.
	if _, err := tx.Exec(ctx, `SELECT 1/0`); err == nil {
		t.Fatal("the poisoning statement succeeded")
	}

	denial := errors.New("denied: not the holder")
	err = auditBoardDenial(ctx, tx, actor, denial)
	// Handing back the plain denial here would tell the caller the board
	// refused and the refusal was filed, when nothing was filed at all. It
	// reports the recording failure instead.
	if !errors.Is(err, ErrUnavailable) {
		t.Fatalf("an unrecordable refusal answered %v, want unavailable", err)
	}
	if errors.Is(err, denial) {
		t.Fatal("an unrecordable refusal was reported as an ordinary denial")
	}
	if recorded := refusalsRecordedFor(t, ctx, pool, boardID); len(recorded) != 0 {
		t.Fatalf("a failed recording left %d rows behind", len(recorded))
	}
}
