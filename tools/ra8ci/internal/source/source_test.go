// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package source

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
)

func TestSnapshotRootAndVerify(t *testing.T) {
	repo := newRepository(t, filepath.Join(t.TempDir(), "root"), "root.txt")
	first, err := Snapshot(context.Background(), repo)
	if err != nil {
		t.Fatal(err)
	}
	second, err := Snapshot(context.Background(), repo)
	if err != nil || !reflect.DeepEqual(first, second) {
		t.Fatalf("snapshot is unstable: first=%+v second=%+v err=%v", first, second, err)
	}
	if first.Manifest.Algorithm != Algorithm || len(first.Manifest.Entries) != 1 || first.Manifest.Entries[0].Path != "" ||
		first.RootCommit != gitOutput(t, repo, "rev-parse", "HEAD") || len(first.Digest) != 64 {
		t.Fatalf("bad root snapshot: %+v", first)
	}
	var decoded Manifest
	if err := json.Unmarshal(first.ManifestJSON, &decoded); err != nil || !reflect.DeepEqual(decoded, first.Manifest) {
		t.Fatalf("manifest bytes do not decode: %+v, %v", decoded, err)
	}
	verified, err := Verify(context.Background(), repo, first.RootCommit, first.Digest)
	if err != nil || verified.Digest != first.Digest {
		t.Fatalf("Verify = %+v, %v", verified, err)
	}
	if _, err := Verify(context.Background(), repo, first.RootCommit, strings.Repeat("0", 64)); !errors.Is(err, ErrSourceMismatch) {
		t.Fatalf("wrong digest error = %v", err)
	}
	if _, err := Verify(context.Background(), repo, "not-a-commit", first.Digest); !errors.Is(err, ErrSourceMismatch) {
		t.Fatalf("invalid commit error = %v", err)
	}
}

