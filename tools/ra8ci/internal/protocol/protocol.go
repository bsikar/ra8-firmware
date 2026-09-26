// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package protocol defines the versioned, client-certificate-authenticated
// assignment and evidence messages shared by the server and outbound agents.
package protocol

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/source"
)

const (
	Version       = 2
	MaxLogBytes   = 32 << 10
	MaxJSONBytes  = 1 << 20
	MaxDeadlineMS = 24 * 60 * 60 * 1000
)

var ErrInvalid = errors.New("invalid agent protocol message")

// HostFacts are guest-origin measurements, never an authenticated host claim.
type HostFacts struct {
	Cores        int       `json:"cores"`
	RAMBytes     int64     `json:"ram_bytes"`
	RAMFreeBytes int64     `json:"ram_free_bytes"`
	Load1        float64   `json:"load_1"`
	LoadKind     string    `json:"load_kind"`
	OS           string    `json:"os"`
	Arch         string    `json:"arch"`
	CapturedAt   time.Time `json:"captured_at"`
}

// TaskRef is a reviewed semantic task identity, never an argv command.
type TaskRef struct {
	Name    string `json:"name"`
	Version int    `json:"version"`
}

// SourceRef identifies the complete recursively pinned source snapshot.
type SourceRef struct {
	Algorithm      string `json:"algorithm"`
	Commit         string `json:"commit"`
	SnapshotSHA256 string `json:"snapshot_sha256"`
}

// Assignment is a server-issued, fenced, one-attempt execution grant.
type Assignment struct {
	SchemaVersion     int       `json:"schema_version"`
	AssignmentID      string    `json:"assignment_id"`
	AttemptID         string    `json:"attempt_id"`
	AssignmentVersion int64     `json:"assignment_version"`
	FencingToken      int64     `json:"fencing_token"`
	Task              TaskRef   `json:"task"`
	CatalogSHA256     string    `json:"catalog_sha256"`
	Source            SourceRef `json:"source"`
	DeadlineAt        time.Time `json:"deadline_at"`
	RemainingMS       int64     `json:"remaining_ms"`
}

// ClaimRequest asks the server to make one durable assignment for this mTLS identity.
type ClaimRequest struct {
	SchemaVersion int       `json:"schema_version"`
	HostFacts     HostFacts `json:"host_facts"`
	PollWaitMS    int64     `json:"poll_wait_ms"`
}

// Ack confirms exact grant identity and verified local source before execution.
type Ack struct {
	SchemaVersion        int       `json:"schema_version"`
	AssignmentID         string    `json:"assignment_id"`
	AttemptID            string    `json:"attempt_id"`
	AssignmentVersion    int64     `json:"assignment_version"`
	FencingToken         int64     `json:"fencing_token"`
	CatalogSHA256        string    `json:"catalog_sha256"`
	SourceSnapshotSHA256 string    `json:"source_snapshot_sha256"`
	HostFacts            HostFacts `json:"host_facts"`
}

// Heartbeat reports an active phase and host facts on each agent interval.
type Heartbeat struct {
	SchemaVersion     int       `json:"schema_version"`
	AssignmentID      string    `json:"assignment_id"`
	AttemptID         string    `json:"attempt_id"`
	AssignmentVersion int64     `json:"assignment_version"`
	FencingToken      int64     `json:"fencing_token"`
	Phase             string    `json:"phase"`
	HostFacts         HostFacts `json:"host_facts"`
}

// HeartbeatResponse carries only intent, bound to the current fenced grant.
type HeartbeatResponse struct {
	SchemaVersion     int   `json:"schema_version"`
	AssignmentVersion int64 `json:"assignment_version"`
	FencingToken      int64 `json:"fencing_token"`
	Cancel            bool  `json:"cancel"`
	Yield             bool  `json:"yield"`
}

// AcceptResponse is the versioned, fenced acknowledgment for a write.
type AcceptResponse struct {
	SchemaVersion     int   `json:"schema_version"`
	AssignmentVersion int64 `json:"assignment_version"`
	FencingToken      int64 `json:"fencing_token"`
	Accepted          bool  `json:"accepted"`
}

// LogChunk has a stable sequence and plaintext digest for idempotent upload.
type LogChunk struct {
	SchemaVersion     int    `json:"schema_version"`
	AssignmentID      string `json:"assignment_id"`
	AttemptID         string `json:"attempt_id"`
	AssignmentVersion int64  `json:"assignment_version"`
	FencingToken      int64  `json:"fencing_token"`
	Sequence          int64  `json:"sequence"`
	Stream            string `json:"stream"`
	StepName          string `json:"step_name"`
	DataBase64        string `json:"data_base64"`
	SHA256            string `json:"sha256"`
}

// StepSummary preserves the executor's exact child and timing evidence.
type StepSummary struct {
	Name         string    `json:"name"`
	StartedAt    time.Time `json:"started_at"`
	EndedAt      time.Time `json:"ended_at"`
	DurationNS   int64     `json:"duration_ns"`
	ExitCode     int       `json:"exit_code"`
	TimedOut     bool      `json:"timed_out"`
	Cancelled    bool      `json:"cancelled"`
	StdoutSHA256 string    `json:"stdout_sha256"`
	StderrSHA256 string    `json:"stderr_sha256"`
	StdoutBytes  int64     `json:"stdout_bytes"`
	StderrBytes  int64     `json:"stderr_bytes"`
}

