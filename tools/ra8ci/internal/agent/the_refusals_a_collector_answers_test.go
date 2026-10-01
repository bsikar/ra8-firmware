// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

// What an artifact collector refuses, and where it refuses it. An artifact is
// evidence about work that has already finished, so the interesting cases are
// the ones where the checkout has changed underneath the collector or the file
// is not what its own metadata said it was. Every refusal here happens before
// a manifest exists, which is what keeps a manifest from closing an upload the
// plane never received.

import (
	"context"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// countingSender keeps the totals rather than the bytes, so a case may stream
// the whole 64 MiB bound without holding it twice in memory.
type countingSender struct {
	chunks int
	bytes  int
	fail   error
}

func (sender *countingSender) send(_ context.Context, chunk protocol.ArtifactChunk) error {
	if sender.fail != nil {
		return sender.fail
	}
	data, err := chunk.Bytes()
	if err != nil {
		return err
	}
	sender.chunks++
	sender.bytes += len(data)
	return nil
}

// collectorOver builds a collector whose clock the caller chooses, so a case
// can spoil the capture time without touching anything else.
func collectorOver(t *testing.T, root string, sender *countingSender, clock func() time.Time) *ArtifactCollector {
	t.Helper()
	collector, err := NewArtifactCollector(root, collectorAssignment(), sender.send, clock)
	if err != nil {
		t.Fatal(err)
	}
	return collector
}

func steadyClock() func() time.Time {
	return func() time.Time { return time.Unix(1750000000, 0).UTC() }
}

func TestNewArtifactCollectorRefusesACheckoutItCannotResolve(t *testing.T) {
	sender := &countingSender{}
	if _, err := NewArtifactCollector(filepath.Join(t.TempDir(), "no-such-checkout"),
		collectorAssignment(), sender.send, steadyClock()); err == nil {
		t.Fatal("absent checkout accepted")
	}
	// A symlinked checkout is resolved, not refused, and the resolved path is
	// what every later path check is judged against.
	real := t.TempDir()
	link := filepath.Join(t.TempDir(), "link")
	if err := os.Symlink(real, link); err != nil {
		t.Skipf("symlinks unavailable: %v", err)
	}
	collector, err := NewArtifactCollector(link, collectorAssignment(), sender.send, steadyClock())
	if err != nil {
		t.Fatalf("symlinked checkout refused: %v", err)
	}
	resolved, err := filepath.EvalSymlinks(real)
	if err != nil {
		t.Fatal(err)
	}
	if collector.root != resolved {
		t.Fatalf("root = %q, want the resolved %q", collector.root, resolved)
	}
	if _, err := NewArtifactCollector(real, protocol.Assignment{}, sender.send, steadyClock()); err == nil {
		t.Fatal("collector built on an unvalidated grant")
	}
}

// The checkout can go away between construction and collection: the image is
// torn down on its own schedule, and a collector holds a path, not a handle.
func TestCollectRefusesACheckoutThatVanished(t *testing.T) {
	root := t.TempDir()
	sender := &countingSender{}
	collector := collectorOver(t, root, sender, steadyClock())
	writeArtifact(t, root, "out/log.txt", []byte("evidence"))
	if err := os.RemoveAll(root); err != nil {
		t.Fatal(err)
	}
	if _, err := collector.Collect(context.Background(), "build", []string{"out/log.txt"}); err == nil {
		t.Fatal("collection from a vanished checkout accepted")
	}
	if sender.chunks != 0 {
		t.Fatalf("uploaded %d chunks from a vanished checkout", sender.chunks)
	}
}

// An output whose parent directory was never created is the same case as an
// output that was never produced: a step that failed early leaves nothing
// behind, and failing the attempt would lose the evidence explaining why.
func TestCollectPassesOverAnOutputWhoseParentWasNeverMade(t *testing.T) {
	root := t.TempDir()
	sender := &countingSender{}
	collector := collectorOver(t, root, sender, steadyClock())
	manifests, err := collector.Collect(context.Background(), "build",
		[]string{"never/made/report.txt", "also/missing.txt"})
	if err != nil {
		t.Fatalf("absent parents refused: %v", err)
	}
	if len(manifests) != 0 || sender.chunks != 0 {
		t.Fatalf("manifests = %d, chunks = %d, want nothing", len(manifests), sender.chunks)
	}
}

// A parent that exists but is a plain file is NOT the absent case: the walk
// must tell the two apart, because one is a step that produced nothing and the
// other is a checkout shaped differently from what the task declared.
func TestCollectRefusesAParentThatIsNotADirectory(t *testing.T) {
	root := t.TempDir()
	writeArtifact(t, root, "blocked", []byte("a file where a directory was declared"))
	sender := &countingSender{}
	collector := collectorOver(t, root, sender, steadyClock())
	if _, err := collector.Collect(context.Background(), "build", []string{"blocked/inside/out.txt"}); err == nil {
		t.Fatal("output under a plain file accepted")
	}
	if sender.chunks != 0 {
		t.Fatalf("uploaded %d chunks under a plain file", sender.chunks)
	}
}

// The step name reaches the wire on every chunk. Collect only asks that it is
// present, so a name the contract will not carry has to be refused at the
// chunk, before the first upload is spent.
func TestCollectRefusesAStepNameTheContractWillNotCarry(t *testing.T) {
	root := t.TempDir()
	writeArtifact(t, root, "out/log.txt", []byte("evidence"))
	sender := &countingSender{}
	collector := collectorOver(t, root, sender, steadyClock())
	_, err := collector.Collect(context.Background(), strings.Repeat("s", 4096), []string{"out/log.txt"})
	if !errors.Is(err, ErrUnsafeArtifact) {
		t.Fatalf("oversized step name accepted: %v", err)
	}
	if sender.chunks != 0 {
		t.Fatalf("uploaded %d chunks on a refused step name", sender.chunks)
	}
}

// A stopped clock passes construction and every path check and only fails at
// the close, where a manifest without a capture time cannot be validated.
func TestCollectRefusesAManifestItCannotClose(t *testing.T) {
	root := t.TempDir()
	writeArtifact(t, root, "out/log.txt", []byte("evidence"))
	sender := &countingSender{}
	collector := collectorOver(t, root, sender, func() time.Time { return time.Time{} })
	_, err := collector.Collect(context.Background(), "build", []string{"out/log.txt"})
	if !errors.Is(err, ErrUnsafeArtifact) {
		t.Fatalf("manifest with no capture time accepted: %v", err)
	}
	// The bytes WERE uploaded before the close was refused. That is the point
	// of the refusal: the plane holds chunks no manifest closes, and the
	// attempt reports the failure rather than closing them dishonestly.
	if sender.chunks == 0 {
		t.Fatal("nothing uploaded, so the refusal was not the close")
	}
}

// erroringReader hands back a fixed prefix and then a failure that is neither
// EOF nor an unexpected EOF: a disk that gave up mid-file.
type erroringReader struct {
	remaining int
	err       error
}

func (reader *erroringReader) Read(buffer []byte) (int, error) {
	if reader.remaining <= 0 {
		return 0, reader.err
	}
	n := len(buffer)
	if n > reader.remaining {
		n = reader.remaining
	}
	for i := range buffer[:n] {
		buffer[i] = 'x'
	}
	reader.remaining -= n
	return n, nil
}

func TestStreamRefusesAReadThatFailedMidFile(t *testing.T) {
	sender := &countingSender{}
	collector := collectorOver(t, t.TempDir(), sender, steadyClock())
	failure := errors.New("input/output error")
	_, err := collector.stream(context.Background(), "build", "out/log.txt",
		&erroringReader{remaining: protocol.MaxArtifactChunkBytes + 7, err: failure})
	if !errors.Is(err, failure) {
		t.Fatalf("mid-file read failure reported as %v", err)
	}
}

// The bound is 64 MiB and whether MORE bytes exist decides Truncated. One byte
// past the bound is the whole difference between a complete artifact and one
// the plane must show as cut short, so both sides of it are pinned exactly.
func TestStreamMarksTruncatedOnlyWhenBytesRemain(t *testing.T) {
	for _, tc := range []struct {
		name      string
		available int
		truncated bool
	}{
		{"exactly the bound", protocol.MaxArtifactBytes, false},
		{"one byte past it", protocol.MaxArtifactBytes + 1, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			sender := &countingSender{}
			collector := collectorOver(t, t.TempDir(), sender, steadyClock())
			manifest, err := collector.stream(context.Background(), "build", "out/log.txt",
				&erroringReader{remaining: tc.available, err: io.EOF})
			if err != nil {
				t.Fatalf("stream at the bound refused: %v", err)
			}
			if manifest.Truncated != tc.truncated {
				t.Fatalf("truncated = %v, want %v", manifest.Truncated, tc.truncated)
			}
			// Either way the manifest closes exactly the bound: the probe byte
			// is read to answer the question and then dropped, never counted.
			if manifest.TotalBytes != int64(protocol.MaxArtifactBytes) {
				t.Fatalf("total = %d, want the bound %d", manifest.TotalBytes, protocol.MaxArtifactBytes)
			}
			if sender.bytes != protocol.MaxArtifactBytes {
				t.Fatalf("uploaded %d bytes, want the bound %d", sender.bytes, protocol.MaxArtifactBytes)
			}
			if err := manifest.Validate(); err != nil {
				t.Fatalf("manifest at the bound does not validate: %v", err)
			}
		})
	}
}

