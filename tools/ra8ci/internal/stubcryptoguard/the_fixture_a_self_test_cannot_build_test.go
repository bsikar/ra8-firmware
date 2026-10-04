// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package stubcryptoguard

import (
	"bytes"
	"context"
	"testing"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/testprivatefile"
)

func setMissingTempDirectory(t *testing.T) {
	t.Helper()
	blocked := t.TempDir()
	if err := testprivatefile.DenyDirectoryCreate(blocked); err != nil {
		t.Fatalf("denying temporary-directory creation: %v", err)
	}
	t.Cleanup(func() {
		if err := testprivatefile.RestoreDirectory(blocked); err != nil {
			t.Errorf("restoring temporary-directory ACL: %v", err)
		}
	})
	for _, key := range []string{"TMPDIR", "TMP", "TEMP"} {
		t.Setenv(key, blocked)
	}
}

func TestASelfTestThatCannotBuildItsFixtureTreeSaysSo(t *testing.T) {
	workRoot := t.TempDir()
	setMissingTempDirectory(t)

	var stdout, stderr bytes.Buffer
	code := Run(context.Background(), workRoot, []string{"--selftest"}, &stdout, &stderr)
	if code != 1 {
		t.Fatalf("exit = %d, want 1: stdout %q, stderr %q", code, stdout.String(), stderr.String())
	}
	if !bytes.Contains(stderr.Bytes(), []byte("create self-test fixture")) {
		t.Fatalf("the refusal does not name the fixture build: %q", stderr.String())
	}
	if bytes.Contains(stdout.Bytes(), []byte("PASS")) {
		t.Fatalf("a self-test that built nothing reported passing: %q", stdout.String())
	}
}