// TerminalReceipt completes the same attempt and its final log sequence.
type TerminalReceipt struct {
	SchemaVersion        int           `json:"schema_version"`
	AssignmentID         string        `json:"assignment_id"`
	AttemptID            string        `json:"attempt_id"`
	AssignmentVersion    int64         `json:"assignment_version"`
	FencingToken         int64         `json:"fencing_token"`
	Outcome              string        `json:"outcome"`
	ChildExitCode        *int          `json:"child_exit_code,omitempty"`
	TimedOut             bool          `json:"timed_out"`
	Cancelled            bool          `json:"cancelled"`
	EvidenceComplete     bool          `json:"evidence_complete"`
	StartedAt            time.Time     `json:"started_at"`
	EndedAt              time.Time     `json:"ended_at"`
	DurationNS           int64         `json:"duration_ns"`
	Steps                []StepSummary `json:"steps"`
	FinalLogSequence     int64         `json:"final_log_sequence"`
	CatalogSHA256        string        `json:"catalog_sha256"`
	SourceSnapshotSHA256 string        `json:"source_snapshot_sha256"`
	HostFactsAtStart     HostFacts     `json:"host_facts_at_start"`
	HostFactsAtEnd       HostFacts     `json:"host_facts_at_end"`
	ErrorCode            string        `json:"error_code,omitempty"`
}

// DecodeStrict rejects unknown fields, trailing values, and oversized input.
func DecodeStrict(reader io.Reader, target any) error {
	if reader == nil || target == nil {
		return fmt.Errorf("%w: nil JSON target or reader", ErrInvalid)
	}
	raw, err := io.ReadAll(io.LimitReader(reader, MaxJSONBytes+1))
	if err != nil || len(raw) > MaxJSONBytes {
		return fmt.Errorf("%w: JSON exceeds limit or cannot be read", ErrInvalid)
	}
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.DisallowUnknownFields()
	if err := dec.Decode(target); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalid, err)
	}
	var extra any
	if err := dec.Decode(&extra); err != io.EOF {
		return fmt.Errorf("%w: trailing or oversized JSON", ErrInvalid)
	}
	return nil
}

// Validate checks all mandatory grant fields independently of the catalog.
func (assignment Assignment) Validate() error {
	if assignment.SchemaVersion != Version || !ValidID(assignment.AssignmentID) || !ValidID(assignment.AttemptID) ||
		assignment.AssignmentVersion < 1 || assignment.FencingToken < 1 ||
		assignment.Task.Name == "" || assignment.Task.Version < 1 ||
		!ValidSHA256(assignment.CatalogSHA256) ||
		assignment.Source.Algorithm != source.Algorithm || !ValidCommit(assignment.Source.Commit) ||
		!ValidSHA256(assignment.Source.SnapshotSHA256) || assignment.DeadlineAt.IsZero() ||
		assignment.RemainingMS < 1 || assignment.RemainingMS > MaxDeadlineMS {
		return ErrInvalid
	}
	return nil
}

// Validate bounds long polling before the server allocates an assignment.
func (claim ClaimRequest) Validate() error {
	if claim.SchemaVersion != Version || claim.PollWaitMS < 0 || claim.PollWaitMS > 25000 {
		return ErrInvalid
	}
	return claim.HostFacts.Validate()
}

// Validate checks host measurement bounds and their labeled semantics.
func (facts HostFacts) Validate() error {
	if facts.Cores < 1 || facts.RAMBytes < 1 || facts.RAMFreeBytes < 0 || facts.RAMFreeBytes > facts.RAMBytes ||
		facts.Load1 < 0 || math.IsNaN(facts.Load1) || math.IsInf(facts.Load1, 0) || facts.CapturedAt.IsZero() ||
		(facts.OS != "linux" && facts.OS != "windows") || facts.Arch == "" {
		return ErrInvalid
	}
	if (facts.OS == "linux" && facts.LoadKind != "linux_load1") ||
		(facts.OS == "windows" && facts.LoadKind != "cpu_busy_equivalent") {
		return ErrInvalid
	}
	return nil
}

// Validate verifies both the encoded bytes and their SHA-256 binding.
func (chunk LogChunk) Validate() error {
	if chunk.SchemaVersion != Version || !ValidID(chunk.AssignmentID) || !ValidID(chunk.AttemptID) || chunk.AssignmentVersion < 1 ||
		chunk.FencingToken < 1 || chunk.Sequence < 1 ||
		(chunk.Stream != "stdout" && chunk.Stream != "stderr") ||
		chunk.StepName == "" || len(chunk.StepName) > 128 || strings.TrimSpace(chunk.StepName) != chunk.StepName || !ValidSHA256(chunk.SHA256) {
		return ErrInvalid
	}
	data, err := base64.StdEncoding.DecodeString(chunk.DataBase64)
	if err != nil || len(data) == 0 || len(data) > MaxLogBytes {
		return ErrInvalid
	}
	sum := sha256.Sum256(data)
	if hex.EncodeToString(sum[:]) != chunk.SHA256 {
		return ErrInvalid
	}
	return nil
}

