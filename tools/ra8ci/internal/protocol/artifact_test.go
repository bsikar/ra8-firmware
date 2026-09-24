// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"strings"
	"testing"
	"time"
)

func artifactBody(data []byte) (string, string) {
	sum := sha256.Sum256(data)
	return base64.StdEncoding.EncodeToString(data), hex.EncodeToString(sum[:])
}

func sampleArtifactChunk() ArtifactChunk {
	body, digest := artifactBody([]byte("elf section table\n"))
	return ArtifactChunk{
		SchemaVersion:     Version,
		AssignmentID:      assignmentID,
		AttemptID:         attemptID,
		AssignmentVersion: 3,
		FencingToken:      7,
		StepName:          "build",
		Path:              "build/ra8.elf.map",
		Sequence:          1,
		Offset:            0,
		DataBase64:        body,
		SHA256:            digest,
	}
}

func sampleArtifactManifest() ArtifactManifest {
	chunk := sampleArtifactChunk()
	whole := sha256.Sum256([]byte("elf section table\n"))
	return ArtifactManifest{
		SchemaVersion:     Version,
		AssignmentID:      chunk.AssignmentID,
		AttemptID:         chunk.AttemptID,
		AssignmentVersion: chunk.AssignmentVersion,
		FencingToken:      chunk.FencingToken,
		StepName:          chunk.StepName,
		Path:              chunk.Path,
		TotalBytes:        int64(len("elf section table\n")),
		SHA256:            hex.EncodeToString(whole[:]),
		FinalSequence:     1,
		CapturedAt:        time.Unix(1790000000, 0).UTC(),
	}
}

func TestValidArtifactPathAccepts(t *testing.T) {
	for _, path := range []string{
		"ra8.elf",
		"build/ra8.elf.map",
		"logs/hil/segment-0001.json",
		"a",
		strings.Repeat("a", maxArtifactSegmentBytes),
		strings.TrimSuffix(strings.Repeat("ab/", maxArtifactDepth), "/"),
	} {
		if !ValidArtifactPath(path) {
			t.Fatalf("rejected a safe path: %q", path)
		}
	}
}

func TestValidArtifactPathRefuses(t *testing.T) {
	for _, path := range []string{
		"",
		"/etc/passwd",
		"build/",
		"../escape",
		"build/../../escape",
		"build/./same",
		"build//double",
		`build\ra8.elf`,
		"C:/build/ra8.elf",
		"build/ra8*.elf",
		"build/ra8?.elf",
		`build/"quoted"`,
		"build/<redirect>",
		"build/pipe|d",
		"build/space here",
		"build/trailing.",
		"build/NUL",
		"build/con.txt",
		"LPT9/report.json",
		"build/tab\there",
		"build/ra8\u00e9.elf",
		strings.Repeat("a", maxArtifactSegmentBytes+1),
		strings.Repeat("ab/", maxArtifactDepth) + "deep",
		strings.Repeat("ab/", MaxArtifactPathBytes),
	} {
		if ValidArtifactPath(path) {
			t.Fatalf("accepted an unsafe path: %q", path)
		}
	}
}

func TestArtifactChunkValidation(t *testing.T) {
	chunk := sampleArtifactChunk()
	if err := chunk.Validate(); err != nil {
		t.Fatal(err)
	}
	mutations := []func(*ArtifactChunk){
		func(c *ArtifactChunk) { c.SchemaVersion = Version + 1 },
		func(c *ArtifactChunk) { c.AssignmentID = "bad" },
		func(c *ArtifactChunk) { c.AttemptID = "bad" },
		func(c *ArtifactChunk) { c.AssignmentVersion = 0 },
		func(c *ArtifactChunk) { c.FencingToken = 0 },
		func(c *ArtifactChunk) { c.StepName = "" },
		func(c *ArtifactChunk) { c.StepName = " build" },
		func(c *ArtifactChunk) { c.Path = "../escape" },
		func(c *ArtifactChunk) { c.Sequence = 0 },
		func(c *ArtifactChunk) { c.Offset = -1 },
		func(c *ArtifactChunk) { c.Offset = MaxArtifactBytes },
		func(c *ArtifactChunk) { c.DataBase64 = "not base64!" },
		func(c *ArtifactChunk) { c.DataBase64, c.SHA256 = artifactBody(nil) },
		func(c *ArtifactChunk) { c.SHA256 = strings.Repeat("0", 64) },
		func(c *ArtifactChunk) { c.SHA256 = "bad" },
		// A first chunk cannot start at a non-zero offset, and a second
		// cannot start beyond one full chunk of payload.
		func(c *ArtifactChunk) { c.Offset = 1 },
		func(c *ArtifactChunk) { c.Sequence, c.Offset = 2, MaxArtifactChunkBytes+1 },
	}
	for index, mutate := range mutations {
		copied := chunk
		mutate(&copied)
		if !errors.Is(copied.Validate(), ErrInvalid) {
			t.Fatalf("mutation %d was accepted: %+v", index, copied)
		}
	}
}

