package hilspec

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/hilpolicy"
)

const maxHistoryRows = 10000

// Workload identifies a genuinely comparable HIL observation cohort. The
// caller supplies board/fixture/program family identity from trusted catalog
// and inventory state, not an unverified run result.
type Workload struct {
	ManifestPath    string
	BoardModel      string
	FixtureRevision string
	ProfileSHA256   string
	ProgramFamily   string
	Mode            Mode
}

type HistoricalObservation struct {
	Workload         Workload
	Duration         time.Duration
	Succeeded        bool
	EvidenceComplete bool
	TimedOut         bool
}

// ObservationSource returns historical rows; Decide independently filters
// exact cohort identity and evidence-complete, successful, uncensored rows.
type ObservationSource interface {
	Observations(context.Context, Workload) ([]HistoricalObservation, error)
}

// Options keeps fixture neutralization/restore time separate from the HIL
// validity window. A board scheduler must reserve both before starting work.
type Options struct {
	FlashRestoreBound time.Duration
	SafetyMaximum     time.Duration
}

type Decision struct {
	ValidityWindow    time.Duration
	FlashRestoreBound time.Duration
	SafetyMaximum     time.Duration
	Source            string
	Samples           int
	RejectedRows      int
	MeanSeconds       float64
	MaxSeconds        float64
	StddevSeconds     float64
}

// MinimumLeaseBudget is the indivisible observation plus flash/restore time;
// the board agent adds its own clock/recovery safety margin before admission.
func (d Decision) MinimumLeaseBudget() time.Duration {
	return d.ValidityWindow + d.FlashRestoreBound
}

// Decide applies the existing hilpolicy max-plus-variability estimator only
// to comparable successful measurements. Until five remain, HIL_TIMEOUT_S is
// the fallback, otherwise 30s. HIL_TIMEOUT_S is NOT silently interpreted as
// a hard maximum; HIL_MAX_TIMEOUT_S and/or fixture SafetyMaximum are caps.
func Decide(ctx context.Context, spec Spec, workload Workload, source ObservationSource, options Options) (Decision, error) {
	if spec.Mode == "" || spec.Path == "" || workload.ManifestPath != spec.Path ||
		workload.Mode != spec.Mode || workload.BoardModel == "" || workload.FixtureRevision == "" ||
		workload.ProfileSHA256 == "" || workload.ProgramFamily == "" ||
		options.FlashRestoreBound <= 0 || options.FlashRestoreBound > time.Hour ||
		options.SafetyMaximum < 0 || options.SafetyMaximum > time.Duration(hilpolicy.MaximumSeconds)*time.Second ||
		(spec.TimeoutDeclared && (spec.TimeoutSeconds < 1 || spec.TimeoutSeconds > hilpolicy.MaximumSeconds)) ||
		(spec.SafetyMaximumSeconds < 0 || spec.SafetyMaximumSeconds > hilpolicy.MaximumSeconds) {
		return Decision{}, ErrInvalidManifest
	}
	fallback := hilpolicy.DefaultSeconds
	if spec.TimeoutDeclared {
		fallback = spec.TimeoutSeconds
	}
	cap := time.Duration(hilpolicy.MaximumSeconds) * time.Second
	if spec.SafetyMaximumSeconds > 0 && time.Duration(spec.SafetyMaximumSeconds)*time.Second < cap {
		cap = time.Duration(spec.SafetyMaximumSeconds) * time.Second
	}
	if options.SafetyMaximum > 0 && options.SafetyMaximum < cap {
		cap = options.SafetyMaximum
	}
	if cap < time.Duration(fallback)*time.Second {
		return Decision{}, fmt.Errorf("%w: safety cap is below fallback observation window", ErrInvalidManifest)
	}
	var rows []HistoricalObservation
	if source != nil {
		var err error
		rows, err = source.Observations(ctx, workload)
		if err != nil {
			return Decision{}, err
		}
	}
	if len(rows) > maxHistoryRows {
		return Decision{}, fmt.Errorf("%w: too many historical rows", ErrInvalidHistory)
	}
	valid := make([]hilpolicy.Observation, 0, len(rows))
	rejected := 0
	for _, row := range rows {
		if row.Workload != workload || !row.Succeeded || !row.EvidenceComplete || row.TimedOut {
			rejected++
			continue
		}
		if row.Duration <= 0 || row.Duration > time.Duration(hilpolicy.MaximumSeconds)*time.Second {
			return Decision{}, fmt.Errorf("%w: invalid successful duration", ErrInvalidHistory)
		}
		valid = append(valid, hilpolicy.Observation{Duration: row.Duration})
	}
	choice, err := hilpolicy.Choose(spec.TimeoutSeconds, spec.TimeoutDeclared, valid)
	if err != nil {
		return Decision{}, errors.Join(ErrInvalidHistory, err)
	}
	window := time.Duration(choice.Seconds) * time.Second
	sourceName := choice.Source
	if window > cap {
		window = cap
		sourceName += "-capped"
	}
	return Decision{ValidityWindow: window, FlashRestoreBound: options.FlashRestoreBound,
		SafetyMaximum: cap, Source: sourceName, Samples: choice.Samples,
		RejectedRows: rejected, MeanSeconds: choice.MeanSeconds,
		MaxSeconds: choice.MaximumObservedSeconds, StddevSeconds: choice.StddevSeconds}, nil
}