// Validate ensures an acknowledgment is bound to a measured, fenced attempt.
func (ack Ack) Validate() error {
	if ack.SchemaVersion != Version || !validGrant(ack.AssignmentID, ack.AttemptID, ack.AssignmentVersion, ack.FencingToken) ||
		!ValidSHA256(ack.CatalogSHA256) || !ValidSHA256(ack.SourceSnapshotSHA256) {
		return ErrInvalid
	}
	return ack.HostFacts.Validate()
}

// Validate ensures a heartbeat is bound to a live grant and measured host.
func (heartbeat Heartbeat) Validate() error {
	if heartbeat.SchemaVersion != Version || !validGrant(heartbeat.AssignmentID, heartbeat.AttemptID, heartbeat.AssignmentVersion, heartbeat.FencingToken) ||
		(heartbeat.Phase != "executing" && heartbeat.Phase != "finishing") {
		return ErrInvalid
	}
	return heartbeat.HostFacts.Validate()
}

// ValidateFor rejects stale control intent before it can cancel a newer grant.
func (response HeartbeatResponse) ValidateFor(assignment Assignment) error {
	if response.SchemaVersion != Version || response.AssignmentVersion != assignment.AssignmentVersion || response.FencingToken != assignment.FencingToken {
		return ErrInvalid
	}
	return nil
}

// ValidateFor rejects a stale or negative acknowledgment.
func (response AcceptResponse) ValidateFor(assignment Assignment) error {
	if response.SchemaVersion != Version || response.AssignmentVersion != assignment.AssignmentVersion ||
		response.FencingToken != assignment.FencingToken || !response.Accepted {
		return ErrInvalid
	}
	return nil
}

// Validate enforces a coherent terminal result without trusting its actor ID.
func (receipt TerminalReceipt) Validate() error {
	if receipt.SchemaVersion != Version || !validGrant(receipt.AssignmentID, receipt.AttemptID, receipt.AssignmentVersion, receipt.FencingToken) ||
		!ValidSHA256(receipt.CatalogSHA256) || !ValidSHA256(receipt.SourceSnapshotSHA256) ||
		receipt.StartedAt.IsZero() || receipt.EndedAt.Before(receipt.StartedAt) || receipt.DurationNS < 0 || receipt.FinalLogSequence < 0 {
		return ErrInvalid
	}
	if err := receipt.HostFactsAtStart.Validate(); err != nil {
		return err
	}
	if err := receipt.HostFactsAtEnd.Validate(); err != nil {
		return err
	}
	switch receipt.Outcome {
	case "succeeded":
		if receipt.ChildExitCode == nil || *receipt.ChildExitCode != 0 || !receipt.EvidenceComplete || receipt.TimedOut || receipt.Cancelled {
			return ErrInvalid
		}
	case "failed":
		if receipt.TimedOut || receipt.Cancelled {
			return ErrInvalid
		}
	case "timed_out":
		if !receipt.TimedOut || receipt.Cancelled {
			return ErrInvalid
		}
	case "cancelled":
		if !receipt.Cancelled || receipt.TimedOut {
			return ErrInvalid
		}
	default:
		return ErrInvalid
	}
	for _, step := range receipt.Steps {
		if step.Name == "" || step.StartedAt.IsZero() || step.EndedAt.Before(step.StartedAt) || step.DurationNS < 0 ||
			step.StdoutBytes < 0 || step.StderrBytes < 0 {
			return ErrInvalid
		}
	}
	// Last, because it reads stamps the loops above have already held in
	// order: every duration must fit the window it was measured in.
	if err := checkReportedDurations(receipt); err != nil {
		return err
	}
	if err := checkStepTimeline(receipt); err != nil {
		return err
	}
	if err := checkStepLogEvidence(receipt); err != nil {
		return err
	}
	return checkLogSequenceCoversSteps(receipt)
}

func validGrant(assignmentID, attemptID string, version, fence int64) bool {
	return ValidID(assignmentID) && ValidID(attemptID) && version > 0 && fence > 0
}

// ValidID accepts only canonical, lowercase UUIDv7 identities.
func ValidID(value string) bool {
	if len(value) != 36 || value[8] != '-' || value[13] != '-' || value[18] != '-' || value[23] != '-' || value[14] != '7' || !strings.ContainsRune("89ab", rune(value[19])) {
		return false
	}
	for index, char := range value {
		if index == 8 || index == 13 || index == 18 || index == 23 {
			continue
		}
		if !strings.ContainsRune("0123456789abcdef", char) {
			return false
		}
	}
	return true
}

// ValidSHA256 accepts a lowercase full-length digest.
func ValidSHA256(value string) bool {
	if len(value) != 64 {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil && strings.ToLower(value) == value
}

// ValidCommit accepts a lowercase full Git object ID from SHA-1 or SHA-256.
func ValidCommit(value string) bool {
	if len(value) != 40 && len(value) != 64 {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil && strings.ToLower(value) == value
}
