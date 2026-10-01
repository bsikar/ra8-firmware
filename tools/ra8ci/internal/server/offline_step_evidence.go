package server

import (
	"fmt"
	"regexp"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/executor"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// Holding a spooled step's log evidence to a shape it could have been
// measured in.
//
// A spooled step carries no logs, by design: the record states two digests
// and two byte counts and nothing else about what the step printed. Those
// four fields are the entire local evidence that a step produced the output
// it did, and offlineInput copied all four into store.LocalStepInput without
// looking at them. executor.runStep takes them from one place, digestWriter's
// digest(), which returns a lowercase hex SHA-256 over everything written and
// the count of those bytes, so a digest that is not that shape, or a negative
// count, was not measured by any run this plane would recognise.
//
// What that costs is the comparison the digests exist for. A later run's
// digest is judged against the one history holds, and a stored value of ""
// or "unknown" is not a digest that fails to match, it is a digest no
// comparison can be made against at all, sitting in a row that reads like
// evidence. Refusing it is the difference between history saying "different
// output" and history saying nothing while looking like it says something.
//
// The digests are held to the same pattern the scaler holds every other
// SHA-256 in this tree to. An empty output is not an exception: the digest of
// nothing is still a digest (e3b0c442...), and that is what the executor
// records for a silent step.
var stepDigestPattern = regexp.MustCompile(`^[0-9a-f]{64}$`)

func checkLocalStepEvidenceIsMeasured(step executor.StepResult) error {
	if !stepDigestPattern.MatchString(step.StdoutSHA256) || !stepDigestPattern.MatchString(step.StderrSHA256) {
		return fmt.Errorf("%w: step %q states a log digest no run could have measured", store.ErrInvalid, step.Name)
	}
	if step.StdoutBytes < 0 || step.StderrBytes < 0 {
		return fmt.Errorf("%w: step %q states %d stdout and %d stderr bytes",
			store.ErrInvalid, step.Name, step.StdoutBytes, step.StderrBytes)
	}
	return nil
}
