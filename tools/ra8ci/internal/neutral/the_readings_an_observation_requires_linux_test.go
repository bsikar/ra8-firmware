// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package neutral

import (
	"context"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// An observation is what says a board is safe to hand on, so it is positive
// evidence or it is nothing. Every way a reading can fail to arrive, or
// arrive wrong, ends the observation rather than being left out of it.

// observerReading builds an observer over the reviewed fixture profile with
// the sysfs readings given, so one reading at a time can be spoiled.
func observerReading(t *testing.T, sysfs map[string]string) (*LinuxObserver, store.NeutralChallenge) {
	t.Helper()
	profile := validProfileFixture()
	now := time.Date(2026, 9, 23, 12, 0, 0, 0, time.UTC)
	observer, err := newLinuxObserver(LinuxObserverConfig{
		Profile: profile, ProfileSHA256: strings.Repeat("a", 64),
		Gate: &testHardwareGate{}, Inspector: testActivityInspector{},
		Readers: map[string]SignalReader{
			"sysfs": testSignalReader(sysfs),
			"tapo":  testSignalReader{"board.power_state": "off"},
		},
		Now: func() time.Time { return now },
	})
	if err != nil {
		t.Fatal(err)
	}
	return observer, store.NeutralChallenge{
		ID: "01996f90-3415-7cfe-8ff1-600058131afd", Nonce: strings.Repeat("b", 64),
		BoardID: profile.BoardID, Purpose: "release",
		LeaseID: "01996f90-3415-7cfe-8ff1-600058131afe", Generation: 4,
		AgentHighWater: 4, FixtureRevision: profile.FixtureRevision,
		ProfileSHA256: strings.Repeat("a", 64), RestorePolicy: profile.RestorePolicy,
		IssuedAt: now.Add(-time.Second), ExpiresAt: now.Add(20 * time.Second),
	}
}

func soundReadings() map[string]string {
	return map[string]string{
		"bus/usb/001/serial":           "RA8D2-001\n",
		"bus/usb/002/serial":           "JLINK-001\n",
		"class/gpio/board_power/value": "1\n",
		"class/gpio/reset/value":       "1\n",
		"class/hwmon/hwmon0/in0_input": "12\n",
	}
}

// A reading that never arrives is not a neutral board. Treating an absent
// signal as a passed check is how a live fixture gets declared safe.
func TestObserveNeutralRefusesASignalThatNeverArrived(t *testing.T) {
	for name, target := range map[string]string{
		"the board identity": "bus/usb/001/serial",
		"the probe identity": "bus/usb/002/serial",
		"the board power":    "class/gpio/board_power/value",
		"the reset line":     "class/gpio/reset/value",
		"the reference rail": "class/hwmon/hwmon0/in0_input",
	} {
		readings := soundReadings()
		delete(readings, target)
		observer, challenge := observerReading(t, readings)
		observation, err := observer.ObserveNeutral(context.Background(), challenge)
		if !errors.Is(err, ErrSignalMismatch) {
			t.Fatalf("%s missing = %v", name, err)
		}
		if observation.Neutral {
			t.Fatalf("%s missing still produced a neutral observation", name)
		}
	}
}

// A reading that arrives with the wrong value is the fixture telling us it
// is not in the state the profile was reviewed against.
func TestObserveNeutralRefusesASignalOutsideItsProfile(t *testing.T) {
	for name, spoil := range map[string]func(map[string]string){
		"another board":               func(r map[string]string) { r["bus/usb/001/serial"] = "RA8D2-002\n" },
		"another probe":               func(r map[string]string) { r["bus/usb/002/serial"] = "JLINK-002\n" },
		"power still on":              func(r map[string]string) { r["class/gpio/board_power/value"] = "0\n" },
		"reset not asserted":          func(r map[string]string) { r["class/gpio/reset/value"] = "0\n" },
		"a rail above its range":      func(r map[string]string) { r["class/hwmon/hwmon0/in0_input"] = "3300\n" },
		"a rail that is not a number": func(r map[string]string) { r["class/hwmon/hwmon0/in0_input"] = "floating\n" },
		"a rail that is not finite":   func(r map[string]string) { r["class/hwmon/hwmon0/in0_input"] = "NaN\n" },
	} {
		readings := soundReadings()
		spoil(readings)
		observer, challenge := observerReading(t, readings)
		if _, err := observer.ObserveNeutral(context.Background(), challenge); !errors.Is(err, ErrSignalMismatch) {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// The gate is taken before anything is read, so a gate that cannot be had
// ends the observation with nothing measured.
func TestObserveNeutralStopsWhenTheGateCannotBeHad(t *testing.T) {
	profile := validProfileFixture()
	held := &testHardwareGate{locked: true}
	observer, err := newLinuxObserver(LinuxObserverConfig{
		Profile: profile, ProfileSHA256: strings.Repeat("a", 64),
		Gate: held, Inspector: testActivityInspector{},
		Readers: map[string]SignalReader{
			"sysfs": testSignalReader(soundReadings()),
			"tapo":  testSignalReader{"board.power_state": "off"},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	_, challenge := observerReading(t, soundReadings())
	if _, err := observer.ObserveNeutral(context.Background(), challenge); err == nil {
		t.Fatal("an observation was taken without the hardware gate")
	}
}

// An observer with no gate or no inspector is unavailable rather than
// permissive: both are what keep another operation off the board.
func TestObserveNeutralWithoutItsGuardsIsUnavailable(t *testing.T) {
	var absentContext context.Context
	sound, challenge := observerReading(t, soundReadings())

	ungated, err := newLinuxObserver(LinuxObserverConfig{
		Profile: validProfileFixture(), ProfileSHA256: strings.Repeat("a", 64),
		Gate: &testHardwareGate{}, Inspector: testActivityInspector{},
		Readers: map[string]SignalReader{
			"sysfs": testSignalReader(soundReadings()),
			"tapo":  testSignalReader{"board.power_state": "off"},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	ungated.gate = nil

	for name, attempt := range map[string]func() (Observation, error){
		"an observer that is not there": func() (Observation, error) {
			return (*LinuxObserver)(nil).ObserveNeutral(context.Background(), challenge)
		},
		"a caller with no context": func() (Observation, error) {
			return sound.ObserveNeutral(absentContext, challenge)
		},
		"an observer with no gate": func() (Observation, error) {
			return ungated.ObserveNeutral(context.Background(), challenge)
		},
	} {
		if _, err := attempt(); !errors.Is(err, ErrGateUnavailable) {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// A board somebody else is working on is busy, not neutral, and the reason
// carries through rather than being flattened into a signal mismatch.
func TestObserveNeutralRefusesABoardSomebodyIsUsing(t *testing.T) {
	busy, err := newLinuxObserver(LinuxObserverConfig{
		Profile: validProfileFixture(), ProfileSHA256: strings.Repeat("a", 64),
		Gate:      &testHardwareGate{},
		Inspector: testActivityInspector{err: ErrHardwareBusy},
		Readers: map[string]SignalReader{
			"sysfs": testSignalReader(soundReadings()),
			"tapo":  testSignalReader{"board.power_state": "off"},
		},
	})
	if err != nil {
		t.Fatal(err)
	}
	_, challenge := observerReading(t, soundReadings())
	if _, err := busy.ObserveNeutral(context.Background(), challenge); !errors.Is(err, ErrHardwareBusy) {
		t.Fatalf("a board in use = %v", err)
	}
}

// An observer is only built over readers that can answer every check the
// reviewed profile names. A profile naming a source nothing reads would
// otherwise fail at observation time, on the board, under the gate.
func TestNewLinuxObserverRefusesReadersThatCannotAnswerTheProfile(t *testing.T) {
	sound := map[string]SignalReader{
		"sysfs": testSignalReader(soundReadings()),
		"tapo":  testSignalReader{"board.power_state": "off"},
	}

	for name, config := range map[string]LinuxObserverConfig{
		"a reader with no name": {Readers: map[string]SignalReader{"": testSignalReader{}}},
		"a reader that is not there": {Readers: map[string]SignalReader{
			"sysfs": testSignalReader(soundReadings()), "tapo": nil}},
		"no reader for a state source": {Readers: map[string]SignalReader{
			"sysfs": testSignalReader(soundReadings())}},
	} {
		config.Profile = validProfileFixture()
		config.ProfileSHA256 = strings.Repeat("a", 64)
		config.Gate = &testHardwareGate{}
		config.Inspector = testActivityInspector{}
		if _, err := newLinuxObserver(config); !errors.Is(err, ErrUnknownSignal) {
			t.Fatalf("%s = %v", name, err)
		}
	}

	// A sensor naming a source nothing reads never reaches the reader
	// check: profile review refuses the source first, and it is worth
	// knowing which of the two answered.
	sensorSource := validProfileFixture()
	sensorSource.Sensors[0].Source = "voltmeter"
	if _, err := newLinuxObserver(LinuxObserverConfig{
		Profile: sensorSource, ProfileSHA256: strings.Repeat("a", 64),
		Gate: &testHardwareGate{}, Inspector: testActivityInspector{}, Readers: sound,
	}); !errors.Is(err, ErrInvalidProfile) {
		t.Fatalf("a sensor source nothing reads = %v", err)
	}
}

// A profile that was never reviewed, or a digest that does not name one,
// does not become an observer at all.
func TestNewLinuxObserverRefusesAnUnreviewedProfile(t *testing.T) {
	sound := map[string]SignalReader{
		"sysfs": testSignalReader(soundReadings()),
		"tapo":  testSignalReader{"board.power_state": "off"},
	}
	incomplete := validProfileFixture()
	incomplete.Identity = nil

	for name, config := range map[string]LinuxObserverConfig{
		"no gate":                  {Profile: validProfileFixture(), ProfileSHA256: strings.Repeat("a", 64)},
		"no digest":                {Profile: validProfileFixture(), Gate: &testHardwareGate{}},
		"a digest that is not one": {Profile: validProfileFixture(), ProfileSHA256: "abc", Gate: &testHardwareGate{}},
		"a profile with no identity checks": {Profile: incomplete,
			ProfileSHA256: strings.Repeat("a", 64), Gate: &testHardwareGate{}},
	} {
		config.Readers = sound
		config.Inspector = testActivityInspector{}
		if _, err := newLinuxObserver(config); !errors.Is(err, ErrInvalidProfile) {
			t.Fatalf("%s = %v", name, err)
		}
	}
}

// A profile file that is not there is reported as it happened, and no
// observer is handed back to be used anyway.
func TestNewLinuxObserverFromFileRefusesAProfileItCannotRead(t *testing.T) {
	observer, err := NewLinuxObserverFromFile(t.TempDir()+"/absent.json", LinuxObserverConfig{
		Gate: &testHardwareGate{}, Inspector: testActivityInspector{},
	})
	if err == nil || observer != nil {
		t.Fatalf("a profile that is not there produced an observer: %v", err)
	}
}
