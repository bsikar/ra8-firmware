//go:build integration

package store

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/jackc/pgx/v5/pgxpool"
)

// upload is one acknowledged attempt, running and inside its deadline, which
// is the only state an artifact may be uploaded against.
type upload struct {
	store      *Store
	pool       *pgxpool.Pool
	cert       []byte
	assignment *protocol.Assignment
}

// artifactUpload claims and acknowledges a task, which is what moves the
// attempt to "running" and issues the version and fence every chunk is
// judged against.
func artifactUpload(t *testing.T) upload {
	t.Helper()
	st, pool, cert, cat, _, facts := dispatchFixture(t)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	grant, err := st.ClaimAgentTask(ctx, cert, facts, cat, repeatHex("a", 40))
	if err != nil || grant == nil {
		t.Fatalf("claim failed: %v", err)
	}
	ack := protocol.Ack{SchemaVersion: protocol.Version, AssignmentID: grant.AssignmentID,
		AttemptID: grant.AttemptID, AssignmentVersion: grant.AssignmentVersion,
		FencingToken: grant.FencingToken, CatalogSHA256: grant.CatalogSHA256,
		SourceSnapshotSHA256: grant.Source.SnapshotSHA256, HostFacts: facts}
	if err := st.AcknowledgeAgentAssignment(ctx, cert, ack); err != nil {
		t.Fatal(err)
	}
	return upload{store: st, pool: pool, cert: cert, assignment: grant}
}

func repeatHex(unit string, count int) string {
	out := ""
	for i := 0; i < count; i++ {
		out += unit
	}
	return out
}

// chunkAt builds a chunk that satisfies the protocol's offset-to-sequence
// coupling, so the test exercises the store rather than the validator.
func (u upload) chunkAt(path string, sequence, offset int64, data []byte) protocol.ArtifactChunk {
	sum := sha256.Sum256(data)
	return protocol.ArtifactChunk{SchemaVersion: protocol.Version,
		AssignmentID: u.assignment.AssignmentID, AttemptID: u.assignment.AttemptID,
		AssignmentVersion: u.assignment.AssignmentVersion, FencingToken: u.assignment.FencingToken,
		StepName: "format-tree-check", Path: path, Sequence: sequence, Offset: offset,
		DataBase64: base64.StdEncoding.EncodeToString(data),
		SHA256:     hex.EncodeToString(sum[:])}
}

func (u upload) manifestFor(path string, total, finalSequence int64, digest string) protocol.ArtifactManifest {
	return protocol.ArtifactManifest{SchemaVersion: protocol.Version,
		AssignmentID: u.assignment.AssignmentID, AttemptID: u.assignment.AttemptID,
		AssignmentVersion: u.assignment.AssignmentVersion, FencingToken: u.assignment.FencingToken,
		StepName: "format-tree-check", Path: path, TotalBytes: total,
		SHA256: digest, FinalSequence: finalSequence, CapturedAt: time.Now().UTC()}
}

// held reads the artifact row the plane keeps, which is the record a later
// reassembly reads rather than the chunks themselves.
func (u upload) held(t *testing.T, path string) (chunks, total int64, closed bool) {
	t.Helper()
	var closedAt *time.Time
	err := u.pool.QueryRow(context.Background(),
		`SELECT chunk_count, total_bytes, closed_at FROM agent_artifacts
		 WHERE attempt_id=$1 AND path=$2`,
		u.assignment.AttemptID, path).Scan(&chunks, &total, &closedAt)
	if err != nil {
		t.Fatalf("read artifact row: %v", err)
	}
	return chunks, total, closedAt != nil
}

