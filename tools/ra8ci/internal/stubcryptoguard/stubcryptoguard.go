// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package stubcryptoguard rejects insecure placeholder crypto outside fail-closed guards.
package stubcryptoguard

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"unicode/utf8"
)

type stub struct {
	path  string
	token string
}

var stubTranslationUnits = []stub{
	{"libs/ra8_secure_app/src/secure_trng.c", "internal_xorshift64"},
	{"libs/ra8_secure_app/src/key_vault.c", "s_vault"},
	{"libs/ra8_hal/src/ra8_rsip_key_injection.c", "ki_compute_mac"},
	{"libs/ra8_hal/src/ra8_rsip_ecc.c", "k_ra8_rsip_asym_op_eddsa_sign"},
	{"libs/ra8_hal/src/ra8_rsip_cipher.c", "internal_sym_run"},
	{"libs/ra8_hal/src/ra8_rsip_rsa.c", "internal_rsa_dispatch"},
	{"libs/ra8_hal/src/ra8_rsip_asym.c", "internal_hash_pull_digest"},
	{"libs/ra8_hal/src/ra8_rsip_devsec.c", "k_ra8_rsip_off_life_state"},
}

var (
	ifDirective    = regexp.MustCompile(`^\s*#\s*if(n?def)?\b`)
	elseDirective  = regexp.MustCompile(`^\s*#\s*else\b`)
	endifDirective = regexp.MustCompile(`^\s*#\s*endif\b`)
	errorDirective = regexp.MustCompile(`^\s*#\s*error\b`)
	guardDirective = regexp.MustCompile(`^\s*#\s*if\b`)
)

// Run executes the self-test or verifies every reviewed placeholder-crypto translation unit.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci stub-crypto-guard: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		if selfTest(stdout, stderr) {
			return 0
		}
		return 1
	}
	if len(args) != 0 {
		fmt.Fprintln(stderr, "usage: ra8ci stub-crypto-guard [--selftest]")
		return 2
	}
	var problems []string
	for _, item := range stubTranslationUnits {
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci stub-crypto-guard: cancelled:", err)
			return 2
		}
		problems = append(problems, checkFile(item.path, item.token, root)...)
	}
	if len(problems) != 0 {
		fmt.Fprintln(stdout, "ra8ci stub-crypto-guard: insecure placeholder crypto not guarded fail-closed:")
		for _, problem := range problems {
			fmt.Fprintln(stdout, "  "+problem)
		}
		fmt.Fprintln(stdout, "Fix each at the root -- wrap the insecure body in")
		fmt.Fprintln(stdout, "  #if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)")
		fmt.Fprintln(stdout, "and make the #else fail closed (return k_ra8_err_* / #error), or replace")
		fmt.Fprintln(stdout, "the placeholder with a real crypto backend.")
		return 1
	}
	fmt.Fprintf(stdout, "ra8ci stub-crypto-guard: PASS -- %d stub crypto TU(s) guarded fail-closed.\n", len(stubTranslationUnits))
	return 0
}

func checkFile(rel, token, root string) []string {
	path := filepath.Join(root, filepath.FromSlash(rel))
	data, err := os.ReadFile(path)
	if err != nil {
		return []string{fmt.Sprintf("%s: file not found (expected an insecure stub TU here)", rel)}
	}
	if !utf8.Valid(data) {
		return []string{fmt.Sprintf("%s: cannot decode UTF-8", rel)}
	}
	lines := strings.Split(string(data), "\n")
	ifIndex, elseIndex, endIndex, ok := guardRegion(lines)
	if !ok {
		return []string{fmt.Sprintf("%s: missing the stub-crypto guard '#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)' with a matching #else / #endif", rel)}
	}
	var problems []string
	failClosed := false
	for _, line := range lines[elseIndex+1 : endIndex] {
		if errorDirective.MatchString(line) || strings.Contains(line, "k_ra8_err_") {
			failClosed = true
			break
		}
	}
	if !failClosed {
		problems = append(problems, fmt.Sprintf("%s: the #else branch is not fail-closed (needs a #error or a k_ra8_err_* hard return, not k_ra8_ok)", rel))
	}
	var inside, escaped []int
	for i, line := range lines {
		if !strings.Contains(line, token) {
			continue
		}
		if i > ifIndex && i < elseIndex {
			inside = append(inside, i)
		} else {
			escaped = append(escaped, i)
		}
	}
	if len(inside) == 0 {
		problems = append(problems, fmt.Sprintf("%s: insecure signature '%s' not found inside the guarded #if region (is the insecure body still present and guarded?)", rel, token))
	}
	if len(escaped) != 0 {
		var locations []string
		for _, line := range escaped {
			locations = append(locations, fmt.Sprintf("line %d", line+1))
		}
		problems = append(problems, fmt.Sprintf("%s: insecure signature '%s' appears OUTSIDE the guard (%s) -- the insecure body must be fully inside the #if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET) block", rel, token, strings.Join(locations, ", ")))
	}
	return problems
}

