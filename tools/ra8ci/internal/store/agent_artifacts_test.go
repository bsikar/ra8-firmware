// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

func artifactChunkFixture(sequence, offset int64, payload string) protocol.ArtifactChunk {
	sum := sha256.Sum256([]byte(payload))
	return protocol.ArtifactChunk{
		SchemaVersion:     protocol.Version,
		AssignmentID:      "11111111-1111-4111-8111-111111111111",
		AttemptID:         "22222222-2222-4222-8222-222222222222",
		AssignmentVersion: 1,
		FencingToken:      1,
		StepName:          "build",
		Path:              "out/report.xml",
		Sequence:          sequence,
		Offset:            offset,
		DataBase64:        base64.StdEncoding.EncodeToString([]byte(payload)),
		SHA256:            hex.EncodeToString(sum[:]),
	}
}

func artifactManifestFixture(total, final int64, payload string) protocol.ArtifactManifest {
	sum := sha256.Sum256([]byte(payload))
	return protocol.ArtifactManifest{
		SchemaVersion:     protocol.Version,
		AssignmentID:      "11111111-1111-4111-8111-111111111111",
		AttemptID:         "22222222-2222-4222-8222-222222222222",
		AssignmentVersion: 1,
		FencingToken:      1,
		StepName:          "build",
		Path:              "out/report.xml",
		TotalBytes:        total,
		SHA256:            hex.EncodeToString(sum[:]),
		FinalSequence:     final,
		CapturedAt:        time.Unix(1790000000, 0).UTC(),
	}
}

func digestOf(payload string) string {
	sum := sha256.Sum256([]byte(payload))
	return hex.EncodeToString(sum[:])
}

func TestArtifactChunkWriteAcceptsTheNextContiguousChunk(t *testing.T) {
	chunk := artifactChunkFixture(1, 0, "first")
	outcome, err := artifactChunkWrite(chunk, 5, heldArtifact{}, heldChunk{})
	if err != nil || outcome != ArtifactAccepted {
		t.Fatalf("first chunk: outcome %q err %v", outcome, err)
	}
	second := artifactChunkFixture(2, 5, "second")
	artifact := heldArtifact{Exists: true, StepKey: "build", TotalBytes: 5, Chunks: 1}
	outcome, err = artifactChunkWrite(second, 6, artifact, heldChunk{})
	if err != nil || outcome != ArtifactAccepted {
		t.Fatalf("second chunk: outcome %q err %v", outcome, err)
	}
}

func TestArtifactChunkWriteIsIdempotentForAnIdenticalReplay(t *testing.T) {
	chunk := artifactChunkFixture(1, 0, "first")
	artifact := heldArtifact{Exists: true, StepKey: "build", TotalBytes: 5, Chunks: 1}
	held := heldChunk{Exists: true, Offset: 0, SHA256: chunk.SHA256, Length: 5}
	for replay := 0; replay < 3; replay++ {
		outcome, err := artifactChunkWrite(chunk, 5, artifact, held)
		if err != nil || outcome != ArtifactDuplicate {
			t.Fatalf("replay %d: outcome %q err %v", replay, outcome, err)
		}
	}
	// A replay after the artifact was closed is still the evidence the plane
	// holds, so it stays a duplicate rather than a conflict.
	closed := artifact
	closed.Closed = true
	closed.SHA256 = digestOf("first")
	outcome, err := artifactChunkWrite(chunk, 5, closed, held)
	if err != nil || outcome != ArtifactDuplicate {
		t.Fatalf("replay after close: outcome %q err %v", outcome, err)
	}
}

func TestArtifactChunkWriteRefusesAnAlteredReplay(t *testing.T) {
	artifact := heldArtifact{Exists: true, StepKey: "build", TotalBytes: 5, Chunks: 1}
	chunk := artifactChunkFixture(1, 0, "other")
	held := heldChunk{Exists: true, Offset: 0, SHA256: digestOf("first"), Length: 5}
	cases := map[string]heldChunk{
		"other bytes":  held,
		"other offset": {Exists: true, Offset: 1, SHA256: chunk.SHA256, Length: 5},
		"other length": {Exists: true, Offset: 0, SHA256: chunk.SHA256, Length: 4},
	}
	for name, stored := range cases {
		if _, err := artifactChunkWrite(chunk, 5, artifact, stored); !errors.Is(err, ErrConflict) {
			t.Fatalf("%s: want conflict, got %v", name, err)
		}
	}
}

