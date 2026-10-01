//go:build integration

package store

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/jackc/pgx/v5/pgxpool"
)

// What a page of attempt logs is held to.
//
// AttemptLogs is the store side of the run-log door. It fetches one more row
// than the caller asked for, which is how it can say whether more exists
// without a second count query, and then judges every row it read before
// handing any of it back: a gap in the sequence, an unknown stream, an empty
// or oversized body, or a body that no longer matches its stored digest all
// stop the page rather than reach a reader. Those refusals are unavailability
// rather than bad input, because a stored row the plane wrote and can no
// longer vouch for is the plane's fault and not the caller's.

// loggedAttempt is an acknowledged attempt carrying count contiguous log
// chunks, which is the only shape AttemptLogs will read: SaveAgentLog refuses
// a gap, so the sequences are 1..count by construction.
func loggedAttempt(t *testing.T, count int) (*Store, *pgxpool.Pool, string, string) {
	t.Helper()
	st, pool, cert, cat, run, facts := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	grant, err := st.ClaimAgentTask(ctx, cert, facts, cat, strings.Repeat("a", 40))
	if err != nil || grant == nil {
		t.Fatalf("claim failed: %+v, %v", grant, err)
	}
	if err := st.AcknowledgeAgentAssignment(ctx, cert, protocol.Ack{
		SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
		AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
		FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
		SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: facts,
	}); err != nil {
		t.Fatalf("acknowledge failed: %v", err)
	}
	for sequence := 1; sequence <= count; sequence++ {
		data := []byte("line " + strconv.Itoa(sequence) + "\n")
		sum := sha256.Sum256(data)
		if err := st.SaveAgentLog(ctx, cert, protocol.LogChunk{
			SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
			AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
			FencingToken: grant.FencingToken, Sequence: int64(sequence),
			Stream: "stdout", StepName: "format-tree-check",
			DataBase64: base64.StdEncoding.EncodeToString(data),
			SHA256:     hex.EncodeToString(sum[:]),
		}); err != nil {
			t.Fatalf("saving chunk %d: %v", sequence, err)
		}
	}
	return st, pool, run.ID, grant.AttemptID
}

func TestIntegrationALogPageSaysWhetherMoreRemains(t *testing.T) {
	st, _, runID, attemptID := loggedAttempt(t, 5)
	ctx := context.Background()

	// Asking for fewer than exist is the case the extra fetched row exists
	// for: the page is full, more remains, and the cursor stops at the last
	// chunk actually handed over rather than at the one that was only looked at.
	short, err := st.AttemptLogs(ctx, runID, attemptID, 0, 4)
	if err != nil {
		t.Fatalf("a short page was refused: %v", err)
	}
	if len(short.Chunks) != 4 || !short.HasMore || short.NextAfter != 4 {
		t.Fatalf("chunks=%d hasMore=%v nextAfter=%d, want 4/true/4",
			len(short.Chunks), short.HasMore, short.NextAfter)
	}

	// Asking for exactly as many as exist must NOT claim more remains, which
	// is the boundary the extra row is most likely to get wrong.
	exact, err := st.AttemptLogs(ctx, runID, attemptID, 0, 5)
	if err != nil {
		t.Fatalf("an exact page was refused: %v", err)
	}
	if len(exact.Chunks) != 5 || exact.HasMore || exact.NextAfter != 5 {
		t.Fatalf("chunks=%d hasMore=%v nextAfter=%d, want 5/false/5",
			len(exact.Chunks), exact.HasMore, exact.NextAfter)
	}
	if exact.AttemptID != attemptID {
		t.Fatalf("the page names attempt %s", exact.AttemptID)
	}
}