func guardRegion(lines []string) (int, int, int, bool) {
	ifIndex := -1
	for i, line := range lines {
		if guardDirective.MatchString(line) && strings.Contains(line, "RA8_INSECURE_STUB_CRYPTO") &&
			strings.Contains(line, "RA8_OFF_TARGET") {
			ifIndex = i
			break
		}
	}
	if ifIndex < 0 {
		return 0, 0, 0, false
	}
	depth, elseIndex := 1, -1
	for i := ifIndex + 1; i < len(lines); i++ {
		switch {
		case ifDirective.MatchString(lines[i]):
			depth++
		case endifDirective.MatchString(lines[i]):
			depth--
			if depth == 0 {
				return ifIndex, elseIndex, i, elseIndex >= 0
			}
		case elseDirective.MatchString(lines[i]) && depth == 1:
			elseIndex = i
		}
	}
	return 0, 0, 0, false
}

func selfTest(stdout, stderr io.Writer) bool {
	const path = "libs/fixture/stub.c"
	const token = "insecure_fixture_signature"
	root, err := os.MkdirTemp("", "stub-crypto-selftest-")
	if err != nil {
		fmt.Fprintln(stderr, "ra8ci stub-crypto-guard: create self-test fixture:", err)
		return false
	}
	defer os.RemoveAll(root)
	file := filepath.Join(root, filepath.FromSlash(path))
	if err := os.MkdirAll(filepath.Dir(file), 0o700); err != nil {
		fmt.Fprintln(stderr, "ra8ci stub-crypto-guard: create self-test fixture:", err)
		return false
	}
	good := "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\nstatic int " + token + ";\n#else\nreturn k_ra8_err_unsupported;\n#endif\n"
	if err := os.WriteFile(file, []byte(good), 0o600); err != nil {
		fmt.Fprintln(stderr, "ra8ci stub-crypto-guard: write self-test fixture:", err)
		return false
	}
	goodProblems := checkFile(path, token, root)
	bad := "#if defined(RA8_INSECURE_STUB_CRYPTO) || defined(RA8_OFF_TARGET)\nstatic int placeholder;\n#else\nreturn k_ra8_ok;\n#endif\nstatic int " + token + ";\n"
	if err := os.WriteFile(file, []byte(bad), 0o600); err != nil {
		fmt.Fprintln(stderr, "ra8ci stub-crypto-guard: write self-test fixture:", err)
		return false
	}
	badProblems := checkFile(path, token, root)
	goodOK := len(goodProblems) == 0
	badOK := len(badProblems) >= 2 && strings.Contains(strings.Join(badProblems, "\n"), "not fail-closed") &&
		strings.Contains(strings.Join(badProblems, "\n"), "OUTSIDE")
	for _, test := range []struct {
		ok   bool
		name string
	}{{goodOK, "guarded token plus hard-error branch stays quiet"}, {badOK, "non-failing else and escaped insecure token both fire"}} {
		if test.ok {
			fmt.Fprintf(stdout, "  [ok] %s\n", test.name)
		} else {
			fmt.Fprintf(stderr, "  [FAIL] %s\n", test.name)
		}
	}
	if !goodOK || !badOK {
		fmt.Fprintln(stderr, "ra8ci stub-crypto-guard --selftest: 1 failure(s)")
		return false
	}
	fmt.Fprintln(stdout, "ra8ci stub-crypto-guard --selftest: all cases pass (both directions).")
	return true
}
