// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package spool

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A spool record is read back by a sweep that no longer has the run in front
// of it, so everything the read can refuse has to refuse by saying which half
// is wrong: the record is absent, the record is not a file, or the record is
// there and unreadable. These hold that wording, and the two writes that must
// never land: a name carrying a path separator, and an identifier that is not
// what this spool hands out.

func aSpool(t *testing.T) *Spool {
	t.Helper()
	s, err := Open(filepath.Join(t.TempDir(), "outbox"))
	if err != nil {
		t.Fatal(err)
	}
	return s
}

// The start record is the frozen half of a run: the task and catalog digest as
// they were before the first command. A read of it separates absent from
// present-but-wrong, because the two mean different things to an operator.
func TestAStartRecordSaysWhichHalfIsWrong(t *testing.T) {
	t.Run("absent", func(t *testing.T) {
		s := aSpool(t)
		_, err := s.readStarted("0123456789abcdef0123456789abcdef")
		if err == nil || !strings.Contains(err.Error(), "missing start record") {
			t.Fatalf("error = %v, want the record reported missing", err)
		}
	})

	t.Run("a directory in its place", func(t *testing.T) {
		s := aSpool(t)
		id := "11111111111111111111111111111111"
		if err := os.Mkdir(filepath.Join(s.directory, id+".started.json"), 0o700); err != nil {
			t.Fatal(err)
		}
		_, err := s.readStarted(id)
		if err == nil || !strings.Contains(err.Error(), "not a regular file") {
			t.Fatalf("error = %v, want a directory refused rather than read", err)
		}
	})

	t.Run("a link to a real record", func(t *testing.T) {
		s := aSpool(t)
		id := "22222222222222222222222222222222"
		honest := filepath.Join(s.directory, "elsewhere.json")
		if err := os.WriteFile(honest, []byte(`{"id":"`+id+`"}`), 0o600); err != nil {
			t.Fatal(err)
		}
		symlinkTest(t, honest, filepath.Join(s.directory, id+".started.json"))
		_, err := s.readStarted(id)
		if err == nil || !strings.Contains(err.Error(), "not a regular file") {
			t.Fatalf("error = %v, want the link refused rather than followed", err)
		}
	})

	t.Run("present and not JSON", func(t *testing.T) {
		s := aSpool(t)
		id := "33333333333333333333333333333333"
		if err := os.WriteFile(filepath.Join(s.directory, id+".started.json"), []byte("{not json"), 0o600); err != nil {
			t.Fatal(err)
		}
		_, err := s.readStarted(id)
		if err == nil || !strings.Contains(err.Error(), "unreadable start record") {
			t.Fatalf("error = %v, want unreadable, which is not the same as missing", err)
		}
	})

	t.Run("an honest record", func(t *testing.T) {
		s := aSpool(t)
		entry, err := s.Begin("ra8ci:build", strings.Repeat("a", 64))
		if err != nil {
			t.Fatal(err)
		}
		started, err := s.readStarted(entry.ID)
		if err != nil {
			t.Fatalf("a record this spool just wrote was refused: %v", err)
		}
		if started.Task != "ra8ci:build" || started.ID != entry.ID {
			t.Fatalf("read back %+v, want the frozen task and id", started)
		}
	})
}

// A record name is joined onto the spool directory, so a name carrying a path
// separator would write outside it. It is refused before any file is created,
// and the refusal leaves nothing behind.
func TestARecordNameMayNotCarryAPath(t *testing.T) {
	s := aSpool(t)

	for _, name := range []string{"../escape.json", "nested/child.json", `back\\slash.json`} {
		if err := s.write(name, map[string]string{"id": "x"}); err == nil || !strings.Contains(err.Error(), "invalid spool name") {
			t.Fatalf("write(%q) error = %v, want the name refused", name, err)
		}
	}
	entries, err := os.ReadDir(s.directory)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 0 {
		t.Fatalf("a refused write left %d entries behind", len(entries))
	}
}

// A written record is private and complete: mode 0600, one JSON document, and
// readable back as itself.
func TestAWrittenRecordIsPrivateAndComplete(t *testing.T) {
	s := aSpool(t)
	if err := s.write("plain.json", map[string]string{"task": "ra8ci:build"}); err != nil {
		t.Fatal(err)
	}

	path := filepath.Join(s.directory, "plain.json")
	info, err := os.Lstat(path)
	if err != nil {
		t.Fatal(err)
	}
	if perm := info.Mode().Perm(); perm != 0o600 {
		t.Fatalf("mode = %v, want 0600", perm)
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var back map[string]string
	if err := json.Unmarshal(raw, &back); err != nil {
		t.Fatalf("the record this spool wrote is not readable JSON: %v", err)
	}
	if back["task"] != "ra8ci:build" {
		t.Fatalf("read back %v", back)
	}
	entries, err := os.ReadDir(s.directory)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 1 {
		t.Fatalf("the write left %d entries, want only the record", len(entries))
	}
}

// An identifier is 32 lowercase hex characters, and every other shape is
// refused. The wrong-length cases and the wrong-alphabet cases are separate
// failures in the same door, so both are held.
func TestAnIdentifierIsThirtyTwoHexCharacters(t *testing.T) {
	good := "0123456789abcdef0123456789abcdef"
	if !validID(good) {
		t.Fatalf("validID(%q) = false, want true", good)
	}
	for _, id := range []string{
		"",
		strings.Repeat("a", 31),
		strings.Repeat("a", 33),
		"0123456789ABCDEF0123456789abcdef",
		"0123456789abcdef0123456789abcde-",
		"0123456789abcdef0123456789abcdeg",
		" 123456789abcdef0123456789abcdef",
	} {
		if validID(id) {
			t.Fatalf("validID(%q) = true, want false", id)
		}
	}
}
