// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

import (
	"errors"
	"strings"
	"testing"
)

// at returns a manifest for one more artifact of the sample's attempt, so a
// set built from several of them differs in nothing but the paths.
func at(path string) ArtifactManifest {
	manifest := sampleArtifactManifest()
	manifest.Path = path
	return manifest
}

// setOf is the set rule read through its only caller, which is how these
// paths actually reach the predicate.
func setOf(paths ...string) error {
	manifests := make([]ArtifactManifest, 0, len(paths))
	for _, path := range paths {
		manifests = append(manifests, at(path))
	}
	return ValidateArtifactSet(manifests)
}

func TestPathsOneHostCanWriteAreAccepted(t *testing.T) {
	cases := [][]string{
		{"build/ra8.elf", "build/ra8.elf.map"},
		{"logs/hil/segment-0001.json", "logs/hil/segment-0002.json"},
		{"build/ra8.elf", "logs/run.txt", "report.xml"},
		{"build/out/a.txt", "build/other/a.txt"},
		// A name that CONTAINS another is not a name UNDER it: the shared
		// prefix stops mid-segment, so no directory is asked to be a file.
		{"build", "buildinfo.txt"},
		{"logs/run.txt", "logs/run.txt.gz"},
	}
	for _, paths := range cases {
		if err := setOf(paths...); err != nil {
			t.Fatalf("%v: an ordinary set was refused: %v", paths, err)
		}
	}
}

func TestASetNamingOnePathTwiceIsRefused(t *testing.T) {
	err := setOf("build/ra8.elf", "build/ra8.elf")
	if !errors.Is(err, ErrInvalid) {
		t.Fatalf("a repeated path was accepted: %v", err)
	}
	if !strings.Contains(err.Error(), "twice") {
		t.Fatalf("refused for the wrong reason: %v", err)
	}
}

func TestASetNamingTwoSpellingsOfOneNameIsRefused(t *testing.T) {
	cases := [][2]string{
		{"logs/run.txt", "logs/Run.txt"},
		{"logs/run.txt", "Logs/run.txt"},
		{"BUILD/RA8.ELF", "build/ra8.elf"},
	}
	for _, pair := range cases {
		err := setOf(pair[0], pair[1])
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("%v: two spellings of one name were accepted: %v", pair, err)
		}
		if !strings.Contains(err.Error(), "folds case") {
			t.Fatalf("%v: refused for the wrong reason: %v", pair, err)
		}
		if !strings.Contains(err.Error(), pair[0]) || !strings.Contains(err.Error(), pair[1]) {
			t.Fatalf("%v: the message does not name both entries: %v", pair, err)
		}
	}
}

func TestASetNeedingOneNameAsFileAndDirectoryIsRefused(t *testing.T) {
	cases := [][2]string{
		{"build", "build/ra8.elf"},
		{"build/ra8.elf", "build"},
		{"logs/hil", "logs/hil/segment-0001.json"},
		{"logs/hil/segment-0001.json", "logs/hil"},
		// Two levels up, not just the immediate parent.
		{"logs", "logs/hil/segment-0001.json"},
		// And folded, so the two rules compose rather than leaving a gap
		// between them.
		{"Build", "build/ra8.elf"},
	}
	for _, pair := range cases {
		err := setOf(pair[0], pair[1])
		if !errors.Is(err, ErrInvalid) {
			t.Fatalf("%v: a file/directory collision was accepted: %v", pair, err)
		}
		if !strings.Contains(err.Error(), "both a file and a directory") {
			t.Fatalf("%v: refused for the wrong reason: %v", pair, err)
		}
	}
}

// The existing per-path rules still run first: a set of one entry is judged
// by Validate before it ever reaches the path set.
func TestTheEntryIsJudgedBeforeItsPlaceInTheSet(t *testing.T) {
	broken := at("build/ra8.elf")
	broken.SHA256 = "not a digest"
	if !errors.Is(ValidateArtifactSet([]ArtifactManifest{at("build"), broken}), ErrInvalid) {
		t.Fatal("an invalid entry was accepted")
	}
}

func TestTheDeepestSetOfPathsIsAccepted(t *testing.T) {
	segments := make([]string, maxArtifactDepth)
	for index := range segments {
		segments[index] = "d"
	}
	deep := strings.Join(segments, "/")
	if !ValidArtifactPath(deep) {
		t.Fatalf("the fixture must be a valid path or the test proves nothing: %q", deep)
	}
	if err := setOf(deep); err != nil {
		t.Fatalf("a single deep path was refused: %v", err)
	}
}

func TestFoldedParentsListsTheDirectoriesAPathNeeds(t *testing.T) {
	cases := []struct {
		path string
		want []string
	}{
		{"ra8.elf", nil},
		{"build/ra8.elf", []string{"build"}},
		{"logs/hil/segment-0001.json", []string{"logs", "logs/hil"}},
		{"a/b/c/d", []string{"a", "a/b", "a/b/c"}},
	}
	for _, testCase := range cases {
		got := foldedParents(testCase.path)
		if len(got) != len(testCase.want) {
			t.Fatalf("%q: got %v, want %v", testCase.path, got, testCase.want)
		}
		for index := range got {
			if got[index] != testCase.want[index] {
				t.Fatalf("%q: got %v, want %v", testCase.path, got, testCase.want)
			}
		}
	}
}

// The budget rules still hold, and a set that both collides and exceeds the
// count is refused either way: this pins that the new rule did not displace
// them.
func TestTheCountBudgetStillHolds(t *testing.T) {
	many := make([]ArtifactManifest, 0, MaxArtifactsPerAttempt+1)
	for index := 0; index <= MaxArtifactsPerAttempt; index++ {
		many = append(many, at("logs/hil/"+strings.Repeat("a", index+1)+".json"))
	}
	if !errors.Is(ValidateArtifactSet(many), ErrInvalid) {
		t.Fatal("a set past the per-attempt count was accepted")
	}
}
