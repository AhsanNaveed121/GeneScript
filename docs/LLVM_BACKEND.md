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

**Status (Part 1 of 4):** supported constructs are gene definitions, `gene nucleus()` as
the entry point, sequence/base literals, and `report`. Anything else stops compilation with
`<file>:<line>: <construct> is not supported by the LLVM backend yet.`

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

## 6. Runtime functions (current)

| Function | Ownership | Purpose |
|---|---|---|
| `void gs_report(const GsSeq*)` | borrows | `report`: prints the bases, `\n`, then flushes (like `std::endl` in Phase 1) |
| `GsSeq* gs_seq_retain(GsSeq*)` | +1, returns its argument | Ownership primitive |
| `void gs_seq_release(GsSeq*)` | -1, frees at 0 | Ownership primitive; NULL and immortal are no-ops |
| `GsSeq* gs_seq_alloc(i32 kind, const char*, i64 len)` | returns owned | Constructor for future operations |

Conventions for new runtime functions: use the `gs_` prefix, **borrow** every `GsSeq*`
argument, and return **owned** results. Runtime errors print
`GeneScript runtime error: …` (or a message matching Phase 1) to stderr and `exit(1)`.
Declare the new function in `gs_runtime.h`, add its IR `declare` string next to `RT_*` in
`codegen.cpp`, and list it in the table above.

## 7. Remaining work (Parts 2–4)

- Variables (declaration, assignment, reads), `express`, gene calls.
- Sequence operations in the runtime: `fuse`, `~`, `transcribe`, `head`, `tail`, `@`, `#`,
  `=`, and the ordering comparisons, including Phase 1's runtime error for ordering on
  non-single bases.
- Control flow: `when` / `otherwise when` / `otherwise`, the three `cycle` / `grow` loops,
  `halt`, `skip`, and `both` / `either` / `lacks` with short-circuit evaluation.
- Top-level statements outside any gene (only used by `error.gene`, which semantics
  rejects anyway).
- The final `genec` CLI that drives clang (Part 4), plus running every Phase 1 test through
  the native path.
