// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package tzdiscard detects discarded errors at TrustZone boot boundaries.
package tzdiscard

import (
	"context"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"unicode/utf8"
)

const fileFloor = 1700

var (
	roots           = []string{"libs", "tests", "examples", "port", "tools", "apps"}
	extensions      = map[string]bool{".c": true, ".h": true, ".cpp": true, ".hpp": true}
	buildRoots      = map[string]bool{"docs": true, "examples": true, "local-poc": true, "port": true, "tests": true, "tools": true, "apps": true}
	toolOutputNames = map[string]bool{"CMakeFiles": true, "_deps": true, "__pycache__": true, "node_modules": true}
	familyPattern   = regexp.MustCompile(`\(\s*void\s*\)\s*(ra8_tz_secure_boot_[a-z0-9_]+)\s*\(`)
	anyRA8Pattern   = regexp.MustCompile(`\(\s*void\s*\)\s*(ra8_[a-z0-9_]+)\s*\(`)
	bootTUPattern   = regexp.MustCompile(`(?m)^\s*void\s+(?:SystemInit|ra8_trustzone_init)\s*\(\s*void\s*\)`)
	waiverPattern   = regexp.MustCompile(`TZ-DISCARD-OK:\s*\S`)
	voidCastSuffix  = regexp.MustCompile(`\(\s*void\s*\)\s*$`)
)

type finding struct {
	line    int
	rule    string
	snippet string
}

type span struct {
	start int
	end   int
}

// Run executes the self-test or checks the whole source tree / explicit file list.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci tz-boundary-discard: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		if selfTest(stdout, stderr) {
			return 0
		}
		return 1
	}
	for _, arg := range args {
		if strings.HasPrefix(arg, "-") {
			fmt.Fprintln(stderr, "usage: ra8ci tz-boundary-discard [--selftest] [file ...]")
			return 2
		}
	}
	wholeTree := len(args) == 0
	files := append([]string(nil), args...)
	var err error
	if wholeTree {
		files, err = discover(ctx, root)
		if err != nil {
			fmt.Fprintln(stderr, "ra8ci tz-boundary-discard: source discovery failed:", err)
			return 2
		}
		if len(files) < fileFloor {
			fmt.Fprintf(stderr, "ra8ci tz-boundary-discard: FATAL -- only %d first-party source file(s) in scope, floor is %d. A collapsed sweep reports a clean tree because it scanned nothing.\n", len(files), fileFloor)
			return 2
		}
	}
	sort.Strings(files)
	total := 0
	for i, file := range files {
		if i > 0 && file == files[i-1] {
			continue
		}
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci tz-boundary-discard: cancelled:", err)
			return 2
		}
		policyPath := file
		if filepath.IsAbs(file) {
			if relative, relErr := filepath.Rel(root, file); relErr == nil {
				policyPath = filepath.ToSlash(relative)
			}
		}
		if !hasAllowedExtension(policyPath) || isBuildOutput(policyPath) || isExempt(policyPath) {
			continue
		}
		findings := checkFile(file, root)
		for _, item := range findings {
			what := "boot-TU ra8_* result discarded"
			if item.rule == "A" {
				what = "world-switch result discarded"
			}
			fmt.Fprintf(stdout, "%s:%d: [rule %s] %s -- handle the ra8_err_t (halt or a documented fallback; RA8_ERROR_CHECK[_NO_ABORT]); never (void)-cast it at a TrustZone boot boundary; %s\n",
				file, item.line, item.rule, what, item.snippet)
			total++
		}
	}
	if total != 0 {
		fmt.Fprintf(stdout, "\nra8ci tz-boundary-discard: %d violation(s). A (void)-cast silences [[nodiscard]] by ISO C23 rule, so -Werror cannot catch these; check the result and fail safe instead (add TZ-DISCARD-OK: <reason> only for a justified exception).\n", total)
		return 1
	}
	fmt.Fprintln(stdout, "ra8ci tz-boundary-discard: clean -- no silent ra8_err_t discards at TrustZone boot boundaries.")
	return 0
}

func discover(ctx context.Context, root string) ([]string, error) {
	var files []string
	for _, relRoot := range roots {
		base := filepath.Join(root, filepath.FromSlash(relRoot))
		err := filepath.WalkDir(base, func(path string, entry fs.DirEntry, walkErr error) error {
			if errorsIsNotExist(walkErr) {
				return nil
			}
			if walkErr != nil {
				return walkErr
			}
			if err := ctx.Err(); err != nil {
				return err
			}
			rel, err := filepath.Rel(root, path)
			if err != nil {
				return err
			}
			rel = filepath.ToSlash(rel)
			if entry.IsDir() && rel != relRoot && (isBuildOutput(rel+"/x") || isExempt(rel+"/x")) {
				return filepath.SkipDir
			}
			if entry.IsDir() || !hasAllowedExtension(rel) || isBuildOutput(rel) || isExempt(rel) {
				return nil
			}
			info, err := os.Stat(path)
			if err != nil {
				if os.IsNotExist(err) {
					return nil
				}
				return err
			}
			if info.Mode().IsRegular() {
				files = append(files, rel)
			}
			return nil
		})
		if err != nil && !os.IsNotExist(err) {
			return nil, err
		}
	}
	sort.Strings(files)
	return files, nil
}