// An upload is taken a chunk at a time and closed by a manifest that
// describes exactly the bytes on file.
func TestIntegrationArtifactUploadTakesTheBytesItDescribes(t *testing.T) {
	u := artifactUpload(t)
	ctx := context.Background()
	first := []byte("first half\n")
	second := []byte("second half\n")
	path := "reports/gate.txt"

	if got, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, u.chunkAt(path, 1, 0, first)); err != nil || got != ArtifactAccepted {
		t.Fatalf("first chunk: %q, %v", got, err)
	}
	if got, err := u.store.SaveAgentArtifactChunk(ctx, u.cert,
		u.chunkAt(path, 2, int64(len(first)), second)); err != nil || got != ArtifactAccepted {
		t.Fatalf("second chunk: %q, %v", got, err)
	}
	chunks, total, closed := u.held(t, path)
	if chunks != 2 || total != int64(len(first)+len(second)) || closed {
		t.Fatalf("after two chunks: chunks=%d total=%d closed=%v", chunks, total, closed)
	}

	// The digest the close is judged against is taken over the chunks in
	// sequence order, which is the order a reassembly concatenates them in.
	whole := sha256.Sum256(append(append([]byte{}, first...), second...))
	manifest := u.manifestFor(path, int64(len(first)+len(second)), 2, hex.EncodeToString(whole[:]))
	if got, err := u.store.CloseAgentArtifact(ctx, u.cert, manifest); err != nil || got != ArtifactAccepted {
		t.Fatalf("close: %q, %v", got, err)
	}
	if _, _, closed := u.held(t, path); !closed {
		t.Fatal("the artifact was not closed")
	}
}

// An exact replay is reported as a duplicate and must not land the bytes
// twice, which is what makes a retry after a lost response safe.
func TestIntegrationArtifactChunkReplayIsADuplicateNotASecondWrite(t *testing.T) {
	u := artifactUpload(t)
	ctx := context.Background()
	path := "reports/replay.txt"
	chunk := u.chunkAt(path, 1, 0, []byte("once\n"))

	if got, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, chunk); err != nil || got != ArtifactAccepted {
		t.Fatalf("first write: %q, %v", got, err)
	}
	got, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, chunk)
	if err != nil || got != ArtifactDuplicate {
		t.Fatalf("exact replay: %q, %v", got, err)
	}
	chunks, total, _ := u.held(t, path)
	if chunks != 1 || total != 5 {
		t.Fatalf("the replay moved the counters: chunks=%d total=%d", chunks, total)
	}
}

// The sequence is contiguous and a stored chunk is immutable, so a gap and an
// altered replay are both refused rather than quietly reordering the bytes.
func TestIntegrationArtifactUploadRefusesAGapAndAnAlteredReplay(t *testing.T) {
	u := artifactUpload(t)
	ctx := context.Background()
	path := "reports/order.txt"
	first := []byte("one\n")
	if _, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, u.chunkAt(path, 1, 0, first)); err != nil {
		t.Fatal(err)
	}
	gap := u.chunkAt(path, 3, int64(len(first)), []byte("three\n"))
	if _, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, gap); !errors.Is(err, ErrConflict) {
		t.Fatalf("a gap was accepted: %v", err)
	}
	altered := u.chunkAt(path, 1, 0, []byte("ONE\n"))
	if _, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, altered); !errors.Is(err, ErrConflict) {
		t.Fatalf("an altered replay was accepted: %v", err)
	}
	chunks, total, _ := u.held(t, path)
	if chunks != 1 || total != int64(len(first)) {
		t.Fatalf("a refused chunk still moved the counters: chunks=%d total=%d", chunks, total)
	}
}

// The manifest is evidence about bytes already uploaded, so each of the three
// claims it makes is checked against the stored chunks and refused on its own.
func TestIntegrationArtifactCloseRefusesAManifestTheBytesDoNotSupport(t *testing.T) {
	u := artifactUpload(t)
	ctx := context.Background()
	path := "reports/claims.txt"
	data := []byte("the only bytes\n")
	if _, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, u.chunkAt(path, 1, 0, data)); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(data)
	honest := hex.EncodeToString(sum[:])
	other := sha256.Sum256([]byte("different bytes\n"))

	for _, bad := range []struct {
		name     string
		manifest protocol.ArtifactManifest
	}{
		{"a digest over bytes that were never uploaded",
			u.manifestFor(path, int64(len(data)), 1, hex.EncodeToString(other[:]))},
		{"a total that is not what was uploaded",
			u.manifestFor(path, int64(len(data))+1, 1, honest)},
		{"a final sequence that is not what was uploaded",
			u.manifestFor(path, int64(len(data)), 2, honest)},
	} {
		if _, err := u.store.CloseAgentArtifact(ctx, u.cert, bad.manifest); !errors.Is(err, ErrConflict) {
			t.Fatalf("%s was accepted: %v", bad.name, err)
		}
		if _, _, closed := u.held(t, path); closed {
			t.Fatalf("%s closed the artifact anyway", bad.name)
		}
	}
	if got, err := u.store.CloseAgentArtifact(ctx, u.cert,
		u.manifestFor(path, int64(len(data)), 1, honest)); err != nil || got != ArtifactAccepted {
		t.Fatalf("the honest manifest was refused: %q, %v", got, err)
	}
}

