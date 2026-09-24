// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/source"
)

const (
	collectorAssignmentID = "01994d50-1234-7abc-8abc-0123456789ab"
	collectorAttemptID    = "01994d50-1234-7abc-8abc-0123456789ac"
)

func collectorAssignment() protocol.Assignment {
	return protocol.Assignment{
		SchemaVersion:     protocol.Version,
		AssignmentID:      collectorAssignmentID,
		AttemptID:         collectorAttemptID,
		AssignmentVersion: 4,
		FencingToken:      9,
		Task:              protocol.TaskRef{Name: "build", Version: 1},
		CatalogSHA256:     hex.EncodeToString(sha256Sum([]byte("catalog"))),
		Source: protocol.SourceRef{
			Algorithm:      source.Algorithm,
			Commit:         hex.EncodeToString(sha256Sum([]byte("commit")))[:40],
			SnapshotSHA256: hex.EncodeToString(sha256Sum([]byte("snapshot"))),
		},
		DeadlineAt:  time.Unix(1790000600, 0).UTC(),
		RemainingMS: 600000,
	}
}

func sha256Sum(data []byte) []byte {
	sum := sha256.Sum256(data)
	return sum[:]
}

type recordingSender struct {
	chunks []protocol.ArtifactChunk
	fail   error
}

func (sender *recordingSender) send(_ context.Context, chunk protocol.ArtifactChunk) error {
	if sender.fail != nil {
		return sender.fail
	}
	sender.chunks = append(sender.chunks, chunk)
	return nil
}

func newCollector(t *testing.T, root string, sender *recordingSender) *ArtifactCollector {
	t.Helper()
	collector, err := NewArtifactCollector(root, collectorAssignment(), sender.send, func() time.Time {
		return time.Unix(1790000000, 0).UTC()
	})
	if err != nil {
		t.Fatal(err)
	}
	return collector
}

