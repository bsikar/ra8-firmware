// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package store

import (
	"encoding/json"
	"errors"
	"strings"
	"testing"
)

// localRunWithArguments is the valid local run fixture carrying these
// arguments, so a refusal in a test can only have come from them.
func localRunWithArguments(arguments ...string) LocalRunInput {
	in := localRun()
	in.Arguments = arguments
	return in
}

func TestArgumentsAJSONBColumnCanHoldAcceptsOrdinaryArguments(t *testing.T) {
	if !argumentsAJSONBColumnCanHold([]string{"--verbose", "-j8", "build/ra8.elf"}) {
		t.Fatal("ordinary command arguments were refused")
	}
}

func TestArgumentsAJSONBColumnCanHoldAcceptsNoArgumentsAtAll(t *testing.T) {
	if !argumentsAJSONBColumnCanHold(nil) || !argumentsAJSONBColumnCanHold([]string{}) {
		t.Fatal("a run with no arguments was refused")
	}
}

func TestArgumentsAJSONBColumnCanHoldAcceptsAnEmptyArgument(t *testing.T) {
	// An empty string is a real argument a shell can pass, and jsonb holds it.
	if !argumentsAJSONBColumnCanHold([]string{"--name", ""}) {
		t.Fatal("an empty argument was refused")
	}
}

func TestArgumentsAJSONBColumnCanHoldAcceptsTextBeyondASCII(t *testing.T) {
	if !argumentsAJSONBColumnCanHold([]string{"--message", "vérification", "日本語"}) {
		t.Fatal("valid UTF-8 beyond ASCII was refused")
	}
}

func TestArgumentsAJSONBColumnCanHoldAcceptsControlCharactersInAValue(t *testing.T) {
	// Deliberately not the identity rule: a newline or a tab inside a value
	// handed to a child process is ordinary, and jsonb escapes both.
	if !argumentsAJSONBColumnCanHold([]string{"--script", "first\nsecond", "a\tb"}) {
		t.Fatal("a tab or newline inside an argument was refused")
	}
}

func TestArgumentsAJSONBColumnCanHoldRefusesANUL(t *testing.T) {
	if argumentsAJSONBColumnCanHold([]string{"--name", "ra8\x00ci"}) {
		t.Fatal("an argument carrying a NUL was accepted")
	}
}

func TestArgumentsAJSONBColumnCanHoldRefusesInvalidUTF8(t *testing.T) {
	if argumentsAJSONBColumnCanHold([]string{string([]byte{0xff, 0xfe})}) {
		t.Fatal("an argument that is not valid UTF-8 was accepted")
	}
}

func TestArgumentsAJSONBColumnCanHoldRefusesABadArgumentAnywhereInTheList(t *testing.T) {
	if argumentsAJSONBColumnCanHold([]string{"--first", "--second", "third\x00"}) {
		t.Fatal("a malformed argument at the end of the list was accepted")
	}
}

// TestMarshalRewritesInvalidUTF8Silently pins the reason this rule refuses
// invalid UTF-8 rather than leaving it to the column: the marshal that builds
// the jsonb value does not fail on it, it substitutes, so the row commits
// carrying arguments the task was never run with.
func TestMarshalRewritesInvalidUTF8Silently(t *testing.T) {
	encoded, err := json.Marshal([]string{string([]byte{0xff})})
	if err != nil {
		t.Fatalf("marshal refused invalid UTF-8: %v", err)
	}
	if !strings.Contains(string(encoded), "\ufffd") {
		t.Fatalf("expected the replacement character in %s", encoded)
	}
}

func TestValidateLocalRunRefusesArgumentsTheColumnCannotHold(t *testing.T) {
	err := validateLocalRun(localRunWithArguments("--name", "ra8\x00ci"))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a NUL in an argument: %v", err)
	}
	err = validateLocalRun(localRunWithArguments(string([]byte{0xff, 0xfe})))
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("invalid UTF-8 in an argument: %v", err)
	}
}

func TestValidateLocalRunAcceptsOrdinaryArguments(t *testing.T) {
	if err := validateLocalRun(localRunWithArguments("--verbose", "build/ra8.elf")); err != nil {
		t.Fatalf("ordinary arguments: %v", err)
	}
}
