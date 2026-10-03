//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package executor

import (
	"errors"
	"os"
	"path/filepath"
	"strings"

	"golang.org/x/sys/windows"
)

func pathsWithin(root, candidate string) (bool, error) {
	canonicalRoot, err := canonicalWindowsPath(root)
	if err != nil {
		return false, err
	}
	canonicalCandidate, err := canonicalWindowsPath(candidate)
	if err != nil {
		return false, err
	}
	if !strings.EqualFold(filepath.VolumeName(canonicalRoot), filepath.VolumeName(canonicalCandidate)) {
		return false, nil
	}
	rootParts := strings.Split(strings.Trim(filepath.Clean(canonicalRoot), `\/`), `\`)
	candidateParts := strings.Split(strings.Trim(filepath.Clean(canonicalCandidate), `\/`), `\`)
	if len(candidateParts) < len(rootParts) {
		return false, nil
	}
	for index, part := range rootParts {
		if !strings.EqualFold(part, candidateParts[index]) {
			return false, nil
		}
	}
	return true, nil
}

func canonicalWindowsPath(path string) (string, error) {
	absolute, err := filepath.Abs(filepath.Clean(path))
	if err != nil {
		return "", err
	}
	var missing []string
	for {
		info, statErr := os.Stat(absolute)
		if statErr == nil && info != nil {
			break
		}
		if statErr != nil && !errors.Is(statErr, os.ErrNotExist) {
			return "", statErr
		}
		parent := filepath.Dir(absolute)
		if parent == absolute {
			return "", os.ErrNotExist
		}
		missing = append(missing, filepath.Base(absolute))
		absolute = parent
	}
	wide, err := windows.UTF16PtrFromString(absolute)
	if err != nil {
		return "", err
	}
	buffer := make([]uint16, 32768)
	length, callErr := windows.GetLongPathName(wide, &buffer[0], uint32(len(buffer)))
	if callErr != nil {
		return "", callErr
	}
	if length == 0 || length >= uint32(len(buffer)) {
		return "", errors.New("Windows could not resolve the long path")
	}
	resolved := windows.UTF16ToString(buffer[:length])
	for index := len(missing) - 1; index >= 0; index-- {
		resolved = filepath.Join(resolved, missing[index])
	}
	return filepath.Clean(strings.TrimPrefix(resolved, `\\?\`)), nil
}
