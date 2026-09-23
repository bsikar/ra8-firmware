// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/source"
)

const (
	assignmentID = "01994d50-1234-7abc-8abc-0123456789ab"
	attemptID    = "01994d50-1234-7abc-8abc-0123456789ac"
)

func TestAssignmentValidation(t *testing.T) {
	assignment := sampleAssignment()
	if err := assignment.Validate(); err != nil {
		t.Fatal(err)
	}
	mutations := []func(*Assignment){
		func(a *Assignment) { a.SchemaVersion = 2 },
		func(a *Assignment) { a.AssignmentID = "bad" },
		func(a *Assignment) { a.AttemptID = "bad" },
		func(a *Assignment) { a.AssignmentVersion = 0 },
		func(a *Assignment) { a.FencingToken = 0 },
		func(a *Assignment) { a.Task.Name = "" },
		func(a *Assignment) { a.Task.Version = 0 },
		func(a *Assignment) { a.CatalogSHA256 = "bad" },
		func(a *Assignment) { a.Source.Algorithm = "unknown" },
		func(a *Assignment) { a.Source.Commit = "bad" },
		func(a *Assignment) { a.Source.SnapshotSHA256 = "bad" },
		func(a *Assignment) { a.DeadlineAt = time.Time{} },
		func(a *Assignment) { a.RemainingMS = 0 },
		func(a *Assignment) { a.RemainingMS = MaxDeadlineMS + 1 },
	}
	for index, mutate := range mutations {
		copy := assignment
		mutate(&copy)
		if !errors.Is(copy.Validate(), ErrInvalid) {
			t.Fatalf("mutation %d was accepted: %+v", index, copy)
		}
	}
}

func TestHostFactsValidation(t *testing.T) {
	facts := sampleFacts()
	if err := facts.Validate(); err != nil {
		t.Fatal(err)
	}
	facts.OS, facts.LoadKind = "windows", "cpu_busy_equivalent"
	if err := facts.Validate(); err != nil {
		t.Fatal(err)
	}
	for _, bad := range []HostFacts{
		{},
		{Cores: 0, RAMBytes: 1, RAMFreeBytes: 1, OS: "linux", LoadKind: "linux_load1", Arch: "amd64", CapturedAt: time.Now()},
		{Cores: 1, RAMBytes: 1, RAMFreeBytes: 2, OS: "linux", LoadKind: "linux_load1", Arch: "amd64", CapturedAt: time.Now()},
		{Cores: 1, RAMBytes: 1, Load1: math.NaN(), OS: "linux", LoadKind: "linux_load1", Arch: "amd64", CapturedAt: time.Now()},
		{Cores: 1, RAMBytes: 1, OS: "windows", LoadKind: "linux_load1", Arch: "amd64", CapturedAt: time.Now()},
	} {
		if !errors.Is(bad.Validate(), ErrInvalid) {
			t.Fatalf("bad host facts accepted: %+v", bad)
		}
	}
}

func TestLogChunkValidation(t *testing.T) {
	data := []byte("hello")
	sum := sha256.Sum256(data)
	chunk := LogChunk{
		SchemaVersion: Version, AssignmentID: assignmentID, AttemptID: attemptID,
		AssignmentVersion: 1, FencingToken: 1, Sequence: 1, Stream: "stdout",
		DataBase64: base64.StdEncoding.EncodeToString(data), SHA256: hex.EncodeToString(sum[:]),
	}
	if err := chunk.Validate(); err != nil {
		t.Fatal(err)
	}
	for _, mutate := range []func(*LogChunk){
		func(c *LogChunk) { c.Sequence = 0 },
		func(c *LogChunk) { c.Stream = "combined" },
		func(c *LogChunk) { c.SHA256 = strings.Repeat("0", 64) },
		func(c *LogChunk) { c.DataBase64 = "invalid!" },
		func(c *LogChunk) { c.DataBase64 = "" },
		func(c *LogChunk) { c.SchemaVersion = 2 },
	} {
		copy := chunk
		mutate(&copy)
		if !errors.Is(copy.Validate(), ErrInvalid) {
			t.Fatalf("bad log chunk accepted: %+v", copy)
		}
	}
}

