# GeneScript LLVM Backend: Architecture and Runtime ABI

Phase 2 compiles GeneScript ahead of time. The front end (Flex lexer, Bison parser,
`SemanticAnalyzer`) is the same one the Phase 1 interpreter uses. After semantic
analysis, the backend writes **textual LLVM IR** (`.ll`), and Clang compiles and
links that IR with a small C runtime into a native executable.

```
source.gene ─► lexer/parser ─► AST ─► SemanticAnalyzer ─► CodeGenerator ─► out.ll
                                       (types, return types)                   │
                                runtime/gs_runtime.c ──► clang ◄───────────────┘
                                                           │
                                                           ▼
                                                    native executable
```

**Status (Part 3 of 4):** supported constructs are gene definitions, `gene nucleus()` as
the entry point, sequence/base literals (including `""`), declarations (`e -> dna x`),
assignments (`e -> x`), variable reads, `report`, `head`, `tail`, `transcribe`, `fuse`,
`~`, `s@i` (literal or loop counter), `#s` as a `cycle` bound, `=`, `<`, `>`, `<=`, `>=`,
`bound` / `unbound`, `both` / `either` / `lacks`, `when` / `otherwise when` /
`otherwise`, `cycle i across s`, `cycle i from a to b`, `grow until`, `halt` and `skip`
(§8). `express` and gene calls are Part 4; using them stops compilation with
`<file>:<line>: <construct> is not supported by the LLVM backend yet.`

Semantic errors are reported by the shared front end, with the same text as Phase 1,
before any IR is written. The backend never writes a `.ll` for an invalid program.

## 1. Building and running (development flow)

```bash
make llvm                                    # builds bin/genescript-llvm (make/make test unchanged)
bin/genescript-llvm prog.gene -o prog.ll     # IR to a file (stdout without -o)
clang prog.ll runtime/gs_runtime.c -o prog   # native executable
./prog
make test-llvm                               # tests/llvm/*.gene: IR → verify → link → run → compare
```

`make test-llvm` needs `clang` on `PATH`; set `CLANG=clang-18` or similar to pick a
version. When `llvm-as` is installed, the test also runs the IR through it as a
stand-alone verifier. Part 4 will fold all of this into the final `genec` CLI.

The test runner (`tests/llvm/run_llvm_tests.sh`) has four sections:

| Section | What it checks |
|---|---|
| `[runtime]` | `tests/llvm/runtime_unit.c` exercises the C runtime directly under ASan, including functions no valid Part 2 program reaches (`gs_seq_len`) and the refcount rules |
| `[valid]` | Each `tests/llvm/*.gene` goes IR → `llvm-as` → `clang -Werror` → link → run. Output plus exit code must equal `expected/<name>.out` **and** the interpreter. The program is linked a second time against an AddressSanitizer build of the runtime and must produce identical output with no use-after-free, double-free or leak report |
| `[errors]` | Each `tests/llvm/errors/*.gene` must be rejected with exit 1 and no `.ll` written. Diagnostics must equal `errors/<name>.err` **and** the interpreter's |
| `[phase1]` | Every `tests/*.gene` goes through the backend. Programs using constructs not lowered yet are listed as SKIP; every other program must match the interpreter |

`GS_NO_ASAN=1` skips the sanitizer runs.

## 2. Code layout

| File | Role |
|---|---|
| `include/codegen.h`, `src/codegen.cpp` | `IRContext` (generic IR text builder) and `CodeGenerator` (AST lowering) |
| `src/llvm_main.cpp` | Development driver `bin/genescript-llvm` |
| `runtime/gs_runtime.h`, `runtime/gs_runtime.c` | C runtime and the C side of the ABI |
| `tests/llvm/` | Backend tests and expected outputs |

### IRContext (GeneScript-agnostic)

- **Module level:** `internSequence(kind, bases)` returns one shared immortal global per
  distinct literal. `declareExternal(sig)` prints each runtime `declare` once.
- **Functions:** `beginFunction` / `endFunction`. Every function has its own buffers:
  `alloca`s go into the `entry` block (`emitAlloca`), so LLVM's mem2reg can promote them,
  and instructions go into the body.
