// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package source

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// theSameSnapshotTwice is the property the whole package rests on: a clean
// checkout answers one identity, and it answers the same one every time.
func TestACleanCheckoutAnswersOneIdentityEveryTime(t *testing.T) {
	repo := newRepository(t, filepath.Join(t.TempDir(), "root"), "root.txt")

	first, err := Snapshot(context.Background(), repo)
	if err != nil {
		t.Fatal(err)
	}
	second, err := Snapshot(context.Background(), repo)
	if err != nil {
		t.Fatal(err)
	}
	if first.Digest != second.Digest || first.RootCommit != second.RootCommit {
		t.Fatalf("the same checkout answered twice: %q/%q then %q/%q",
			first.RootCommit, first.Digest, second.RootCommit, second.Digest)
	}
	if !validSHA256(first.Digest) || !validObjectID(first.RootCommit) {
		t.Fatalf("identity is not well formed: commit %q digest %q", first.RootCommit, first.Digest)
	}
	if len(first.Manifest.Entries) != 1 || first.Manifest.Entries[0].Path != "" {
		t.Fatalf("the root entry is not the whole manifest: %+v", first.Manifest.Entries)
	}
	if first.Manifest.Algorithm != Algorithm {
		t.Fatalf("algorithm = %q, want %q", first.Manifest.Algorithm, Algorithm)
	}
	if string(first.ManifestJSON) != string(second.ManifestJSON) {
		t.Fatalf("manifest bytes differ between snapshots of one checkout")
	}

	// A new commit is a new identity, or the digest is not binding anything.
	if err := os.WriteFile(filepath.Join(repo, "root.txt"), []byte("second\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	commitAll(t, repo, "second")
	moved, err := Snapshot(context.Background(), repo)
	if err != nil {
		t.Fatal(err)
	}
	if moved.RootCommit == first.RootCommit || moved.Digest == first.Digest {
		t.Fatalf("a new commit kept the old identity: %q/%q", moved.RootCommit, moved.Digest)
	}
}

// Verify judges the expected pair BEFORE it looks at the checkout, so a
// caller who asks with a malformed identity is told the identity is wrong
// rather than being handed whatever the tree happens to be.
func TestVerifyRefusesAnIdentityThatIsNotOne(t *testing.T) {
	repo := newRepository(t, filepath.Join(t.TempDir(), "root"), "root.txt")
	good, err := Snapshot(context.Background(), repo)
	if err != nil {
		t.Fatal(err)
	}

	for name, pair := range map[string][2]string{
		"no commit":         {"", good.Digest},
		"short commit":      {strings.Repeat("a", 39), good.Digest},
		"long commit":       {strings.Repeat("a", 41), good.Digest},
		"shouted commit":    {strings.ToUpper(good.RootCommit), good.Digest},
		"non-hex commit":    {strings.Repeat("g", 40), good.Digest},
		"no digest":         {good.RootCommit, ""},
		"short digest":      {good.RootCommit, strings.Repeat("a", 63)},
		"commit as digest":  {good.RootCommit, good.RootCommit},
		"shouted digest":    {good.RootCommit, strings.ToUpper(good.Digest)},
		"non-hex digest":    {good.RootCommit, strings.Repeat("z", 64)},
		"both malformed":    {"nope", "nope"},
		"digest and commit": {good.Digest, good.RootCommit},
	} {
		if _, err := Verify(context.Background(), repo, pair[0], pair[1]); !errors.Is(err, ErrSourceMismatch) {
			t.Fatalf("%s: err = %v, want ErrSourceMismatch", name, err)
		}
	}

	// A well-formed identity that simply is not this tree's is the same
	// refusal, and either half alone is enough to fail it.
	other := strings.Repeat("b", 40)
	otherDigest := strings.Repeat("c", 64)
	for name, pair := range map[string][2]string{
		"other commit": {other, good.Digest},
		"other digest": {good.RootCommit, otherDigest},
		"other both":   {other, otherDigest},
	} {
		if _, err := Verify(context.Background(), repo, pair[0], pair[1]); !errors.Is(err, ErrSourceMismatch) {
			t.Fatalf("%s: err = %v, want ErrSourceMismatch", name, err)
		}
	}

	verified, err := Verify(context.Background(), repo, good.RootCommit, good.Digest)
	if err != nil {
		t.Fatal(err)
	}
	if verified.Digest != good.Digest || verified.RootCommit != good.RootCommit {
		t.Fatalf("Verify answered a different snapshot: %+v", verified)
	}
	if string(verified.ManifestJSON) != string(good.ManifestJSON) {
		t.Fatal("Verify answered different manifest bytes than Snapshot")
	}
}

// A dirty tree has no identity to verify, and Verify must report the dirt
// rather than flattening it into a mismatch: the two send an operator to
// different places.
func TestVerifyCarriesTheCheckoutsOwnComplaint(t *testing.T) {
	repo := newRepository(t, filepath.Join(t.TempDir(), "root"), "root.txt")
	good, err := Snapshot(context.Background(), repo)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(repo, "root.txt"), []byte("edited\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := Verify(context.Background(), repo, good.RootCommit, good.Digest); !errors.Is(err, ErrDirty) {
		t.Fatalf("err = %v, want ErrDirty", err)
	}
}

// inside() answers on filepath.Rel, which fails outright when one path is
// absolute and the other is not. That failure must read as "outside", never
// as an accidental yes.
func TestInsideRefusesWhatItCannotPlace(t *testing.T) {
	for name, pair := range map[string][2]string{
		"relative root":      {"relative", "/absolute/child"},
		"relative candidate": {"/absolute", "relative/child"},
	} {
		if inside(pair[0], pair[1]) {
			t.Fatalf("%s: %q was judged inside %q", name, pair[1], pair[0])
		}
	}
	for name, pair := range map[string][2]string{
		"itself":     {"/one", "/one"},
		"child":      {"/one", "/one/two"},
		"grandchild": {"/one", "/one/two/three"},
	} {
		if !inside(pair[0], pair[1]) {
			t.Fatalf("%s: %q was judged outside %q", name, pair[1], pair[0])
		}
	}
	for name, pair := range map[string][2]string{
		"parent":       {"/one/two", "/one"},
		"sibling":      {"/one", "/other"},
		"near-sibling": {"/one", "/onetwo"},
		"the climb":    {"/one", "/one/../other"},
	} {
		if inside(pair[0], pair[1]) {
			t.Fatalf("%s: %q was judged inside %q", name, pair[1], pair[0])
		}
	}
}

// The control-character sweep is the last rule in validateRelativePath, and
// it is the only one that catches these: NUL, CR and LF are refused earlier
// by the character set, so a path has to reach the sweep to be judged by it.
func TestARelativePathCarriesNoControlCharacter(t *testing.T) {
	for name, value := range map[string]string{
		"start of heading": "a\x01b",
		"bell":             "deps/\x07child",
		"escape":           "deps/child\x1b",
		"unit separator":   "a\x1fb",
		"delete":           "deps/\x7fchild",
		"tab":              "deps/child\tname",
	} {
		err := validateRelativePath(value)
		if !errors.Is(err, ErrUnsafePath) {
			t.Fatalf("%s: err = %v, want ErrUnsafePath", name, err)
		}
	}
	for name, value := range map[string]string{
		"plain":        "deps/child",
		"with a space": "deps/child repo",
		"deep":         "a/b/c/d/e",
		"dotted name":  "deps/child.git",
		"leading dot":  "deps/.hidden",
	} {
		if err := validateRelativePath(value); err != nil {
			t.Fatalf("%s: %q refused: %v", name, value, err)
		}
	}
}

// The entry bound is checked on the way in, alongside the depth bound, so a
// submodule graph cannot be walked past it however it is shaped.
func TestInspectTreeRefusesAGraphPastItsBounds(t *testing.T) {
	repo := newRepository(t, filepath.Join(t.TempDir(), "root"), "root.txt")
	gitPath := gitBinary(t)

	full := make([]Entry, maxEntries)
	if err := inspectTree(context.Background(), gitPath, repo, repo, "", "", 0, &full); !errors.Is(err, ErrUnsafePath) {
		t.Fatalf("at the entry bound: err = %v, want ErrUnsafePath", err)
	}
	over := make([]Entry, maxEntries+1)
	if err := inspectTree(context.Background(), gitPath, repo, repo, "", "", 0, &over); !errors.Is(err, ErrUnsafePath) {
		t.Fatalf("past the entry bound: err = %v, want ErrUnsafePath", err)
	}

	// One under the bound still walks, so the refusal above is the bound
	// and not a walk that was broken all along.
	nearly := make([]Entry, maxEntries-1)
	if err := inspectTree(context.Background(), gitPath, repo, repo, "", "", 0, &nearly); err != nil {
		t.Fatalf("one entry under the bound: %v", err)
	}
	if len(nearly) != maxEntries {
		t.Fatalf("entries = %d, want the walk to have appended one", len(nearly))
	}
}

// A cancelled context stops the walk at its first Git call rather than
// producing a partial identity somebody could store.
func TestASpentContextProducesNoIdentity(t *testing.T) {
	repo := newRepository(t, filepath.Join(t.TempDir(), "root"), "root.txt")
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	result, err := Snapshot(ctx, repo)
	if !errors.Is(err, ErrGit) {
		t.Fatalf("err = %v, want ErrGit", err)
	}
	if result.Digest != "" || result.RootCommit != "" || len(result.Manifest.Entries) != 0 {
		t.Fatalf("a cancelled snapshot still answered an identity: %+v", result)
	}
}

func gitBinary(t *testing.T) string {
	t.Helper()
	for _, candidate := range []string{"/usr/bin/git", "/bin/git", "/usr/local/bin/git"} {
		if _, err := os.Stat(candidate); err == nil {
			return candidate
		}
	}
	t.Skip("git is not installed at a known path")
	return ""
}