func TestArtifactChunkRefusesOversizedPayload(t *testing.T) {
	chunk := sampleArtifactChunk()
	chunk.DataBase64, chunk.SHA256 = artifactBody(make([]byte, MaxArtifactChunkBytes+1))
	if !errors.Is(chunk.Validate(), ErrInvalid) {
		t.Fatal("a chunk larger than the chunk limit was accepted")
	}
	chunk.DataBase64, chunk.SHA256 = artifactBody(make([]byte, MaxArtifactChunkBytes))
	if err := chunk.Validate(); err != nil {
		t.Fatalf("a chunk at exactly the limit was refused: %v", err)
	}
}

func TestArtifactChunkRefusesPayloadPastTheArtifactLimit(t *testing.T) {
	chunk := sampleArtifactChunk()
	data := make([]byte, MaxArtifactChunkBytes)
	chunk.DataBase64, chunk.SHA256 = artifactBody(data)
	chunk.Sequence = MaxArtifactBytes/MaxArtifactChunkBytes + 1
	chunk.Offset = MaxArtifactBytes - int64(len(data)) + 1
	if !errors.Is(chunk.Validate(), ErrInvalid) {
		t.Fatal("a chunk ending past the artifact limit was accepted")
	}
}

func TestArtifactChunkBytesRoundTrip(t *testing.T) {
	chunk := sampleArtifactChunk()
	data, err := chunk.Bytes()
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "elf section table\n" {
		t.Fatalf("decoded the wrong payload: %q", data)
	}
	chunk.SHA256 = strings.Repeat("0", 64)
	if _, err := chunk.Bytes(); !errors.Is(err, ErrInvalid) {
		t.Fatal("Bytes returned a payload for an unvalidated chunk")
	}
}

func TestMinArtifactChunks(t *testing.T) {
	cases := []struct {
		total int64
		want  int64
	}{
		{0, 0},
		{-1, 0},
		{1, 1},
		{MaxArtifactChunkBytes, 1},
		{MaxArtifactChunkBytes + 1, 2},
		{3 * MaxArtifactChunkBytes, 3},
		{MaxArtifactBytes, MaxArtifactBytes / MaxArtifactChunkBytes},
	}
	for _, testCase := range cases {
		if got := MinArtifactChunks(testCase.total); got != testCase.want {
			t.Fatalf("MinArtifactChunks(%d) = %d, want %d", testCase.total, got, testCase.want)
		}
	}
}

func TestArtifactManifestValidation(t *testing.T) {
	manifest := sampleArtifactManifest()
	if err := manifest.Validate(); err != nil {
		t.Fatal(err)
	}
	manifest.Truncated = true
	if err := manifest.Validate(); err != nil {
		t.Fatalf("a truncated artifact is still evidence: %v", err)
	}
	mutations := []func(*ArtifactManifest){
		func(m *ArtifactManifest) { m.SchemaVersion = Version + 1 },
		func(m *ArtifactManifest) { m.AssignmentID = "bad" },
		func(m *ArtifactManifest) { m.AttemptID = "bad" },
		func(m *ArtifactManifest) { m.AssignmentVersion = 0 },
		func(m *ArtifactManifest) { m.FencingToken = 0 },
		func(m *ArtifactManifest) { m.StepName = "" },
		func(m *ArtifactManifest) { m.Path = "/absolute" },
		func(m *ArtifactManifest) { m.SHA256 = "bad" },
		func(m *ArtifactManifest) { m.CapturedAt = time.Time{} },
		func(m *ArtifactManifest) { m.TotalBytes = 0 },
		func(m *ArtifactManifest) { m.TotalBytes = MaxArtifactBytes + 1 },
		func(m *ArtifactManifest) { m.FinalSequence = 0 },
		// More chunks than bytes, and fewer chunks than the bytes need.
		func(m *ArtifactManifest) { m.FinalSequence = m.TotalBytes + 1 },
		func(m *ArtifactManifest) {
			m.TotalBytes, m.FinalSequence = 4*MaxArtifactChunkBytes, 3
		},
	}
	for index, mutate := range mutations {
		copied := manifest
		mutate(&copied)
		if !errors.Is(copied.Validate(), ErrInvalid) {
			t.Fatalf("mutation %d was accepted: %+v", index, copied)
		}
	}
}

