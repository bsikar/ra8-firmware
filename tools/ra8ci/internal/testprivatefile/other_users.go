// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package testprivatefile

// OtherUsersReadable adds an access grant that a private-file check must reject.
func OtherUsersReadable(path string) error { return otherUsersReadable(path) }
