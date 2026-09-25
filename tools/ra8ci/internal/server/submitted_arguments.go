package server

import (
	"fmt"

	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/catalog"
	"github.com/bsikar/ra8-firmware/tools/ra8ci/internal/store"
)

// checkSubmittedArgumentsAreBound states, at submission, that a requested
// task's own argv is the argv the plane binds from the names it was sent
// with.
//
// The two travel together in one request and were judged apart: Args was held
// only to "a submitter states no argv", while the argv actually written to
// the row was bound from Values, which that rule never reads. A request could
// therefore state one thing about its arguments and persist another, and
// nothing between the two ever said they agreed. The store guard added for a
// claimed task catches such a row later, when it is handed out; this refuses
// it at the door, which is where the submitter can still be told.
//
// Equality against the binding, not a shape check: the binding IS the rule
// (a positional in declared order, a flag as --name=value), so re-stating it
// here is how the two definitions drift. An absent Args and an empty Args are
// the same statement, "no arguments", because that is the shape every request
// written before Values existed has.
func checkSubmittedArgumentsAreBound(definition catalog.Task, requested taskRequest) error {
	bound, err := definition.BindArguments(requested.Values)
	if err != nil {
		return fmt.Errorf("%w: %s", store.ErrInvalid, err)
	}
	if len(bound) != len(requested.Args) {
		return fmt.Errorf("%w: task %q was submitted with %d argument(s), its values bind %d",
			store.ErrInvalid, requested.Name, len(requested.Args), len(bound))
	}
	for i := range bound {
		if bound[i] != requested.Args[i] {
			return fmt.Errorf("%w: task %q argument %d is not what its values bind",
				store.ErrInvalid, requested.Name, i)
		}
	}
	return nil
}
