# Compiler roadmap

This document contains only remaining work and forward-looking design decisions.
Completed migrations, historical checkpoints, acceptance logs, and already-implemented
language/runtime behavior are intentionally omitted.

## Priorities

| Priority | Goal | Why |
|----------|------|-----|
| P2 | Confirm hosted Linux CI results | Observe native execution, ABI, runtime/EOF, sanitizer and Clang fuzz jobs on GitHub Actions. |
| P3 | Selfhosting foundations | Add the remaining low-level language/library capabilities needed to implement the compiler in DMM. |
| P3 | Selfhosting | Port compiler stages incrementally and verify staged compiler output against the C bootstrap compiler. |

## P2 remaining validation

- [ ] Confirm the next hosted Linux CI run, including the native corpus, C ABI,
  runtime/EOF tests, sanitizer execution, and all five Clang fuzz targets.

## Selfhosting

Before porting compiler stages, add the minimum capabilities required by compiler
code itself:

- [ ] type-size/alignment queries and typed allocation over core byte regions;
- [ ] practical byte buffers/slices and dynamic collections;
- [ ] dependency fetching, version selection, module cache and checksum/lockfile support;
- [ ] explicit package initialization order and runtime variable initializers;
- [ ] stable C export declarations and separate library linking workflows;
- [ ] explicit file-error handling;
- [ ] deterministic artifact writing.

Then port incrementally:

```text
C bootstrap compiler
        ↓
DMM lexer/parser
        ↓
DMM semantic analysis
        ↓
DMM typed IR
        ↓
DMM backend/linker
        ↓
staged-build comparison
```

- [ ] Bootstrap stage 1 with the C compiler.
- [ ] Compile later compiler stages with DMM.
- [ ] Compare deterministic outputs and regression results between stages before
  retiring the C bootstrap path.
