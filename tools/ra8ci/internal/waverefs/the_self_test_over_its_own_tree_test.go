// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package waverefs

import (
	"bytes"
	"context"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The self-test derives its scope from the repository the gate ships in, so it
// is the one assertion in this package that can go red because the TREE moved
// rather than because the gate did. CI runs it that way, so the tests do too.
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
	if !strings.Contains(out.String(), "PASS") {
		t.Errorf("stdout = %q, want the PASS line", out.String())
	}
	// A self-test that held has nothing to report. Anything on stderr here is
	// noise an operator reads as a failure in a CI log.
	if errs.String() != "" {
		t.Errorf("stderr = %q, want nothing from a self-test that held", errs.String())
	}
}

// The scope line carries the count and the floor, which is what tells an
// operator how much headroom the real tree has before the floor refuses it.
func TestTheSelfTestReportsTheRealScopeAgainstItsFloor(t *testing.T) {
	var out bytes.Buffer
	if code := Run(context.Background(), treeTheGateShipsIn(t), []string{"--selftest"}, &out, io.Discard); code != 0 {
		t.Fatalf("code = %d, want 0", code)
	}
	if !strings.Contains(out.String(), "scope has") {
		t.Fatalf("stdout = %q, want the scope count", out.String())
	}
	if !strings.Contains(out.String(), "floor 2500") {
		t.Errorf("stdout = %q, want the floor named", out.String())
	}
}
