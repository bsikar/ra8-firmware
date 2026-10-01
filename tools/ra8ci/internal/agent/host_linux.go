//go:build linux

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"strings"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

// HostFacts measures capacity and one-minute Linux load at the call site.
func HostFacts() (protocol.HostFacts, error) {
	mem, err := os.ReadFile("/proc/meminfo")
	if err != nil {
		return protocol.HostFacts{}, err
	}
	var total, free int64
	for _, line := range strings.Split(string(mem), "\n") {
		fields := strings.Fields(line)
		if len(fields) < 3 || fields[2] != "kB" {
			continue
		}
		value, err := strconv.ParseInt(fields[1], 10, 64)
		if err != nil || value < 0 || value > (1<<63-1)/1024 {
			return protocol.HostFacts{}, fmt.Errorf("invalid /proc/meminfo value")
		}
		switch fields[0] {
		case "MemTotal:":
			total = value * 1024
		case "MemAvailable:":
			free = value * 1024
		}
	}
	load, err := os.ReadFile("/proc/loadavg")
	if err != nil {
		return protocol.HostFacts{}, err
	}
	fields := strings.Fields(string(load))
	if len(fields) < 1 {
		return protocol.HostFacts{}, fmt.Errorf("invalid /proc/loadavg")
	}
	load1, err := strconv.ParseFloat(fields[0], 64)
	if err != nil {
		return protocol.HostFacts{}, err
	}
	facts := protocol.HostFacts{Cores: runtime.NumCPU(), RAMBytes: total, RAMFreeBytes: free,
		Load1: load1, LoadKind: "linux_load1", OS: runtime.GOOS, Arch: runtime.GOARCH,
		CapturedAt: time.Now().UTC()}
	return facts, facts.Validate()
}