func TestArtifactChunkWriteRefusesGapsAndClosedArtifacts(t *testing.T) {
	artifact := heldArtifact{Exists: true, StepKey: "build", TotalBytes: 5, Chunks: 1}
	skipped := artifactChunkFixture(3, 5, "third")
	if _, err := artifactChunkWrite(skipped, 5, artifact, heldChunk{}); !errors.Is(err, ErrConflict) {
		t.Fatalf("skipped sequence: want conflict, got %v", err)
	}
	gap := artifactChunkFixture(2, 9, "second")
	if _, err := artifactChunkWrite(gap, 6, artifact, heldChunk{}); !errors.Is(err, ErrConflict) {
		t.Fatalf("byte gap: want conflict, got %v", err)
	}
	closed := artifact
	closed.Closed = true
	next := artifactChunkFixture(2, 5, "second")
	if _, err := artifactChunkWrite(next, 6, closed, heldChunk{}); !errors.Is(err, ErrConflict) {
		t.Fatalf("append after close: want conflict, got %v", err)
	}
	other := artifact
	other.StepKey = "test"
	if _, err := artifactChunkWrite(next, 6, other, heldChunk{}); !errors.Is(err, ErrConflict) {
		t.Fatalf("other step: want conflict, got %v", err)
	}
}

func TestArtifactChunkWriteRefusesUploadsPastTheBounds(t *testing.T) {
	artifact := heldArtifact{Exists: true, StepKey: "build",
		TotalBytes: protocol.MaxArtifactBytes - 1, Chunks: maxAgentArtifactChunks - 1}
	chunk := artifactChunkFixture(maxAgentArtifactChunks, protocol.MaxArtifactBytes-1, "xx")
	if _, err := artifactChunkWrite(chunk, 2, artifact, heldChunk{}); !errors.Is(err, ErrConflict) {
		t.Fatalf("byte ceiling: want conflict, got %v", err)
	}
	counted := heldArtifact{Exists: true, StepKey: "build", TotalBytes: 10, Chunks: maxAgentArtifactChunks}
	past := artifactChunkFixture(maxAgentArtifactChunks+1, 10, "tail")
	if _, err := artifactChunkWrite(past, 4, counted, heldChunk{}); !errors.Is(err, ErrConflict) {
		t.Fatalf("chunk ceiling: want conflict, got %v", err)
	}
	empty := artifactChunkFixture(1, 0, "")
	if _, err := artifactChunkWrite(empty, 0, heldArtifact{}, heldChunk{}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("empty chunk: want invalid, got %v", err)
	}
}

// maxAgentArtifactChunks has to leave room for the largest artifact the wire
// contract allows, or a compliant upload would be refused at full size.
func TestArtifactChunkCeilingCoversTheLargestArtifact(t *testing.T) {
	if got := protocol.MinArtifactChunks(protocol.MaxArtifactBytes); got > maxAgentArtifactChunks {
		t.Fatalf("largest artifact needs %d chunks, store allows %d", got, maxAgentArtifactChunks)
	}
}

func TestArtifactCloseAcceptsTheUploadItDescribes(t *testing.T) {
	payload := "first" + "second"
	artifact := heldArtifact{Exists: true, StepKey: "build",
		TotalBytes: int64(len(payload)), Chunks: 2}
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	outcome, err := artifactClose(manifest, artifact, digestOf(payload))
	if err != nil || outcome != ArtifactAccepted {
		t.Fatalf("close: outcome %q err %v", outcome, err)
	}
}