func TestIntegrationLogPagesWalkTheWholeAttemptExactlyOnce(t *testing.T) {
	st, _, runID, attemptID := loggedAttempt(t, 7)
	ctx := context.Background()

	seen := make([]int64, 0, 7)
	cursor := int64(0)
	for page := 0; page < 10; page++ {
		got, err := st.AttemptLogs(ctx, runID, attemptID, cursor, 2)
		if err != nil {
			t.Fatalf("page at %d was refused: %v", cursor, err)
		}
		for _, chunk := range got.Chunks {
			seen = append(seen, chunk.Sequence)
		}
		if !got.HasMore {
			break
		}
		if got.NextAfter <= cursor {
			t.Fatalf("the cursor did not advance past %d", cursor)
		}
		cursor = got.NextAfter
	}
	if len(seen) != 7 {
		t.Fatalf("walked %d chunks of 7: %v", len(seen), seen)
	}
	for i, sequence := range seen {
		if sequence != int64(i+1) {
			t.Fatalf("the walk repeated or skipped a chunk: %v", seen)
		}
	}

	// A cursor past the end is an empty page, not an error: a reader that has
	// caught up asks this every time it polls.
	empty, err := st.AttemptLogs(ctx, runID, attemptID, 7, 2)
	if err != nil {
		t.Fatalf("a caught-up reader was refused: %v", err)
	}
	if len(empty.Chunks) != 0 || empty.HasMore || empty.NextAfter != 7 {
		t.Fatalf("chunks=%d hasMore=%v nextAfter=%d, want 0/false/7",
			len(empty.Chunks), empty.HasMore, empty.NextAfter)
	}
}

func TestIntegrationALogPageRefusesParametersItCannotPage(t *testing.T) {
	st, _, runID, attemptID := loggedAttempt(t, 1)
	ctx := context.Background()

	for _, that := range []struct {
		named     string
		runID     string
		attemptID string
		after     int64
		limit     int
	}{
		{"a run that is not an identifier", "not-a-uuid", attemptID, 0, 1},
		{"an attempt that is not an identifier", runID, "not-a-uuid", 0, 1},
		{"a cursor before the beginning", runID, attemptID, -1, 1},
		{"a page of nothing", runID, attemptID, 0, 0},
		{"a page past the ceiling", runID, attemptID, 0, MaxLogPageSize + 1},
	} {
		t.Run(that.named, func(t *testing.T) {
			if _, err := st.AttemptLogs(ctx, that.runID, that.attemptID, that.after, that.limit); !errors.Is(err, ErrInvalid) {
				t.Fatalf("accepted: %v", err)
			}
		})
	}

	// The ceiling itself is allowed, so the bound is exact on both sides
	// rather than one short.
	if _, err := st.AttemptLogs(ctx, runID, attemptID, 0, MaxLogPageSize); err != nil {
		t.Fatalf("the largest allowed page was refused: %v", err)
	}
}

func TestIntegrationALogChunkThatNoLongerMatchesItsDigestIsNotServed(t *testing.T) {
	st, pool, runID, attemptID := loggedAttempt(t, 3)
	ctx := context.Background()

	if _, err := pool.Exec(ctx,
		`UPDATE log_chunks SET bytes=$1 WHERE attempt_id=$2 AND seq=2`,
		[]byte("tampered"), attemptID); err != nil {
		t.Fatalf("could not alter the stored chunk: %v", err)
	}

	// The digest is what makes a stored chunk evidence rather than just
	// bytes, so a page containing one that no longer matches is withheld
	// whole: a reader never gets the honest chunks around it and a quietly
	// altered one in between.
	_, err := st.AttemptLogs(ctx, runID, attemptID, 0, 3)
	if !errors.Is(err, ErrUnavailable) {
		t.Fatalf("a tampered chunk was served: %v", err)
	}

	// And the chunk before it still reads on its own, which is what shows the
	// refusal is about that row rather than the attempt being unreadable.
	first, err := st.AttemptLogs(ctx, runID, attemptID, 0, 1)
	if err != nil || len(first.Chunks) != 1 || first.Chunks[0].Sequence != 1 {
		t.Fatalf("the untouched chunk before it did not read: %+v, %v", first, err)
	}
}
