# C ABI Runtime Contract Vectors

Select every row matching an exported function's shape. Each failure vector
must assert the public error and that all outputs retain their sentinels.

| API shape | Required C vectors |
| --- | --- |
| Required scalar or aggregate input | valid; null; malformed field; mapped native error |
| Borrowed pointer and length | null/zero; non-null/zero; null/nonzero; non-null/nonzero; above maximum |
| Caller-owned output and capacity | exact capacity; excess capacity; insufficient capacity; null output; null output count |
| Opaque create | success; null output; injected allocation failure; exhausted pool; later success |
| Opaque destroy | live; null pointer slot; null handle; invalid handle; outstanding-resource rejection; repeated teardown |
| Library-owned output | success; every input-span vector; null outputs; injected allocation failure; occupied slot; later success |
| Named release | live allocation; null pointer slot; null value; wrong live pointer; no live allocation; repeated release |
| Callback registration | success; callback error; cancellation; unregister idle/active; reentrancy; destroy ordering |

The fixture's test-only allocation control is intentionally part of its public
C test contract. Production libraries instead keep the equivalent deterministic
failure seam private to their host-test build.
