<!--
Copyright (c) 2026 Brighton Sikarskie
SPDX-License-Identifier: MIT
-->

# apps/shared_libs/mdl

The portable core of the comic downloader: everything the product *does*, with
nothing that knows how it was asked to do it. Following a series, the persistent
library state, the page cache, the exporters, verification, progress reporting
and packing all live here -- and so do the site descriptors, because a
descriptor is data the core reads rather than anything a particular product
owns.

It is written against seams rather than against a host. The network is an
injected function-pointer vtable, and storage and streams have the same shape,
so the code that follows a series on a laptop is the code that follows one on
the board. Only the leaf backend changes.

## Why it is not inside the product

A *form* is a way of running the core. The host command line under
`apps/host/mdl` is one; a loadable on-device module is the second.
A core that lived inside one form would have to be reached out of the other by
relative path, which is how a shared dependency turns into a copy -- and two
copies of a downloader are two downloaders that drift.

So the layering is a directory rule, and it is one-way: **nothing under
`apps/shared_libs` may include from `apps/host`, `apps/board/stand_alone`, or
`apps/board/threadx_modules`.**
The proof is mechanical rather than a promise. This directory configures,
builds and runs its own test suite on its own, with no host HTTP library named
anywhere in its listfile, so a dependency belonging to a form cannot reach in
without that standalone build failing.

## What is deliberately absent

No `main()`, no argument parsing, no usage text, no host transport. A file here
that named a host HTTP library, or knew that a command-line flag existed, would
be a form's file sitting in the core's directory. Where the core needs a
decision only a form can make, it takes it as an argument or through a seam --
that is the whole of the arrangement, and it is what lets the same bytes run in
both places.

The device-side transport does live here, because every form that runs on the
board shares it; it is the *host* half that belongs to the host form.
