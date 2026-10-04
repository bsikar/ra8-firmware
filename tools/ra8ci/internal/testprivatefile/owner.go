// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package testprivatefile prepares filesystem permission fixtures for host tests.
package testprivatefile

// OwnerOnly applies the platform's owner-only file permissions.
func OwnerOnly(path string) error { return ownerOnly(path) }

// DenyDirectoryRead prevents the current user from listing a directory.
func DenyDirectoryRead(path string) error { return denyDirectoryRead(path) }

// DenyDirectoryCreate prevents the current user from creating entries in a directory.
func DenyDirectoryCreate(path string) error { return denyDirectoryCreate(path) }

// RestoreDirectory restores owner-only access so tests can clean up their fixtures.
func RestoreDirectory(path string) error { return restoreDirectory(path) }
