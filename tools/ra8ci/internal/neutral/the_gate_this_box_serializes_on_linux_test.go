// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"
)

// The gate is what stops an observation and a flash from reaching the same
// board at once. Everything about it is a safety property: it is exclusive
// while held, it hands back rather than blocking forever, and a release
// releases exactly the hold it was given out for.

func TestSerialGateIsExclusiveWhileHeld(t *testing.T) {
	gate := NewSerialGate()
	release, err := gate.Lock(context.Background())
	if err != nil || release == nil {
		t.Fatalf("an unlocked gate refused the first holder: %v", err)
	}

	// A second holder does not get in while the first has it.
	blocked, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	if _, err := gate.Lock(blocked); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("a held gate admitted a second holder: %v", err)
	}

	release()
	second, err := gate.Lock(context.Background())
	if err != nil || second == nil {
		t.Fatalf("a released gate refused the next holder: %v", err)
	}
	second()
}

// A release is for one hold. Calling it twice must not open the gate for
// whoever holds it now, which is how two operations would reach one board.
func TestSerialGateReleaseIsForOneHoldOnly(t *testing.T) {
	gate := NewSerialGate()
	release, err := gate.Lock(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	release()
	release()
	release()

	next, err := gate.Lock(context.Background())
	if err != nil {
		t.Fatalf("the gate did not come back after its holder released it: %v", err)
	}
	defer next()

	blocked, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	if _, err := gate.Lock(blocked); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatal("a repeated release opened the gate under its current holder")
	}
}

// A caller that has stopped waiting is handed its own reason back, and the
// gate is not left half-taken by the attempt.
func TestSerialGateStopsWaitingWhenTheCallerHas(t *testing.T) {
	gate := NewSerialGate()
	held, err := gate.Lock(context.Background())
	if err != nil {
		t.Fatal(err)
	}

	cancelled, cancel := context.WithCancel(context.Background())
	cancel()
	if release, err := gate.Lock(cancelled); !errors.Is(err, context.Canceled) || release != nil {
		t.Fatalf("a cancelled caller = %v (release %v)", err, release != nil)
	}

	held()
	next, err := gate.Lock(context.Background())
	if err != nil {
		t.Fatalf("a cancelled attempt left the gate unusable: %v", err)
	}
	next()
}

// A gate that was never built is unavailable rather than open. Failing open
// here would mean hardware operations racing with nothing serializing them.
func TestSerialGateWithoutASemaphoreIsUnavailable(t *testing.T) {
	var absentContext context.Context
	for name, attempt := range map[string]func() (func(), error){
		"a gate that is not there": func() (func(), error) { return (*SerialGate)(nil).Lock(context.Background()) },
		"a gate with no semaphore": func() (func(), error) { return (&SerialGate{}).Lock(context.Background()) },
		"a caller with no context": func() (func(), error) { return NewSerialGate().Lock(absentContext) },
	} {
		release, err := attempt()
		if !errors.Is(err, ErrGateUnavailable) || release != nil {
			t.Fatalf("%s = %v (release %v)", name, err, release != nil)
		}
	}
}

// Under contention the gate admits exactly one holder at a time, which is
// the only property the board actually depends on.
func TestSerialGateAdmitsOneHolderAtATime(t *testing.T) {
	gate := NewSerialGate()
	var mu sync.Mutex
	inside, most := 0, 0
	var wg sync.WaitGroup
	for i := 0; i < 16; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			release, err := gate.Lock(context.Background())
			if err != nil {
				t.Error(err)
				return
			}
			mu.Lock()
			inside++
			if inside > most {
				most = inside
			}
			mu.Unlock()
			time.Sleep(time.Millisecond)
			mu.Lock()
			inside--
			mu.Unlock()
			release()
		}()
	}
	wg.Wait()
	if most != 1 {
		t.Fatalf("%d holders were inside the gate at once", most)
	}
}

// The function adapter is the seam production wires a reader through, so a
// reader's answer and its refusal both have to come back unchanged.
func TestSignalReadFuncCarriesBothAnswers(t *testing.T) {
	var asked string
	reader := SignalReadFunc(func(_ context.Context, target string) (string, error) {
		asked = target
		return "0x8250", nil
	})
	value, err := reader.ReadSignal(context.Background(), "device-id")
	if err != nil || value != "0x8250" || asked != "device-id" {
		t.Fatalf("a reader's answer changed on the way back: value=%q asked=%q err=%v", value, asked, err)
	}

	refused := errors.New("the signal file is not readable")
	failing := SignalReadFunc(func(context.Context, string) (string, error) { return "", refused })
	if _, err := failing.ReadSignal(context.Background(), "device-id"); !errors.Is(err, refused) {
		t.Fatalf("a reader's refusal did not come back: %v", err)
	}
}