// A close may be retried, and a second close that says something else is
// refused rather than overwriting what is already on file.
func TestIntegrationArtifactCloseIsIdempotentAndRefusesADifferentClose(t *testing.T) {
	u := artifactUpload(t)
	ctx := context.Background()
	path := "reports/final.txt"
	data := []byte("settled\n")
	if _, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, u.chunkAt(path, 1, 0, data)); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(data)
	manifest := u.manifestFor(path, int64(len(data)), 1, hex.EncodeToString(sum[:]))
	if got, err := u.store.CloseAgentArtifact(ctx, u.cert, manifest); err != nil || got != ArtifactAccepted {
		t.Fatalf("close: %q, %v", got, err)
	}
	got, err := u.store.CloseAgentArtifact(ctx, u.cert, manifest)
	if err != nil || got != ArtifactDuplicate {
		t.Fatalf("exact close replay: %q, %v", got, err)
	}
	truncated := manifest
	truncated.Truncated = !manifest.Truncated
	if _, err := u.store.CloseAgentArtifact(ctx, u.cert, truncated); !errors.Is(err, ErrConflict) {
		t.Fatalf("a different close was accepted: %v", err)
	}
	// A chunk arriving after the close is refused too: the artifact is a
	// finished record, not an open file.
	late := u.chunkAt(path, 2, int64(len(data)), []byte("late\n"))
	if _, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, late); !errors.Is(err, ErrConflict) {
		t.Fatalf("a chunk after the close was accepted: %v", err)
	}
}

// Evidence is only taken while the attempt is running and inside its
// deadline, so a finished or long-expired attempt cannot be appended to.
func TestIntegrationArtifactUploadRefusesEvidenceOutsideTheAttemptWindow(t *testing.T) {
	t.Run("an attempt that is no longer running", func(t *testing.T) {
		u := artifactUpload(t)
		if _, err := u.pool.Exec(context.Background(),
			`UPDATE task_attempts SET state='succeeded' WHERE id=$1`, u.assignment.AttemptID); err != nil {
			t.Fatal(err)
		}
		chunk := u.chunkAt("reports/after.txt", 1, 0, []byte("too late\n"))
		if _, err := u.store.SaveAgentArtifactChunk(context.Background(), u.cert, chunk); !errors.Is(err, ErrConflict) {
			t.Fatalf("a finished attempt took evidence: %v", err)
		}
	})
	t.Run("a deadline further past than the grace allows", func(t *testing.T) {
		u := artifactUpload(t)
		// The grace is sixty seconds, so two minutes back is outside it
		// without depending on how long the test itself took.
		if _, err := u.pool.Exec(context.Background(),
			`UPDATE task_attempts SET deadline_at=clock_timestamp()-interval '2 minutes' WHERE id=$1`,
			u.assignment.AttemptID); err != nil {
			t.Fatal(err)
		}
		// This package shares one database, and an attempt left both running
		// and past its deadline is exactly what the reaper sweeps. Putting it
		// beyond the reaper's reach again keeps that sweep counting only the
		// attempts its own test planted.
		t.Cleanup(func() {
			_, _ = u.pool.Exec(context.Background(),
				`UPDATE task_attempts SET state='succeeded',
				 deadline_at=clock_timestamp()+interval '1 hour' WHERE id=$1`,
				u.assignment.AttemptID)
		})
		chunk := u.chunkAt("reports/expired.txt", 1, 0, []byte("expired\n"))
		if _, err := u.store.SaveAgentArtifactChunk(context.Background(), u.cert, chunk); !errors.Is(err, ErrConflict) {
			t.Fatalf("an expired attempt took evidence: %v", err)
		}
	})
}

