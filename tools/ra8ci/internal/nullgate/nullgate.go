// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package nullgate enforces nullptr-only spelling in first-party C sources.
package nullgate

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"unicode/utf8"
)

var nullToken = regexp.MustCompile(`\bNULL\b`)

var (
	soupPrefixes   = []string{"libs/third_party/", "apps/shared_libs/third_party/", "libs/ra8_fonts/", "tools/vela/generated/", "port/threadx/"}
	generatedPaths = map[string]bool{"libs/ra8_c6link/src/ra8_media_download.pb-c.c": true, "libs/ra8_c6link/inc/ra8_media_download.pb-c.h": true}
	allowedTokens  = []string{"UX_NULL", "TX_NULL", "FX_NULL", "NX_NULL"}
	extensions     = []string{".c", ".h", ".cpp", ".hpp"}
	toolOutputDirs = map[string]bool{"CMakeFiles": true, "_deps": true, "__pycache__": true, "node_modules": true}
	buildTreeRoots = map[string]bool{"docs": true, "examples": true, "local-poc": true, "port": true, "tests": true, "tools": true, "apps": true}
)

// Run checks explicit paths, --all, or --selftest.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci null: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		return selfTest(stdout, stderr)
	}
	var candidates []string
	if len(args) == 1 && args[0] == "--all" {
		var err error
		candidates, err = allFiles(ctx, root)
		if err != nil {
			fmt.Fprintln(stderr, "ra8ci null: discovery failed:", err)
			return 2
		}
	} else if len(args) == 0 {
		fmt.Fprintln(stderr, "usage: check_no_null.py [-h] [--all] [--selftest] [files ...]")
		return 2
	} else {
		for _, arg := range args {
			if strings.HasPrefix(arg, "-") {
				fmt.Fprintln(stderr, "ra8ci null: unknown option:", arg)
				return 2
			}
			if needsCheck(root, arg) {
				candidates = append(candidates, arg)
			}
		}
	}
	total := 0
	for _, name := range candidates {
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci null: cancelled:", err)
			return 2
		}
		filePath := name
		if !filepath.IsAbs(filePath) {
			filePath = filepath.Join(root, filepath.FromSlash(filePath))
		}
		for _, item := range findViolations(filePath) {
			fmt.Fprintf(stderr, "%s:%d: bare NULL -- use nullptr (C23): %s\n", name, item.line, item.snippet)
			total++
		}
	}
	if total != 0 {
		fmt.Fprintf(stderr, "\n%d bare NULL token(s) found. Replace with `nullptr` (C23 builtin). Allowed: UX_NULL / TX_NULL / FX_NULL / NX_NULL vendor macros, comments, string literals.\n", total)
		return 1
	}
	fmt.Fprintln(stdout, "check_no_null.py: 0 findings.")
	return 0
}

