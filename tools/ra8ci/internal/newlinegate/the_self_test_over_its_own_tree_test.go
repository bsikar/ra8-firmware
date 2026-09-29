// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package newlinegate

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// This gate's self-test derives its scope from the repository it ships in, so
// it can go red because the TREE moved rather than because the gate did. CI
// runs it that way, so the tests do too.
func treeTheGateShipsIn(t *testing.T) string {
	t.Helper()
	root, err := filepath.Abs(filepath.Join("..", "..", "..", ".."))
	if err != nil {
		t.Fatalf("repository root: %v", err)
	}
	if _, err := os.Stat(filepath.Join(root, ".git")); err != nil {
		t.Skipf("not running inside a checkout: %v", err)
	}
	return root
}

func TestTheSelfTestHoldsOverTheTreeItShipsIn(t *testing.T) {
	var out, errs bytes.Buffer
	if code := Run(context.Background(), treeTheGateShipsIn(t), []string{"--selftest"}, &out, &errs); code != 0 {
		t.Fatalf("code = %d, want 0; stderr %q", code, errs.String())
	}
	if !strings.Contains(out.String(), "selftest passed") {
		t.Errorf("stdout = %q, want the passing line", out.String())
	}
	// A self-test that held has nothing to report.
	if errs.String() != "" {
		t.Errorf("stderr = %q, want nothing from a self-test that held", errs.String())
	}
}
