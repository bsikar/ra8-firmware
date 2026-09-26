// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import (
	"strings"
	"testing"
)

func manifest(lines ...string) string {
	return strings.Join(lines, "\n") + "\n"
}

func parseManifest(t *testing.T, text string) (Spec, error) {
	t.Helper()
	return Parse(strings.NewReader(text), "examples/board/hil.conf")
}

func TestPhasesLongerThanTheSafetyCapAreRefused(t *testing.T) {
	_, err := parseManifest(t, manifest(
		"HIL_MODE=hil_eth_tcp",
		"HIL_BOARD_IP=192.0.2.10",
		"HIL_MAX_TIMEOUT_S=20",
		"HIL_BOOT_TIMEOUT_S=25",
		"HIL_PROBE_TIMEOUT_S=10",
	))
	if err == nil {
		t.Fatal("a 35s pair of phases inside a 20s safety maximum was accepted")
	}
	for _, want := range []string{"HIL_BOOT_TIMEOUT_S=25", "HIL_PROBE_TIMEOUT_S=10", "35s", "20s"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("refusal does not name %q: %v", want, err)
		}
	}
}

func TestPhasesThatFitTheSafetyCapAreAccepted(t *testing.T) {
	spec, err := parseManifest(t, manifest(
		"HIL_MODE=hil_eth_tcp",
		"HIL_BOARD_IP=192.0.2.10",
		"HIL_MAX_TIMEOUT_S=40",
		"HIL_BOOT_TIMEOUT_S=25",
		"HIL_PROBE_TIMEOUT_S=10",
	))
	if err != nil {
		t.Fatalf("35s of phases inside a 40s safety maximum was refused: %v", err)
	}
	if spec.BootTimeoutSeconds != 25 || spec.ProbeTimeoutSeconds != 10 {
		t.Fatalf("phases not promoted: boot %d probe %d", spec.BootTimeoutSeconds, spec.ProbeTimeoutSeconds)
	}
}

func TestPhasesExactlyFillingTheSafetyCapAreAccepted(t *testing.T) {
	if _, err := parseManifest(t, manifest(
		"HIL_MODE=hil_eth_tcp",
		"HIL_BOARD_IP=192.0.2.10",
		"HIL_MAX_TIMEOUT_S=35",
		"HIL_BOOT_TIMEOUT_S=25",
		"HIL_PROBE_TIMEOUT_S=10",
	)); err != nil {
		t.Fatalf("phases summing to exactly the cap were refused: %v", err)
	}
}

// The cap is the only bound a phase is held to. HIL_TIMEOUT_S is the fallback
// the estimator uses until five comparable measurements exist, and history may
// legitimately choose a longer window, so a phase longer than the fallback is
// ordinary. Do not tighten this to the fallback.
func TestAPhaseLongerThanTheDeclaredFallbackIsOrdinary(t *testing.T) {
	if _, err := parseManifest(t, manifest(
		"HIL_MODE=hil_eth_tcp",
		"HIL_BOARD_IP=192.0.2.10",
		"HIL_TIMEOUT_S=15",
		"HIL_MAX_TIMEOUT_S=120",
		"HIL_BOOT_TIMEOUT_S=25",
		"HIL_PROBE_TIMEOUT_S=10",
	)); err != nil {
		t.Fatalf("phases longer than the declared fallback were refused: %v", err)
	}
}

// Zero means no safety maximum was declared, the same convention the existing
// safety-versus-fallback check and catalog.validateHILTask both use. Do not
// turn this into "a manifest must declare a safety maximum".
func TestAManifestWithNoSafetyMaximumIsNotJudged(t *testing.T) {
	if _, err := parseManifest(t, manifest(
		"HIL_MODE=hil_eth_tcp",
		"HIL_BOARD_IP=192.0.2.10",
		"HIL_BOOT_TIMEOUT_S=600",
		"HIL_PROBE_TIMEOUT_S=600",
	)); err != nil {
		t.Fatalf("a manifest declaring no safety maximum was judged: %v", err)
	}
}

