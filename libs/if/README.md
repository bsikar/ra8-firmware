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
