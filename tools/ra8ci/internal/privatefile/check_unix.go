//go:build !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package privatefile

import (
	"fmt"
	"os"
)

func check(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("stat private file %s: %w", path, err)
	}
	return checkMode(path, info)
}

func checkFile(file *os.File) error {
	if file == nil {
		return fmt.Errorf("private file handle is nil")
	}
	info, err := file.Stat()
	if err != nil {
		return fmt.Errorf("stat private file: %w", err)
	}
	return checkMode(file.Name(), info)
}

func checkDirectory(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("stat private directory %s: %w", path, err)
	}
	if !info.IsDir() {
		return fmt.Errorf("private path %s is not a directory", path)
	}
	if mode := info.Mode().Perm(); mode&0o077 != 0 {
		return fmt.Errorf("private directory %s mode %04o grants access beyond its owner", path, mode)
	}
	return nil
}

func checkDirectoryNoUntrustedWrite(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("stat protected directory %s: %w", path, err)
	}
	if !info.IsDir() {
		return fmt.Errorf("protected path %s is not a directory", path)
	}
	if mode := info.Mode().Perm(); mode&0o022 != 0 {
		return fmt.Errorf("protected directory %s mode %04o permits untrusted writes", path, mode)
	}
	return nil
}

func checkMode(path string, info os.FileInfo) error {
	if !info.Mode().IsRegular() {
		return fmt.Errorf("private file %s is not a regular file", path)
	}
	if mode := info.Mode().Perm(); mode&0o077 != 0 {
		return fmt.Errorf("private file %s mode %04o grants access beyond its owner", path, mode)
	}
	return nil
}

func checkNoUntrustedWrite(path string) error {
	info, err := os.Stat(path)
	if err != nil {
		return fmt.Errorf("stat protected file %s: %w", path, err)
	}
	return checkWriteMode(path, info)
}

func checkFileNoUntrustedWrite(file *os.File) error {
	if file == nil {
		return fmt.Errorf("protected file handle is nil")
	}
	info, err := file.Stat()
	if err != nil {
		return fmt.Errorf("stat protected file: %w", err)
	}
	return checkWriteMode(file.Name(), info)
}

func checkWriteMode(path string, info os.FileInfo) error {
	if !info.Mode().IsRegular() {
		return fmt.Errorf("protected file %s is not a regular file", path)
	}
	if mode := info.Mode().Perm(); mode&0o022 != 0 {
		return fmt.Errorf("protected file %s mode %04o permits untrusted writes", path, mode)
	}
	return nil
}
