# libs/if -- `fw_fs` vs `ra8_io_vfs` vs `ra8_fs`

Three things in this tree will open a file for you. They are not alternatives
that happen to look alike; they sit in a stack, and the reason to pick one over
another is which of the three you are willing to depend on.

`ra8_fs` is the **filesystem itself**: it formats a volume, walks directories
and reads and writes blocks (`ra8_fs_format`, `ra8_fs_open`). It knows FAT and
exFAT. `ra8_io_vfs` is the **mount table** over it: a composition root mounts a
device under a name, and from then on the prefix of a path selects the medium,
so `sd:/BOOKS/A.RBK` and `ram:/A.RBK` go to different places through one call
(`ra8_io_vfs_file_open`, `ra8_io_vfs_rename`). `fw_fs`, this directory, is the
**port**: a vtable an application programs against so the filesystem underneath
it can be swapped for a host POSIX tree, a RAM fake, or a contract harness
without the application changing.

**Which do I use?** Application and library code that just needs a file should
take `fw_fs`, because that is what a test can substitute. Code composing a
board at startup mounts devices with `ra8_io_vfs`. Only the filesystem layer
itself and the things bringing a volume into existence should name `ra8_fs`.

`libs/if_ra8_vfs` is the one binding between the top and the bottom of that
stack: `fw_fs_ra8_vfs_init()` fills a `fw_fs_t` whose calls land on
`ra8_io_vfs`. It is a binding, not a fourth filesystem, and nothing should
include it except a composition root and the tests that pin its guards.

## The part that is genuinely duplicated today

`fw_fs` carries a staged-publication transaction -- write into a hidden
sibling, validate the closed artifact, publish it with one rename
(`fw_fs_transaction_begin`, `fw_fs_transaction_commit`, gated by
`k_fw_fs_cap_transactions`, with `fw_fs_transaction_policy_t` distinguishing
create-new from atomic replacement). `apps/shared_libs/mdl_storage_vfs`
implements the same dance a second time, directly on `ra8_io_vfs`, for the
media-download coordinator.

That is not an intentional split. It is issue #762, and until it resolves,
**new consumers of staged publication should take `fw_fs_transaction_*`**: it
is the richer of the two contracts and the one with a substitutable backend.

One thing that looks like a fourth filesystem and is not: `ra8_ftl` is a
flash translation layer beneath a volume, not a way to open a file.

