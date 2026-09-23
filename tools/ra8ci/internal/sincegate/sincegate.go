// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package sincegate checks Doxygen @since values and public API tag presence.
package sincegate

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

var (
	publicDecl = regexp.MustCompile(`^(?:\[\[nodiscard\]\]\s+)?(?:static\s+inline\s+)?\s*ra8_\w+(?:\s*\*)?\s+(ra8_\w+)\s*\(`)
	sinceTag   = regexp.MustCompile(`@since`)
	sinceValue = regexp.MustCompile(`@since\s+(?:Version\s+)?([0-9]+(?:\.[0-9]+){1,2}[a-z]?)`)
	versionRE  = regexp.MustCompile(`^[0-9]+\.[0-9]+\.[0-9]+$`)
)

var sourceSuffixes = map[string]bool{".c": true, ".h": true, ".cpp": true, ".hpp": true}
var excludedPrefixes = []string{"libs/third_party/", "apps/shared_libs/third_party/", "libs/ra8_fonts/", "tools/vela/generated/", "port/threadx/"}
var buildRoots = map[string]bool{"docs": true, "examples": true, "local-poc": true, "port": true, "tests": true, "tools": true, "apps": true}

const trackedFloor = 1000

// Run executes the checker. It returns 0 for clean, 1 for findings, and 2 if
// the requested scan cannot be trusted or its arguments are invalid.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		if stderr != nil {
			fmt.Fprintln(stderr, "ra8ci since: invalid input")
		}
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		if selfTest(ctx, root, stdout, stderr) {
			return 0
		}
		return 2
	}
	version, err := readProjectVersion(root)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci since:", err)
		return 2
	}
	var paths []string
	if len(args) == 1 && args[0] == "--all" {
		paths, err = collectRepoPaths(ctx, root)
		if err != nil {
			fmt.Fprintln(stderr, "ra8ci since:", err)
			return 2
		}
	} else if len(args) > 0 && !containsFlag(args) {
		for _, name := range args {
			absolute, absErr := filepath.Abs(name)
			if absErr != nil {
				fmt.Fprintln(stderr, "ra8ci since:", absErr)
				return 2
			}
			paths = append(paths, absolute)
		}
	} else {
		fmt.Fprintln(stderr, "usage: ra8ci since FILE [FILE ...] | --all | --selftest")
		return 2
	}
	var failures []string
	for _, name := range paths {
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci since: scan cancelled:", err)
			return 2
		}
		info, statErr := os.Stat(name)
		if statErr != nil || !info.Mode().IsRegular() {
			continue
		}
		if isUnderLibInc(name) {
			failures = append(failures, checkPresence(name)...)
		}
		if sourceSuffixes[filepath.Ext(name)] {
			failures = append(failures, checkValues(name, version)...)
		}
	}
	if len(failures) != 0 {
		fmt.Fprintf(stderr, "ra8ci since: project version is %s\n", version)
		for _, failure := range failures {
			fmt.Fprintln(stderr, failure)
		}
		fmt.Fprintf(stderr, "\n%d issue(s) found.\n", len(failures))
		return 1
	}
	return 0
}

func containsFlag(args []string) bool {
	for _, arg := range args {
		if strings.HasPrefix(arg, "-") {
			return true
		}
	}
	return false
}

func readProjectVersion(root string) (string, error) {
	path := filepath.Join(root, "VERSION")
	data, err := os.ReadFile(path)
	if err != nil {
		return "", fmt.Errorf("%s: %w", path, err)
	}
	version := strings.TrimSpace(string(data))
	if !versionRE.MatchString(version) {
		return "", fmt.Errorf("%s content %q is not semver MAJOR.MINOR.PATCH", path, version)
	}
	return version, nil
}

func checkPresence(path string) []string {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	lines := splitLines(string(data))
	var problems []string
	for index, line := range lines {
		match := publicDecl.FindStringSubmatch(line)
		if match == nil {
			continue
		}
		start := index - 30
		if start < 0 {
			start = 0
		}
		if !sinceTag.MatchString(strings.Join(lines[start:index], "\n")) {
			problems = append(problems, fmt.Sprintf("%s:%d: %s missing @since", path, index+1, match[1]))
		}
	}
	return problems
}

