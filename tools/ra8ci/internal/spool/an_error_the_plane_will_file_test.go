// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"errors"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
)

func TestAnErrorThePlaneWillFileAcceptsEveryOrdinaryMessage(t *testing.T) {
	for _, c := range []struct {
		name    string
		message string
	}{
		{"no failure at all", ""},
		{"an ordinary wrapped failure", "run step \"build\": exit status 2"},
		{"several lines of child output", "step \"build\" failed:\n  cc: no such file\n  make: *** [all] Error 1"},
		{"a tab-indented child message", "step \"test\":\n\tFAIL\tinternal/store\t0.4s"},
		{"a coloured child message", "\x1b[31merror\x1b[0m: linker script missing"},
		{"text outside ASCII", "µ-controller timed out after 600s"},
	} {
		if err := checkTheErrorIsOneThePlaneWillFile(c.message); err != nil {
			t.Fatalf("%s was refused: %v", c.name, err)
		}
	}
}

func TestMessagesATextColumnCannotCarryAreRefused(t *testing.T) {
	for _, c := range []struct {
		name    string
		message string
	}{
		{"a NUL in the message", "exit status 2\x00"},
		{"a NUL alone", "\x00"},
		{"bytes that are not UTF-8", "cc: \xff\xfe not found"},
		{"a truncated multi-byte rune", "timed out after 600\xc2"},
	} {
		if err := checkTheErrorIsOneThePlaneWillFile(c.message); !errors.Is(err, errUnfilableError) {
			t.Fatalf("%s was accepted: %v", c.name, err)
		}
	}
}

// The store makes the same cut on this field that this door does: the control
// ranges the identities are held to are ordinary in a message a person reads,
// so only the two spellings the column cannot take are refused.
func TestTheControlRangesAreLeftAloneInAMessage(t *testing.T) {
	for _, message := range []string{"\n", "\t", "\r\n", "\x1b[0m", "\x7f", "\u0085"} {
		if err := checkTheErrorIsOneThePlaneWillFile(message); err != nil {
			t.Fatalf("%q was refused: %v", message, err)
		}
	}
}

// Length is left to the doors that can refuse it without destroying the
// record: syncclient stops an oversized record before the request and leaves
// it in the outbox, and validateLocalRun names the field at 1024 bytes.
func TestALongMessageIsStillWrittenDown(t *testing.T) {
	if err := checkTheErrorIsOneThePlaneWillFile(strings.Repeat("e", 300<<10)); err != nil {
		t.Fatalf("a long but readable message was refused: %v", err)
	}
}

// The door judges the text alone. Whether a message may sit beside a given
// verdict is the server's reading of the record as a whole, and the column's
// CHECK behind it.
func TestTheDoorDoesNotJudgeWhenAMessageMayBePresent(t *testing.T) {
	if err := checkTheErrorIsOneThePlaneWillFile("exit status 0"); err != nil {
		t.Fatalf("a message stating a zero exit was refused: %v", err)
	}
}

func TestFinishRefusesAnErrorThePlaneWillNotFile(t *testing.T) {
	spool, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	metadata := Metadata{Source: filable("bsikar/ra8-firmware", "ra8ci/dev"),
		Tier: "required", Scope: "safe-local-read-only", DeadlineSeconds: 600}
	entry, err := spool.BeginWithMetadata("format-check", strings.Repeat("b", 64), metadata)
	if err != nil {
		t.Fatalf("begin: %v", err)
	}
	result := executor.Result{TaskName: "format-check", ExitCode: 1}
	if _, err := spool.Finish(entry, result, errors.New("cc: \xff\xfe not found")); !errors.Is(err, errUnfilableError) {
		t.Fatalf("a message that is not UTF-8 was written into a terminal record: %v", err)
	}
	if _, err := spool.Finish(entry, result, errors.New("exit status 2\x00")); !errors.Is(err, errUnfilableError) {
		t.Fatalf("a message holding a NUL was written into a terminal record: %v", err)
	}
	finished, err := spool.Finish(entry, result, errors.New("run step \"build\": exit status 2"))
	if err != nil {
		t.Fatalf("an ordinary failure was refused: %v", err)
	}
	if finished.Error != "run step \"build\": exit status 2" {
		t.Fatalf("the record does not carry the message it froze: %q", finished.Error)
	}
}
