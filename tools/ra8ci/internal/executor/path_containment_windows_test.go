//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"golang.org/x/sys/windows"
)

func TestWindowsPathContainmentHandlesCaseAndShortNames(t *testing.T) {
	root := filepath.Join(t.TempDir(), "LongDirectoryName")
	child := filepath.Join(root, "NestedDirectory")
	if err := os.MkdirAll(child, 0o700); err != nil {
		t.Fatal(err)
	}
	within, err := pathsWithin(root, strings.ToUpper(child))
	if err != nil || !within {
		t.Fatalf("case-insensitive containment = %v, err = %v", within, err)
	}

	wide, err := windows.UTF16PtrFromString(child)
	if err != nil {
		t.Fatal(err)
	}
	buffer := make([]uint16, 32768)
	length, callErr := windows.GetShortPathName(wide, &buffer[0], uint32(len(buffer)))
	if callErr == nil && length > 0 && length < uint32(len(buffer)) {
		shortPath := windows.UTF16ToString(buffer[:length])
		if !strings.EqualFold(shortPath, child) {
			within, err = pathsWithin(root, shortPath)
			if err != nil || !within {
				t.Fatalf("short-name containment = %v, err = %v", within, err)
			}
		}
	}

	outside := t.TempDir()
	within, err = pathsWithin(root, outside)
	if err != nil || within {
		t.Fatalf("outside containment = %v, err = %v", within, err)
	}

	logicalDrives, err := windows.GetLogicalDrives()
	if err != nil {
		t.Fatal(err)
	}
	rootDrive := filepath.VolumeName(root)[0] - 'A'
	for drive := byte(0); drive < 26; drive++ {
		if drive == rootDrive || logicalDrives&(1<<drive) == 0 {
			continue
		}
		otherVolume := string([]rune{'A' + rune(drive), ':'}) + string(filepath.Separator)
		within, err = pathsWithin(root, otherVolume)
		if err != nil || within {
			t.Fatalf("different-volume containment = %v, err = %v", within, err)
		}
		return
	}
	t.Log("no second mounted drive; different-volume case not exercised")
}

func TestWindowsPathContainmentHandlesLongAndExtendedPaths(t *testing.T) {
	longName := strings.Repeat("LongPathSegment", 12)
	root := filepath.Join(t.TempDir(), longName)
	child := filepath.Join(root, "NestedDirectory")
	if len(child) <= 260 {
		t.Fatalf("fixture path is not long enough: %d characters", len(child))
	}
	if err := os.MkdirAll(child, 0o700); err != nil {
		t.Fatal(err)
	}

	for name, candidate := range map[string]string{
		"plain":    child,
		"extended": `\\?\` + child,
	} {
		t.Run(name, func(t *testing.T) {
			within, err := pathsWithin(root, candidate)
			if err != nil || !within {
				t.Fatalf("long-path containment = %v, err = %v", within, err)
			}
		})
	}
}