// Both per-attempt ceilings are enforced where the upload lands, not only in
// the protocol's per-chunk validation.
func TestIntegrationArtifactUploadHoldsThePerAttemptCeilings(t *testing.T) {
	t.Run("the artifact count", func(t *testing.T) {
		u := artifactUpload(t)
		ctx := context.Background()
		data := []byte("x")
		for i := 0; i < protocol.MaxArtifactsPerAttempt; i++ {
			path := "reports/many/" + itoaForPath(i) + ".txt"
			if _, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, u.chunkAt(path, 1, 0, data)); err != nil {
				t.Fatalf("artifact %d was refused early: %v", i, err)
			}
		}
		over := u.chunkAt("reports/many/one-too-many.txt", 1, 0, data)
		if _, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, over); !errors.Is(err, ErrConflict) {
			t.Fatalf("the artifact past the ceiling was accepted: %v", err)
		}
		// A further chunk on an artifact that already exists is still taken:
		// the ceiling counts artifacts, not writes.
		more := u.chunkAt("reports/many/0.txt", 2, 1, data)
		if got, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, more); err != nil || got != ArtifactAccepted {
			t.Fatalf("an existing artifact was refused by the count ceiling: %q, %v", got, err)
		}
	})
	t.Run("the byte budget", func(t *testing.T) {
		u := artifactUpload(t)
		ctx := context.Background()
		// The budget is 64 MiB across the attempt. Planting a row already at
		// the ceiling reaches the refusal without writing 64 MiB of chunks.
		if _, err := u.pool.Exec(ctx, `INSERT INTO agent_artifacts
			(attempt_id, path, step_key, total_bytes, chunk_count)
			VALUES ($1,'reports/already-large.bin','format-tree-check',$2,1)`,
			u.assignment.AttemptID, int64(protocol.MaxArtifactBytes)); err != nil {
			t.Fatal(err)
		}
		over := u.chunkAt("reports/one-more.txt", 1, 0, []byte("x"))
		if _, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, over); !errors.Is(err, ErrConflict) {
			t.Fatalf("a chunk past the attempt byte budget was accepted: %v", err)
		}
	})
}

func itoaForPath(value int) string {
	if value == 0 {
		return "0"
	}
	digits := ""
	for value > 0 {
		digits = string(rune('0'+value%10)) + digits
		value /= 10
	}
	return digits
}

// An artifact is evidence attributed to the agent that uploaded it, so a
// certificate the plane does not know is refused before anything is stored.
func TestIntegrationArtifactUploadDeniesAnUnknownCertificate(t *testing.T) {
	u := artifactUpload(t)
	ctx := context.Background()
	chunk := u.chunkAt("reports/forged.txt", 1, 0, []byte("forged\n"))
	if _, err := u.store.SaveAgentArtifactChunk(ctx, []byte("unknown certificate"), chunk); !errors.Is(err, ErrDenied) {
		t.Fatalf("an unknown certificate uploaded a chunk: %v", err)
	}
	sum := sha256.Sum256([]byte("forged\n"))
	manifest := u.manifestFor("reports/forged.txt", 7, 1, hex.EncodeToString(sum[:]))
	if _, err := u.store.CloseAgentArtifact(ctx, []byte("unknown certificate"), manifest); !errors.Is(err, ErrDenied) {
		t.Fatalf("an unknown certificate closed an artifact: %v", err)
	}
}

// Closing an artifact is an auditable act, and the record names the artifact
// rather than only the attempt.
func TestIntegrationArtifactCloseFilesAnAuditRecord(t *testing.T) {
	u := artifactUpload(t)
	ctx := context.Background()
	path := "reports/audited.txt"
	data := []byte("audited\n")
	if _, err := u.store.SaveAgentArtifactChunk(ctx, u.cert, u.chunkAt(path, 1, 0, data)); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(data)
	manifest := u.manifestFor(path, int64(len(data)), 1, hex.EncodeToString(sum[:]))
	if _, err := u.store.CloseAgentArtifact(ctx, u.cert, manifest); err != nil {
		t.Fatal(err)
	}
	var count int
	if err := u.pool.QueryRow(ctx,
		`SELECT COUNT(*) FROM audit
		 WHERE action='task.artifact.closed' AND reason->>'path'=$1
		 AND reason->>'attempt_id'=$2`, path, u.assignment.AttemptID).Scan(&count); err != nil {
		t.Fatalf("read audit: %v", err)
	}
	if count != 1 {
		t.Fatalf("the close filed %d audit records naming the artifact, want 1", count)
	}
}
