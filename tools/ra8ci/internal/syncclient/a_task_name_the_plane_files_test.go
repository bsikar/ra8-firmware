// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package syncclient

import (
	"errors"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/spool"
)

func named(task string) spool.Entry {
	return spool.Entry{SchemaVersion: 2, ID: "0123456789abcdef0123456789abcdef", Task: task}
}

// The ordinary name every reviewed definition carries.
func TestATaskNameThePlaneFilesIsUploaded(t *testing.T) {
	for _, name := range []string{
		"build",
		"zig-build-check",
		"tools/ra8ci/lint",
		"vérification",
		strings.Repeat("t", maxFilableTaskNameBytes),
	} {
		if err := checkUploadedTaskNameIsOneThePlaneFiles(named(name)); err != nil {
			t.Fatalf("task name %q was refused: %v", name, err)
		}
	}
}

// The column is 1..128 bytes, so both ends are refusals.
func TestATaskNameOutsideTheColumnIsRefused(t *testing.T) {
	for _, c := range []struct {
		what string
		name string
	}{
		{"empty", ""},
		{"one byte past the bound", strings.Repeat("t", maxFilableTaskNameBytes+1)},
	} {
		err := checkUploadedTaskNameIsOneThePlaneFiles(named(c.name))
		if err == nil {
			t.Fatalf("a task name %s was accepted", c.what)
		}
		if !errors.Is(err, ErrUnfilableTaskName) {
			t.Fatalf("the refusal for %s does not travel as unfilable: %v", c.what, err)
		}
	}
}

// The bound is bytes, not runes: the store measures len(), so a name of 128
// multi-byte runes is past the column even though it reads as short.
func TestTheBoundIsBytesNotRunes(t *testing.T) {
	name := strings.Repeat("é", maxFilableTaskNameBytes)
	if err := checkUploadedTaskNameIsOneThePlaneFiles(named(name)); err == nil {
		t.Fatal("a name of 256 bytes was accepted because it reads as 128 runes")
	}
}

// A padded name is two entries out of one task in anything that groups by
// name, and the store refuses it rather than trimming for the caller.
func TestAPaddedTaskNameIsRefused(t *testing.T) {
	for _, c := range []struct {
		what string
		name string
	}{
		{"a leading space", " build"},
		{"a trailing space", "build "},
		{"a trailing newline", "build\n"},
		{"only space", " "},
	} {
		err := checkUploadedTaskNameIsOneThePlaneFiles(named(c.name))
		if err == nil {
			t.Fatalf("a task name with %s was accepted", c.what)
		}
		if !errors.Is(err, ErrUnfilableTaskName) {
			t.Fatalf("the refusal for %s does not travel as unfilable: %v", c.what, err)
		}
	}
}

// The same spellings store.namesATextColumnCanHold refuses, refused here
// before the record leaves the host.
func TestATaskNameTheColumnCannotHoldIsRefused(t *testing.T) {
	for _, c := range []struct {
		what string
		name string
	}{
		{"a NUL", "bu\x00ild"},
		{"an embedded newline", "build\nother-task"},
		{"a carriage return", "build\rother"},
		{"a tab", "bu\tild"},
		{"an escape sequence", "build\x1b[31m"},
		{"a delete", "bu\x7fild"},
		{"a C1 control", "build\u0085other"},
		{"invalid UTF-8", "build" + string([]byte{0xff, 0xfe})},
	} {
		err := checkUploadedTaskNameIsOneThePlaneFiles(named(c.name))
		if err == nil {
			t.Fatalf("a task name carrying %s was accepted", c.what)
		}
		if !errors.Is(err, ErrUnfilableTaskName) {
			t.Fatalf("the refusal for %s does not travel as unfilable: %v", c.what, err)
		}
	}
}

// The refusal names the field, which is the whole point of asking here
// rather than reading an opaque 400 back from the far end.
func TestTheRefusalNamesTheTaskName(t *testing.T) {
	err := checkUploadedTaskNameIsOneThePlaneFiles(named(" build"))
	if err == nil || !strings.Contains(err.Error(), "task name") {
		t.Fatalf("the refusal does not name the field: %v", err)
	}
}

// This door judges the SHAPE of the name, never whether the catalog holds
// it. That comparison needs the catalog the server alone has, and a client
// guessing at it would refuse evidence the plane would have taken.
func TestTheTaskNameDoorDoesNotChooseACatalog(t *testing.T) {
	if err := checkUploadedTaskNameIsOneThePlaneFiles(named("a-task-no-catalog-holds")); err != nil {
		t.Fatalf("the door refused a well-formed name it could not find: %v", err)
	}
}