- **Names:** temporaries are named `%t0, %t1, …` and labels are `<hint>.<n>`. Both
  counters restart for each function. Named values avoid LLVM's strict numbering of
  unnamed `%0, %1, …`.
- **Blocks:** `emit` adds an instruction, `emitTerminator` closes the block, and
  `startBlock(label)` opens a block, inserting a fall-through `br` when the previous block
  is still open. Any instruction emitted after a terminator goes into a fresh `dead.N`
  block, so the IR stays well-formed.
- **Scopes:** `pushScope`, `popScope` (which returns that scope's variables for cleanup),
  `declareVar`, `lookupVar` (innermost scope first), and `liveVars` (every variable that
  needs releasing on an early `express`).

### CodeGenerator

- `genGene` lowers one gene to `define ptr @gs_gene_<name>(ptr %p.<param>, …)`.
- `genEntryPoint` emits the C `main`, which calls `@gs_gene_nucleus`, releases its result,
  and returns `0`.
- `genStmt` and `genExpr` dispatch on the AST node type. An expression lowers to an
  `IRValue { ref, type, owned }`.
- Semantic results the backend relies on: `ExprAST::deduced_type` (for example, the
  DNA/RNA type of a literal, inferred from its context) and
  `SemanticAnalyzer::gene_signatures[...].return_type`.

## 3. Value representation

| GeneScript | LLVM IR | Notes |
|---|---|---|
| `dna`, `rna`, single base `'G'` | `ptr` to `GsSeq` | A base is a sequence of length 1 |
| boolean condition (`bound`, comparisons) | `i1` | Never stored or passed (language rule) |
| integer (loop counters, bounds, `#s`, `@` index) | `i64` | Never stored or passed (language rule) |

```c
typedef struct GsSeq {
    int64_t refcount; /* offset 0  : >0 heap object, <0 immortal literal */
    int64_t len;      /* offset 8  : number of bases                     */
    int32_t kind;     /* offset 16 : 1 = DNA, 2 = RNA                     */
    char    data[];   /* offset 20 : len bases + NUL                      */
} GsSeq;
```

A literal is a private constant global with exactly this layout. `_Static_assert`s in
`gs_runtime.c` enforce the layout:

```llvm
@.seq.0 = private unnamed_addr constant { i64, i64, i32, [5 x i8] }
          { i64 -1, i64 4, i32 1, [5 x i8] c"ATGC\00" }, align 8
```

Sequences are **immutable**: an operation always produces a new `GsSeq`. The DNA/RNA type
is fixed at compile time by semantics. `kind` is carried at runtime for diagnostics and
for operations whose result depends on it.

## 4. Calling convention

- Gene `g` becomes `@gs_gene_g`. The prefix keeps a gene called `main`, `puts`, etc. from
  colliding with C symbols.
- **Every gene returns `ptr`.** Semantics gives every gene a sequence return type and
  defaults to `dna` when no `express` fixes it. A gene that reaches `stop` without an
  `express` returns `""`, as in Phase 1.
- Every parameter is a `ptr` (only `dna`/`rna` parameters exist).
- The C `main` is generated in the IR. The runtime does not define `main`.
- The module declares the host's `target triple` (`x86_64-pc-linux-gnu` or
  `aarch64-unknown-linux-gnu`, from `IRContext::hostTargetTriple`). Without it, clang
  emits `-Woverride-module`, which the tests treat as an error. No `datalayout` is
  emitted; clang fills it in for the triple.

## 5. Ownership and memory lifetime

GeneScript uses **reference counting**. Literals are immortal (`refcount < 0`); retain and
release are no-ops on them, so they can appear anywhere at no cost.

Every `IRValue` is either **owned** (+1; the holder must release it exactly once) or
**borrowed** (valid only for a limited time; never released).

| Producer | Result |
|---|---|
| Literal | borrowed (immortal, always valid) |
| Variable read | borrowed (valid until that variable is next written) |
| Any runtime function returning `GsSeq*` | **owned** |
| Gene call | **owned** |

