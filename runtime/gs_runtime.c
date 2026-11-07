/*
 * GeneScript native runtime.  See gs_runtime.h and docs/LLVM_BACKEND.md.
 *
 * Part 1 scope: only what the first compiled program needs (report) plus the
 * reference-counting primitives that define the ownership model every later
 * operation will follow.
 */
#include "gs_runtime.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The IR emitted by the compiler hard-codes this layout. */
_Static_assert(offsetof(GsSeq, refcount) == 0,  "GsSeq ABI: refcount offset");
_Static_assert(offsetof(GsSeq, len)      == 8,  "GsSeq ABI: len offset");
_Static_assert(offsetof(GsSeq, kind)     == 16, "GsSeq ABI: kind offset");
_Static_assert(offsetof(GsSeq, data)     == 20, "GsSeq ABI: data offset");

static void gs_fatal(const char *msg) {
    fflush(stdout);
    fprintf(stderr, "GeneScript runtime error: %s\n", msg);
    exit(1);
}

GsSeq *gs_seq_retain(GsSeq *s) {
    if (s && s->refcount > 0) {
        s->refcount++;
    }
    return s;
}

void gs_seq_release(GsSeq *s) {
    if (!s || s->refcount < 0) {
        return; /* NULL or immortal literal */
    }
    if (s->refcount == 0) {
        gs_fatal("release of a sequence that was already freed");
    }
    if (--s->refcount == 0) {
        free(s);
    }
}

GsSeq *gs_seq_alloc(int32_t kind, const char *bases, int64_t len) {
    if (len < 0) {
        gs_fatal("negative sequence length");
    }
    GsSeq *s = (GsSeq *)malloc(sizeof(GsSeq) + (size_t)len + 1);
    if (!s) {
        gs_fatal("out of memory");
    }
    s->refcount = 1;
    s->len = len;
    s->kind = kind;
    if (bases) {
        memcpy(s->data, bases, (size_t)len);
    } else {
        memset(s->data, 0, (size_t)len);
    }
    s->data[len] = '\0';
    return s;
}

void gs_report(const GsSeq *s) {
    if (s && s->len > 0) {
        fwrite(s->data, 1, (size_t)s->len, stdout);
    }
    fputc('\n', stdout);
    fflush(stdout);
}
