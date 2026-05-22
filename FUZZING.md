# Fuzzing

The compiler provides libFuzzer targets for the lexer, parser, semantic
analysis, typed-IR lowering/verifier, and the standalone assembly syntax
converter. Build them with Clang on a Unix-like platform:

```sh
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang \
  -DDMM_BUILD_FUZZERS=ON -DBUILD_TESTING=OFF
cmake --build build-fuzz --parallel
```

Run a source-pipeline target with its seed corpus and the language dictionary:

```sh
./build-fuzz/fuzz_lexer tests/fuzz/corpus/parser -dict=tests/fuzz/dmm.dict
./build-fuzz/fuzz_parser tests/fuzz/corpus/parser -dict=tests/fuzz/dmm.dict
./build-fuzz/fuzz_semantic tests/fuzz/corpus/semantic -dict=tests/fuzz/dmm.dict
./build-fuzz/fuzz_ir tests/fuzz/corpus/ir -dict=tests/fuzz/dmm.dict
mkdir -p build-fuzz/converter-corpus
./build-fuzz/fuzz_syntax_converter build-fuzz/converter-corpus
```

The parser and semantic targets accept arbitrary in-memory bytes and perform no
filesystem imports. The IR target lowers valid inputs, verifies the resulting
module, then mutates non-owning instruction fields and invokes the verifier a
second time. This exercises rejection of malformed opcodes, types, values,
symbols, control-flow targets, and operators without corrupting cleanup state.

All pipeline harnesses cap individual inputs at 64 KiB and buffer diagnostics.
`fuzz_pipeline_smoke` executes their core logic in ordinary test builds so API
and ownership regressions are caught without requiring libFuzzer.

CI runs all five targets with short time limits and uses writable copies of the
seed corpora. Source targets also receive the generic/trait/sum execution fixtures.
The dictionary includes type arguments, trait keywords, `Self`, and match patterns.
