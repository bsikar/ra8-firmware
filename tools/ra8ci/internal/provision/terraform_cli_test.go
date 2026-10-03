// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package provision

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestTerraformWorkspaceRejectsSymlinksAndBoundsOutput(t *testing.T) {
	root := t.TempDir()
	workspace := filepath.Join(root, "reservation")
	if err := os.Mkdir(workspace, 0o700); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(root, "target")
	if err := os.WriteFile(target, []byte("data"), 0o600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(workspace, "linked")
	symlinkTest(t, target, link)
	if err := requirePrivateFileInside(workspace, link, 1024); err == nil {
		t.Fatal("accepted a symlinked Terraform input")
	}
	buffer := &boundedTerraformBuffer{limit: 4}
	if n, err := buffer.Write([]byte("four")); err != nil || n != 4 {
		t.Fatalf("bounded write = %d, %v", n, err)
	}
	if n, err := buffer.Write([]byte("x")); err == nil || n != 0 {
		t.Fatalf("output limit did not fail: n=%d err=%v", n, err)
	}
	if strings.Contains(string(buffer.data), "secret") {
		t.Fatal("unexpected secret in bounded output")
	}
}
