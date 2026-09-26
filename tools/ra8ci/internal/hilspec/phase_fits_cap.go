// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package hilspec

import "fmt"

// checkPhasesFitTheSafetyCap refuses a manifest whose own declared phases
// cannot finish inside the cap it declares for itself.
//
// HIL_MAX_TIMEOUT_S bounds the observation window and nothing else: Decide
// narrows its cap with it, the estimator's answer is clamped to that cap, and
// flash/restore time is reserved separately through Options.FlashRestoreBound.
// So whatever a mode does between flashing and the verdict happens inside that
// cap.
//
// The phase knobs say how long those steps take, and the modes that declare
// them run them one after another inside a single observation: hil_eth_tcp
// waits HIL_BOOT_TIMEOUT_S for the ready banner and then gives the wire-side
// probe HIL_PROBE_TIMEOUT_S (scripts/hil/all.sh --boot-timeout/--probe-timeout);
// jlink_memprobe dwells HIL_PROBE_BOOT_S and then samples for
// HIL_PROBE_SECONDS (scripts/hil/run_local.sh); alive dwells HIL_BOOT_S before
// it snapshots. Their sum is therefore a floor on the observation, not an
// estimate of it, and a floor above the cap describes a run that cannot pass:
// the window expires mid-phase every time, on real hardware, after the board
// was leased and flashed. Parse already refuses the same contradiction stated
// at the top level (a safety maximum below the declared fallback); this is
// that rule applied to the phases the manifest declares underneath it.
func checkPhasesFitTheSafetyCap(spec Spec) error {
	if spec.SafetyMaximumSeconds <= 0 {
		return nil
	}
	total := 0
	for _, phase := range declaredPhases(spec) {
		total += phase.seconds
	}
	if total <= spec.SafetyMaximumSeconds {
		return nil
	}
	return fmt.Errorf("%w: %s declares %s totalling %ds inside a %ds safety maximum",
		ErrInvalidManifest, spec.Path, phaseNames(spec), total, spec.SafetyMaximumSeconds)
}

type declaredPhase struct {
	key     string
	seconds int
}

// declaredPhases lists the phases THIS mode runs inside the observation, in
// the order it runs them.
//
// A knob the mode ignores is not held against the cap. The schema keeps every
// HIL_* knob in Values whatever the mode is, deliberately, and a manifest
// carrying HIL_BOOT_TIMEOUT_S under an alive mode is stating something no step
// reads rather than reserving time. Refusing that is a different rule about
// unused knobs, and it is not this one.
func declaredPhases(spec Spec) []declaredPhase {
	switch spec.Mode {
	case ModeEthernetTCP:
		return nonZeroPhases(
			declaredPhase{"HIL_BOOT_TIMEOUT_S", spec.BootTimeoutSeconds},
			declaredPhase{"HIL_PROBE_TIMEOUT_S", spec.ProbeTimeoutSeconds},
		)
	case ModeJLinkMemprobe:
		return nonZeroPhases(
			declaredPhase{"HIL_PROBE_BOOT_S", spec.ProbeBootSeconds},
			declaredPhase{"HIL_PROBE_SECONDS", spec.ProbeSeconds},
		)
	case ModeAlive:
		return nonZeroPhases(declaredPhase{"HIL_BOOT_S", spec.BootSeconds})
	default:
		return nil
	}
}

func nonZeroPhases(phases ...declaredPhase) []declaredPhase {
	declared := make([]declaredPhase, 0, len(phases))
	for _, phase := range phases {
		if phase.seconds > 0 {
			declared = append(declared, phase)
		}
	}
	return declared
}

// phaseNames names what was actually declared, so the refusal points at the
// lines to change rather than at the mode.
func phaseNames(spec Spec) string {
	names := ""
	for i, phase := range declaredPhases(spec) {
		if i > 0 {
			names += " plus "
		}
		names += fmt.Sprintf("%s=%d", phase.key, phase.seconds)
	}
	return names
}