func selfTest(out, errOut io.Writer) int {
	fmt.Fprintln(out, "check_no_null.py --selftest")
	failures := 0
	expect := func(ok bool, name string) {
		label := "ok"
		if !ok {
			label = "FAIL"
			failures++
		}
		fmt.Fprintf(out, "  [%s] %s\n", label, name)
	}
	dir, err := os.MkdirTemp("", "ra8ci-null-selftest-")
	if err != nil {
		fmt.Fprintln(errOut, "nullgate selftest: cannot create fixture:", err)
		return 1
	}
	defer os.RemoveAll(dir)
	bad, good := filepath.Join(dir, "bad.c"), filepath.Join(dir, "good.c")
	if err := os.WriteFile(bad, []byte("int f(void) { char *p = NULL; return p == NULL; }\n"), 0o600); err != nil {
		fmt.Fprintln(errOut, "nullgate selftest: cannot write fixture:", err)
		return 1
	}
	goodText := "int f(void) { char *p = nullptr;   // NULL in a comment is fine\n" +
		"  const char *s = \"NULL literal\";  // and in a string literal\n" +
		"  return (p == nullptr) && (UX_NULL == p); }\n"
	if err := os.WriteFile(good, []byte(goodText), 0o600); err != nil {
		fmt.Fprintln(errOut, "nullgate selftest: cannot write fixture:", err)
		return 1
	}
	expect(len(findViolations(bad)) > 0, "bare NULL in code fires")
	expect(len(findViolations(good)) == 0, "nullptr / vendor macro / comment / string stays quiet")
	expect(inScope("tools/mkbookimg/src/mkbookimg.c"), "tools/ is in scope (ROOT_DIRS omitted it before #358)")
	expect(!inScope("tests/test_x.c"), "tests/ exempt (deliberate NULL stimulus)")
	generated := "libs/ra8_c6link/src/ra8_media_download.pb-c.c"
	expect(generatedPaths[generated] && !inScope(generated), "registered generated source is exempt")
	expect(inScope("libs/ra8_c6link/src/future_generated.pb-c.c"), "generated-looking future source is not automatically exempt")
	expect(!inScope("libs/third_party/threadx/src/tx.c"), "platform SOUP exempt")
	expect(!inScope("apps/shared_libs/third_party/miniz/miniz.c"), "app SOUP exempt")
	expect(inScope("apps/shared_libs/compress/src/compress.c"), "adjacent app first-party code remains in scope")
	if failures > 0 {
		fmt.Fprintf(errOut, "\nSELFTEST FAILED: %d assertion(s)\n", failures)
		return 1
	}
	fmt.Fprintln(out, "selftest: all assertions held (both directions).")
	return 0
}

type violation struct {
	line    int
	snippet string
}

func findViolations(name string) []violation {
	data, err := os.ReadFile(name)
	if err != nil {
		return nil
	}
	lines := strings.Split(decodeReplace(data), "\n")
	var found []violation
	inBlock := false
	for i, raw := range lines {
		cur := raw
		if inBlock {
			end := strings.Index(cur, "*/")
			if end < 0 {
				continue
			}
			cur = cur[end+2:]
			inBlock = false
		}
		bo := strings.Index(cur, "/*")
		if bo >= 0 && strings.Index(cur[bo+2:], "*/") < 0 {
			cur = cur[:bo]
			inBlock = true
		}
		code := stripNoncode(cur)
		if !strings.Contains(code, "NULL") {
			continue
		}
		for _, token := range allowedTokens {
			code = strings.ReplaceAll(code, token, "_OK_")
		}
		if nullToken.MatchString(code) {
			found = append(found, violation{i + 1, strings.TrimSpace(raw)})
		}
	}
	return found
}

func stripNoncode(line string) string {
	var out strings.Builder
	inString, inChar, inBlock := false, false, false
	for i := 0; i < len(line); {
		c := line[i]
		var next byte
		if i+1 < len(line) {
			next = line[i+1]
		}
		if inBlock {
			if c == '*' && next == '/' {
				inBlock = false
				i += 2
				continue
			}
			i++
			continue
		}
		if inString || inChar {
			if c == '\\' && i+1 < len(line) {
				i += 2
				continue
			}
			if inString && c == '"' {
				inString = false
			} else if inChar && c == '\'' {
				inChar = false
			}
			i++
			continue
		}
		if c == '/' && next == '/' {
			break
		}
		if c == '/' && next == '*' {
			inBlock = true
			i += 2
			continue
		}
		if c == '"' {
			inString = true
			i++
			continue
		}
		if c == '\'' {
			inChar = true
			i++
			continue
		}
		out.WriteByte(c)
		i++
	}
	return out.String()
}

