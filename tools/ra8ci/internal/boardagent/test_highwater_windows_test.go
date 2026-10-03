// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package boardagent

import (
	"sync"
	"testing"
)

type memoryTestHighWater struct {
	mu    sync.Mutex
	value uint64
}

func (state *memoryTestHighWater) Load() (uint64, error) {
	state.mu.Lock()
	defer state.mu.Unlock()
	return state.value, nil
}

func (state *memoryTestHighWater) Advance(generation uint64) error {
	state.mu.Lock()
	defer state.mu.Unlock()
	if generation == 0 {
		return ErrGenerationInvalid
	}
	if generation < state.value {
		return ErrGenerationRollback
	}
	state.value = generation
	return nil
}

func newTestHighWater(t *testing.T) (HighWaterStore, string) {
	t.Helper()
	return &memoryTestHighWater{}, ""
}
