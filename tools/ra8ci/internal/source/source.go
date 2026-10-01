// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package source binds an execution to a clean Git commit and pinned submodules.
package source

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"unicode/utf8"
)

const (
	Algorithm    = "git-archive-recursive-v1"
	maxGitOutput = 32 << 20
	maxDepth     = 16
	maxEntries   = 512
)

var (
	ErrDirty          = errors.New("source checkout is dirty")
	ErrPinMismatch    = errors.New("submodule commit differs from pinned gitlink")
	ErrUnsafePath     = errors.New("unsafe submodule path")
	ErrSourceMismatch = errors.New("source snapshot mismatch")
	ErrGit            = errors.New("source Git operation failed")
)

// Entry binds one tree, including the root at path "", to its exact archive.
type Entry struct {
	Path          string `json:"path"`
	Commit        string `json:"commit"`
	ArchiveSHA256 string `json:"archive_sha256"`
}

// Manifest is encoded with its declared field order and no trailing newline.
type Manifest struct {
	Algorithm string  `json:"algorithm"`
	Entries   []Entry `json:"entries"`
}

// Result includes the audit-ready manifest bytes and their SHA-256 identity.
type Result struct {
	RootCommit   string
	Manifest     Manifest
	ManifestJSON []byte
	Digest       string
}

type gitLink struct {
	path   string
	commit string
}

// Snapshot hashes Git archives for the root and every recursively pinned gitlink.
// All checkouts must be initialized, at their pinned commits, and clean.
func Snapshot(ctx context.Context, root string) (Result, error) {
	if ctx == nil || root == "" {
		return Result{}, fmt.Errorf("%w: missing context or checkout", ErrGit)
	}
	absolute, err := filepath.Abs(root)
	if err != nil {
		return Result{}, fmt.Errorf("%w: root: %v", ErrGit, err)
	}
	canonicalRoot, err := filepath.EvalSymlinks(absolute)
	if err != nil {
		return Result{}, fmt.Errorf("%w: root: %v", ErrGit, err)
	}
	gitPath, err := exec.LookPath("git")
	if err != nil {
		return Result{}, fmt.Errorf("%w: git is missing: %v", ErrGit, err)
	}
	gitPath, err = filepath.EvalSymlinks(gitPath)
	if err != nil || !filepath.IsAbs(gitPath) {
		return Result{}, fmt.Errorf("%w: git executable path is not canonical", ErrGit)
	}
	if inside(canonicalRoot, gitPath) {
		return Result{}, fmt.Errorf("%w: git executable is inside checkout", ErrUnsafePath)
	}
	entries := make([]Entry, 0, 2)
	if err := inspectTree(ctx, gitPath, canonicalRoot, canonicalRoot, "", "", 0, &entries); err != nil {
		return Result{}, err
	}
	sort.Slice(entries, func(i, j int) bool { return entries[i].Path < entries[j].Path })
	if len(entries) == 0 || entries[0].Path != "" {
		return Result{}, fmt.Errorf("%w: root entry is missing", ErrGit)
	}
	for index := 1; index < len(entries); index++ {
		for previous := 0; previous < index; previous++ {
			if strings.EqualFold(entries[previous].Path, entries[index].Path) {
				return Result{}, fmt.Errorf("%w: colliding submodule paths", ErrUnsafePath)
			}
		}
	}
	manifest := Manifest{Algorithm: Algorithm, Entries: entries}
	canonical, err := json.Marshal(manifest)
	if err != nil {
		return Result{}, fmt.Errorf("%w: encode manifest: %v", ErrGit, err)
	}
	sum := sha256.Sum256(canonical)
	return Result{RootCommit: entries[0].Commit, Manifest: manifest, ManifestJSON: canonical, Digest: hex.EncodeToString(sum[:])}, nil
}

