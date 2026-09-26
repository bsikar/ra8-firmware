// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"strings"
	"time"
)

// Artifact bounds. A chunk is smaller than the JSON envelope so a base64 body
// plus its fields always fits MaxJSONBytes. The per-attempt totals exist so a
// guest cannot fill the plane's storage through the same endpoint it uses for
// evidence.
const (
	MaxArtifactChunkBytes   = 256 << 10
	MaxArtifactBytes        = 64 << 20
	MaxArtifactsPerAttempt  = 64
	MaxArtifactPathBytes    = 256
	maxArtifactSegmentBytes = 128
	maxArtifactDepth        = 16
)

// reservedDeviceNames are the Windows device names a path segment may not use.
// An artifact is produced in a guest and written back out on a host that may
// run either OS, so the stricter rule is the shared one.
var reservedDeviceNames = map[string]bool{
	"CON": true, "PRN": true, "AUX": true, "NUL": true,
	"COM1": true, "COM2": true, "COM3": true, "COM4": true, "COM5": true,
	"COM6": true, "COM7": true, "COM8": true, "COM9": true,
	"LPT1": true, "LPT2": true, "LPT3": true, "LPT4": true, "LPT5": true,
	"LPT6": true, "LPT7": true, "LPT8": true, "LPT9": true,
}

// ArtifactChunk streams one ordered slice of a single artifact. Sequence and
// Offset are per artifact, not per attempt, so two artifacts from one step
// upload independently and a redelivered chunk is recognized by both.
type ArtifactChunk struct {
	SchemaVersion     int    `json:"schema_version"`
	AssignmentID      string `json:"assignment_id"`
	AttemptID         string `json:"attempt_id"`
	AssignmentVersion int64  `json:"assignment_version"`
	FencingToken      int64  `json:"fencing_token"`
	StepName          string `json:"step_name"`
	Path              string `json:"path"`
	Sequence          int64  `json:"sequence"`
	Offset            int64  `json:"offset"`
	DataBase64        string `json:"data_base64"`
	SHA256            string `json:"sha256"`
}

// ArtifactManifest closes one artifact and states what the plane should now
// hold. It is evidence about bytes already uploaded, never an intent: SHA256
// and TotalBytes cover the uploaded chunks, so a receiver can refuse a
// manifest that does not match what it stored.
type ArtifactManifest struct {
	SchemaVersion     int       `json:"schema_version"`
	AssignmentID      string    `json:"assignment_id"`
	AttemptID         string    `json:"attempt_id"`
	AssignmentVersion int64     `json:"assignment_version"`
	FencingToken      int64     `json:"fencing_token"`
	StepName          string    `json:"step_name"`
	Path              string    `json:"path"`
	TotalBytes        int64     `json:"total_bytes"`
	SHA256            string    `json:"sha256"`
	FinalSequence     int64     `json:"final_sequence"`
	Truncated         bool      `json:"truncated"`
	CapturedAt        time.Time `json:"captured_at"`
}

// ValidArtifactPath accepts a relative, slash-separated, ASCII path that is
// safe to create under a collection directory on both Linux and Windows.
func ValidArtifactPath(value string) bool {
	if value == "" || len(value) > MaxArtifactPathBytes ||
		strings.HasPrefix(value, "/") || strings.HasSuffix(value, "/") {
		return false
	}
	for _, char := range value {
		if char < 0x21 || char > 0x7e || strings.ContainsRune(`\:*?"<>|`, char) {
			return false
		}
	}
	segments := strings.Split(value, "/")
	if len(segments) > maxArtifactDepth {
		return false
	}
	for _, segment := range segments {
		if !validArtifactSegment(segment) {
			return false
		}
	}
	return true
}

func validArtifactSegment(segment string) bool {
	if segment == "" || segment == "." || segment == ".." ||
		len(segment) > maxArtifactSegmentBytes || strings.HasSuffix(segment, ".") {
		return false
	}
	base := segment
	if index := strings.Index(base, "."); index >= 0 {
		base = base[:index]
	}
	return !reservedDeviceNames[strings.ToUpper(base)]
}

// MinArtifactChunks is the fewest chunks that can carry a payload of this
// size. A manifest claiming fewer has not described the upload it is closing.
func MinArtifactChunks(total int64) int64 {
	if total <= 0 {
		return 0
	}
	return (total + MaxArtifactChunkBytes - 1) / MaxArtifactChunkBytes
}

