// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

func carrying(arguments ...string) spool.Entry {
	return spool.Entry{
		SchemaVersion: 2,
		ID:            "0123456789abcdef0123456789abcdef",
		Task:          "build",
		Args:          arguments,
	}
}

// The ordinary shapes a reviewed schema binds, plus the two edges of the
// count bound.
func TestArgumentsThePlaneFilesAreUploaded(t *testing.T) {
	for _, c := range []struct {
		what      string
		arguments []string
	}{
		{"none at all", nil},
		{"an empty list", []string{}},
		{"a flag and a value", []string{"--target", "ra8p1"}},
		{"an empty element", []string{""}},
		{"a tab inside a value", []string{"one\ttwo"}},
		{"a newline inside a value", []string{"one\ntwo"}},
		{"text outside ASCII", []string{"vérification"}},
		{"the list at the bound", make([]string, maxUploadableArguments)},
	} {
		if err := checkUploadedArgumentsAreOnesThePlaneWillFile(carrying(c.arguments...)); err != nil {
			t.Fatalf("arguments with %s were refused: %v", c.what, err)
		}
	}
}

// One past the count the plane files.
func TestAnOverlongArgumentListIsRefused(t *testing.T) {
	err := checkUploadedArgumentsAreOnesThePlaneWillFile(carrying(make([]string, maxUploadableArguments+1)...))
	if err == nil {
		t.Fatal("an argument list past the bound was accepted")
	}
	if !errors.Is(err, ErrUnfilableArguments) {
		t.Fatalf("the refusal does not travel as unfilable: %v", err)
	}
}

// The NUL is the spelling jsonb refuses outright, inside the ingest
// transaction, where it reads as an unavailable store.
func TestAnArgumentCarryingANULIsRefused(t *testing.T) {
	err := checkUploadedArgumentsAreOnesThePlaneWillFile(carrying("--target", "ra8\x00p1"))
	if err == nil {
		t.Fatal("an argument carrying a NUL was accepted")
	}
	if !errors.Is(err, ErrUnfilableArguments) {
		t.Fatalf("the refusal does not travel as unfilable: %v", err)
	}
}

// Invalid UTF-8 is the quiet one: json.Marshal rewrites it rather than
// refusing, so the row commits and the evidence on file is not the evidence
// the run produced.
func TestAnArgumentThatIsNotValidUTF8IsRefused(t *testing.T) {
	err := checkUploadedArgumentsAreOnesThePlaneWillFile(carrying("--target", "ra8"+string([]byte{0xff, 0xfe})))
	if err == nil {
		t.Fatal("an argument that is not valid UTF-8 was accepted")
	}
	if !errors.Is(err, ErrUnfilableArguments) {
		t.Fatalf("the refusal does not travel as unfilable: %v", err)
	}
}

// The refusal names which argument, which is why it is asked here rather
// than read back as an opaque 400 from the far end.
func TestTheArgumentRefusalNamesThePosition(t *testing.T) {
	err := checkUploadedArgumentsAreOnesThePlaneWillFile(carrying("fine", "ra8\x00p1"))
	if err == nil || !strings.Contains(err.Error(), "argument 1") {
		t.Fatalf("the refusal does not name the argument: %v", err)
	}
}

// This door judges what the columns hold, never what a task's schema
// allows. catalog.ValidArgumentValue refuses shell metacharacters and
// anything flag-shaped, and that comparison needs the catalog a spooled
// record does not carry, so a client guessing would refuse evidence the
// plane would have taken.
func TestTheArgumentDoorDoesNotChooseACatalog(t *testing.T) {
	if err := checkUploadedArgumentsAreOnesThePlaneWillFile(carrying("rm -rf /", "$(whoami)", "--not-a-flag")); err != nil {
		t.Fatalf("the door judged a value the catalog alone rules on: %v", err)
	}
}