func checkValues(path, version string) []string {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var problems []string
	for lineNo, line := range splitLines(string(data)) {
		match := sinceValue.FindStringSubmatch(line)
		if match != nil && match[1] != version {
			problems = append(problems, fmt.Sprintf("%s:%d: @since %s != project %s", path, lineNo+1, match[1], version))
		}
	}
	return problems
}

func splitLines(text string) []string {
	text = strings.ReplaceAll(text, "\r\n", "\n")
	text = strings.ReplaceAll(text, "\r", "\n")
	return strings.Split(text, "\n")
}

func isUnderLibInc(path string) bool {
	slash := filepath.ToSlash(path)
	return strings.Contains(slash, "libs/ra8_") && strings.HasSuffix(slash, ".h") && strings.Contains(slash, "/inc/")
}

func collectRepoPaths(ctx context.Context, root string) ([]string, error) {
	command := exec.CommandContext(ctx, "git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
	output, err := command.Output()
	if err != nil {
		return nil, fmt.Errorf("git ls-files failed: %w", err)
	}
	rels := bytes.Split(output, []byte{0})
	count := 0
	for _, raw := range rels {
		if len(raw) == 0 {
			continue
		}
		path := filepath.Join(root, filepath.FromSlash(string(raw)))
		info, statErr := os.Stat(path)
		if statErr == nil && info.Mode().IsRegular() {
			count++
		}
	}
	if count < trackedFloor {
		return nil, fmt.Errorf("only %d tracked path(s), floor is %d", count, trackedFloor)
	}
	paths := make([]string, 0)
	for _, raw := range rels {
		if len(raw) == 0 {
			continue
		}
		rel := filepath.ToSlash(string(raw))
		if !sourceSuffixes[filepath.Ext(rel)] || excluded(rel) {
			continue
		}
		absolute := filepath.Join(root, filepath.FromSlash(rel))
		info, statErr := os.Stat(absolute)
		if statErr != nil || !info.Mode().IsRegular() {
			continue
		}
		paths = append(paths, absolute)
	}
	sort.Strings(paths)
	return paths, nil
}

func excluded(rel string) bool {
	for _, prefix := range excludedPrefixes {
		if strings.HasPrefix(rel, prefix) {
			return true
		}
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

func selfTest(ctx context.Context, root string, stdout, stderr io.Writer) bool {
	version, err := readProjectVersion(root)
	if err != nil {
		fmt.Fprintln(stderr, err)
		return false
	}
	dir, err := os.MkdirTemp("", "ra8ci-since-selftest-")
	if err != nil {
		return false
	}
	defer os.RemoveAll(dir)
	bad := filepath.Join(dir, "bad.c")
	good := filepath.Join(dir, "good.c")
	header := filepath.Join(dir, "public.h")
	if os.WriteFile(bad, []byte("/** @since 9.9.9 */\n"), 0600) != nil ||
		os.WriteFile(good, []byte("/** @since "+version+" */\n"), 0600) != nil ||
		os.WriteFile(header, []byte("ra8_err_t ra8_public(void);\n"), 0600) != nil {
		return false
	}
	if len(checkValues(bad, version)) == 0 || len(checkValues(good, version)) != 0 || len(checkPresence(header)) == 0 {
		return false
	}
	paths, err := collectRepoPaths(ctx, root)
	if err != nil {
		fmt.Fprintln(stderr, err)
		return false
	}
	var hasTools, hasSOUP bool
	for _, path := range paths {
		rel, relErr := filepath.Rel(root, path)
		if relErr != nil {
			return false
		}
		slash := filepath.ToSlash(rel)
		if strings.HasPrefix(slash, "tools/") {
			hasTools = true
		}
		if strings.HasPrefix(slash, "libs/third_party/") || strings.HasPrefix(slash, "apps/shared_libs/third_party/") {
			hasSOUP = true
		}
	}
	if !hasTools || hasSOUP {
		fmt.Fprintf(stderr, "scope selftest: tools=%t vendored=%t\n", hasTools, hasSOUP)
		return false
	}
	fmt.Fprintln(stdout, "ra8ci since selftest passed (wrong/right values, missing API tag, derived scope).")
	return true
}
