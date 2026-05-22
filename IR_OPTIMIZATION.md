# IR optimization

The driver optimizes verified typed IR before dumps and code generation.
`-O1` enables the pass and is the default; `-O0` emits the original lowered IR.
All output modes use the same module, verified before and after optimization.
Failure stops compilation. `--debug` reports transformation counts.

The pass repeats these transformations until stable:

- Constant folding of integer arithmetic, comparisons, logical operations,
  unary operations, casts and finite floating-point expressions.
- Constant and copy propagation through local storage and SSA operands.
  Forward dataflow meets predecessor facts at joins and converges across loops.
- Integer identities such as x+0, x-0, x*1, x/1, x-x and x*0;
  double negation and identity casts. Operand effects remain independently live.
- Constant branches become jumps; unreachable blocks disappear and PHIs with
  one remaining predecessor become their incoming value.
- Dead value elimination retains effectful or potentially trapping instructions.
- Backward CFG liveness removes dead and overwritten stores to unescaped locals.
  Declarations disappear only after every storage reference disappears.
- Address/dereference cancellation and repeated address/load simplification
  within a basic block and memory epoch.

Values, labels and call arguments are compacted so removed values no longer
inflate stack frames. Source spans survive. Synthesized literals store bits in
the IR rather than modifying tokens or the source AST; `dmm-ir-v3` exposes them.

## Semantic boundaries

Integer folding models 64-bit virtual arithmetic with unsigned wraparound,
avoiding host signed-overflow undefined behavior. Division by zero and
INT64_MIN/-1 remain runtime operations. The pass preserves full local virtual
slots and typed-width indirect writes, including upper bytes of partial stores.

Floating folding preserves float/double precision and signed zero. Algebraic
rewrites for unknown floating values are excluded because NaN, infinity and
signed zero invalidate integer identities. Non-finite arithmetic results and
invalid float-to-integer conversions remain backend operations.

Taking a local's address excludes it from storage dataflow and dead-store
elimination while that escape remains in IR. Indirect writes invalidate memory
facts; calls, writes, allocation and release end load-reuse epochs. Heap/field
stores remain. Unused bounds checks and dereferences remain unless a specific
rewrite proves an access redundant. There is no whole-program alias analysis.

## Validation

`ir_optimizer_unit` checks transformed IR, CFG joins, loop backedges, PHI repair,
copy/store elimination, pointer aliasing, load reuse, overflow traps, bounds
accesses, floating identities and idempotence.
`ir_optimization_execution` compares -O0 and -O1 executable output across the
execution/regression corpus and verifies unused divisions and out-of-bounds
accesses still fail. Corpus tests also run optimized assembly, objects and
internally linked executables. IR fuzzing optimizes before verifier mutations.

Local acceptance: Windows 28/28 CTest suites and a strict GCC compiler build.
Linux, sanitizer and Clang fuzz execution remain CI checks.
