package store

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"
)

func testRun() CreateRunInput {
	return CreateRunInput{
		Trigger: "manual", ActorID: "tester", Repository: "bsikar/ra8-firmware",
		Branch: "ci/orchestrator", CommitSHA: strings.Repeat("a", 40),
		SnapshotSHA256: strings.Repeat("b", 64), CatalogSHA256: strings.Repeat("c", 64),
		Tasks: []TaskInput{
			{Key: "format", Name: "format-check", Arguments: json.RawMessage(`{}`), Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 900},
			{Key: "test", Name: "test-go", Arguments: json.RawMessage(`{"argv":[]}`), DependsOnKeys: []string{"format"}, Tier: "required", Scope: "linux-vm", DeadlineSeconds: 1800},
		},
	}
}

func TestUUIDv7(t *testing.T) {
	a, err := NewID()
	if err != nil {
		t.Fatal(err)
	}
	b, err := NewID()
	if err != nil {
		t.Fatal(err)
	}
	if !ValidID(a) || !ValidID(b) || a == b {
		t.Fatalf("invalid or colliding UUIDv7 IDs: %q %q", a, b)
	}
	if ValidID(strings.ToUpper(a)) || ValidID("00000000-0000-4000-8000-000000000000") || ValidID("broken") {
		t.Fatal("accepted a non-canonical or non-v7 ID")
	}
}

func TestValidateRunDAG(t *testing.T) {
	in := testRun()
	if err := validateRun(in); err != nil {
		t.Fatalf("valid DAG rejected: %v", err)
	}
	in.Tasks[0].DependsOnKeys = []string{"test"}
	if !errors.Is(validateRun(in), ErrInvalid) {
		t.Fatal("accepted cyclic DAG")
	}
	in = testRun()
	in.Tasks[1].DependsOnKeys = []string{"missing"}
	if !errors.Is(validateRun(in), ErrInvalid) {
		t.Fatal("accepted missing dependency")
	}
	in = testRun()
	in.Tasks[1].DependsOnKeys = []string{"format", "format"}
	if !errors.Is(validateRun(in), ErrInvalid) {
		t.Fatal("accepted repeated dependency")
	}
	in = testRun()
	in.Tasks[0].Arguments = json.RawMessage(`[]`)
	if !errors.Is(validateRun(in), ErrInvalid) {
		t.Fatal("accepted non-object task arguments")
	}
	in = testRun()
	in.IdempotencyKey = "once"
	if !errors.Is(validateRun(in), ErrInvalid) {
		t.Fatal("accepted missing request digest")
	}
}

func TestAttemptResultValidation(t *testing.T) {
	id, err := NewID()
	if err != nil {
		t.Fatal(err)
	}
	zero := 0
	good := FinishAttemptInput{AttemptID: id, ActorID: "tester", Result: "succeeded", ChildExitCode: &zero, EvidenceComplete: true}
	if !validAttemptResult(good) {
		t.Fatal("valid success rejected")
	}
	good.EvidenceComplete = false
	if validAttemptResult(good) {
		t.Fatal("accepted unevidenced success")
	}
	good.Result = "timed_out"
	good.HitDeadline = false
	if validAttemptResult(good) {
		t.Fatal("accepted timeout without deadline evidence")
	}
	good.HitDeadline = true
	if !validAttemptResult(good) {
		t.Fatal("valid timeout rejected")
	}
}
