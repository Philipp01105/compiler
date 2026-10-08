# Standard-library contracts

The table below maps the standard-library contracts to their validation coverage. Remaining work is tracked in [TODO.md](../../TODO.md).

| Contract | Coverage |
|---|---|
| stdlib_redesign_contract | O0/O1 execution, ELF/COFF objects, ownership/iteration, text boundaries/NUL, complete I/O, filesystem, Map/Set policy and lifetime rejections; internal-package imports rejected |
| stdlib_redesign_allocation_contract | Instrumented fresh installed allocator, first/later failure, unchanged contents, incoming drops, cleanup and env/args/cwd/run failures |
| stdlib_language_gaps_contract | Generic inference, move-only payloads, precise heap-slot loans, List/Map, test-only rings, callbacks, files, guards, advanced pollers and canonical async composition |
| stdlib_borrowed_algorithms_contract | Move-only borrowed predicates, mutable callbacks, short circuit and alias/lifetime rejection |
| stdlib_process_contract | Startup argv ownership, empty/quoted/non-ASCII args, inherited/snapshot environment, cwd, direct run and wait/reap; O0/O1 and cross-target objects |
| network_runtime_contract | Portable facade and raw engine, loopback TCP/UDP/DNS, EOF/truncation, full duplex, cancellation acknowledgement and retained buffers; spawn with graph-owned resources |
| unicode_conformance_contract | Official normalization rows/invariants, full casing mappings and scalar codec round trips |
| shared_language_contract | Payload-dependent Send/Sync, atomic refcounts, allocation failure and retained getter origins |
| stdlib_package_contract | Fresh installed source resolution, provider retention, target filtering and no runtime bundle |

Private TestRing and test factories retain wraparound, collision, failure/retry and stored-loan regression coverage after removing public Deque/Arena/root containers. They are test support files, not standalone executable fixtures. Remaining dynamic index/key equivalence and partial-place ownership limits are compiler constraints documented in [the language specification](../../LANGUAGE_SPEC.md).

For a focused Windows check, build the compiler and select the affected ctest contracts. Under Linux, follow AGENTS.md: run only necessary checks and leave the full suite to CI. Normal resource APIs use the compiler-reported platform link profile; raw-only fixtures retain freestanding validation.