func errorsIsNotExist(err error) bool {
	return err != nil && os.IsNotExist(err)
}

func hasAllowedExtension(path string) bool {
	return extensions[filepath.Ext(path)]
}

func isExempt(path string) bool {
	path = "/" + strings.Trim(filepath.ToSlash(path), "/") + "/"
	return strings.Contains(path, "/third_party/") || strings.Contains(path, "/ra8_fonts/")
}

func isBuildOutput(path string) bool {
	parts := strings.Split(filepath.ToSlash(path), "/")
	for i, part := range parts[:max(0, len(parts)-1)] {
		if toolOutputNames[part] {
			return true
		}
		if isBuildDirectory(part) && (i == 0 || buildRoots[parts[0]]) {
			return true
		}
	}
	return false
}

func isBuildDirectory(name string) bool {
	return name == "build" || strings.HasPrefix(name, "build-") ||
		strings.HasPrefix(name, "build_") || strings.HasPrefix(name, "cmake-build-")
}

func checkFile(path, root string) []finding {
	textPath := path
	if !filepath.IsAbs(textPath) {
		textPath = filepath.Join(root, filepath.FromSlash(textPath))
	}
	data, err := os.ReadFile(textPath)
	if err != nil || !utf8.Valid(data) {
		return nil
	}
	text := string(data)
	if !strings.Contains(strings.ReplaceAll(text, " ", ""), "(void)") {
		return nil
	}
	bootTU := strings.HasSuffix(filepath.ToSlash(path), ".c") && bootTUPattern.MatchString(text)
	lines := strings.Split(text, "\n")
	var out []finding
	for i, raw := range lines {
		line := raw
		if voidCastSuffix.MatchString(raw) && i+1 < len(lines) {
			line += lines[i+1]
		}
		if waiverPattern.MatchString(line) {
			continue
		}
		seen := map[span]bool{}
		patterns := []struct {
			re   *regexp.Regexp
			rule string
		}{{familyPattern, "A"}}
		if bootTU {
			patterns = append(patterns, struct {
				re   *regexp.Regexp
				rule string
			}{anyRA8Pattern, "B"})
		}
		for _, candidate := range patterns {
			for _, match := range candidate.re.FindAllStringSubmatchIndex(line, -1) {
				location := span{start: match[0], end: match[1]}
				if seen[location] || commentPosition(line, location.start) {
					continue
				}
				seen[location] = true
				snippet := []rune(strings.TrimSpace(raw))
				if len(snippet) > 100 {
					snippet = snippet[:100]
				}
				out = append(out, finding{line: i + 1, rule: candidate.rule, snippet: string(snippet)})
			}
		}
	}
	return out
}

func commentPosition(line string, position int) bool {
	stripped := strings.TrimLeft(line, " \t\r\n")
	if strings.HasPrefix(stripped, "*") || strings.HasPrefix(stripped, "//") || strings.HasPrefix(stripped, "/*") {
		return true
	}
	before := line[:position]
	if strings.Contains(before, "//") {
		return true
	}
	return strings.Contains(before, "/*") && !strings.Contains(before, "*/")
}

func selfTest(stdout, stderr io.Writer) bool {
	root, err := os.MkdirTemp("", "tz-discard-selftest-")
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci tz-boundary-discard: create self-test fixture:", err)
		return false
	}
	defer os.RemoveAll(root)
	familyPath := filepath.Join(root, "ordinary.c")
	bootPath := filepath.Join(root, "boot.c")
	goodPath := filepath.Join(root, "good.c")
	fixtures := map[string]string{
		familyPath: "void f(void) { (void)ra8_tz_secure_boot_verify(); }\n",
		bootPath:   "void SystemInit(void) { (void)ra8_cgc_init(); }\n",
		goodPath: "void SystemInit(void) { if (ra8_cgc_init() != k_ra8_ok) { halt(); } }\n" +
			"void f(void) { (void)ra8_tz_secure_boot_verify(); } /* TZ-DISCARD-OK: synthetic documented fallback */\n",
	}
	for path, text := range fixtures {
		if err := os.WriteFile(path, []byte(text), 0o600); err != nil {
			fmt.Fprintln(stderr, "ra8ci tz-boundary-discard: write self-test fixture:", err)
			return false
		}
	}
	familyFindings := checkFile(familyPath, root)
	bootFindings := checkFile(bootPath, root)
	goodFindings := checkFile(goodPath, root)
	badRules := map[string]bool{}
	for _, item := range append(familyFindings, bootFindings...) {
		badRules[item.rule] = true
	}
	rulesOK := len(badRules) == 2 && badRules["A"] && badRules["B"]
	goodOK := len(goodFindings) == 0
	for _, item := range []struct {
		ok   bool
		name string
	}{{rulesOK, "world-switch and boot-translation-unit discards both fire"}, {goodOK, "handled results and exact reasoned waiver stay quiet"}} {
		if item.ok {
			fmt.Fprintf(stdout, "  [ok] %s\n", item.name)
		} else {
			fmt.Fprintf(stderr, "  [FAIL] %s\n", item.name)
		}
	}
	if !rulesOK || !goodOK {
		fmt.Fprintln(stderr, "ra8ci tz-boundary-discard --selftest: 1 failure(s)")
		return false
	}
	fmt.Fprintln(stdout, "ra8ci tz-boundary-discard --selftest: all cases pass (both directions).")
	return true
}