// Validate verifies the encoded bytes, their digest binding, and the position
// this chunk claims inside its artifact.
func (chunk ArtifactChunk) Validate() error {
	if chunk.SchemaVersion != Version ||
		!validGrant(chunk.AssignmentID, chunk.AttemptID, chunk.AssignmentVersion, chunk.FencingToken) ||
		!validStepName(chunk.StepName) || !ValidArtifactPath(chunk.Path) ||
		chunk.Sequence < 1 || chunk.Offset < 0 || chunk.Offset >= MaxArtifactBytes ||
		!ValidSHA256(chunk.SHA256) {
		return ErrInvalid
	}
	data, err := base64.StdEncoding.DecodeString(chunk.DataBase64)
	if err != nil || len(data) == 0 || len(data) > MaxArtifactChunkBytes {
		return ErrInvalid
	}
	if chunk.Offset+int64(len(data)) > MaxArtifactBytes {
		return ErrInvalid
	}
	// The lowest offset a chunk at this sequence can hold is one byte per
	// earlier chunk, and the highest is a full chunk each.
	if chunk.Offset < chunk.Sequence-1 || chunk.Offset > (chunk.Sequence-1)*MaxArtifactChunkBytes {
		return ErrInvalid
	}
	sum := sha256.Sum256(data)
	if hex.EncodeToString(sum[:]) != chunk.SHA256 {
		return ErrInvalid
	}
	return nil
}

// Bytes returns the decoded payload of an already validated chunk.
func (chunk ArtifactChunk) Bytes() ([]byte, error) {
	if err := chunk.Validate(); err != nil {
		return nil, err
	}
	return base64.StdEncoding.DecodeString(chunk.DataBase64)
}

// Validate enforces a coherent close: a digest over real bytes, a chunk count
// that could have carried them, and a capture time.
func (manifest ArtifactManifest) Validate() error {
	if manifest.SchemaVersion != Version ||
		!validGrant(manifest.AssignmentID, manifest.AttemptID, manifest.AssignmentVersion, manifest.FencingToken) ||
		!validStepName(manifest.StepName) || !ValidArtifactPath(manifest.Path) ||
		!ValidSHA256(manifest.SHA256) || manifest.CapturedAt.IsZero() ||
		manifest.TotalBytes < 1 || manifest.TotalBytes > MaxArtifactBytes {
		return ErrInvalid
	}
	if manifest.FinalSequence < MinArtifactChunks(manifest.TotalBytes) || manifest.FinalSequence > manifest.TotalBytes {
		return ErrInvalid
	}
	return nil
}

// Covers reports whether a chunk belongs to this artifact on the same grant
// and lands inside the length the manifest closes.
func (manifest ArtifactManifest) Covers(chunk ArtifactChunk) error {
	if err := manifest.Validate(); err != nil {
		return err
	}
	if err := chunk.Validate(); err != nil {
		return err
	}
	if chunk.AssignmentID != manifest.AssignmentID || chunk.AttemptID != manifest.AttemptID ||
		chunk.AssignmentVersion != manifest.AssignmentVersion || chunk.FencingToken != manifest.FencingToken ||
		chunk.StepName != manifest.StepName || chunk.Path != manifest.Path ||
		chunk.Sequence > manifest.FinalSequence {
		return ErrInvalid
	}
	data, err := base64.StdEncoding.DecodeString(chunk.DataBase64)
	if err != nil || chunk.Offset+int64(len(data)) > manifest.TotalBytes {
		return ErrInvalid
	}
	return nil
}

// ValidateArtifactSet checks what no single manifest can: one entry per path,
// the per-attempt count, and the total the attempt is allowed to upload.
func ValidateArtifactSet(manifests []ArtifactManifest) error {
	if len(manifests) > MaxArtifactsPerAttempt {
		return ErrInvalid
	}
	seen := make(map[string]bool, len(manifests))
	var total int64
	for _, manifest := range manifests {
		if err := manifest.Validate(); err != nil {
			return err
		}
		// Before this entry joins the count, the byte total or the path
		// set: all three are budgets of one attempt, and a set naming more
		// than one grant answers them against the wrong denominator.
		if err := checkArtifactSetNamesOneAttempt(manifests[0], manifest); err != nil {
			return err
		}
		if seen[manifest.Path] {
			return ErrInvalid
		}
		seen[manifest.Path] = true
		total += manifest.TotalBytes
		if total > MaxArtifactBytes {
			return ErrInvalid
		}
	}
	return nil
}

func validStepName(name string) bool {
	return name != "" && len(name) <= 128 && strings.TrimSpace(name) == name
}