// Verify compares a live checkout with the server's exact source identity.
func Verify(ctx context.Context, root, expectedCommit, expectedDigest string) (Result, error) {
	if !validObjectID(expectedCommit) || !validSHA256(expectedDigest) {
		return Result{}, ErrSourceMismatch
	}
	result, err := Snapshot(ctx, root)
	if err != nil {
		return Result{}, err
	}
	if result.RootCommit != expectedCommit || result.Digest != expectedDigest {
		return Result{}, ErrSourceMismatch
	}
	return result, nil
}

func inspectTree(ctx context.Context, gitPath, root, directory, relative, expectedCommit string, depth int, entries *[]Entry) error {
	if depth > maxDepth || len(*entries) >= maxEntries {
		return fmt.Errorf("%w: submodule graph exceeds bounds", ErrUnsafePath)
	}
	commitOutput, err := runGit(ctx, gitPath, directory, "rev-parse", "--verify", "HEAD^{commit}")
	if err != nil {
		return err
	}
	commit := strings.TrimSpace(string(commitOutput))
	if !validObjectID(commit) {
		return fmt.Errorf("%w: invalid commit at %q", ErrGit, relative)
	}
	if expectedCommit != "" && commit != expectedCommit {
		return fmt.Errorf("%w: %q has %s, expected %s", ErrPinMismatch, relative, commit, expectedCommit)
	}
	status, err := runGit(ctx, gitPath, directory, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--ignore-submodules=all")
	if err != nil {
		return err
	}
	if len(status) != 0 {
		return fmt.Errorf("%w: %q", ErrDirty, relative)
	}
	archiveDigest, err := archiveSHA256(ctx, gitPath, directory)
	if err != nil {
		return err
	}
	*entries = append(*entries, Entry{Path: relative, Commit: commit, ArchiveSHA256: archiveDigest})
	tree, err := runGit(ctx, gitPath, directory, "ls-tree", "-r", "-z", "HEAD")
	if err != nil {
		return err
	}
	links, err := parseGitlinks(tree)
	if err != nil {
		return err
	}
	for _, link := range links {
		childRelative := link.path
		if relative != "" {
			childRelative = relative + "/" + link.path
		}
		if err := validateRelativePath(childRelative); err != nil {
			return err
		}
		childDirectory := filepath.Join(root, filepath.FromSlash(childRelative))
		resolved, err := filepath.EvalSymlinks(childDirectory)
		if err != nil || !inside(root, resolved) || resolved == root {
			return fmt.Errorf("%w: uninitialized or escaping submodule %q", ErrUnsafePath, childRelative)
		}
		info, err := os.Stat(resolved)
		if err != nil || !info.IsDir() {
			return fmt.Errorf("%w: submodule %q is not a directory", ErrUnsafePath, childRelative)
		}
		if _, err := os.Lstat(filepath.Join(resolved, ".git")); err != nil {
			return fmt.Errorf("%w: submodule %q is not initialized", ErrUnsafePath, childRelative)
		}
		if err := inspectTree(ctx, gitPath, root, resolved, childRelative, link.commit, depth+1, entries); err != nil {
			return err
		}
	}
	return nil
}

func parseGitlinks(tree []byte) ([]gitLink, error) {
	links := make([]gitLink, 0)
	for _, record := range bytes.Split(tree, []byte{0}) {
		if len(record) == 0 {
			continue
		}
		separator := bytes.IndexByte(record, '\t')
		if separator < 0 {
			return nil, fmt.Errorf("%w: malformed ls-tree record", ErrGit)
		}
		header := strings.Fields(string(record[:separator]))
		if len(header) != 3 {
			return nil, fmt.Errorf("%w: malformed ls-tree header", ErrGit)
		}
		if header[0] != "160000" {
			continue
		}
		if header[1] != "commit" || !validObjectID(header[2]) {
			return nil, fmt.Errorf("%w: invalid gitlink", ErrGit)
		}
		linkPath := string(record[separator+1:])
		if err := validateRelativePath(linkPath); err != nil {
			return nil, err
		}
		links = append(links, gitLink{path: linkPath, commit: header[2]})
	}
	return links, nil
}

func validateRelativePath(value string) error {
	if value == "" || !utf8.ValidString(value) || strings.ContainsAny(value, "\\:\x00\r\n") ||
		path.IsAbs(value) || path.Clean(value) != value {
		return fmt.Errorf("%w: %q", ErrUnsafePath, value)
	}
	for _, component := range strings.Split(value, "/") {
		if component == "" || component == "." || component == ".." {
			return fmt.Errorf("%w: %q", ErrUnsafePath, value)
		}
	}
	for _, char := range value {
		if char < 32 || char == 127 {
			return fmt.Errorf("%w: control character in path", ErrUnsafePath)
		}
	}
	return nil
}

func validObjectID(value string) bool {
	if len(value) != 40 && len(value) != 64 {
		return false
	}
	for _, char := range value {
		if (char < '0' || char > '9') && (char < 'a' || char > 'f') {
			return false
		}
	}
	return true
}

func validSHA256(value string) bool {
	return len(value) == sha256.Size*2 && validObjectID(value)
}

func inside(root, candidate string) bool {
	relative, err := filepath.Rel(root, candidate)
	if err != nil {
		return false
	}
	return relative != ".." && !strings.HasPrefix(relative, ".."+string(filepath.Separator))
}

func archiveSHA256(ctx context.Context, gitPath, directory string) (string, error) {
	cmd := gitCommand(ctx, gitPath, directory, "archive", "--format=tar", "HEAD")
	hash := sha256.New()
	var stderr boundedBuffer
	cmd.Stdout = hash
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return "", fmt.Errorf("%w: archive %q: %v: %s", ErrGit, directory, err, stderr.String())
	}
	if stderr.Len() != 0 {
		return "", fmt.Errorf("%w: archive warning at %q: %s", ErrGit, directory, stderr.String())
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func runGit(ctx context.Context, gitPath, directory string, args ...string) ([]byte, error) {
	cmd := gitCommand(ctx, gitPath, directory, args...)
	var stdout, stderr boundedBuffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return nil, fmt.Errorf("%w: git %s at %q: %v: %s", ErrGit, args[0], directory, err, stderr.String())
	}
	if stderr.Len() != 0 {
		return nil, fmt.Errorf("%w: git %s warning at %q: %s", ErrGit, args[0], directory, stderr.String())
	}
	return stdout.Bytes(), nil
}

func gitCommand(ctx context.Context, gitPath, directory string, args ...string) *exec.Cmd {
	argv := []string{"-C", directory, "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false"}
	argv = append(argv, args...)
	cmd := exec.CommandContext(ctx, gitPath, argv...)
	cmd.Env = []string{
		"PATH=" + os.Getenv("PATH"),
		"HOME=" + os.Getenv("HOME"),
		"GIT_CONFIG_NOSYSTEM=1",
		"GIT_CONFIG_GLOBAL=" + os.DevNull,
		"GIT_OPTIONAL_LOCKS=0",
		"GIT_TERMINAL_PROMPT=0",
		"LC_ALL=C",
	}
	if runtime.GOOS == "windows" {
		cmd.Env = append(cmd.Env, "SYSTEMROOT="+os.Getenv("SYSTEMROOT"), "TEMP="+os.Getenv("TEMP"))
	}
	return cmd
}

type boundedBuffer struct {
	bytes.Buffer
}

func (buffer *boundedBuffer) Write(data []byte) (int, error) {
	if buffer.Len()+len(data) > maxGitOutput {
		return 0, fmt.Errorf("%w: Git output exceeds %d bytes", ErrGit, maxGitOutput)
	}
	return buffer.Buffer.Write(data)
}

var _ io.Writer = (*boundedBuffer)(nil)
