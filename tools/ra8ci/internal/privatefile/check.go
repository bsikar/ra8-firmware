// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package privatefile validates that a file is accessible only to its owner
// and the platform's trusted system administrators.
package privatefile

import "os"

// Check reports whether path names a regular file protected for its owner.
func Check(path string) error { return check(path) }

// CheckFile applies Check to the file already opened by the caller, preserving
// descriptor identity across the caller's validation and read.
func CheckFile(file *os.File) error { return checkFile(file) }

// RestrictFile makes an already-open file readable and writable only by its
// owner and the platform's trusted system administrators.
func RestrictFile(file *os.File) error { return restrictFile(file) }

// CheckDirectory reports whether path names a directory with owner-only access.
func CheckDirectory(path string) error { return checkDirectory(path) }

// CheckDirectoryNoUntrustedWrite permits public reads but rejects directory
// entries that let another account replace protected files.
func CheckDirectoryNoUntrustedWrite(path string) error {
	return checkDirectoryNoUntrustedWrite(path)
}

// CheckNoUntrustedWrite reports whether a regular file cannot be modified by
// another untrusted account. It is for published evidence whose readers need
// not be restricted, but whose contents must remain stable.
func CheckNoUntrustedWrite(path string) error { return checkNoUntrustedWrite(path) }

// CheckFileNoUntrustedWrite applies CheckNoUntrustedWrite to an open file.
func CheckFileNoUntrustedWrite(file *os.File) error { return checkFileNoUntrustedWrite(file) }
