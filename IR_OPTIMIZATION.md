# IR optimization

`-O1` is the default and enables AST and IR optimization. `-O0` preserves the typed AST for lowering and emits the
original lowered IR. The driver finishes semantic analysis and writes any requested AST dump before the AST pass, so
diagnostics and typed source dumps still cover the complete source tree. All output modes use the same IR module,
verified before and after IR optimization. Failure stops compilation. `--debug` reports transformation counts.
`--dump-ir-before-opt` captures IR after AST optimization and before the IR passes.

The typed AST pass folds decisive short circuits, selects `if` arms with known numeric or boolean truth, removes loops
with a known false condition while preserving `for` initializers, and cuts off statements after a guaranteed return,
break, or continue.
It changes function bodies only after semantic validation. Expressions with unknown truth or observable evaluation stay.
It also replaces a counted `while` loop with its final integer updates when the induction start, bound, step, and any
accumulated increment are compile-time constants. The body must contain only those local updates, and the induction and
combined increment must fit the `int` range. IR propagation can then reduce the result to one constant. Loops with calls,
unknown bounds, or potential induction overflow remain loops.

The pass repeats these transformations until stable:

- Constant folding of integer arithmetic, comparisons, logical operations, unary operations, casts and finite
  floating-point expressions.
- Constant and copy propagation through local storage and SSA operands. Forward dataflow meets predecessor facts at
  joins and converges across loops.
- Integer identities such as x+0, x-0, x *1, x/1, x-x and x*0; double negation and identity casts. Operand effects
  remain independently live.
- Constant branches become jumps; unreachable blocks disappear and PHIs with one remaining predecessor become their
  incoming value.
- Jumps through blocks containing only a label and jump are redirected when the destination has no PHI. Unreachable
  intermediate blocks then disappear in the control-flow pass.
- Common subexpressions reuse identical nontrapping computations and stable parameter loads in the same or dominated
  blocks. Fixed-array element values are still loaded independently; only a repeated bounds check is omitted when a
  matching access dominates it and its base and index are the same SSA values.
- Loop-invariant constants, stable parameter loads, and nontrapping calculations move to a unique preheader when every
  operand is available there. Address-taking, stores, and potentially trapping operations prevent the relevant move.
- Dead value elimination retains effectful or potentially trapping instructions.
- Backward CFG liveness removes dead and overwritten stores to unescaped locals. Declarations disappear only after every
  storage reference disappears.
- Address/dereference cancellation and repeated address/load simplification within a basic block and memory epoch.
- Unreachable private top-level functions are removed after call-graph traversal. Public functions, entry points, and
  methods remain available, including methods reached through interface dispatch.

Values, labels and call arguments are compacted so removed values no longer inflate stack frames. Source spans survive.
IR-synthesized numeric literals store bits in instructions rather than changing source tokens; `dmm-ir-v3` exposes them.
For optimized IR, x86 lowering fuses a signed integer comparison used only by the next branch, skips local loads used
solely as store targets, emits direct integer local updates, and lets a branch fall through to an adjacent true block.

## Semantic boundaries

Integer folding models 64-bit virtual arithmetic with unsigned wraparound, avoiding host signed-overflow undefined
behavior. Division by zero and INT64_MIN/-1 remain runtime operations. The pass preserves full local virtual slots and
typed-width indirect writes, including upper bytes of partial stores.

Floating folding preserves float/double precision and signed zero. Algebraic rewrites for unknown floating values are
excluded because NaN, infinity and signed zero invalidate integer identities. Non-finite arithmetic results and invalid
float-to-integer conversions remain backend operations.

Taking a local's address excludes it from storage dataflow and dead-store elimination while that escape remains in IR.
Indirect writes invalidate memory facts; calls, writes, allocation and release end load-reuse epochs. Heap/field stores
remain. Unused bounds checks and dereferences remain unless a specific rewrite proves an access redundant. There is no
whole-program alias analysis.

## Validation

`ir_optimizer_unit` checks transformed IR, CFG joins, loop backedges, PHI repair, copy/store elimination, pointer
aliasing, load reuse, overflow traps, bounds accesses, floating identities and idempotence.
`ir_optimization_execution` compares -O0 and -O1 executable output across the execution/regression corpus and verifies
unused divisions and out-of-bounds accesses still fail. Corpus tests also run optimized assembly, objects and internally
linked executables. IR fuzzing optimizes before verifier mutations.
