// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package nscveneers verifies that each public NSC veneer declaration has a definition.
package nscveneers

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
)

var declaration = regexp.MustCompile(`RA8_NSC_VENEER\s+\w[\w\s\*]*?\b(ra8_nsc_\w+)\s*\(`)

// Run executes the self-test or scans the public header against NSC source definitions.
func Run(ctx context.Context, root string, args []string, stdout, stderr io.Writer) int {
	if ctx == nil || root == "" || stdout == nil || stderr == nil {
		fmt.Fprintln(stderr, "ra8ci nsc-veneer-defs: invalid input")
		return 2
	}
	if len(args) == 1 && args[0] == "--selftest" {
		if selfTest(stdout, stderr) {
			return 0
		}
		return 1
	}
	if len(args) != 0 {
		fmt.Fprintln(stderr, "usage: ra8ci nsc-veneer-defs [--selftest]")
		return 2
	}
	headerRel := filepath.Join("libs", "ra8_nsc", "inc", "ra8_nsc.h")
	sourceRel := filepath.Join("libs", "ra8_nsc", "src")
	data, err := os.ReadFile(filepath.Join(root, headerRel))
	if err != nil {
		fmt.Fprintf(stderr, "ra8ci nsc-veneer-defs: header not found: %s\n", filepath.ToSlash(headerRel))
		return 1
	}
	entries, err := os.ReadDir(filepath.Join(root, sourceRel))
	if err != nil {
		fmt.Fprintf(stderr, "ra8ci nsc-veneer-defs: cannot read source directory: %v\n", err)
		return 1
	}
	var sources [][]byte
	for _, entry := range entries {
		if entry.IsDir() || filepath.Ext(entry.Name()) != ".c" {
			continue
		}
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci nsc-veneer-defs: cancelled:", err)
			return 2
		}
		source, err := os.ReadFile(filepath.Join(root, sourceRel, entry.Name()))
		if err != nil {
			fmt.Fprintf(stderr, "ra8ci nsc-veneer-defs: cannot read source %s: %v\n", entry.Name(), err)
			return 1
		}
		sources = append(sources, source)
	}
	veneers := declared(string(data))
	var missing []string
	for _, name := range veneers {
		if err := ctx.Err(); err != nil {
			fmt.Fprintln(stderr, "ra8ci nsc-veneer-defs: cancelled:", err)
			return 2
		}
		if !isDefined(name, sources) {
			missing = append(missing, name)
		}
	}
	if len(missing) != 0 {
		fmt.Fprintln(stdout, "ra8ci nsc-veneer-defs: RA8_NSC_VENEER declared without a definition:")
		for _, name := range missing {
			fmt.Fprintf(stdout, "  %s: declared in %s, no definition in %s/*.c\n", name,
				filepath.ToSlash(headerRel), filepath.ToSlash(sourceRel))
		}
		fmt.Fprintln(stdout, "Fix each at the root -- implement the veneer, or delete the declaration.")
		fmt.Fprintln(stdout, "A phantom NS->S entry point in the public header is a trust hazard.")
		return 1
	}
	fmt.Fprintf(stdout, "ra8ci nsc-veneer-defs: PASS -- all %d RA8_NSC_VENEER declaration(s) defined.\n", len(veneers))
	return 0
}

func declared(header string) []string {
	seen := make(map[string]bool)
	var names []string
	for _, match := range declaration.FindAllStringSubmatch(header, -1) {
		if !seen[match[1]] {
			seen[match[1]] = true
			names = append(names, match[1])
		}
	}
	return names
}

func isDefined(name string, sources [][]byte) bool {
	pattern := regexp.MustCompile(`RA8_NSC_VENEER\s+\w[\w\s\*]*?\b` + regexp.QuoteMeta(name) + `\s*\(`)
	for _, source := range sources {
		if pattern.Match(source) {
			return true
		}
	}
	return false
}

func selfTest(stdout, stderr io.Writer) bool {
	header := "RA8_NSC_VENEER ra8_err_t ra8_nsc_defined(void);\nRA8_NSC_VENEER void ra8_nsc_phantom(void);\n"
	source := "RA8_NSC_VENEER ra8_err_t ra8_nsc_defined(void) { return 0; }\nvoid caller(void) { ra8_nsc_phantom(); }\n"
	names := declared(header)
	sources := [][]byte{[]byte(source)}
	definedOK := len(names) == 2 && names[0] == "ra8_nsc_defined" && names[1] == "ra8_nsc_phantom" &&
		isDefined("ra8_nsc_defined", sources) && !isDefined("ra8_nsc_phantom", sources)
	missingOK := len(names) == 2 && !isDefined(names[1], sources)
	for _, test := range []struct {
		ok   bool
		name string
	}{{definedOK, "matching veneer definition stays quiet"}, {missingOK, "call-only phantom veneer fires"}} {
		if test.ok {
			fmt.Fprintf(stdout, "  [ok] %s\n", test.name)
		} else {
			fmt.Fprintf(stderr, "  [FAIL] %s\n", test.name)
		}
	}
	if !definedOK || !missingOK {
		fmt.Fprintln(stderr, "ra8ci nsc-veneer-defs --selftest: 1 failure(s)")
		return false
	}
	fmt.Fprintln(stdout, "ra8ci nsc-veneer-defs --selftest: all cases pass (both directions).")
	return true
}
