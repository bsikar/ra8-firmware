// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"io/fs"
	"path/filepath"
	"regexp"
	"testing"
)

func TestCurrentTextCapturePatternsCompileInGoRE2(t *testing.T) {
	root := repositoryRoot(t)
	checked := 0
	err := filepath.WalkDir(filepath.Join(root, "examples"), func(file string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if entry.IsDir() || entry.Name() != "hil.conf" {
			return nil
		}
		relative, err := filepath.Rel(root, file)
		if err != nil {
			return err
		}
		spec, err := Load(root, relative)
		if err != nil {
			return err
		}
		if spec.Mode != ModeUARTScrape && spec.Mode != ModeRTTScrape {
			return nil
		}
		if spec.ExpectNegative != "" {
			if _, err := regexp.Compile(spec.ExpectNegative); err != nil {
				t.Errorf("%s: negative expectation is not Go/RE2-compatible: %v", relative, err)
			}
		}
		checked++
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if checked == 0 {
		t.Fatal("no UART/RTT capture manifests were checked")
	}
}
