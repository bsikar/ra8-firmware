// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"crypto/ed25519"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// A profile is the reviewed description of a fixture, and its digest is what
// a challenge is bound to. A file the loader cannot prove it read whole, or
// read the same file it stat'd, is refused rather than digested.

// plantProfile writes a profile document at mode 0o644 and hands back its
// path, so each case only has to say what is wrong with it.
func plantProfile(t *testing.T, document string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "profile.json")
	if err := os.WriteFile(path, []byte(document), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func soundProfileDocument(t *testing.T) string {
	t.Helper()
	encoded, err := json.Marshal(validProfileFixture())
	if err != nil {
		t.Fatal(err)
	}
	return string(encoded)
}

// The reviewed fixture round-trips, so every refusal below is about the
// document rather than about the loader disliking the shape in general.
func TestLoadProfileDigestsTheReviewedDocument(t *testing.T) {
	profile, digest, err := LoadProfile(plantProfile(t, soundProfileDocument(t)))
	if err != nil {
		t.Fatalf("the reviewed fixture was refused: %v", err)
	}
	if len(digest) != 64 {
		t.Fatalf("digest = %q", digest)
	}
	if profile.BoardID != validProfileFixture().BoardID {
		t.Fatalf("board = %q", profile.BoardID)
	}
}

// A file whose permissions let anyone else edit it is not evidence of a
// review, however well-formed its contents are.
func TestLoadProfileRefusesAFileOthersCanEdit(t *testing.T) {
	for name, mode := range map[string]os.FileMode{
		"group writable":  0o664,
		"world writable":  0o646,
		"writable by all": 0o666,
	} {
		path := plantProfile(t, soundProfileDocument(t))
		if err := os.Chmod(path, mode); err != nil {
			t.Fatal(err)
		}
		if _, _, err := LoadProfile(path); !errors.Is(err, ErrInvalidProfile) {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// An empty file, a directory, a symlink and a file that is not there are all
// refused before anything is decoded.
func TestLoadProfileRefusesWhatIsNotAReviewedFile(t *testing.T) {
	root := t.TempDir()
	empty := filepath.Join(root, "empty.json")
	if err := os.WriteFile(empty, nil, 0o644); err != nil {
		t.Fatal(err)
	}
	linked := filepath.Join(root, "linked.json")
	if err := os.Symlink(plantProfile(t, soundProfileDocument(t)), linked); err != nil {
		t.Fatal(err)
	}

	for name, path := range map[string]string{
		"a file that is not there": filepath.Join(root, "absent.json"),
		"an empty file":            empty,
		"a directory":              root,
		"a symlink to a good one":  linked,
	} {
		if _, _, err := LoadProfile(path); !errors.Is(err, ErrInvalidProfile) {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// A file that stats clean and then cannot be opened is reported as the open
// failure it was, not silently treated as an absent profile.
func TestLoadProfileReportsAFileItCannotOpen(t *testing.T) {
	path := plantProfile(t, soundProfileDocument(t))
	if err := os.Chmod(path, 0o000); err != nil {
		t.Fatal(err)
	}
	if os.Geteuid() == 0 {
		t.Skip("a sealed file is still readable by root")
	}
	_, _, err := LoadProfile(path)
	if err == nil {
		t.Fatal("a sealed profile was loaded")
	}
	if !errors.Is(err, os.ErrPermission) {
		t.Fatalf("a sealed profile = %v", err)
	}
}

// Anything after the profile object is a second document nobody reviewed.
func TestLoadProfileRefusesMoreThanOneDocument(t *testing.T) {
	sound := soundProfileDocument(t)
	for name, document := range map[string]string{
		"a second object":     sound + "\n" + sound,
		"a trailing scalar":   sound + "\n7",
		"a trailing fragment": sound + "]",
		"a bare array":        "[" + sound + "]",
		"nothing at all":      " ",
		"an unclosed object":  strings.TrimSuffix(sound, "}"),
		"an unknown field":    `{"schema_version":1,"board_id":"ek-ra8d2","surprise":true}`,
	} {
		if _, _, err := LoadProfile(plantProfile(t, document)); err == nil {
			t.Fatalf("%s was loaded as a reviewed profile", name)
		}
	}
}

// A document that parses but describes an incomplete fixture is refused by
// review, so the loader never hands back a digest for it.
func TestLoadProfileRefusesADocumentReviewWouldNotPass(t *testing.T) {
	incomplete := validProfileFixture()
	incomplete.State = nil
	encoded, err := json.Marshal(incomplete)
	if err != nil {
		t.Fatal(err)
	}
	profile, digest, err := LoadProfile(plantProfile(t, string(encoded)))
	if !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("a fixture with no state checks = %v", err)
	}
	if digest != "" || profile.BoardID != "" {
		t.Fatalf("a refused profile still came back: %q %q", digest, profile.BoardID)
	}
}

// A producer signs on an observer's word, so an observer that is not really
// there has to be caught at construction rather than at release time. A typed
// nil slips past a bare interface comparison, which is the whole point of the
// check.
type valueObserver struct{}

func (valueObserver) ObserveNeutral(context.Context, store.NeutralChallenge) (Observation, error) {
	return Observation{}, ErrObservationAbsent
}

func TestNewProducerRefusesAnObserverThatIsNotReallyThere(t *testing.T) {
	_, private, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	var typedNil *LinuxObserver

	for name, observer := range map[string]NeutralObserver{
		"no observer at all":   nil,
		"a typed nil observer": typedNil,
	} {
		if _, err := NewProducer("key-1", private, observer, nil); !errors.Is(err, ErrObservationAbsent) {
			t.Fatalf("%s = %v", name, err)
		}
	}

	// An observer held by value is not nil-able, and is accepted: the
	// check is about absence, not about how the observer is held.
	if _, err := NewProducer("key-1", private, valueObserver{}, nil); err != nil {
		t.Fatalf("an observer held by value was refused: %v", err)
	}
}