// A read that fails AFTER the bound is reached is still a complete artifact:
// the probe only asks whether anything remains, and a failing answer means
// something does.
func TestStreamTreatsAFailingProbeAsRemainingBytes(t *testing.T) {
	sender := &countingSender{}
	collector := collectorOver(t, t.TempDir(), sender, steadyClock())
	manifest, err := collector.stream(context.Background(), "build", "out/log.txt",
		&erroringReader{remaining: protocol.MaxArtifactBytes, err: errors.New("input/output error")})
	if err != nil {
		t.Fatalf("stream refused at the bound: %v", err)
	}
	if !manifest.Truncated {
		t.Fatal("a failing probe was read as the end of the file")
	}
}

// collectAttemptArtifacts wires an uploader and a collector in that order, and
// each refusal has to carry rather than be reported as no artifacts at all.
func TestCollectAttemptArtifactsCarriesAWiringRefusal(t *testing.T) {
	t.Run("grant the uploader refuses", func(t *testing.T) {
		agent := &Agent{root: t.TempDir()}
		_, err := agent.collectAttemptArtifacts(context.Background(), protocol.Assignment{},
			outputTask(), producedResult("build"), artifactClock())
		if err == nil {
			t.Fatal("unvalidated grant accepted")
		}
	})
	t.Run("checkout the collector refuses", func(t *testing.T) {
		agent := &Agent{root: filepath.Join(t.TempDir(), "no-such-checkout")}
		_, err := agent.collectAttemptArtifacts(context.Background(), collectorAssignment(),
			outputTask(), producedResult("build"), artifactClock())
		if err == nil {
			t.Fatal("unresolvable checkout accepted")
		}
		if errors.Is(err, ErrUnsafeAssignment) {
			t.Fatalf("checkout refusal reported as a grant refusal: %v", err)
		}
	})
	t.Run("no clock", func(t *testing.T) {
		agent := &Agent{root: t.TempDir()}
		_, err := agent.collectAttemptArtifacts(context.Background(), collectorAssignment(),
			outputTask(), producedResult("build"), nil)
		if !errors.Is(err, ErrUnsafeArtifact) {
			t.Fatalf("collection without a clock accepted: %v", err)
		}
	})
}
