// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"testing"
)

func TestTheLargestArtifactStillFitsTheChunkCeiling(t *testing.T) {
	// The ceiling is a rule about spending rows, never a second length
	// rule: whatever the wire admits by size has to remain uploadable.
	if got := MinArtifactChunks(MaxArtifactBytes); got > MaxArtifactChunks {
		t.Fatalf("largest artifact needs %d chunks, the contract allows %d", got, MaxArtifactChunks)
	}
}

func TestAManifestCannotCloseMoreChunksThanThePlaneStores(t *testing.T) {
	manifest := sampleArtifactManifest()
	manifest.TotalBytes = MaxArtifactChunks + 1
	manifest.FinalSequence = MaxArtifactChunks
	if err := manifest.Validate(); err != nil {
		t.Fatalf("a close at the ceiling is ordinary: %v", err)
	}
	manifest.FinalSequence = MaxArtifactChunks + 1
	if !errors.Is(manifest.Validate(), ErrInvalid) {
		t.Fatal("a manifest closing more chunks than the plane stores was accepted")
	}
}

func TestAByteAtATimeIsNoLongerACloseTheContractAdmits(t *testing.T) {
	// The case the length rules alone admitted: one byte per chunk over a
	// four-megabyte artifact, four million rows for four megabytes.
	manifest := sampleArtifactManifest()
	manifest.TotalBytes = 4 << 20
	manifest.FinalSequence = 4 << 20
	if !errors.Is(manifest.Validate(), ErrInvalid) {
		t.Fatal("a one-byte-per-chunk close was accepted")
	}
}

func TestAChunkNumberedPastTheCeilingIsRefused(t *testing.T) {
	chunk := chunkOf(MaxArtifactChunks+1, MaxArtifactChunks, 1)
	if !errors.Is(chunk.Validate(), ErrInvalid) {
		t.Fatal("a chunk numbered past the ceiling was accepted")
	}
	inside := chunkOf(MaxArtifactChunks, MaxArtifactChunks-1, 1)
	if err := inside.Validate(); err != nil {
		t.Fatalf("a chunk at the ceiling is ordinary: %v", err)
	}
}

func TestTheCeilingReadsOnlyTheCount(t *testing.T) {
	for _, sequence := range []int64{0, -1, MaxArtifactChunks + 1, 1 << 40} {
		if chunkCountThePlaneWillStore(sequence) {
			t.Fatalf("accepted %d", sequence)
		}
	}
	for _, sequence := range []int64{1, 2, MaxArtifactChunks} {
		if !chunkCountThePlaneWillStore(sequence) {
			t.Fatalf("refused %d", sequence)
		}
	}
}