func allFiles(ctx context.Context, root string) ([]string, error) {
	cmd := exec.CommandContext(ctx, "git", "ls-files", "-z", "--cached", "--others", "--exclude-standard")
	cmd.Dir = root
	data, err := cmd.Output()
	if err != nil {
		return nil, err
	}
	paths := strings.Split(string(data), "\x00")
	tracked := make([]string, 0, len(paths))
	for _, rel := range paths {
		if rel == "" {
			continue
		}
		info, err := os.Stat(filepath.Join(root, filepath.FromSlash(rel)))
		if err == nil && info.Mode().IsRegular() {
			tracked = append(tracked, rel)
		}
	}
	if len(tracked) < 1000 {
		return nil, fmt.Errorf("lint_targets.py: FATAL -- only %d tracked path(s), floor is 1000. A collapsed enumeration reports a clean tree because it enumerated nothing.", len(tracked))
	}
	sort.Strings(tracked)
	out := make([]string, 0)
	for _, rel := range tracked {
		if inScope(rel) {
			out = append(out, filepath.Join(root, filepath.FromSlash(rel)))
		}
	}
	return out, nil
}

func needsCheck(root, name string) bool {
	if !hasExtension(strings.ToLower(filepath.Ext(name))) {
		return false
	}
	full := name
	if !filepath.IsAbs(full) {
		full = filepath.Join(root, full)
	}
	resolved, err := filepath.EvalSymlinks(full)
	if err != nil {
		resolved, err = filepath.Abs(full)
	}
	if err != nil {
		return true
	}
	rel, err := filepath.Rel(root, resolved)
	if err != nil || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return true
	}
	return inScope(filepath.ToSlash(rel))
}

func inScope(rel string) bool {
	if !hasExtension(path.Ext(rel)) {
		return false
	}
	if strings.Contains(rel, "/tests/") || strings.HasPrefix(rel, "tests/") || generatedPaths[rel] {
		return false
	}
	for _, prefix := range soupPrefixes {
		if strings.HasPrefix(rel, prefix) {
			return false
		}
	}
	parts := strings.Split(rel, "/")
	for i, part := range parts[:len(parts)-1] {
		if toolOutputDirs[part] || (isBuildDir(part) && (i == 0 || buildTreeRoots[parts[0]])) {
			return false
		}
	}
	return true
}

func hasExtension(ext string) bool {
	for _, candidate := range extensions {
		if ext == candidate {
			return true
		}
	}
	return false
}
func isBuildDir(name string) bool {
	return name == "build" || strings.HasPrefix(name, "build-") || strings.HasPrefix(name, "build_") || strings.HasPrefix(name, "cmake-build-")
}

func decodeReplace(data []byte) string {
	var out strings.Builder
	out.Grow(len(data))
	for len(data) > 0 {
		r, size := utf8.DecodeRune(data)
		if r != utf8.RuneError || size != 1 {
			out.Write(data[:size])
			data = data[size:]
			continue
		}
		invalid := invalidUTF8Prefix(data)
		out.WriteRune(utf8.RuneError)
		data = data[invalid:]
	}
	return out.String()
}

// invalidUTF8Prefix returns the number of bytes Python's UTF-8
// errors="replace" decoder consumes for one malformed subsequence.
func invalidUTF8Prefix(data []byte) int {
	lead := data[0]
	expected := 0
	switch {
	case lead >= 0xc2 && lead <= 0xdf:
		expected = 2
	case lead >= 0xe0 && lead <= 0xef:
		expected = 3
	case lead >= 0xf0 && lead <= 0xf4:
		expected = 4
	default:
		return 1
	}
	if len(data) == 1 {
		return 1
	}
	second := data[1]
	validSecond := second >= 0x80 && second <= 0xbf
	if lead == 0xe0 {
		validSecond = second >= 0xa0 && second <= 0xbf
	} else if lead == 0xed {
		validSecond = second >= 0x80 && second <= 0x9f
	} else if lead == 0xf0 {
		validSecond = second >= 0x90 && second <= 0xbf
	} else if lead == 0xf4 {
		validSecond = second >= 0x80 && second <= 0x8f
	}
	if !validSecond {
		return 1
	}
	consumed := 2
	for consumed < expected && consumed < len(data) {
		if data[consumed] < 0x80 || data[consumed] > 0xbf {
			return consumed
		}
		consumed++
	}
	return consumed
}
