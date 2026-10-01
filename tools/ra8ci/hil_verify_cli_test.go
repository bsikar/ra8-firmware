// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilspec"
)

func TestVerifyHILCaptureUsesManifestAndAssertions(t *testing.T) {
	root := t.TempDir()
	manifestPath := filepath.Join(root, "examples", "demo", "hil.conf")
	if err := os.MkdirAll(filepath.Dir(manifestPath), 0700); err != nil {
		t.Fatal(err)
	}
	manifest := "HIL_MODE=uart_scrape\nHIL_EXPECT=verdict=PASS\nHIL_EXPECT_NEGATIVE=HardFault\n"
	if err := os.WriteFile(manifestPath, []byte(manifest), 0600); err != nil {
		t.Fatal(err)
	}
	result, err := verifyHILCapture(root, "examples/demo/hil.conf", strings.NewReader("boot\nverdict=PASS\n"))
	if err != nil || !result.Accepted || result.CaptureBytes == 0 || result.Mode != "uart_scrape" {
		t.Fatalf("valid capture failed: %+v err=%v", result, err)
	}
	_, err = verifyHILCapture(root, "examples/demo/hil.conf", strings.NewReader("verdict=PASS\nHardFault\n"))
	if !errors.Is(err, hilspec.ErrNegativeExpectation) {
		t.Fatalf("negative assertion was ignored: %v", err)
	}
	_, err = verifyHILCapture(root, "../outside/hil.conf", strings.NewReader("verdict=PASS"))
	if !errors.Is(err, hilspec.ErrUnsafePath) {
		t.Fatalf("manifest traversal was accepted: %v", err)
	}
}

func TestVerifyHILCaptureRejectsOversizedInput(t *testing.T) {
	root := t.TempDir()
	manifestPath := filepath.Join(root, "examples", "demo", "hil.conf")
	if err := os.MkdirAll(filepath.Dir(manifestPath), 0700); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(manifestPath, []byte("HIL_MODE=uart_scrape\nHIL_EXPECT=verdict=PASS\n"), 0600); err != nil {
		t.Fatal(err)
	}
	_, err := verifyHILCapture(root, "examples/demo/hil.conf", strings.NewReader(strings.Repeat("x", maxHILCaptureBytes+1)))
	if err == nil {
		t.Fatal("oversized capture was accepted")
	}
}