func writeArtifact(t *testing.T, root, name string, data []byte) {
	t.Helper()
	path := filepath.Join(root, filepath.FromSlash(name))
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestCollectStreamsAndClosesAnArtifact(t *testing.T) {
	root := t.TempDir()
	payload := make([]byte, 2*protocol.MaxArtifactChunkBytes+17)
	for index := range payload {
		payload[index] = byte(index)
	}
	writeArtifact(t, root, "build/ra8.elf.map", payload)
	sender := &recordingSender{}
	collector := newCollector(t, root, sender)

	manifests, err := collector.Collect(context.Background(), "build", []string{"build/ra8.elf.map"})
	if err != nil {
		t.Fatal(err)
	}
	if len(manifests) != 1 {
		t.Fatalf("got %d manifests, want 1", len(manifests))
	}
	manifest := manifests[0]
	if manifest.TotalBytes != int64(len(payload)) || manifest.FinalSequence != 3 || manifest.Truncated {
		t.Fatalf("manifest does not describe the file: %+v", manifest)
	}
	if manifest.SHA256 != hex.EncodeToString(sha256Sum(payload)) {
		t.Fatal("manifest digest does not cover the file")
	}
	if len(sender.chunks) != 3 {
		t.Fatalf("got %d chunks, want 3", len(sender.chunks))
	}
	var rebuilt []byte
	for index, chunk := range sender.chunks {
		if chunk.Sequence != int64(index+1) || chunk.Offset != int64(len(rebuilt)) {
			t.Fatalf("chunk %d is out of order: %+v", index, chunk)
		}
		if err := manifest.Covers(chunk); err != nil {
			t.Fatalf("chunk %d is not covered by its manifest: %v", index, err)
		}
		data, err := base64.StdEncoding.DecodeString(chunk.DataBase64)
		if err != nil {
			t.Fatal(err)
		}
		rebuilt = append(rebuilt, data...)
	}
	if string(rebuilt) != string(payload) {
		t.Fatal("the uploaded chunks do not reassemble into the file")
	}
}

func TestCollectSkipsAnOutputTheStepNeverProduced(t *testing.T) {
	root := t.TempDir()
	writeArtifact(t, root, "build/ra8.elf.map", []byte("map"))
	sender := &recordingSender{}
	collector := newCollector(t, root, sender)

	manifests, err := collector.Collect(context.Background(), "build",
		[]string{"build/missing.json", "build/ra8.elf.map"})
	if err != nil {
		t.Fatal(err)
	}
	if len(manifests) != 1 || manifests[0].Path != "build/ra8.elf.map" {
		t.Fatalf("a missing declared output should be skipped, got %+v", manifests)
	}
}

func TestCollectKeepsDeclarationOrder(t *testing.T) {
	root := t.TempDir()
	writeArtifact(t, root, "second.txt", []byte("second"))
	writeArtifact(t, root, "first.txt", []byte("first"))
	sender := &recordingSender{}
	collector := newCollector(t, root, sender)

	manifests, err := collector.Collect(context.Background(), "build", []string{"first.txt", "second.txt"})
	if err != nil {
		t.Fatal(err)
	}
	if len(manifests) != 2 || manifests[0].Path != "first.txt" || manifests[1].Path != "second.txt" {
		t.Fatalf("collection reordered the declared outputs: %+v", manifests)
	}
}

func TestCollectRefusesUnsafePaths(t *testing.T) {
	root := t.TempDir()
	writeArtifact(t, root, "build/ra8.elf.map", []byte("map"))
	for _, output := range []string{"../escape", "/etc/passwd", "build/../../escape", `build\ra8.elf`, "build/NUL"} {
		sender := &recordingSender{}
		collector := newCollector(t, root, sender)
		if _, err := collector.Collect(context.Background(), "build", []string{output}); !errors.Is(err, ErrUnsafeArtifact) {
			t.Fatalf("collected an unsafe path %q: %v", output, err)
		}
		if len(sender.chunks) != 0 {
			t.Fatalf("an unsafe path %q spent an upload", output)
		}
	}
}

func TestCollectRefusesSomethingThatIsNotAPlainFile(t *testing.T) {
	root := t.TempDir()
	writeArtifact(t, root, "outside.txt", []byte("secret"))
	if err := os.Symlink(filepath.Join(root, "outside.txt"), filepath.Join(root, "link.txt")); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	if err := os.MkdirAll(filepath.Join(root, "adir"), 0o755); err != nil {
		t.Fatal(err)
	}
	for _, output := range []string{"link.txt", "adir"} {
		sender := &recordingSender{}
		collector := newCollector(t, root, sender)
		if _, err := collector.Collect(context.Background(), "build", []string{output}); !errors.Is(err, ErrUnsafeArtifact) {
			t.Fatalf("collected %q, which is not a plain file: %v", output, err)
		}
		if len(sender.chunks) != 0 {
			t.Fatalf("%q spent an upload", output)
		}
	}
}

func TestCollectRefusesAnEmptyFile(t *testing.T) {
	root := t.TempDir()
	writeArtifact(t, root, "empty.log", nil)
	sender := &recordingSender{}
	collector := newCollector(t, root, sender)
	if _, err := collector.Collect(context.Background(), "build", []string{"empty.log"}); !errors.Is(err, ErrUnsafeArtifact) {
		t.Fatalf("an empty file was closed by a manifest: %v", err)
	}
}

func TestCollectStopsOnAnUploadFailure(t *testing.T) {
	root := t.TempDir()
	writeArtifact(t, root, "build/ra8.elf.map", make([]byte, protocol.MaxArtifactChunkBytes+1))
	failure := errors.New("upload refused")
	sender := &recordingSender{fail: failure}
	collector := newCollector(t, root, sender)
	if _, err := collector.Collect(context.Background(), "build", []string{"build/ra8.elf.map"}); !errors.Is(err, failure) {
		t.Fatalf("a failed upload did not end collection: %v", err)
	}
}

func TestCollectRefusesMoreArtifactsThanTheAttemptAllows(t *testing.T) {
	root := t.TempDir()
	outputs := make([]string, 0, protocol.MaxArtifactsPerAttempt+1)
	for index := 0; index <= protocol.MaxArtifactsPerAttempt; index++ {
		name := "logs/segment-" + hex.EncodeToString([]byte{byte(index)}) + ".json"
		writeArtifact(t, root, name, []byte("segment"))
		outputs = append(outputs, name)
	}
	sender := &recordingSender{}
	collector := newCollector(t, root, sender)
	if _, err := collector.Collect(context.Background(), "build", outputs); !errors.Is(err, ErrUnsafeArtifact) {
		t.Fatalf("an over-sized artifact set was accepted: %v", err)
	}
}

func TestNewArtifactCollectorRefusesPartialWiring(t *testing.T) {
	root := t.TempDir()
	sender := &recordingSender{}
	clock := func() time.Time { return time.Unix(1790000000, 0).UTC() }
	if _, err := NewArtifactCollector("", collectorAssignment(), sender.send, clock); err == nil {
		t.Fatal("a collector with no checkout was built")
	}
	if _, err := NewArtifactCollector(root, collectorAssignment(), nil, clock); err == nil {
		t.Fatal("a collector with no uploader was built")
	}
	if _, err := NewArtifactCollector(root, collectorAssignment(), sender.send, nil); err == nil {
		t.Fatal("a collector with no clock was built")
	}
	if _, err := NewArtifactCollector(root, protocol.Assignment{}, sender.send, clock); !errors.Is(err, protocol.ErrInvalid) {
		t.Fatalf("a collector was built on an unvalidated grant: %v", err)
	}
}

func TestCollectRefusesAnEmptyStepName(t *testing.T) {
	root := t.TempDir()
	sender := &recordingSender{}
	collector := newCollector(t, root, sender)
	if _, err := collector.Collect(context.Background(), "", nil); !errors.Is(err, ErrUnsafeArtifact) {
		t.Fatalf("collection ran without a step: %v", err)
	}
}
