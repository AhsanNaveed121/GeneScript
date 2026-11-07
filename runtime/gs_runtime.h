/*
 * GeneScript native runtime -- C side of the LLVM backend ABI.
 *
 * Generated LLVM IR (see src/codegen.cpp) is linked against gs_runtime.c:
 *
 *     clang program.ll runtime/gs_runtime.c -o program
 *
 * The full contract (value layout, calling convention, ownership rules) is in
 * docs/LLVM_BACKEND.md.  Anything changed here must be changed there and in
 * the IR emitted by src/codegen.cpp (search for "ABI").
 */
#ifndef GS_RUNTIME_H
#define GS_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sequence kinds.  Stored in GsSeq.kind. */
enum {
    GS_KIND_DNA = 1,
    GS_KIND_RNA = 2
};

/* Refcount value that marks an immortal sequence (compiler-emitted literal).
 * Any negative refcount is immortal: retain/release never touch it. */
#define GS_REFCOUNT_IMMORTAL (-1)

/*
 * Every DNA/RNA value is a pointer to one immutable GsSeq.
 *
 * In LLVM IR a value is a plain `ptr`.  A literal of N bases is emitted as a
 * constant global of type
 *
 *     { i64, i64, i32, [N+1 x i8] }      ; refcount, len, kind, data + NUL
 *
 * which has exactly the same layout as this struct (data starts at byte 20).
 */
typedef struct GsSeq {
    int64_t refcount; /* offset 0  : >0 heap object, <0 immortal literal  */
    int64_t len;      /* offset 8  : number of bases, excluding the NUL   */
    int32_t kind;     /* offset 16 : GS_KIND_DNA or GS_KIND_RNA           */
    char    data[];   /* offset 20 : len bases followed by a NUL byte     */
} GsSeq;

/* ---- Ownership -------------------------------------------------------- */

/* Adds one reference and returns `s` (so it can be used inline).
 * No-op on immortal literals. */
GsSeq *gs_seq_retain(GsSeq *s);

/* Drops one reference; frees the object when the count reaches zero.
 * No-op on immortal literals and on NULL. */
void gs_seq_release(GsSeq *s);

/* Allocates a fresh heap sequence of `len` bases, copied from `bases`
 * (which may be NULL to leave the data zeroed).  Returns an OWNED (+1)
 * reference.  For use by runtime operations; codegen does not call it yet. */
GsSeq *gs_seq_alloc(int32_t kind, const char *bases, int64_t len);

/* ---- Statements ------------------------------------------------------- */

/* `report e`: writes the bases of `s` followed by a newline to stdout and
 * flushes (matching the interpreter's `std::cout << ... << std::endl`).
 * BORROWS `s`. */
void gs_report(const GsSeq *s);

/* ---- Sequence operations (Part 2) --------------------------------------
 * Every function BORROWS its GsSeq* arguments and every GsSeq* result is a
 * fresh OWNED (+1) heap object, even when it is empty.  Semantics fixes all
 * DNA/RNA types at compile time; `kind` in the result is informational. */

/* #s -> number of bases (IR: i64). */
int64_t gs_seq_len(const GsSeq *s);

/* head(s) -> first base, or "" when s is empty.  Keeps s's kind. */
GsSeq *gs_seq_head(const GsSeq *s);

/* tail(s) -> s without its first base, or "" when len(s) <= 1. */
GsSeq *gs_seq_tail(const GsSeq *s);

/* s@i -> base at 0-based position i, or "" when i is out of range
 * (same as the Phase 1 interpreter). */
GsSeq *gs_seq_at(const GsSeq *s, int64_t i);

/* fuse a b -> a followed by b. */
GsSeq *gs_seq_fuse(const GsSeq *a, const GsSeq *b);

/* ~s -> reverse complement (A<->T, C<->G, reversed).  Result is DNA. */
GsSeq *gs_seq_revcomp(const GsSeq *s);

/* transcribe(s) -> every T replaced by U.  Result is RNA. */
GsSeq *gs_seq_transcribe(const GsSeq *s);

/* a = b -> 1 if the bases are identical, else 0 (IR: i32). */
int32_t gs_seq_equal(const GsSeq *a, const GsSeq *b);

/* Ordering (<, >, <=, >=) on single bases: returns <0, 0 or >0 like strcmp
 * (IR: i32; codegen applies the operator with icmp).  If either operand is
 * not exactly one base, prints the Phase 1 runtime error for source line
 * `line` to stderr and exits with status 1. */
int32_t gs_seq_compare_bases(const GsSeq *a, const GsSeq *b, int64_t line);

#ifdef __cplusplus
}
#endif

#endif /* GS_RUNTIME_H */
