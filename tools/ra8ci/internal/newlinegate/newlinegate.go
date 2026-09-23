// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package newlinegate enforces a final newline on first-party source files.
package newlinegate

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

var suffixes = map[string]bool{
	".c": true, ".h": true, ".cpp": true, ".hpp": true, ".cc": true,
	".cxx": true, ".hh": true, ".hxx": true, ".m": true, ".mm": true,
	".inl": true, ".py": true, ".sh": true, ".cmake": true, ".mk": true,
	".just": true, ".yml": true, ".yaml": true, ".ld": true,
}
var sourceNames = map[string]bool{"CMakeLists.txt": true, "justfile": true, "Justfile": true}
var excludedPrefixes = []string{
	"libs/third_party/", "apps/shared_libs/third_party/", "libs/ra8_fonts/",
	"port/threadx/", "tools/vela/generated/",
}
var buildRoots = map[string]bool{
	"docs": true, "examples": true, "local-poc": true, "port": true,
	"tests": true, "tools": true, "apps": true,
}

const (
	fileFloor    = 2200
	trackedFloor = 1000
)

// Run scans explicitly named paths or the whole derived first-party source set.
// Exit 0 is clean, 1 means files are missing a newline, and 2 means the scan
// could not be trusted.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		if stderr != nil {
			fmt.Fprintln(stderr, "ra8ci final-newline: invalid input")
		}
		return 2
	}
	if contains(args, "--selftest") {
		if selfTest(ctx, root, stdout, stderr) {
			return 0
		}
		return 2
	}
	all := len(args) == 0 || (len(args) == 1 && args[0] == "--all")
	if !all && contains(args, "--all") {
		fmt.Fprintln(stderr, "usage: ra8ci final-newline [--all|FILE ...] | --selftest")
		return 2
	}
	var targets []string
	var err error
	if all {
		targets, err = derivedTargets(ctx, root)
	} else {
		targets, err = explicitTargets(root, args)
	}
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci final-newline:", err)
		return 2
	}
	if all && len(targets) < fileFloor {
		fmt.Fprintf(stderr, "ra8ci final-newline: FATAL -- only %d file(s) in scope, floor is %d. A collapsed sweep reports a clean tree because it scanned nothing.\n", len(targets), fileFloor)
		return 2
	}
	if len(targets) == 0 {
		fmt.Fprintln(stderr, "ra8ci final-newline: no files to scan")
		return 0
	}
	var missing []string
	for _, path := range targets {
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci final-newline: scan cancelled:", err)
			return 2
		}
		data, readErr := os.ReadFile(path)
		if readErr != nil {
			continue
		}
		if len(data) > 0 && data[len(data)-1] != '\n' {
			missing = append(missing, displayPath(root, path))
		}
	}
	sort.Strings(missing)
	if len(missing) == 0 {
		fmt.Fprintf(stdout, "ra8ci final-newline: %d file(s) scanned, all end in a newline.\n", len(targets))
		return 0
	}
	fmt.Fprintf(stderr, "ra8ci final-newline: %d file(s) missing a trailing newline:\n\n", len(missing))
	for _, path := range missing {
		fmt.Fprintf(stderr, "  %s\n", path)
	}
	fmt.Fprintln(stderr, "\nAdd a single newline at end of file.")
	return 1
}

func contains(values []string, value string) bool {
	for _, item := range values {
		if item == value {
			return true
		}
	}
	return false
}

func displayPath(root, path string) string {
	rel, err := filepath.Rel(root, path)
	if err == nil && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return filepath.ToSlash(rel)
	}
	return path
}

func isSource(path string) bool {
	return suffixes[filepath.Ext(path)] || sourceNames[filepath.Base(path)]
}

func excluded(root, path string) bool {
	rel, err := filepath.Rel(root, path)
	if err != nil {
		return true
	}
	rel = filepath.ToSlash(rel)
	for _, prefix := range excludedPrefixes {
		if strings.Contains(rel, prefix) {
			return true
		}
	}
	if strings.Contains(rel, "_unsupported/") {
		return true
	}
	parts := strings.Split(rel, "/")
	for index, part := range parts[:len(parts)-1] {
		if part == "CMakeFiles" || part == "_deps" || part == "__pycache__" || part == "node_modules" {
			return true
		}
		build := part == "build" || strings.HasPrefix(part, "build-") || strings.HasPrefix(part, "build_") || strings.HasPrefix(part, "cmake-build-")
		if build && (index == 0 || buildRoots[parts[0]]) {
			return true
		}
	}
	return false
}

