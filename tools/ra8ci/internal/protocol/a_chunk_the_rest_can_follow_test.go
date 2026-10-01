// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"testing"
)

// chunkOf builds a valid chunk of the given length at the given position.
func chunkOf(sequence, offset, length int64) ArtifactChunk {
	data := make([]byte, length)
	for index := range data {
		data[index] = byte('a' + index%26)
	}
	sum := sha256.Sum256(data)
	chunk := sampleArtifactChunk()
	chunk.Sequence = sequence
	chunk.Offset = offset
	chunk.DataBase64 = base64.StdEncoding.EncodeToString(data)
	chunk.SHA256 = hex.EncodeToString(sum[:])
	return chunk
}

// closing builds a manifest over an artifact of this length and chunk count.
func closing(total, final int64) ArtifactManifest {
	manifest := sampleArtifactManifest()
	manifest.TotalBytes = total
	manifest.FinalSequence = final
	return manifest
}

func TestTheFinalChunkEndsWhereTheArtifactEnds(t *testing.T) {
	manifest := closing(64, 1)
	if err := manifest.Covers(chunkOf(1, 0, 64)); err != nil {
		t.Fatalf("the whole artifact in one chunk is covered: %v", err)
	}
	if !errors.Is(manifest.Covers(chunkOf(1, 0, 18)), ErrInvalid) {
		t.Fatal("a final chunk leaving 46 bytes unaccounted for was covered")
	}
}

func TestAChunkLeavesAtLeastOneByteForEachChunkAfterIt(t *testing.T) {
	// Four chunks close 64 bytes. A first chunk holding 62 leaves two
	// bytes for the three chunks that follow, which cannot be.
	manifest := closing(64, 4)
	if !errors.Is(manifest.Covers(chunkOf(1, 0, 62)), ErrInvalid) {
		t.Fatal("a chunk starving the chunks after it was covered")
	}
	if err := manifest.Covers(chunkOf(1, 0, 61)); err != nil {
		t.Fatalf("one byte apiece for the rest is enough: %v", err)
	}
}

func TestAChunkLeavesNoMoreThanTheChunksAfterItCanCarry(t *testing.T) {
	// Two chunks close an artifact three full chunks long, so whatever the
	// first one carries, the second cannot finish it.
	total := 3 * MaxArtifactChunkBytes
	manifest := closing(int64(total), 2)
	if !errors.Is(manifest.Covers(chunkOf(1, 0, MaxArtifactChunkBytes)), ErrInvalid) {
		t.Fatal("a chunk leaving more than one chunk can carry was covered")
	}
}

func TestAMiddleChunkIsStillCovered(t *testing.T) {
	total := int64(3 * MaxArtifactChunkBytes)
	manifest := closing(total, 3)
	for sequence := int64(1); sequence <= 3; sequence++ {
		offset := (sequence - 1) * MaxArtifactChunkBytes
		if err := manifest.Covers(chunkOf(sequence, offset, MaxArtifactChunkBytes)); err != nil {
			t.Fatalf("chunk %d of an exact three-chunk upload: %v", sequence, err)
		}
	}
}

func TestAnUnevenUploadIsCovered(t *testing.T) {
	// Chunks a real uploader produces are not all the same size: it cuts a
	// chunk per write, so the rule has to admit a short one in the middle.
	manifest := closing(300, 3)
	if err := manifest.Covers(chunkOf(1, 0, 10)); err != nil {
		t.Fatalf("a short first chunk: %v", err)
	}
	if err := manifest.Covers(chunkOf(2, 10, 289)); err != nil {
		t.Fatalf("a long middle chunk: %v", err)
	}
	if err := manifest.Covers(chunkOf(3, 299, 1)); err != nil {
		t.Fatalf("a one-byte final chunk: %v", err)
	}
}

func TestAChunkPastTheCloseIsStillRefused(t *testing.T) {
	// The bound Covers already held: this rule narrows it, never widens it.
	manifest := closing(64, 2)
	if !errors.Is(manifest.Covers(chunkOf(1, 0, 65)), ErrInvalid) {
		t.Fatal("a chunk ending past the closed length was covered")
	}
}

func TestTheRuleReadsOnlyWhatTheManifestCloses(t *testing.T) {
	// Directly, so the arithmetic is pinned without the surrounding checks.
	cases := []struct {
		total, final, sequence, offset, payload int64
		want                                    bool
	}{
		{64, 1, 1, 0, 64, true},
		{64, 1, 1, 0, 63, false},
		{64, 2, 1, 0, 63, true},
		{64, 2, 1, 0, 64, false},
		{64, 2, 2, 63, 1, true},
		{64, 2, 2, 0, 63, false},
	}
	for index, testCase := range cases {
		manifest := closing(testCase.total, testCase.final)
		chunk := sampleArtifactChunk()
		chunk.Sequence, chunk.Offset = testCase.sequence, testCase.offset
		err := checkChunkLeavesRoomForTheChunksThatFollow(manifest, chunk, testCase.payload)
		if (err == nil) != testCase.want {
			t.Fatalf("case %d: err = %v, want ok = %v", index, err, testCase.want)
		}
	}
}