func TestAckHeartbeatAndTerminalReceipt(t *testing.T) {
	facts := sampleFacts()
	claim := ClaimRequest{SchemaVersion: Version, HostFacts: facts, PollWaitMS: 25000}
	if err := claim.Validate(); err != nil {
		t.Fatal(err)
	}
	claim.PollWaitMS++
	if !errors.Is(claim.Validate(), ErrInvalid) {
		t.Fatal("unbounded claim wait accepted")
	}
	accepted := AcceptResponse{SchemaVersion: Version, AssignmentVersion: 1, FencingToken: 1, Accepted: true}
	if err := accepted.ValidateFor(sampleAssignment()); err != nil {
		t.Fatal(err)
	}
	accepted.Accepted = false
	if !errors.Is(accepted.ValidateFor(sampleAssignment()), ErrInvalid) {
		t.Fatal("negative acknowledgment accepted")
	}
	ack := Ack{SchemaVersion: Version, AssignmentID: assignmentID, AttemptID: attemptID, AssignmentVersion: 1, FencingToken: 1,
		CatalogSHA256: strings.Repeat("a", 64), SourceSnapshotSHA256: strings.Repeat("b", 64), HostFacts: facts}
	if err := ack.Validate(); err != nil {
		t.Fatal(err)
	}
	ack.HostFacts.Cores = 0
	if !errors.Is(ack.Validate(), ErrInvalid) {
		t.Fatal("invalid acknowledgment accepted")
	}
	heartbeat := Heartbeat{SchemaVersion: Version, AssignmentID: assignmentID, AttemptID: attemptID,
		AssignmentVersion: 1, FencingToken: 1, Phase: "executing", HostFacts: facts}
	if err := heartbeat.Validate(); err != nil {
		t.Fatal(err)
	}
	heartbeat.Phase = "unknown"
	if !errors.Is(heartbeat.Validate(), ErrInvalid) {
		t.Fatal("invalid heartbeat accepted")
	}
	response := HeartbeatResponse{SchemaVersion: Version, AssignmentVersion: 1, FencingToken: 1}
	if err := response.ValidateFor(sampleAssignment()); err != nil {
		t.Fatal(err)
	}
	response.FencingToken = 2
	if !errors.Is(response.ValidateFor(sampleAssignment()), ErrInvalid) {
		t.Fatal("stale heartbeat intent accepted")
	}
	zero := 0
	now := time.Now().UTC()
	receipt := TerminalReceipt{
		SchemaVersion: Version, AssignmentID: assignmentID, AttemptID: attemptID,
		AssignmentVersion: 1, FencingToken: 1, Outcome: "succeeded", ChildExitCode: &zero,
		EvidenceComplete: true, StartedAt: now, EndedAt: now.Add(time.Second), DurationNS: int64(time.Second),
		CatalogSHA256: strings.Repeat("a", 64), SourceSnapshotSHA256: strings.Repeat("b", 64),
		HostFactsAtStart: facts, HostFactsAtEnd: facts,
		Steps: []StepSummary{{Name: "one", StartedAt: now, EndedAt: now.Add(time.Second), DurationNS: int64(time.Second)}},
	}
	if err := receipt.Validate(); err != nil {
		t.Fatal(err)
	}
	receipt.EvidenceComplete = false
	if !errors.Is(receipt.Validate(), ErrInvalid) {
		t.Fatal("success without evidence accepted")
	}
	receipt.Outcome = "failed"
	if err := receipt.Validate(); err != nil {
		t.Fatal(err)
	}
	receipt.TimedOut = true
	if !errors.Is(receipt.Validate(), ErrInvalid) {
		t.Fatal("failed and timed-out receipt accepted")
	}
	receipt.Outcome = "timed_out"
	if err := receipt.Validate(); err != nil {
		t.Fatal(err)
	}
}

func TestDecodeStrictAndVersionHelpers(t *testing.T) {
	assignment := sampleAssignment()
	raw, err := json.Marshal(assignment)
	if err != nil {
		t.Fatal(err)
	}
	var decoded Assignment
	if err := DecodeStrict(strings.NewReader(string(raw)), &decoded); err != nil || decoded.AssignmentID != assignmentID {
		t.Fatalf("DecodeStrict = %+v, %v", decoded, err)
	}
	for _, input := range []string{
		`{"unknown":true}`,
		string(raw) + ` {}`,
		string(raw) + strings.Repeat(" ", MaxJSONBytes),
	} {
		if !errors.Is(DecodeStrict(strings.NewReader(input), &decoded), ErrInvalid) {
			t.Fatal("invalid JSON accepted")
		}
	}
	if !ValidID(assignmentID) || ValidID(strings.ToUpper(assignmentID)) || ValidID("bad") {
		t.Fatal("UUIDv7 validation is incorrect")
	}
	if !ValidSHA256(strings.Repeat("a", 64)) || ValidSHA256(strings.Repeat("A", 64)) || ValidSHA256("bad") {
		t.Fatal("SHA-256 validation is incorrect")
	}
	if !ValidCommit(strings.Repeat("a", 40)) || ValidCommit(strings.Repeat("g", 40)) {
		t.Fatal("commit validation is incorrect")
	}
}

func sampleAssignment() Assignment {
	return Assignment{
		SchemaVersion: Version, AssignmentID: assignmentID, AttemptID: attemptID,
		AssignmentVersion: 1, FencingToken: 1,
		Task: TaskRef{Name: "format-check", Version: 1}, CatalogSHA256: strings.Repeat("a", 64),
		Source:     SourceRef{Algorithm: source.Algorithm, Commit: strings.Repeat("a", 40), SnapshotSHA256: strings.Repeat("b", 64)},
		DeadlineAt: time.Now().UTC().Add(time.Hour), RemainingMS: 60000,
	}
}

func sampleFacts() HostFacts {
	return HostFacts{Cores: 4, RAMBytes: 1024, RAMFreeBytes: 512, Load1: 0.25,
		LoadKind: "linux_load1", OS: "linux", Arch: "amd64", CapturedAt: time.Now().UTC()}
}
