// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nscveneers

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A gate that cannot read the boundary must say so rather than report it
// clean: a checkout it could not inspect is not a checkout with no phantom
// veneers in it. These hold the invocations the gate refuses and the reads
// it gives up on. tree() and scan() come from
// headers_the_boundary_publishes_test.go.

// ran runs the gate over root with args and keeps the two streams apart,
// because a finding belongs on stdout and a failure to look belongs on
// stderr.
func ran(t *testing.T, root string, args []string) (int, string, string) {
	t.Helper()
	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), root, args, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

func TestRunRefusesAnInvocationItDoesNotOffer(t *testing.T) {
	root := tree(t,
		map[string]string{"ra8_nsc.h": "RA8_NSC_VENEER void ra8_nsc_open(void);\n"},
		map[string]string{"open.c": "RA8_NSC_VENEER void ra8_nsc_open(void) { }\n"})

	for _, args := range [][]string{
		{"--selftest", "--selftest"},
		{"--help"},
		{"libs/ra8_nsc/inc/ra8_nsc.h"},
		{""},
	} {
		code, stdout, stderr := ran(t, root, args)
		if code != 2 || !strings.Contains(stderr, "usage: ra8ci nsc-veneer-defs") {
			t.Fatalf("args %q = %d, stderr %q", args, code, stderr)
		}
		if stdout != "" {
			t.Fatalf("args %q printed a verdict it had not reached: %q", args, stdout)
		}
	}
}

// A caller that gave up is told so, and the gate never prints a PASS it
// did not finish earning.
func TestRunStopsWhenTheCallerGivesUp(t *testing.T) {
	root := tree(t,
		map[string]string{"ra8_nsc.h": "RA8_NSC_VENEER void ra8_nsc_open(void);\n"},
		map[string]string{"open.c": "RA8_NSC_VENEER void ra8_nsc_open(void) { }\n"})

	stopped, cancel := context.WithCancel(context.Background())
	cancel()
	var stdout, stderr bytes.Buffer
	if code := Run(stopped, root, nil, &stdout, &stderr); code != 2 {
		t.Fatalf("a cancelled scan = %d", code)
	}
	if !strings.Contains(stderr.String(), "cancelled") {
		t.Fatalf("a cancelled scan did not say so: %q", stderr.String())
	}
	if strings.Contains(stdout.String(), "PASS") {
		t.Fatal("a cancelled scan still passed the boundary")
	}
}

// The source directory is half the evidence. Without it the gate cannot
// tell a defined veneer from a phantom one, so it fails rather than
// reporting every declaration missing.
func TestRunFailsWhenTheSourceDirectoryCannotBeRead(t *testing.T) {
	root := tree(t,
		map[string]string{"ra8_nsc.h": "RA8_NSC_VENEER void ra8_nsc_open(void);\n"},
		nil)
	if err := os.Remove(filepath.Join(root, "libs", "ra8_nsc", "src")); err != nil {
		t.Fatal(err)
	}

	code, stdout, stderr := ran(t, root, nil)
	if code != 1 || !strings.Contains(stderr, "cannot read source directory") {
		t.Fatalf("no source directory = %d, stderr %q", code, stderr)
	}
	if strings.Contains(stdout, "declared without a definition") {
		t.Fatal("a directory it could not read was reported as a phantom veneer")
	}
}

// A directory named like a source file, and a source that is not C, are
// both passed over: the definition has to come from a .c file the gate
// actually read.
func TestRunReadsOnlyCSourcesFromTheSourceDirectory(t *testing.T) {
	root := tree(t,
		map[string]string{"ra8_nsc.h": "RA8_NSC_VENEER void ra8_nsc_open(void);\n"},
		map[string]string{
			"open.cpp":   "RA8_NSC_VENEER void ra8_nsc_open(void) { }\n",
			"notes.txt":  "RA8_NSC_VENEER void ra8_nsc_open(void) { }\n",
			"open.c.bak": "RA8_NSC_VENEER void ra8_nsc_open(void) { }\n",
		})
	if err := os.MkdirAll(filepath.Join(root, "libs", "ra8_nsc", "src", "legacy.c"), 0o755); err != nil {
		t.Fatal(err)
	}

	code, stdout, _ := ran(t, root, nil)
	if code != 1 || !strings.Contains(stdout, "ra8_nsc_open") {
		t.Fatalf("a definition in no .c file = %d, stdout %q", code, stdout)
	}

	defined := tree(t,
		map[string]string{"ra8_nsc.h": "RA8_NSC_VENEER void ra8_nsc_open(void);\n"},
		map[string]string{"open.c": "RA8_NSC_VENEER void ra8_nsc_open(void) { }\n"})
	if code, stdout, _ := ran(t, defined, nil); code != 0 || !strings.Contains(stdout, "PASS") {
		t.Fatalf("the same definition in a .c file = %d, stdout %q", code, stdout)
	}
}

// A comment that never ends is not whitespace before a body: the scan runs
// out of file, and an unterminated comment must not be read as the
// definition the header was promised.
func TestAnUnterminatedCommentBeforeTheBodyIsNotADefinition(t *testing.T) {
	for name, source := range map[string]string{
		"a block comment that never closes": "RA8_NSC_VENEER void ra8_nsc_open(void) /* still going",
		"a line comment at the end of file": "RA8_NSC_VENEER void ra8_nsc_open(void) // still going",
		"nothing after the parameter list":  "RA8_NSC_VENEER void ra8_nsc_open(void)",
		"a semicolon after a comment":       "RA8_NSC_VENEER void ra8_nsc_open(void) /* declared */ ;",
	} {
		if definesVeneer("ra8_nsc_open", []byte(source)) {
			t.Fatalf("%s was read as a definition", name)
		}
	}
	if !definesVeneer("ra8_nsc_open", []byte("RA8_NSC_VENEER void ra8_nsc_open(void) /* here */ /* and here */ { }")) {
		t.Fatal("two closed comments before the body hid the definition")
	}
}
