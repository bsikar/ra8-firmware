//go:build !linux && !windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"errors"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// HostFacts fails closed on an unsupported host OS.
func HostFacts() (protocol.HostFacts, error) {
	return protocol.HostFacts{}, errors.New("agent host facts unsupported on this OS")
}
