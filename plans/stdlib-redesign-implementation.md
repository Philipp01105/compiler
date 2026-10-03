# Standard library redesign: implementation status

This tracks the implementation explicitly authorized after review of
[the audit](stdlib-redesign-audit.md). The audit remains the historical design
snapshot; this file records actual contracts and outstanding migration work.

## Working foundations

- Option/Result and propagation now have their canonical identity in stdlib/core.
  Option defaults to None; Result requires initialization.
- core.Iterator<T> defines next -> Option<T>. Compiler for loops recognize the
  canonical core.Option, independent of the caller's import aliases.
- core.AllocError contains OutOfMemory and CapacityOverflow; success is Ok.
- memory.Shared<T> owns its reference-counted block outside core. Box and Shared
  return Result<...,core.AllocError>. Heap-backed sequence/map owners have
  conditional Send/Sync properties; owned String/Builder support task transfer.

## Working redesigned library

- collections.List<T> and Buffer<T> handle move-only elements, checked getters,
  explicit allocation failure, and value/shared/mutable iteration.
- collections.Map<K,V>/Set<T> use borrowed, noncapturing hash/equality functions.
  Default primitive/string policies use per-owner entropy and SipHash-2-4.
  text.map<V>/set supply owned UTF-8 String policies without an import cycle.
  Replacement retains the stored key and destroys the incoming equivalent key.
  Removal transfers V (Map) or the actual stored T (Set).
- iter supplies compact eager map/filter/enumerate and allocation-free fold.
- text.String and StringBuilder maintain valid UTF-8. Text borrows either an
  owned String or a checked byte descriptor; substring validates boundaries.
  Text/codepoint/split iteration supports embedded NUL. Text comparisons accept
  literals or another Text without conversion allocation.
- text supplies primitive string length/search convenience functions. Byte
  search, splitting, cloning and replacement live in stdlib/bytes.
  Split borrows a checked byte descriptor with an explicit lifetime; no artificial
  destructor is used to force loan retention. The old
  text.StringBuilder and capacity-based factory have been removed.
- text/utf8 and text/utf16 provide small codecs. Filesystem code no longer loads
  Unicode tables or allocates a second byte owner for each directory entry.
- io supplies Reader/Writer, standard handles, checked memory adapters,
  buffered reads/writes, readExact/writeAll/readAll/readToEnd/copy, and Result
  print/println with partial-transfer diagnostics.
- fs uses Error/Entry/SeekOrigin and createDir/removeDir/openDir. Directory.next
  returns Result<Option<Entry>,Error>; EOF is not an error. File implements
  Reader/Writer, with io.Error for resource I/O. readFile/writeFile/readDir and
  createDirs provide common allocating operations with ordinary RAII cleanup.
- path owns output in List<u8>, uses Result allocation errors, and preserves
  lexical POSIX/drive/UNC behavior. Existing callers have been migrated where
  the public filesystem/text/shared contracts changed.

## Implementation corrections to the audit

DMM reserves reserve, string and slice, so use ensureCapacity, fromString and
substring. Struct receiver mutability is inferred; mut func is interface syntax.

List instantiation currently checks non-generic method bodies eagerly. A
one-argument List.contains would instantiate a standard equality policy for
unsupported types even when that method is unused. Default equality is therefore
collections.contains(&list,&value); list.contains(&value,equal) supports arbitrary
elements without copying. A method form remains dependent on deferred checking
or a suitable existing compile-time constraint, not a fictional equality feature.

Text views require a checked byte descriptor or an owned String. A primitive
string stored directly in a Text enum payload did not preserve the required
origin in the compiler's current analysis; that convenience overload is excluded.
Use text's free primitive-string operations, or a descriptor plus text.view(&data).

Checked raw-storage getters and Copy-element views explicitly access self.storage/self.entries.
Receiver-origin mapping in the borrow checker now handles implicit self parameters
separately from ordinary call arguments. The
contract tests ensure their views cannot outlive owners. Do not replace this
with unanchored field access when refactoring.

A private HashPolicy sum selects plain or keyed callbacks; no dummy callback or
allocated policy interface is needed. seededMap/seededSet are explicit policy
constructors for text's String-key factories and other keyed hash policies.

## Remaining work in the approved migration

No migration stage is claimed complete solely because new APIs coexist with old
ones. Complete these before declaring the redesign finished:

1. Migrate remaining root collections, old hash policies/Deque, and users/tests;
   remove obsolete owning containers and Arena only after preserving relevant
   ownership regression coverage.
2. Consolidate Unicode codecs onto the small text codec packages; migrate
   environment and remaining legacy byte owners.
3. Finish numeric/binary/algorithm API consolidation and error naming.
4. Build portable net facade types on the existing reactor and retain its
   cancellation, full-duplex and in-flight ownership guarantees.
5. Consolidate async composition/poll boundaries and shared deadline types.
6. Add process argv startup capture, synchronized env/cwd, and direct run/wait
   with Linux/Windows implementations. Do not replace argv capture with procfs.
7. Move raw allocation/lifetime, atomics and compiler-facing runtime packages
   to explicit low-level/internal paths; update compiler provider/ABI retention.
8. Remove old stdio/root I/O/status APIs, update docs/examples/install contracts,
   and run the final required contracts after the final code change.

## Validation gates

The redesign contract runs at O0/O1, executes on Windows, and compiles ELF/COFF
objects. It covers owned/mutable sequence iteration, eager composition, UTF-8
boundaries/NUL, I/O short transfers, filesystem operations, Map/Set owners,
SipHash vectors, task transfer, and borrowing a Map value past a temporary key.

Rejections cover dangling owner/text views, Map mutation with active entries,
multiple retained mutable entries, captured hash-policy environments, fake
Option identities, mutable iterator escape, and uninitialized Result.

The separate allocation contract injects failure in an isolated installed
stdlib copy. It must preserve contents/borrows, destroy failed incoming owners
exactly once, and leave no allocation outstanding. Existing shared, system,
Unicode/text, iterator and language-gap contracts remain migration gates.

Linux validation follows AGENTS.md: run only relevant checks locally; leave the
full Linux suite to CI.