// The schema keeps every knob whatever the mode is. A knob this mode never
// reads reserves no time, so it is not held against the cap.
func TestAKnobTheModeDoesNotRunIsNotHeldAgainstTheCap(t *testing.T) {
	if _, err := parseManifest(t, manifest(
		"HIL_MODE=uart_scrape",
		`HIL_EXPECT="boot complete banner"`,
		"HIL_MAX_TIMEOUT_S=20",
		"HIL_BOOT_TIMEOUT_S=600",
		"HIL_PROBE_TIMEOUT_S=600",
	)); err != nil {
		t.Fatalf("uart_scrape was judged on phases it never runs: %v", err)
	}
	if _, err := parseManifest(t, manifest(
		"HIL_MODE=alive",
		"HIL_MAX_TIMEOUT_S=20",
		"HIL_BOOT_S=5",
		"HIL_PROBE_SECONDS=600",
	)); err != nil {
		t.Fatalf("alive was judged on the memprobe sample window: %v", err)
	}
}

func TestEachModeIsJudgedOnTheStepsItActuallyRuns(t *testing.T) {
	for _, row := range []struct {
		name    string
		lines   []string
		refused bool
	}{
		{"memprobe over", []string{"HIL_MODE=jlink_memprobe", "HIL_PROBE_SYMBOL=g_tick", "HIL_MAX_TIMEOUT_S=10", "HIL_PROBE_BOOT_S=8", "HIL_PROBE_SECONDS=5"}, true},
		{"memprobe within", []string{"HIL_MODE=jlink_memprobe", "HIL_PROBE_SYMBOL=g_tick", "HIL_MAX_TIMEOUT_S=15", "HIL_PROBE_BOOT_S=8", "HIL_PROBE_SECONDS=5"}, false},
		{"alive over", []string{"HIL_MODE=alive", "HIL_MAX_TIMEOUT_S=3", "HIL_BOOT_S=4"}, true},
		{"alive within", []string{"HIL_MODE=alive", "HIL_MAX_TIMEOUT_S=4", "HIL_BOOT_S=4"}, false},
		{"eth one phase over", []string{"HIL_MODE=hil_eth_tcp", "HIL_BOARD_IP=192.0.2.10", "HIL_MAX_TIMEOUT_S=20", "HIL_BOOT_TIMEOUT_S=25"}, true},
		{"eth one phase within", []string{"HIL_MODE=hil_eth_tcp", "HIL_BOARD_IP=192.0.2.10", "HIL_MAX_TIMEOUT_S=25", "HIL_BOOT_TIMEOUT_S=25"}, false},
		{"rtt untouched", []string{"HIL_MODE=rtt_scrape", `HIL_EXPECT="self test passed ok"`, "HIL_MAX_TIMEOUT_S=5", "HIL_BOOT_S=600"}, false},
		{"camera untouched", []string{"HIL_MODE=c6_camera_livestream", "HIL_MAX_TIMEOUT_S=5", "HIL_PROBE_SECONDS=600"}, false},
	} {
		t.Run(row.name, func(t *testing.T) {
			_, err := parseManifest(t, manifest(row.lines...))
			if row.refused && err == nil {
				t.Fatal("accepted phases that cannot finish inside the cap")
			}
			if !row.refused && err != nil {
				t.Fatalf("refused a manifest whose phases fit: %v", err)
			}
		})
	}
}

// Decide clamps its window to the same cap Parse holds the phases against, so
// an accepted manifest's phases always fit the widest window Decide can hand
// back. This crosses the two rather than trusting they agree.
func TestAnAcceptedManifestsPhasesFitTheWidestWindowDecideCanChoose(t *testing.T) {
	for _, capSeconds := range []int{5, 10, 35, 60, 120, 900} {
		spec, err := parseManifest(t, manifest(
			"HIL_MODE=hil_eth_tcp",
			"HIL_BOARD_IP=192.0.2.10",
			"HIL_MAX_TIMEOUT_S="+itoa(capSeconds),
			"HIL_BOOT_TIMEOUT_S=25",
			"HIL_PROBE_TIMEOUT_S=10",
		))
		if err != nil {
			continue
		}
		total := 0
		for _, phase := range declaredPhases(spec) {
			total += phase.seconds
		}
		if total > spec.SafetyMaximumSeconds {
			t.Fatalf("cap %ds: accepted %ds of phases that cannot finish inside it", capSeconds, total)
		}
	}
}

// The existing top-level contradiction is unchanged and still applies.
func TestTheSafetyMaximumIsStillHeldAboveTheDeclaredFallback(t *testing.T) {
	if _, err := parseManifest(t, manifest(
		"HIL_MODE=alive",
		"HIL_TIMEOUT_S=30",
		"HIL_MAX_TIMEOUT_S=10",
	)); err == nil {
		t.Fatal("a safety maximum below the declared fallback was accepted")
	}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	digits := ""
	for n > 0 {
		digits = string(rune('0'+n%10)) + digits
		n /= 10
	}
	return digits
}