| Consumer | Rule |
|---|---|
| Runtime function arguments | **borrowed**. The caller keeps ownership and releases owned temporaries after the call (`releaseIfOwned`). |
| Gene call arguments | **borrowed** by the callee. The caller releases owned temporaries after the call returns. |
| Gene entry | Retains every parameter into its own local slot, so the slot owns +1 and the parameter can be reassigned. |
| Declaration `e -> dna x` | Stores an owned reference. Retain `e` first if it is borrowed. |
| Assignment `e -> x` | Make `e` owned **first**, then load and release the old value, then store. This order makes `tail(x) -> x` safe. |
| `express e` | Make `e` owned, release every `liveVars()` slot, `ret ptr`. |
| Scope exit (`stop` of a block or gene) | Release every sequence variable that `popScope()` returns. |
| Loops: `halt` / `skip` | Release the variables of the scopes being left (up to the loop scope) before the `br`. |

A borrowed variable read is safe while expressions are evaluated, because GeneScript
expressions cannot write variables: a gene call only touches the callee's own locals.

Every sequence slot is `alloca`'d in the `entry` block and initialized to `null` there,
so a scope-exit release of a slot whose declaration never ran (possible once Part 3 adds
branches) is a harmless `gs_seq_release(NULL)`. A literal stored into a variable needs no
retain, because it is immortal (`makeOwned` skips `@` globals).

Worked example (`g -> dna keep` then `tail(g) -> g`):

```llvm
%t1 = load ptr, ptr %v.g.0               ; borrowed read of g
%t2 = call ptr @gs_seq_retain(ptr %t1)   ; keep needs its own +1
store ptr %t2, ptr %v.keep.3
%t4 = load ptr, ptr %v.g.0
%t5 = call ptr @gs_seq_tail(ptr %t4)     ; new value (owned) computed FIRST
%t6 = load ptr, ptr %v.g.0
call void @gs_seq_release(ptr %t6)       ; then the old value is dropped
store ptr %t5, ptr %v.g.0
```

## 6. Runtime functions (current)

| Function | Ownership | Purpose |
|---|---|---|
| `void gs_report(const GsSeq*)` | borrows | `report`: prints the bases, `\n`, then flushes (like `std::endl` in Phase 1) |
| `GsSeq* gs_seq_retain(GsSeq*)` | +1, returns its argument | Ownership primitive |
| `void gs_seq_release(GsSeq*)` | -1, frees at 0 | Ownership primitive; NULL and immortal are no-ops |
| `GsSeq* gs_seq_alloc(i32 kind, const char*, i64 len)` | returns owned | Constructor used by the operations below |
| `int64_t gs_seq_len(s)` | borrows | `#s` (IR `i64`) |
| `GsSeq* gs_seq_head(s)` / `gs_seq_tail(s)` | borrows, returns owned | `head` / `tail`; `""` when there is no base to return |
| `GsSeq* gs_seq_at(s, i64 i)` | borrows, returns owned | `s@i`; `""` when out of range (Phase 1 behaviour) |
| `GsSeq* gs_seq_fuse(a, b)` | borrows both, returns owned | `fuse a b`; safe when `a == b` |
| `GsSeq* gs_seq_revcomp(s)` | borrows, returns owned | `~s` (DNA result) |
| `GsSeq* gs_seq_transcribe(s)` | borrows, returns owned | `transcribe(s)` (RNA result) |
| `int32_t gs_seq_equal(a, b)` | borrows both | `=`; codegen turns it into `i1` with `icmp ne 0` |
| `int32_t gs_seq_compare_bases(a, b, i64 line)` | borrows both | Ordering operators: returns `<0`, `0` or `>0` and codegen applies `icmp slt/sgt/sle/sge 0`. If an operand is not exactly one base, prints Phase 1's `Runtime Error (line N): …` and calls `exit(1)` |

Every operation returns a **fresh** heap object, even for `""`. Returning the input
(for example, `tail` of a one-base sequence) would require retaining a borrowed `const`
argument. That optimisation is possible, but the ABI does not guarantee it, so callers
must not depend on object identity.

