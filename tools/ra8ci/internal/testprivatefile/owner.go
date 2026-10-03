// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

// Package testprivatefile prepares private-file fixtures for host tests.
package testprivatefile

// OwnerOnly applies the platform's owner-only file permissions.
func OwnerOnly(path string) error { return ownerOnly(path) }
