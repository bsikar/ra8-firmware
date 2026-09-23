// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package nscveneers

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"testing"
)

func TestVeneerDetection(t *testing.T) {
	const header = "RA8_NSC_VENEER ra8_err_t ra8_nsc_defined(void);\nRA8_NSC_VENEER void ra8_nsc_phantom(void);\n"
	const source = "RA8_NSC_VENEER ra8_err_t ra8_nsc_defined(void) { return 0; }\nvoid caller(void) { ra8_nsc_phantom(); }\n"
	names := declared(header)
	if len(names) != 2 || names[0] != "ra8_nsc_defined" || names[1] != "ra8_nsc_phantom" {
		t.Fatalf("declared() = %v", names)
	}
	sources := [][]byte{[]byte(source)}
	if !isDefined("ra8_nsc_defined", sources) || isDefined("ra8_nsc_phantom", sources) {
		t.Fatal("definition matching did not distinguish a definition from a call")
	}
}

func TestRunSelfTestAndMissingHeader(t *testing.T) {
	var stdout, stderr bytes.Buffer
	if got := Run(context.Background(), t.TempDir(), []string{"--selftest"}, &stdout, &stderr); got != 0 {
		t.Fatalf("self-test exit=%d stdout=%s stderr=%s", got, stdout.String(), stderr.String())
	}
	stdout.Reset()
	stderr.Reset()
	if got := Run(context.Background(), t.TempDir(), nil, &stdout, &stderr); got != 1 {
		t.Fatalf("missing header exit=%d stderr=%s", got, stderr.String())
	}
}

func TestRunScansHeaderAndSource(t *testing.T) {
	root := t.TempDir()
	header := filepath.Join(root, "libs", "ra8_nsc", "inc", "ra8_nsc.h")
	source := filepath.Join(root, "libs", "ra8_nsc", "src", "nsc.c")
	if err := os.MkdirAll(filepath.Dir(header), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(source), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(header, []byte("RA8_NSC_VENEER void ra8_nsc_ok(void);\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(source, []byte("RA8_NSC_VENEER void ra8_nsc_ok(void) {}\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	var stdout, stderr bytes.Buffer
	if got := Run(context.Background(), root, nil, &stdout, &stderr); got != 0 {
		t.Fatalf("scan exit=%d stdout=%s stderr=%s", got, stdout.String(), stderr.String())
	}
	if err := os.WriteFile(source, []byte("void caller(void) { ra8_nsc_ok(); }\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	stdout.Reset()
	if got := Run(context.Background(), root, nil, &stdout, &stderr); got != 1 {
		t.Fatalf("phantom veneer exit=%d stdout=%s", got, stdout.String())
	}
}