Booleans (`i1`) and integers (`i64`) are plain SSA values with no ownership. Integer
literals lower to immediate constants (`s@3` → `call @gs_seq_at(ptr %s, i64 3)`).

### Phase 1 compatibility: `report <condition>`

Semantics rejects `report` only for integers, so `report g = "A"` is accepted. The
interpreter prints an empty line for it, because a boolean `Value` has an empty `str_val`.
The backend reproduces this: it evaluates the condition, so the runtime single-base check
still fires, then reports `""`. The proposal says only sequences may be printed, so the
team may want to make semantics reject this. That would change Phase 1 behaviour, so it
was left as is.

Conventions for new runtime functions: use the `gs_` prefix, **borrow** every `GsSeq*`
argument, and return **owned** results. Runtime errors print
`GeneScript runtime error: …` (or a message matching Phase 1) to stderr and `exit(1)`.
Declare the new function in `gs_runtime.h`, add its IR `declare` string next to `RT_*` in
`codegen.cpp`, and list it in the table above.

## 7. Remaining work (Part 4)

- `express` and user-defined gene calls (the calling convention in §4–5 is already
  fixed). `express` inside a loop must release `liveVars()`, which already includes each
  enclosing `cycle across` loop's held sequence (§8).
- Top-level statements outside any gene (only used by `error.gene`, which semantics
  rejects anyway).
- The final `genec` CLI that drives clang, plus running every Phase 1 test through the
  native path (they are now blocked only by `express`).
- Known Phase 1 difference: the interpreter keeps one flat set of variables per gene call,
  so an inner declaration or loop counter that reuses an outer name overwrites the outer
  one. The backend uses block scope, which matches the proposal ("counters … exist only
  within their loops"). No test reuses a name; the team should either give the
  interpreter block scopes or make semantics reject such reuse.

## 8. Control flow (Part 3)

All control flow uses ordinary basic blocks with `br i1` / `br label`. Labels come from
`IRContext::newLabel` (`when.then.3`, `across.cond.7`, …), so they are unique within the
function. `IRContext::currentBlock()` gives the block name needed for `phi` edges.

| Construct | Blocks | Notes |
|---|---|---|
| `when c … otherwise when … otherwise … stop` | `when.then.N`, `when.else.N`, `when.end.N` | Conditions are evaluated in order, only until one is true |
| `cycle i across s` | `across.cond`, `across.body`, `across.step`, `across.end` | `s` is evaluated once and retained in a hidden variable for the whole loop; `len(s)` is read once. `i = 0 … len-1` |
| `cycle i from a to b` | `count.cond`, `count.body`, `count.step`, `count.end` | `a` and `b` are evaluated once; `i = a … b-1` (`icmp slt`), zero passes when `a >= b` |
| `grow until c` | `grow.cond`, `grow.body`, `grow.end` | `c` is tested before every pass; the loop ends when it becomes true |
| `both` / `either` | `both.rhs`, `both.end` (`either.*`) | Short-circuit: the right side runs only when needed; the result is a `phi i1` |
| `lacks c` | none | `xor i1 %c, true` |
| `halt` / `skip` | none | `br` to the innermost loop's end / step (`grow.cond` for `grow until`) |

**Counters** are `i64` values in an `alloca`'d slot in a *loop scope* around the body.
Semantics makes them read-only, so the slot is also the loop index; clang's mem2reg turns
it into a `phi` at `-O1` and above.

**Scopes and cleanup.** Every block (each branch, `otherwise`, each loop body) is its own
scope. When a block falls through, its sequence variables are released. `halt` and `skip`
release the variables of every scope from the loop body inwards (`IRContext::varsFrom`)
before the `br`. The loop scope itself (counter and held `across` sequence) is released
at the loop's end block, which every `halt` reaches.

**Front-end rules added for Part 3** (shared with the interpreter, which rejects the same
programs): conditions of `when`, `grow until`, `both`, `either` and `lacks` must be
boolean, and `=`, `fuse`, `head`, `tail`, `@` and `cycle … across` need DNA/RNA operands,
not loop counters or `bound`/`unbound`. Before this, Phase 1 silently treated, for example,
`when g` as false.
