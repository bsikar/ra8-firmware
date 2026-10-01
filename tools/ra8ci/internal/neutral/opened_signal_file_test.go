// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func regularFile(t *testing.T, dir, name, content string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatal(err)
	}
	return path
}

func openFor(t *testing.T, path string) *os.File {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { file.Close() })
	return file
}

func TestAHandleOnTheJudgedFileIsConfirmed(t *testing.T) {
	path := regularFile(t, t.TempDir(), "value", "neutral\n")
	judged, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := confirmSignalFile(judged, openFor(t, path)); err != nil {
		t.Fatalf("the file that was judged was refused: %v", err)
	}
}

// The swap, stated directly: the properties belong to one file and the handle
// to another, both of them ordinary regular files.
func TestAHandleOnADifferentFileIsRefused(t *testing.T) {
	dir := t.TempDir()
	judgedPath := regularFile(t, dir, "value", "neutral\n")
	swapped := regularFile(t, dir, "other", "not neutral\n")
	judged, err := os.Stat(judgedPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := confirmSignalFile(judged, openFor(t, swapped)); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("a handle on another file was confirmed: %v", err)
	}
}

func TestAHandleOnSomethingThatIsNotARegularFileIsRefused(t *testing.T) {
	dir := t.TempDir()
	judged, err := os.Stat(regularFile(t, dir, "value", "neutral\n"))
	if err != nil {
		t.Fatal(err)
	}
	if err := confirmSignalFile(judged, openFor(t, dir)); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("a directory handle was confirmed: %v", err)
	}
}

func TestNothingToConfirmIsRefused(t *testing.T) {
	path := regularFile(t, t.TempDir(), "value", "neutral\n")
	judged, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := confirmSignalFile(nil, openFor(t, path)); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("an absent judgement was confirmed: %v", err)
	}
	if err := confirmSignalFile(judged, nil); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("an absent handle was confirmed: %v", err)
	}
}

func TestAnOrdinarySignalStillReads(t *testing.T) {
	root := t.TempDir()
	regularFile(t, root, "value", "neutral\n")
	value, err := SysfsReader{Root: root}.ReadSignal(context.Background(), "value")
	if err != nil || strings.TrimSpace(value) != "neutral" {
		t.Fatalf("ordinary signal value=%q err=%v", value, err)
	}
}

// What the confirmation does NOT buy, stated so nobody expects it to: a root
// whose leaf is atomically replaced by another regular file is judged and
// read as that file, consistently. Holding the read to the judged file is a
// rule about this reader; which file is allowed to sit at the path is the
// root's own business.
func TestAReplacedRegularFileIsJudgedAndReadAsItself(t *testing.T) {
	root := t.TempDir()
	staging := t.TempDir()
	regularFile(t, root, "value", "neutral\n")
	replacement := regularFile(t, staging, "replacement", "replaced\n")
	if err := os.Rename(replacement, filepath.Join(root, "value")); err != nil {
		t.Fatal(err)
	}
	value, err := SysfsReader{Root: root}.ReadSignal(context.Background(), "value")
	if err != nil || strings.TrimSpace(value) != "replaced" {
		t.Fatalf("replaced signal value=%q err=%v", value, err)
	}
}
