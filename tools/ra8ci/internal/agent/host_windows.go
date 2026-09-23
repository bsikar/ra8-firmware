//go:build windows

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package agent

import (
	"fmt"
	"runtime"
	"syscall"
	"time"
	"unsafe"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/protocol"
)

type memoryStatus struct {
	Length               uint32
	MemoryLoad           uint32
	TotalPhys            uint64
	AvailPhys            uint64
	TotalPageFile        uint64
	AvailPageFile        uint64
	TotalVirtual         uint64
	AvailVirtual         uint64
	AvailExtendedVirtual uint64
}

var (
	kernelDLL            = syscall.NewLazyDLL("kernel32.dll")
	globalMemoryStatusEx = kernelDLL.NewProc("GlobalMemoryStatusEx")
	getSystemTimes       = kernelDLL.NewProc("GetSystemTimes")
)

func filetimeValue(value syscall.Filetime) uint64 {
	return uint64(value.HighDateTime)<<32 | uint64(value.LowDateTime)
}

func systemTimes() (idle, total uint64, err error) {
	var idleTime, kernelTime, userTime syscall.Filetime
	result, _, callErr := getSystemTimes.Call(uintptr(unsafe.Pointer(&idleTime)),
		uintptr(unsafe.Pointer(&kernelTime)), uintptr(unsafe.Pointer(&userTime)))
	if result == 0 {
		return 0, 0, fmt.Errorf("GetSystemTimes: %v", callErr)
	}
	return filetimeValue(idleTime), filetimeValue(kernelTime) + filetimeValue(userTime), nil
}

// HostFacts labels Windows CPU utilization as an equivalent, not a Unix load.
func HostFacts() (protocol.HostFacts, error) {
	mem := memoryStatus{Length: uint32(unsafe.Sizeof(memoryStatus{}))}
	result, _, callErr := globalMemoryStatusEx.Call(uintptr(unsafe.Pointer(&mem)))
	if result == 0 {
		return protocol.HostFacts{}, fmt.Errorf("GlobalMemoryStatusEx: %v", callErr)
	}
	idle0, total0, err := systemTimes()
	if err != nil {
		return protocol.HostFacts{}, err
	}
	time.Sleep(100 * time.Millisecond)
	idle1, total1, err := systemTimes()
	if err != nil {
		return protocol.HostFacts{}, err
	}
	if total1 <= total0 || idle1 < idle0 || idle1-idle0 > total1-total0 {
		return protocol.HostFacts{}, fmt.Errorf("invalid Windows CPU sample")
	}
	busy := float64(total1-total0-(idle1-idle0)) / float64(total1-total0) * 100
	facts := protocol.HostFacts{Cores: runtime.NumCPU(), RAMBytes: int64(mem.TotalPhys),
		RAMFreeBytes: int64(mem.AvailPhys), Load1: busy, LoadKind: "cpu_busy_equivalent",
		OS: runtime.GOOS, Arch: runtime.GOARCH, CapturedAt: time.Now().UTC()}
	return facts, facts.Validate()
}