<!-- disambig
this: libs/if
that: libs/ra8_io
that: libs/ra8_fs
that: libs/if_ra8_vfs
symbol: fw_fs_transaction_begin
symbol: fw_fs_transaction_commit
symbol: fw_fs_transaction_policy_t
symbol: k_fw_fs_cap_transactions
symbol: fw_fs_open
symbol: fw_fs_ra8_vfs_init
symbol: ra8_io_vfs_file_open
symbol: ra8_io_vfs_rename
symbol: ra8_fs_format
symbol: ra8_fs_open
users: ra8_fs = 40
users: ra8_io = 31
users: fw_if_fs = 1
users: if_ra8_vfs = 1
files: libs/if/src/*.c = 3
files: libs/if/inc/*.h = 4
files: libs/if_ra8_vfs/src/*.c = 1
files: libs/ra8_fs/src/*.c = 32
files: libs/ra8_io/src/*.c = 25
-->

## The OSAL seam: `fw_os`

`fw_os.h` is child (c) of epic #692, the OS port of #693. Portable libraries
state what they need of an operating system there; a binding chosen by the
composition root supplies it. Nothing above the header names ThreadX.

The surface is derived from what this tree actually calls, not from what an
RTOS offers. Two facts from the measurement below shaped it.

The coupling is almost entirely in `examples/`. Of the 76 first-party files
that call `tx_*`, only five sit in `libs/`: `ra8_wdt_supervisor` (header and
source), `ra8_modem_at`, `ra8_core/src/ra8_time.c`, and the `ra8_fs` seam
header. So the seam's job is to free those five and give the examples one thing
to call, not to wrap ThreadX completely.

The surface is small. Threads, mutexes, semaphores and a clock read cover
nearly all of it. Queues appear in two files and byte pools in four, so queues
are capability-gated behind `FW_OS_HAS_QUEUE` and declined by default, and byte
pools are not in the contract at all: four callers is not enough evidence to
fix an allocator shape into a portable port, and three of the four are USB
examples that could take caller-owned memory instead.

Three design calls worth naming, since they are choices rather than readings:

- **Milliseconds, not ticks.** `tx_time_get` returns ticks and the tick rate is
  an RTOS build constant, so a tick count means nothing to a portable caller
  without a second fact. Durations and instants are milliseconds;
  `fw_os_tick_hz` is there for the callers that genuinely need the resolution.
- **A four-level priority band, not a number.** RTOS priority scales disagree
  on direction and width. Four levels is what this tree's threads actually
  distinguish, and a caller needing finer control is expressing a scheduling
  policy that belongs in the composition root.
- **ThreadX's preemption threshold, time slice, trace hooks and FPU
  enable/disable pair are deliberately absent.** They are real features with no
  portable meaning, and a seam carrying them is a ThreadX header with a new
  prefix. A binding that wants them exposes them in its own binding header,
  which the composition root may name, because it already knows which RTOS it
  picked.

The contract carries its own storage-size defaults behind `#ifndef` rather than
including a per-binding capability header, and a binding raises one from its
own build if its control block does not fit. `src/fw_os_contract.c` is a translation unit with
no code in it whose only job is to be compiled by the ordinary
`libs/if/src/*.c` discovery, so the contract cannot rot the way `arch/arch.h`
did while nothing fed it to a compiler.

Two bindings satisfy the seam today, and neither is a caller:

| Binding | Where | What it is for |
|---|---|---|
| Host test | [`tests/support/src/fw_os_host_test.c`](../../tests/support/src/fw_os_host_test.c) | Single-threaded, runs in the host unit-test build. Waits never block and a created thread is run by hand, so the conformance vectors can drive every failure answer without a scheduler. |
| Eclipse ThreadX | [`port/threadx/src/fw_os_threadx.c`](../../port/threadx/src/fw_os_threadx.c) | The real one. Places a `TX_THREAD` / `TX_MUTEX` / `TX_SEMAPHORE` inside the caller's storage and forwards to `tx_*`. Declines the queue block. |

Both decline `FW_OS_HAS_QUEUE`, which is what an optional surface is for. The
ThreadX binding is the one that proves the storage sizes were not guessed: it
static_asserts a real `TX_THREAD` into `K_FW_OS_THREAD_STORAGE_WORDS`, so the
default stops compiling the day it stops being big enough.

## What still reaches an RTOS directly

The seam's progress bar. Every row falls as callers move onto `fw_os`; a row
that grows means a new direct reach-in landed.

Each row counts files with a real call, not files that mention the name: the
patterns are anchored past a leading comment marker, so the doxygen `@retval`
prose in `ra8_wdt_supervisor.h`, the `tx_acquire`/`tx_release` example in
`ra8_fs_seams.h` and the `tx_kernel_enter()` note in `ra8_time.c` are not
counted as callers. The filter is a line filter, so it drops a comment line
and not a comment block's continuation text that starts with a word, which
means these counts are an upper bound rather than an exact one.

Under `libs/`, exactly one file still calls ThreadX: `ra8_wdt_supervisor.c`,
for `tx_mutex_*`, `tx_thread_*` and `tx_time_get`. The rest of the count is
examples, apps and tests, which is where the migration ends rather than
starts.

| Direct RTOS use | First-party files |
| --- | --- |
| `tx_thread_ callers` | 64 |
| `tx_mutex_ callers` | 2 |
| `tx_semaphore_ callers` | 8 |
| `tx_queue_ callers` | 1 |
| `tx_byte_ callers` | 2 |
| `tx_api.h includers` | 80 |
| `ra8_systick.h includers` | 4 |

## How the numbers here are measured

Every count above is one entry in the block below: the claim it backs and the
command that produces it. `scripts/checks/check_measured_counts.py` re-runs all
of them, so a number here cannot drift from the tree without failing, in either
direction.

```sh
# MEASURED BLOCK -- re-run by scripts/checks/check_measured_counts.py
# tx_thread_ callers -- 64 file(s)
grep -rlE '^[^*/]*\btx_thread_[a-z_]+\(' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
# tx_mutex_ callers -- 2 file(s)
grep -rlE '^[^*/]*\btx_mutex_[a-z_]+\(' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
# tx_semaphore_ callers -- 8 file(s)
grep -rlE '^[^*/]*\btx_semaphore_[a-z_]+\(' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
# tx_queue_ callers -- 1 file(s)
grep -rlE '^[^*/]*\btx_queue_[a-z_]+\(' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
# tx_byte_ callers -- 2 file(s)
grep -rlE '^[^*/]*\btx_byte_[a-z_]+\(' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
# tx_api.h includers -- 80 file(s)
grep -rlE '#[ \t]*include[ \t]+[<"]tx_api\.h[>"]' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
# ra8_systick.h includers -- 4 file(s)
grep -rlE '#[ \t]*include[ \t]+"ra8_systick\.h"' libs apps examples tests --include=*.c --include=*.h | grep -v /third_party/ | wc -l
```
