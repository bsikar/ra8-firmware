// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package asciigate implements the repository's ASCII scan and transliteration.
package asciigate

import (
	"bytes"
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"unicode/utf8"
)

const fileFloor = 2500

var textExtensions = map[string]bool{
	".c": true, ".h": true, ".cpp": true, ".hpp": true, ".dox": true,
	".md": true, ".yml": true, ".yaml": true, ".sh": true, ".py": true,
	".cmake": true, ".json": true, ".toml": true, ".cfg": true, ".conf": true,
	".tex": true, ".txt": true, ".ini": true, ".ld": true, ".s": true, ".m": true,
}

var replacements = strings.NewReplacer(
	"—", "--", "–", "-", "‘", "'", "’", "'", "“", "\"", "”", "\"",
	"…", "...", " ", " ", "°", " deg", "±", "+/-", "µ", "u", "μ", "u",
	"≤", "<=", "≥", ">=", "≠", "!=", "→", "->", "←", "<-", "×", "x", "÷", "/",
)

type options struct {
	check  bool
	all    bool
	self   bool
	target string
}

// Run scans or rewrites files. Exit 0 means clean/success, 1 means findings,
// and 2 means the requested scan could not be trusted.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		if stderr == nil {
			return 2
		}
		fmt.Fprintln(stderr, "ra8ci ascii: invalid input")
		return 2
	}
	opts, err := parseOptions(args)
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci ascii:", err)
		return 2
	}
	if opts.self {
		if selfTest(ctx, root, stdout, stderr) {
			fmt.Fprintln(stdout, "ra8ci ascii selftest passed (both directions, scope and rewrite).")
			return 0
		}
		fmt.Fprintln(stderr, "ra8ci ascii selftest FAILED")
		return 2
	}
	var targets []string
	if opts.all {
		targets, err = derivedTargets(ctx, root)
	} else {
		targets, err = walkTargets(opts.target)
	}
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci ascii: FATAL --", err)
		return 2
	}
	if opts.all && len(targets) < fileFloor {
		fmt.Fprintf(stderr, "ra8ci ascii: FATAL -- only %d file(s) in scope, floor is %d; refusing a vacuous scan\n", len(targets), fileFloor)
		return 2
	}
	changed := 0
	for _, target := range targets {
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci ascii: scan cancelled:", err)
			return 2
		}
		path := target
		if opts.all {
			path = filepath.Join(root, filepath.FromSlash(target))
		}
		count, err := process(path, !opts.check)
		if err != nil {
			fmt.Fprintln(stderr, "ra8ci ascii: FATAL --", err)
			return 2
		}
		changed += count
		if count > 0 {
			if opts.check {
				fmt.Fprintf(stdout, "[NEEDS-FIX] %s: %d non-ASCII characters\n", target, count)
			} else {
				fmt.Fprintf(stdout, "[FIXED] %s: %d replacements\n", target, count)
			}
		}
	}
	if opts.all {
		fmt.Fprintf(stdout, "ra8ci ascii: %d non-ASCII character(s) across %d file(s)\n", changed, len(targets))
	}
	if opts.check && changed > 0 {
		return 1
	}
	return 0
}