func explicitTargets(root string, args []string) ([]string, error) {
	var targets []string
	for _, raw := range args {
		path := raw
		if !filepath.IsAbs(path) {
			path = filepath.Join(root, path)
		}
		info, err := os.Stat(path)
		if err == nil && info.IsDir() {
			err = filepath.WalkDir(path, func(name string, entry os.DirEntry, walkErr error) error {
				if walkErr != nil {
					return walkErr
				}
				if entry.IsDir() {
					if name != path && excluded(root, name) {
						return filepath.SkipDir
					}
					return nil
				}
				if !isSource(name) || excluded(root, name) {
					return nil
				}
				targets = append(targets, name)
				return nil
			})
			if err != nil {
				return nil, err
			}
		} else if isSource(path) && !excluded(root, path) {
			targets = append(targets, path)
		}
	}
	sort.Strings(targets)
	return targets, nil
}

func derivedTargets(ctx context.Context, root string) ([]string, error) {
	command := exec.CommandContext(ctx, "git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
	output, err := command.Output()
	if err != nil {
		return nil, fmt.Errorf("git ls-files failed: %w", err)
	}
	items := bytes.Split(output, []byte{0})
	paths := make([]string, 0)
	tracked := 0
	for _, item := range items {
		if len(item) == 0 {
			continue
		}
		rel := filepath.ToSlash(string(item))
		absolute := filepath.Join(root, filepath.FromSlash(rel))
		info, statErr := os.Stat(absolute)
		if statErr != nil || !info.Mode().IsRegular() {
			continue
		}
		tracked++
		if !isSource(rel) || excluded(root, absolute) {
			continue
		}
		paths = append(paths, absolute)
	}
	if tracked < trackedFloor {
		return nil, fmt.Errorf("only %d tracked path(s), floor is %d", tracked, trackedFloor)
	}
	sort.Strings(paths)
	return paths, nil
}

func selfTest(ctx context.Context, root string, stdout, stderr io.Writer) bool {
	dir, err := os.MkdirTemp("", "ra8ci-newline-selftest-")
	if err != nil {
		return false
	}
	defer os.RemoveAll(dir)
	good := filepath.Join(dir, "good.py")
	bad := filepath.Join(dir, "bad.py")
	empty := filepath.Join(dir, "empty.py")
	if os.WriteFile(good, []byte("x = 1\n"), 0600) != nil ||
		os.WriteFile(bad, []byte("x = 1"), 0600) != nil ||
		os.WriteFile(empty, nil, 0600) != nil {
		return false
	}
	goodData, goodErr := os.ReadFile(good)
	badData, badErr := os.ReadFile(bad)
	emptyData, emptyErr := os.ReadFile(empty)
	if goodErr != nil || badErr != nil || emptyErr != nil ||
		goodData[len(goodData)-1] != '\n' || badData[len(badData)-1] == '\n' ||
		len(emptyData) != 0 {
		return false
	}
	targets, err := derivedTargets(ctx, root)
	if err != nil || len(targets) < fileFloor {
		fmt.Fprintf(stderr, "derived scope has %d file(s), floor is %d: %v\n", len(targets), fileFloor, err)
		return false
	}
	var justRoot, infraRoot bool
	for _, path := range targets {
		rel := filepath.ToSlash(displayPath(root, path))
		justRoot = justRoot || strings.HasPrefix(rel, "just/")
		infraRoot = infraRoot || strings.HasPrefix(rel, "infra/")
	}
	if !justRoot || !infraRoot {
		fmt.Fprintf(stderr, "derived scope roots: just=%t infra=%t\n", justRoot, infraRoot)
		return false
	}
	fmt.Fprintln(stdout, "ra8ci final-newline selftest passed (clean, missing and empty files; derived scope).")
	return true
}
