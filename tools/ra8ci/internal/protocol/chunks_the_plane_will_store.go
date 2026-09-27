// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

// MaxArtifactChunks is the most chunks one artifact may be uploaded in.
//
// The size rules alone do not bound the count: a chunk may carry a single
// byte, so TotalBytes is also the largest FinalSequence the length rules
// admit, and a 4 MiB artifact could be closed as four million chunks. That is
// a count no honest upload produces and no receiver will store: the plane
// refuses a sequence past this number outright (internal/store, the artifact
// chunk write), so a manifest stating more describes an upload that was
// refused, and a chunk numbered past it is a row that will never be written.
//
// The number is restated here rather than imported because internal/store
// depends on this package and not the reverse, and a wire contract that
// admits what the far end refuses is not a contract. It leaves room for the
// largest artifact by a wide margin: 64 MiB needs 256 full chunks, and the
// slack above that covers a collector flushing short at the end of a file.
const MaxArtifactChunks = 1024

// chunkCountThePlaneWillStore reports whether a sequence or a final sequence
// names a chunk the receiving end would keep.
func chunkCountThePlaneWillStore(sequence int64) bool {
	return sequence >= 1 && sequence <= MaxArtifactChunks
}