func parseOptions(args []string) (options, error) {
	flags := flag.NewFlagSet("ascii", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	check := flags.Bool("check", false, "report without changing files")
	all := flags.Bool("all", false, "scan the derived first-party file set")
	self := flags.Bool("selftest", false, "prove detection, rewrite and scope behavior")
	if err := flags.Parse(args); err != nil {
		return options{}, err
	}
	if *self {
		if *check || *all || flags.NArg() != 0 {
			return options{}, errors.New("--selftest cannot be combined with other modes")
		}
		return options{self: true}, nil
	}
	if *all == (flags.NArg() != 0) || flags.NArg() > 1 {
		return options{}, errors.New("pass exactly one of --all or a target path")
	}
	opts := options{check: *check, all: *all}
	if !*all {
		opts.target = flags.Arg(0)
	}
	return opts, nil
}

func process(name string, rewrite bool) (int, error) {
	raw, err := os.ReadFile(name)
	if err != nil {
		return 0, fmt.Errorf("%s: %w", name, err)
	}
	if !utf8.Valid(raw) {
		return 0, fmt.Errorf("%s: invalid UTF-8; refusing a trusted ASCII scan", name)
	}
	text := normalizeNewlines(string(raw))
	clean, count := transliterate(text)
	if count == 0 || !rewrite {
		return count, nil
	}
	info, err := os.Stat(name)
	if err != nil || !info.Mode().IsRegular() {
		return 0, fmt.Errorf("%s: target is not a regular file", name)
	}
	if err := os.WriteFile(name, []byte(clean), info.Mode().Perm()); err != nil {
		return 0, fmt.Errorf("%s: rewrite: %w", name, err)
	}
	return count, nil
}

func normalizeNewlines(text string) string {
	text = strings.ReplaceAll(text, "\r\n", "\n")
	return strings.ReplaceAll(text, "\r", "\n")
}

func transliterate(text string) (string, int) {
	count := 0
	for _, character := range text {
		if character > 0x7f {
			count++
		}
	}
	clean := replacements.Replace(text)
	var result strings.Builder
	result.Grow(len(clean))
	for _, character := range clean {
		if character <= 0x7f {
			result.WriteRune(character)
		} else {
			result.WriteByte('?')
		}
	}
	return result.String(), count
}

func walkTargets(target string) ([]string, error) {
	if target == "" {
		return nil, errors.New("empty target")
	}
	info, err := os.Stat(target)
	if err != nil {
		return nil, fmt.Errorf("'%s' does not exist or cannot be read: %w", target, err)
	}
	if info.Mode().IsRegular() {
		return []string{target}, nil
	}
	if !info.IsDir() {
		return nil, fmt.Errorf("'%s' is not a regular file or directory", target)
	}
	var targets []string
	err = filepath.WalkDir(target, func(path string, entry os.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() {
			if path != target && excludedWalkPart(entry.Name()) {
				return filepath.SkipDir
			}
			return nil
		}
		if entry.Type()&os.ModeSymlink != 0 || !entry.Type().IsRegular() || !textExtensions[strings.ToLower(filepath.Ext(path))] || hasExcludedWalkPart(path) {
			return nil
		}
		targets = append(targets, path)
		return nil
	})
	return targets, err
}

func excludedWalkPart(part string) bool {
	switch part {
	case "third_party", "_deps", "build", "build-cov", "doxygen_theme", "fixtures":
		return true
	default:
		return false
	}
}

func hasExcludedWalkPart(path string) bool {
	for _, part := range strings.Split(filepath.ToSlash(path), "/") {
		if excludedWalkPart(part) {
			return true
		}
	}
	return false
}

func derivedTargets(ctx context.Context, root string) ([]string, error) {
	command := exec.CommandContext(ctx, "git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
	output, err := command.Output()
	if err != nil {
		return nil, fmt.Errorf("git ls-files failed: %w", err)
	}
	tracked := bytes.Split(output, []byte{0})
	suffixTargets := make([]string, 0, len(tracked))
	extensionlessTargets := make([]string, 0)
	for _, item := range tracked {
		if len(item) == 0 {
			continue
		}
		relative := filepath.ToSlash(string(item))
		absolute := filepath.Join(root, filepath.FromSlash(relative))
		info, statErr := os.Lstat(absolute)
		if statErr != nil || !info.Mode().IsRegular() || isExcluded(relative) {
			continue
		}
		ext := strings.ToLower(filepath.Ext(relative))
		if textExtensions[ext] {
			suffixTargets = append(suffixTargets, relative)
			continue
		}
		if ext == "" && hasShellOrPythonShebang(absolute) {
			extensionlessTargets = append(extensionlessTargets, relative)
		}
	}
	combined := append(suffixTargets, extensionlessTargets...)
	sortStrings(combined)
	return combined, nil
}

func hasShellOrPythonShebang(path string) bool {
	file, err := os.Open(path)
	if err != nil {
		return false
	}
	defer file.Close()
	var first [200]byte
	n, _ := file.Read(first[:])
	line := string(first[:n])
	if !strings.HasPrefix(line, "#!") {
		return false
	}
	line = strings.ReplaceAll(line[2:], "/usr/bin/env", " ")
	line = strings.ReplaceAll(line, "/", " ")
	for _, word := range strings.Fields(line) {
		base := strings.SplitN(word, "-", 2)[0]
		switch base {
		case "sh", "bash", "zsh", "dash", "python", "python3":
			return true
		}
	}
	return false
}

func isExcluded(relative string) bool {
	for _, prefix := range []string{"libs/third_party/", "apps/shared_libs/third_party/", "libs/ra8_fonts/", "tools/vela/generated/"} {
		if strings.HasPrefix(relative, prefix) {
			return true
		}
	}
	parts := strings.Split(relative, "/")
	for index, part := range parts[:len(parts)-1] {
		build := part == "build" || strings.HasPrefix(part, "build-") || strings.HasPrefix(part, "build_") || strings.HasPrefix(part, "cmake-build-")
		if part == "CMakeFiles" || part == "_deps" || part == "__pycache__" || part == "node_modules" ||
			(build && (index == 0 || isBuildTreeRoot(parts[0]))) {
			return true
		}
	}
	return false
}

func isBuildTreeRoot(root string) bool {
	switch root {
	case "docs", "examples", "local-poc", "port", "tests", "tools", "apps":
		return true
	default:
		return false
	}
}

func sortStrings(values []string) {
	for i := 1; i < len(values); i++ {
		for j := i; j > 0 && values[j] < values[j-1]; j-- {
			values[j], values[j-1] = values[j-1], values[j]
		}
	}
}

func selfTest(ctx context.Context, root string, stdout, stderr io.Writer) bool {
	clean, findings := transliterate("ASCII -- clean\n")
	if clean != "ASCII -- clean\n" || findings != 0 {
		return false
	}
	translated, findings := transliterate("an em—dash and µ\r\n")
	if translated != "an em--dash and u\r\n" || findings != 2 {
		return false
	}
	unknown, findings := transliterate("snowman ☃")
	if unknown != "snowman ?" || findings != 1 {
		return false
	}
	directory, err := os.MkdirTemp("", "ra8ci-ascii-selftest-")
	if err != nil {
		return false
	}
	defer os.RemoveAll(directory)
	fixture := filepath.Join(directory, "fixture.md")
	if err := os.WriteFile(fixture, []byte("dash—\n"), 0600); err != nil {
		return false
	}
	count, err := process(fixture, false)
	if err != nil || count != 1 {
		return false
	}
	contents, err := os.ReadFile(fixture)
	if err != nil || string(contents) != "dash—\n" {
		return false
	}
	count, err = process(fixture, true)
	if err != nil || count != 1 {
		return false
	}
	contents, err = os.ReadFile(fixture)
	if err != nil || string(contents) != "dash--\n" {
		return false
	}
	invalid := filepath.Join(directory, "invalid.md")
	if err := os.WriteFile(invalid, []byte{0xff}, 0600); err != nil {
		return false
	}
	if _, err := process(invalid, false); err == nil {
		return false
	}
	if _, err := walkTargets(filepath.Join(directory, "missing")); err == nil {
		return false
	}
	targets, err := derivedTargets(ctx, root)
	if err != nil || len(targets) < fileFloor {
		fmt.Fprintf(stderr, "live derived scope has %d files, floor is %d\n", len(targets), fileFloor)
		return false
	}
	extensionlessCount := 0
	foundCommitHook := false
	for _, target := range targets {
		if filepath.Ext(target) == "" && hasShellOrPythonShebang(filepath.Join(root, target)) {
			extensionlessCount++
		}
		if filepath.ToSlash(target) == "scripts/git/commit-msg" {
			foundCommitHook = true
		}
	}
	if extensionlessCount < 7 || !foundCommitHook {
		fmt.Fprintf(stderr, "extensionless derived scope has %d entries; commit-msg included=%t\n", extensionlessCount, foundCommitHook)
		return false
	}
	return true
}