func TestSnapshotRejectsDirtyRoot(t *testing.T) {
	repo := newRepository(t, filepath.Join(t.TempDir(), "root"), "root.txt")
	if err := os.WriteFile(filepath.Join(repo, "root.txt"), []byte("changed\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Snapshot(context.Background(), repo); !errors.Is(err, ErrDirty) {
		t.Fatalf("dirty root error = %v", err)
	}
}

func TestSnapshotRejectsUntrackedRootFile(t *testing.T) {
	repo := newRepository(t, filepath.Join(t.TempDir(), "root"), "root.txt")
	if err := os.WriteFile(filepath.Join(repo, "surprise.txt"), []byte("untracked\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Snapshot(context.Background(), repo); !errors.Is(err, ErrDirty) {
		t.Fatalf("untracked root error = %v", err)
	}
}

func TestSnapshotIncludesNestedPinnedSubmodules(t *testing.T) {
	outer := t.TempDir()
	grand := newRepository(t, filepath.Join(outer, "grand"), "grand.txt")
	child := newRepository(t, filepath.Join(outer, "child"), "child.txt")
	addSubmodule(t, child, grand, "deps/grand")
	commitAll(t, child, "add grand")
	root := newRepository(t, filepath.Join(outer, "root"), "root.txt")
	addSubmodule(t, root, child, "deps/child")
	commitAll(t, root, "add child")
	gitOutput(t, root, "-c", "protocol.file.allow=always", "submodule", "update", "--init", "--recursive")
	snapshot, err := Snapshot(context.Background(), root)
	if err != nil {
		t.Fatal(err)
	}
	want := []string{"", "deps/child", "deps/child/deps/grand"}
	got := make([]string, 0, len(snapshot.Manifest.Entries))
	for _, entry := range snapshot.Manifest.Entries {
		got = append(got, entry.Path)
		if len(entry.ArchiveSHA256) != 64 || !validObjectID(entry.Commit) {
			t.Fatalf("invalid nested evidence: %+v", entry)
		}
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("entry paths = %v, want %v", got, want)
	}
	if strings.Contains(string(snapshot.ManifestJSON), "\n") {
		t.Fatal("canonical manifest includes whitespace")
	}
}

func TestSnapshotRejectsWrongSubmoduleCommit(t *testing.T) {
	outer := t.TempDir()
	child := newRepository(t, filepath.Join(outer, "child"), "child.txt")
	if err := os.WriteFile(filepath.Join(child, "child.txt"), []byte("second\n"), 0600); err != nil {
		t.Fatal(err)
	}
	commitAll(t, child, "second child commit")
	root := newRepository(t, filepath.Join(outer, "root"), "root.txt")
	addSubmodule(t, root, child, "sub")
	commitAll(t, root, "add sub")
	gitOutput(t, filepath.Join(root, "sub"), "checkout", "-q", "HEAD~1")
	if _, err := Snapshot(context.Background(), root); !errors.Is(err, ErrPinMismatch) {
		t.Fatalf("wrong pin error = %v", err)
	}
}

func TestSnapshotRejectsDirtySubmodule(t *testing.T) {
	outer := t.TempDir()
	child := newRepository(t, filepath.Join(outer, "child"), "child.txt")
	root := newRepository(t, filepath.Join(outer, "root"), "root.txt")
	addSubmodule(t, root, child, "sub")
	commitAll(t, root, "add sub")
	if err := os.WriteFile(filepath.Join(root, "sub", "child.txt"), []byte("dirty\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if _, err := Snapshot(context.Background(), root); !errors.Is(err, ErrDirty) {
		t.Fatalf("dirty submodule error = %v", err)
	}
}

func TestSnapshotRejectsUninitializedSubmodule(t *testing.T) {
	outer := t.TempDir()
	child := newRepository(t, filepath.Join(outer, "child"), "child.txt")
	root := newRepository(t, filepath.Join(outer, "root"), "root.txt")
	addSubmodule(t, root, child, "sub")
	commitAll(t, root, "add sub")
	gitOutput(t, root, "submodule", "deinit", "-f", "sub")
	if _, err := Snapshot(context.Background(), root); !errors.Is(err, ErrUnsafePath) {
		t.Fatalf("uninitialized submodule error = %v", err)
	}
}

func TestSnapshotRejectsMissingContextAndRepository(t *testing.T) {
	if _, err := Snapshot(nil, "missing"); !errors.Is(err, ErrGit) {
		t.Fatalf("nil context error = %v", err)
	}
	if _, err := Snapshot(context.Background(), ""); !errors.Is(err, ErrGit) {
		t.Fatalf("empty root error = %v", err)
	}
	if _, err := Snapshot(context.Background(), filepath.Join(t.TempDir(), "missing")); !errors.Is(err, ErrGit) {
		t.Fatalf("missing root error = %v", err)
	}
	if _, err := Snapshot(context.Background(), t.TempDir()); !errors.Is(err, ErrGit) {
		t.Fatalf("non-repository root error = %v", err)
	}
}

func TestSnapshotRejectsGitExecutableInsideCheckout(t *testing.T) {
	repo := newRepository(t, filepath.Join(t.TempDir(), "root"), "root.txt")
	gitTool := filepath.Join(repo, "git")
	if err := os.WriteFile(gitTool, []byte("not executed\n"), 0700); err != nil {
		t.Fatal(err)
	}
	t.Setenv("PATH", repo+string(os.PathListSeparator)+os.Getenv("PATH"))
	if _, err := Snapshot(context.Background(), repo); !errors.Is(err, ErrUnsafePath) {
		t.Fatalf("in-tree Git executable error = %v", err)
	}
}

func TestInspectTreeBoundsAndArchiveFailure(t *testing.T) {
	repo := newRepository(t, filepath.Join(t.TempDir(), "root"), "root.txt")
	var entries []Entry
	if err := inspectTree(context.Background(), "/usr/bin/git", repo, repo, "", "", maxDepth+1, &entries); !errors.Is(err, ErrUnsafePath) {
		t.Fatalf("depth error = %v", err)
	}
	if _, err := archiveSHA256(context.Background(), "/usr/bin/git", t.TempDir()); !errors.Is(err, ErrGit) {
		t.Fatalf("archive in non-repository error = %v", err)
	}
}

func TestBoundedBufferRejectsExcessOutput(t *testing.T) {
	var buffer boundedBuffer
	if n, err := buffer.Write([]byte("okay")); n != 4 || err != nil {
		t.Fatalf("small write = %d, %v", n, err)
	}
	if n, err := buffer.Write(make([]byte, maxGitOutput)); n != 0 || !errors.Is(err, ErrGit) {
		t.Fatalf("oversized write = %d, %v", n, err)
	}
}

func TestValidateRelativePathAndGitlinkRecords(t *testing.T) {
	for _, bad := range []string{"", "/absolute", "../parent", "a/../b", "a//b", "a\\b", "C:/disk", "a\nb", string([]byte{0xff})} {
		if !errors.Is(validateRelativePath(bad), ErrUnsafePath) {
			t.Errorf("unsafe path %q accepted", bad)
		}
	}
	if err := validateRelativePath("deps/child repo"); err != nil {
		t.Fatal(err)
	}
	commit := strings.Repeat("a", 40)
	links, err := parseGitlinks([]byte("100644 blob " + commit + "\tplain.txt\x00160000 commit " + commit + "\tdeps/child\x00"))
	if err != nil || len(links) != 1 || links[0].path != "deps/child" || links[0].commit != commit {
		t.Fatalf("parseGitlinks = %+v, %v", links, err)
	}
	for _, bad := range []string{"bad record\x00", "160000 tree " + commit + "\tsub\x00", "160000 commit " + commit + "\t../escape\x00"} {
		if _, err := parseGitlinks([]byte(bad)); err == nil {
			t.Errorf("bad gitlink record %q accepted", bad)
		}
	}
	for _, id := range []string{"", strings.Repeat("A", 40), strings.Repeat("g", 40), strings.Repeat("a", 39)} {
		if validObjectID(id) {
			t.Errorf("invalid object ID %q accepted", id)
		}
	}
	if !inside("/one", "/one/two") || inside("/one", "/other") {
		t.Fatal("inside() confused child and sibling paths")
	}
}

func TestManifestJSONFieldOrder(t *testing.T) {
	manifest := Manifest{Algorithm: Algorithm, Entries: []Entry{{Path: "", Commit: strings.Repeat("a", 40), ArchiveSHA256: strings.Repeat("b", 64)}}}
	encoded, err := json.Marshal(manifest)
	if err != nil {
		t.Fatal(err)
	}
	want := `{"algorithm":"git-archive-recursive-v1","entries":[{"path":"","commit":"` + strings.Repeat("a", 40) + `","archive_sha256":"` + strings.Repeat("b", 64) + `"}]}`
	if string(encoded) != want {
		t.Fatalf("canonical bytes = %q, want %q", encoded, want)
	}
}

func newRepository(t *testing.T, directory, filename string) string {
	t.Helper()
	if err := os.MkdirAll(directory, 0700); err != nil {
		t.Fatal(err)
	}
	gitOutput(t, directory, "init", "-q")
	gitOutput(t, directory, "config", "user.name", "Brighton Sikarskie")
	gitOutput(t, directory, "config", "user.email", "bsikar@tuta.io")
	if err := os.WriteFile(filepath.Join(directory, filename), []byte("first\n"), 0600); err != nil {
		t.Fatal(err)
	}
	commitAll(t, directory, "initial")
	return directory
}

func addSubmodule(t *testing.T, parent, child, relative string) {
	t.Helper()
	gitOutput(t, parent, "-c", "protocol.file.allow=always", "submodule", "add", "-q", child, relative)
}

func commitAll(t *testing.T, repo, message string) {
	t.Helper()
	gitOutput(t, repo, "add", ".")
	gitOutput(t, repo, "commit", "-qm", message)
}

func gitOutput(t *testing.T, directory string, args ...string) string {
	t.Helper()
	argv := append([]string{"-C", directory}, args...)
	cmd := exec.Command("git", argv...)
	cmd.Env = []string{"PATH=" + os.Getenv("PATH"), "HOME=" + os.Getenv("HOME"), "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=" + os.DevNull, "GIT_TERMINAL_PROMPT=0"}
	output, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git %v failed: %v: %s", args, err, output)
	}
	return strings.TrimSpace(string(output))
}
