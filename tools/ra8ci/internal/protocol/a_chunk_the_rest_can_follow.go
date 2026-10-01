// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

package protocol

// checkChunkLeavesRoomForTheChunksThatFollow holds one chunk against the
// close it is being read under: not only that it ends inside the artifact,
// but that what is left over is exactly what the chunks after it can carry.
//
// ArtifactChunk.Validate already bounds an offset against its own sequence
// from the front (at least one byte per earlier chunk, at most a full chunk
// each). That argument says nothing about the end, because a chunk alone does
// not know how long its artifact is. A manifest does, and reading the same
// argument backwards is what Covers was missing: after this chunk's payload,
// TotalBytes-Offset-len bytes remain, and FinalSequence-Sequence chunks remain
// to carry them, so those bytes must fit between one apiece and a full chunk
// apiece.
//
// The case that matters most falls out of it: for the final chunk no chunks
// remain, so no bytes may remain either, and a manifest closing 4 MiB over a
// single chunk that carried 18 bytes is refused here rather than accepted as
// coverage of an upload that never finished.
func checkChunkLeavesRoomForTheChunksThatFollow(manifest ArtifactManifest, chunk ArtifactChunk, payload int64) error {
	remaining := manifest.TotalBytes - chunk.Offset - payload
	following := manifest.FinalSequence - chunk.Sequence
	if remaining < following || remaining > following*MaxArtifactChunkBytes {
		return ErrInvalid
	}
	return nil
}
