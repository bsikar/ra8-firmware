// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"errors"
	"strings"
	"testing"
	"time"
)

// localRun builds a valid local run input, so a case states only the thing it
// is judging.
func localRun() LocalRunInput {
	started := time.Now().UTC().Add(-2 * time.Second)
	finished := started.Add(time.Second)
	empty := strings.Repeat("a", 64)
	return LocalRunInput{
		PrincipalID: "principal", LocalID: strings.Repeat("a", 32),
		PayloadSHA256: empty, CatalogSHA256: empty, Repository: "bsikar/ra8-firmware",
		Branch: "offline-test", SourceVerification: "unverified",
		CommitSHA: strings.Repeat("b", 40), TaskName: "format-check",
		Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 600,
		StartedAt: started, FinishedAt: finished,
		DurationNS: finished.Sub(started).Nanoseconds(), Result: "succeeded",
		Steps: []LocalStepInput{{
			Key: "format-tree-check", Ordinal: 0, StartedAt: started, EndedAt: finished,
			DurationNS:   finished.Sub(started).Nanoseconds(),
			StdoutSHA256: empty, StderrSHA256: empty,
		}},
	}
}

func TestAStatedLocalRunIsAccepted(t *testing.T) {
	if err := validateLocalRun(localRun()); err != nil {
		t.Fatalf("a stated local run was refused: %v", err)
	}
}

func TestANameATextColumnCannotHoldIsRefused(t *testing.T) {
	for _, c := range []struct {
		name  string
		value string
	}{
		{"a NUL", "format\x00check"},
		{"a newline", "format\ncheck"},
		{"a carriage return", "format\rcheck"},
		{"a tab", "format\tcheck"},
		{"an escape sequence", "format\x1b[31mcheck"},
		{"a delete", "format\x7fcheck"},
		{"a C1 control", "format\u0085check"},
		{"invalid UTF-8", "format" + string([]byte{0xff, 0xfe}) + "check"},
	} {
		run := localRun()
		run.TaskName = c.value
		if err := validateLocalRun(run); err == nil {
			t.Fatalf("task name carrying %s was accepted", c.name)
		} else if !errors.Is(err, ErrInvalid) {
			t.Fatalf("task name refusal for %s does not travel as invalid: %v", c.name, err)
		}
		run = localRun()
		run.Steps[0].Key = c.value
		if err := validateLocalRun(run); err == nil {
			t.Fatalf("step key carrying %s was accepted", c.name)
		} else if !errors.Is(err, ErrInvalid) {
			t.Fatalf("step key refusal for %s does not travel as invalid: %v", c.name, err)
		}
	}
}

// TestOrdinaryTextStaysAcceptable pins what the rule must not refuse: the
// alphabet reviewed tasks actually use, and printable text beyond ASCII, which
// a text column holds perfectly well.
func TestOrdinaryTextStaysAcceptable(t *testing.T) {
	for _, value := range []string{
		"format-check", "compile", "test-agent", "check.sh", "build_2",
		"vérification", "検査", "a b",
	} {
		run := localRun()
		run.TaskName = value
		run.Steps[0].Key = value
		if err := validateLocalRun(run); err != nil {
			t.Fatalf("ordinary name %q was refused: %v", value, err)
		}
	}
}

func TestTheRuleItselfJudgesOneStringAtATime(t *testing.T) {
	for _, value := range []string{"format-check", "a b", "検査", "~"} {
		if !namesATextColumnCanHold(value) {
			t.Fatalf("%q was judged unfilable", value)
		}
	}
	for _, value := range []string{"\x00", "\n", "\x1b", "\x7f", "\u0085", string([]byte{0xff})} {
		if namesATextColumnCanHold(value) {
			t.Fatalf("%q was judged filable", value)
		}
	}
}

// TestTheLengthBoundIsUnchanged states that this rule was added beside the
// existing bounds rather than in place of them.
func TestTheLengthBoundIsUnchanged(t *testing.T) {
	run := localRun()
	run.TaskName = strings.Repeat("a", 129)
	if err := validateLocalRun(run); err == nil {
		t.Fatal("an over-long task name was accepted")
	}
	run = localRun()
	run.Steps[0].Key = strings.Repeat("a", 129)
	if err := validateLocalRun(run); err == nil {
		t.Fatal("an over-long step key was accepted")
	}
	run = localRun()
	run.TaskName = " format-check "
	if err := validateLocalRun(run); err == nil {
		t.Fatal("a task name with surrounding whitespace was accepted")
	}
}
