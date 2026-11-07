/* Unit test for runtime/gs_runtime.c, built with AddressSanitizer by
 * tests/llvm/run_llvm_tests.sh.  Covers functions that no valid Part 2
 * program can reach (gs_seq_len: #s only appears in cycle bounds) and the
 * ownership rules themselves.  Every object created here is released, so
 * LeakSanitizer also checks that release frees exactly once. */
#include "gs_runtime.h"

#include <stdio.h>
#include <string.h>

static int checks = 0, failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++checks;                                                         \
        if (!(cond)) {                                                    \
            ++failures;                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

/* Takes ownership of `s`, compares, releases. */
static void expect(GsSeq *s, const char *bases, int kind) {
    CHECK(s != NULL);
    CHECK(s->refcount == 1);                 /* results are fresh +1 objects */
    CHECK(s->len == (int64_t)strlen(bases));
    CHECK(strcmp(s->data, bases) == 0);      /* NUL-terminated */
    CHECK(s->kind == kind);
    gs_seq_release(s);
}

int main(void) {
    GsSeq *dna = gs_seq_alloc(GS_KIND_DNA, "GATTACA", 7);
    GsSeq *rna = gs_seq_alloc(GS_KIND_RNA, "AUG", 3);
    GsSeq *empty = gs_seq_alloc(GS_KIND_DNA, NULL, 0);
    GsSeq *base = gs_seq_alloc(GS_KIND_DNA, "C", 1);

    /* length */
    CHECK(gs_seq_len(dna) == 7);
    CHECK(gs_seq_len(empty) == 0);

    /* head / tail / at keep the kind; out of range gives "" */
    expect(gs_seq_head(dna), "G", GS_KIND_DNA);
    expect(gs_seq_head(empty), "", GS_KIND_DNA);
    expect(gs_seq_tail(rna), "UG", GS_KIND_RNA);
    expect(gs_seq_tail(base), "", GS_KIND_DNA);
    expect(gs_seq_tail(empty), "", GS_KIND_DNA);
    expect(gs_seq_at(dna, 6), "A", GS_KIND_DNA);
    expect(gs_seq_at(dna, 7), "", GS_KIND_DNA);
    expect(gs_seq_at(dna, -1), "", GS_KIND_DNA);
    expect(gs_seq_at(empty, 0), "", GS_KIND_DNA);

    /* fuse, including empty operands (kind comes from the non-empty side) */
    expect(gs_seq_fuse(dna, base), "GATTACAC", GS_KIND_DNA);
    expect(gs_seq_fuse(empty, rna), "AUG", GS_KIND_RNA);
    expect(gs_seq_fuse(empty, empty), "", GS_KIND_DNA);
    expect(gs_seq_fuse(dna, dna), "GATTACAGATTACA", GS_KIND_DNA);  /* aliasing */

    /* ~ and transcribe */
    expect(gs_seq_revcomp(dna), "TGTAATC", GS_KIND_DNA);
    expect(gs_seq_revcomp(empty), "", GS_KIND_DNA);
    expect(gs_seq_transcribe(dna), "GAUUACA", GS_KIND_RNA);

    /* = and ordering */
    GsSeq *dna2 = gs_seq_alloc(GS_KIND_DNA, "GATTACA", 7);
    CHECK(gs_seq_equal(dna, dna2) == 1);
    CHECK(gs_seq_equal(dna, base) == 0);
    CHECK(gs_seq_equal(empty, empty) == 1);
    GsSeq *a = gs_seq_alloc(GS_KIND_DNA, "A", 1);
    CHECK(gs_seq_compare_bases(a, base, 1) < 0);
    CHECK(gs_seq_compare_bases(base, a, 1) > 0);
    CHECK(gs_seq_compare_bases(a, a, 1) == 0);

    /* inputs are borrowed: no operation changed their refcounts */
    CHECK(dna->refcount == 1 && empty->refcount == 1 && rna->refcount == 1);

    /* retain/release: +1/-1, freed at zero (LSan/ASan verify the free) */
    CHECK(gs_seq_retain(dna) == dna);
    CHECK(dna->refcount == 2);
    gs_seq_release(dna);
    CHECK(dna->refcount == 1);

    /* immortal objects (compiler literals) are never counted or freed */
    static struct { int64_t rc, len; int32_t kind; char data[5]; } lit = {
        GS_REFCOUNT_IMMORTAL, 4, GS_KIND_DNA, "ATGC"};
    GsSeq *l = (GsSeq *)&lit;
    gs_seq_retain(l);
    gs_seq_release(l);
    gs_seq_release(l);
    CHECK(lit.rc == GS_REFCOUNT_IMMORTAL);
    expect(gs_seq_tail(l), "TGC", GS_KIND_DNA);
    gs_seq_release(NULL);                      /* no-op */

    gs_seq_release(dna);
    gs_seq_release(dna2);
    gs_seq_release(rna);
    gs_seq_release(empty);
    gs_seq_release(base);
    gs_seq_release(a);

    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("%d checks", checks);
    return 0;
}