func TestArtifactCloseRefusesAManifestTheBytesDoNotSupport(t *testing.T) {
	payload := "firstsecond"
	artifact := heldArtifact{Exists: true, StepKey: "build",
		TotalBytes: int64(len(payload)), Chunks: 2}
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	if _, err := artifactClose(manifest, heldArtifact{}, ""); !errors.Is(err, ErrConflict) {
		t.Fatalf("no chunks: want conflict, got %v", err)
	}
	if _, err := artifactClose(manifest, artifact, digestOf("other bytes entirely")); !errors.Is(err, ErrConflict) {
		t.Fatalf("digest mismatch: want conflict, got %v", err)
	}
	short := artifact
	short.TotalBytes = 3
	if _, err := artifactClose(manifest, short, digestOf(payload)); !errors.Is(err, ErrConflict) {
		t.Fatalf("total mismatch: want conflict, got %v", err)
	}
	fewer := artifact
	fewer.Chunks = 1
	if _, err := artifactClose(manifest, fewer, digestOf(payload)); !errors.Is(err, ErrConflict) {
		t.Fatalf("sequence mismatch: want conflict, got %v", err)
	}
	other := artifact
	other.StepKey = "test"
	if _, err := artifactClose(manifest, other, digestOf(payload)); !errors.Is(err, ErrConflict) {
		t.Fatalf("other step: want conflict, got %v", err)
	}
}

func TestArtifactCloseIsIdempotentAndRefusesADifferentClose(t *testing.T) {
	payload := "firstsecond"
	manifest := artifactManifestFixture(int64(len(payload)), 2, payload)
	closed := heldArtifact{Exists: true, Closed: true, StepKey: "build",
		TotalBytes: int64(len(payload)), Chunks: 2, SHA256: digestOf(payload)}
	outcome, err := artifactClose(manifest, closed, "")
	if err != nil || outcome != ArtifactDuplicate {
		t.Fatalf("repeat close: outcome %q err %v", outcome, err)
	}
	truncated := manifest
	truncated.Truncated = true
	if _, err := artifactClose(truncated, closed, ""); !errors.Is(err, ErrConflict) {
		t.Fatalf("other truncation evidence: want conflict, got %v", err)
	}
	longer := artifactManifestFixture(int64(len(payload))+1, 2, payload+"!")
	if _, err := artifactClose(longer, closed, ""); !errors.Is(err, ErrConflict) {
		t.Fatalf("other digest: want conflict, got %v", err)
	}
}

func TestArtifactOutcomesIsTheClosedSet(t *testing.T) {
	outcomes := ArtifactOutcomes()
	if len(outcomes) != 2 {
		t.Fatalf("outcomes: %v", outcomes)
	}
	seen := map[ArtifactOutcome]bool{}
	for _, outcome := range outcomes {
		if outcome == "" || seen[outcome] {
			t.Fatalf("outcome %q repeated or empty", outcome)
		}
		seen[outcome] = true
	}
	if !seen[ArtifactAccepted] || !seen[ArtifactDuplicate] {
		t.Fatalf("outcomes missing a member: %v", outcomes)
	}
}

// The path column and the wire contract have to agree on length, or a path
// the agent may send is one the plane cannot store.
func TestArtifactPathFitsTheColumn(t *testing.T) {
	// The longest path the contract accepts: full-length segments, since a
	// single segment is capped well below the path itself.
	path := strings.Repeat("a", 127) + "/" + strings.Repeat("a", 128)
	if len(path) != protocol.MaxArtifactPathBytes || !protocol.ValidArtifactPath(path) {
		t.Fatalf("longest valid path (%d bytes) rejected by the contract", len(path))
	}
	if protocol.MaxArtifactPathBytes > 256 {
		t.Fatalf("path column is 256 bytes, contract allows %d", protocol.MaxArtifactPathBytes)
	}
}

func TestSaveAgentArtifactChunkRefusesAStorelessReceiver(t *testing.T) {
	var store *Store
	if _, err := store.SaveAgentArtifactChunk(t.Context(), []byte{1},
		artifactChunkFixture(1, 0, "first")); !errors.Is(err, ErrInvalid) {
		t.Fatalf("nil store chunk: want invalid, got %v", err)
	}
	if _, err := store.CloseAgentArtifact(t.Context(), []byte{1},
		artifactManifestFixture(5, 1, "first")); !errors.Is(err, ErrInvalid) {
		t.Fatalf("nil store manifest: want invalid, got %v", err)
	}
}