func TestArtifactManifestCovers(t *testing.T) {
	manifest := sampleArtifactManifest()
	chunk := sampleArtifactChunk()
	if err := manifest.Covers(chunk); err != nil {
		t.Fatal(err)
	}
	mutations := []func(*ArtifactChunk){
		func(c *ArtifactChunk) { c.AssignmentID = attemptID },
		func(c *ArtifactChunk) { c.AttemptID = assignmentID },
		func(c *ArtifactChunk) { c.AssignmentVersion++ },
		func(c *ArtifactChunk) { c.FencingToken++ },
		func(c *ArtifactChunk) { c.StepName = "test" },
		func(c *ArtifactChunk) { c.Path = "build/other.map" },
		func(c *ArtifactChunk) { c.Sequence = 2 },
	}
	for index, mutate := range mutations {
		copied := chunk
		mutate(&copied)
		if !errors.Is(manifest.Covers(copied), ErrInvalid) {
			t.Fatalf("mutation %d was covered: %+v", index, copied)
		}
	}
}

func TestArtifactManifestCoversRefusesPayloadPastTheClose(t *testing.T) {
	manifest := sampleArtifactManifest()
	manifest.TotalBytes = 4
	manifest.FinalSequence = 1
	if !errors.Is(manifest.Covers(sampleArtifactChunk()), ErrInvalid) {
		t.Fatal("a chunk ending past the closed length was covered")
	}
}

func TestArtifactManifestCoversRefusesInvalidInput(t *testing.T) {
	manifest := sampleArtifactManifest()
	manifest.TotalBytes = 0
	if !errors.Is(manifest.Covers(sampleArtifactChunk()), ErrInvalid) {
		t.Fatal("an invalid manifest covered a chunk")
	}
	manifest = sampleArtifactManifest()
	chunk := sampleArtifactChunk()
	chunk.Sequence = 0
	if !errors.Is(manifest.Covers(chunk), ErrInvalid) {
		t.Fatal("an invalid chunk was covered")
	}
}

func TestValidateArtifactSet(t *testing.T) {
	first := sampleArtifactManifest()
	second := sampleArtifactManifest()
	second.Path = "logs/hil/segment-0001.json"
	if err := ValidateArtifactSet([]ArtifactManifest{first, second}); err != nil {
		t.Fatal(err)
	}
	if err := ValidateArtifactSet(nil); err != nil {
		t.Fatalf("an attempt with no artifacts is normal: %v", err)
	}
	if !errors.Is(ValidateArtifactSet([]ArtifactManifest{first, first}), ErrInvalid) {
		t.Fatal("two manifests for one path were accepted")
	}
	broken := sampleArtifactManifest()
	broken.TotalBytes = 0
	if !errors.Is(ValidateArtifactSet([]ArtifactManifest{first, broken}), ErrInvalid) {
		t.Fatal("an invalid manifest was accepted inside a set")
	}
}

func TestValidateArtifactSetBounds(t *testing.T) {
	many := make([]ArtifactManifest, 0, MaxArtifactsPerAttempt+1)
	for index := 0; index <= MaxArtifactsPerAttempt; index++ {
		manifest := sampleArtifactManifest()
		manifest.Path = "logs/segment-" + strings.Repeat("0", 3) + string(rune('a'+index%26)) + hex.EncodeToString([]byte{byte(index)})
		many = append(many, manifest)
	}
	if !errors.Is(ValidateArtifactSet(many), ErrInvalid) {
		t.Fatal("more artifacts than the per-attempt limit were accepted")
	}

	big := sampleArtifactManifest()
	big.TotalBytes = MaxArtifactBytes
	big.FinalSequence = MinArtifactChunks(big.TotalBytes)
	other := big
	other.Path = "logs/second.bin"
	if !errors.Is(ValidateArtifactSet([]ArtifactManifest{big, other}), ErrInvalid) {
		t.Fatal("an attempt uploading past the total limit was accepted")
	}
}
