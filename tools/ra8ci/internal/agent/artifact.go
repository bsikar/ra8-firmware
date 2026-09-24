// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// ErrUnsafeArtifact is a guest-side refusal: the declared output is not a
// plain file inside the checkout, so the agent will not read it at all.
var ErrUnsafeArtifact = errors.New("agent refused an unsafe artifact path")

// SendArtifactChunk uploads one validated chunk. A failure ends collection of
// the whole set: a partially uploaded artifact must not be closed by a
// manifest claiming bytes the plane never received.
type SendArtifactChunk func(context.Context, protocol.ArtifactChunk) error

// ArtifactCollector turns a step's declared outputs into the wire contract in
// internal/protocol. It reads, it never writes, and it never deletes the file
// it collected: a failed attempt's artifacts stay on the guest for the image
// teardown to carry or discard.
type ArtifactCollector struct {
	root       string
	assignment protocol.Assignment
	send       SendArtifactChunk
	now        func() time.Time
}

// NewArtifactCollector refuses a partial wiring, since a collector with no
// uploader would report artifacts the plane never got.
func NewArtifactCollector(root string, assignment protocol.Assignment, send SendArtifactChunk, now func() time.Time) (*ArtifactCollector, error) {
	if root == "" || send == nil || now == nil {
		return nil, fmt.Errorf("%w: collector needs a checkout, an uploader and a clock", ErrUnsafeAssignment)
	}
	if err := assignment.Validate(); err != nil {
		return nil, err
	}
	resolved, err := filepath.EvalSymlinks(root)
	if err != nil {
		return nil, err
	}
	return &ArtifactCollector{root: resolved, assignment: assignment, send: send, now: now}, nil
}

// Collect streams every declared output that exists and returns the manifests
// closing them, in declaration order. A declared output that was not produced
// is not an error: a step that fails early legitimately leaves nothing behind,
// and failing the attempt on a missing file would lose the evidence that
// explains why.
func (collector *ArtifactCollector) Collect(ctx context.Context, stepName string, outputs []string) ([]protocol.ArtifactManifest, error) {
	if ctx == nil || stepName == "" {
		return nil, fmt.Errorf("%w: collection needs a context and a step", ErrUnsafeArtifact)
	}
	manifests := make([]protocol.ArtifactManifest, 0, len(outputs))
	for _, output := range outputs {
		manifest, collected, err := collector.collectOne(ctx, stepName, output)
		if err != nil {
			return nil, err
		}
		if collected {
			manifests = append(manifests, manifest)
		}
	}
	if err := protocol.ValidateArtifactSet(manifests); err != nil {
		return nil, fmt.Errorf("%w: collected set exceeds the attempt contract", ErrUnsafeArtifact)
	}
	return manifests, nil
}

func (collector *ArtifactCollector) collectOne(ctx context.Context, stepName, output string) (protocol.ArtifactManifest, bool, error) {
	if !protocol.ValidArtifactPath(output) {
		return protocol.ArtifactManifest{}, false, fmt.Errorf("%w: %q", ErrUnsafeArtifact, output)
	}
	path := filepath.Join(collector.root, filepath.FromSlash(output))
	info, err := os.Lstat(path)
	if errors.Is(err, os.ErrNotExist) {
		return protocol.ArtifactManifest{}, false, nil
	}
	if err != nil {
		return protocol.ArtifactManifest{}, false, err
	}
	// Lstat, not Stat: a symlink is refused rather than followed, so a task
	// cannot hand back /etc/shadow by planting a link inside the checkout.
	if !info.Mode().IsRegular() {
		return protocol.ArtifactManifest{}, false, fmt.Errorf("%w: %q is not a plain file", ErrUnsafeArtifact, output)
	}
	file, err := os.Open(path)
	if err != nil {
		return protocol.ArtifactManifest{}, false, err
	}
	defer func() { _ = file.Close() }()
	manifest, err := collector.stream(ctx, stepName, output, file)
	if err != nil {
		return protocol.ArtifactManifest{}, false, err
	}
	return manifest, true, nil
}

func (collector *ArtifactCollector) stream(ctx context.Context, stepName, output string, reader io.Reader) (protocol.ArtifactManifest, error) {
	whole := sha256.New()
	buffer := make([]byte, protocol.MaxArtifactChunkBytes)
	var offset, sequence int64
	truncated := false
	for offset < protocol.MaxArtifactBytes {
		read, err := io.ReadFull(reader, buffer)
		if read > 0 {
			payload := buffer[:read]
			sum := sha256.Sum256(payload)
			sequence++
			chunk := protocol.ArtifactChunk{
				SchemaVersion:     protocol.Version,
				AssignmentID:      collector.assignment.AssignmentID,
				AttemptID:         collector.assignment.AttemptID,
				AssignmentVersion: collector.assignment.AssignmentVersion,
				FencingToken:      collector.assignment.FencingToken,
				StepName:          stepName,
				Path:              output,
				Sequence:          sequence,
				Offset:            offset,
				DataBase64:        base64.StdEncoding.EncodeToString(payload),
				SHA256:            hex.EncodeToString(sum[:]),
			}
			if err := chunk.Validate(); err != nil {
				return protocol.ArtifactManifest{}, fmt.Errorf("%w: %q chunk %d", ErrUnsafeArtifact, output, sequence)
			}
			if err := collector.send(ctx, chunk); err != nil {
				return protocol.ArtifactManifest{}, err
			}
			whole.Write(payload)
			offset += int64(read)
		}
		if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
			break
		}
		if err != nil {
			return protocol.ArtifactManifest{}, err
		}
	}
	if offset >= protocol.MaxArtifactBytes {
		// Whether more bytes exist decides Truncated, and one read is the
		// only way to know. The bytes themselves are dropped.
		var probe [1]byte
		if read, err := reader.Read(probe[:]); read > 0 || (err != nil && !errors.Is(err, io.EOF)) {
			truncated = true
		}
	}
	if offset == 0 {
		// An empty file carries no evidence and the contract has no message
		// for zero bytes. Skipping it keeps the manifest honest.
		return protocol.ArtifactManifest{}, fmt.Errorf("%w: %q is empty", ErrUnsafeArtifact, output)
	}
	manifest := protocol.ArtifactManifest{
		SchemaVersion:     protocol.Version,
		AssignmentID:      collector.assignment.AssignmentID,
		AttemptID:         collector.assignment.AttemptID,
		AssignmentVersion: collector.assignment.AssignmentVersion,
		FencingToken:      collector.assignment.FencingToken,
		StepName:          stepName,
		Path:              output,
		TotalBytes:        offset,
		SHA256:            hex.EncodeToString(whole.Sum(nil)),
		FinalSequence:     sequence,
		Truncated:         truncated,
		CapturedAt:        collector.now().UTC(),
	}
	if err := manifest.Validate(); err != nil {
		return protocol.ArtifactManifest{}, fmt.Errorf("%w: %q manifest", ErrUnsafeArtifact, output)
	}
	return manifest, nil
}
