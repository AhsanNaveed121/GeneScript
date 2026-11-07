/*
 * GeneScript native runtime.  See gs_runtime.h and docs/LLVM_BACKEND.md.
 *
 * Part 1: report + the reference-counting primitives that define the
 * ownership model.  Part 2: the sequence operations (head, tail, @, #,
 * fuse, ~, transcribe, =, ordering).
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

/* ---- Sequence operations ------------------------------------------------ */

int64_t gs_seq_len(const GsSeq *s) {
    return s->len;
}

/* Owned copy of s->data[from .. from+len). */
static GsSeq *gs_slice(const GsSeq *s, int64_t from, int64_t len) {
    return gs_seq_alloc(s->kind, s->data + from, len);
}

GsSeq *gs_seq_head(const GsSeq *s) {
    return gs_slice(s, 0, s->len > 0 ? 1 : 0);
}

GsSeq *gs_seq_tail(const GsSeq *s) {
    return s->len > 1 ? gs_slice(s, 1, s->len - 1) : gs_slice(s, 0, 0);
}

GsSeq *gs_seq_at(const GsSeq *s, int64_t i) {
    return (i >= 0 && i < s->len) ? gs_slice(s, i, 1) : gs_slice(s, 0, 0);
}

GsSeq *gs_seq_fuse(const GsSeq *a, const GsSeq *b) {
    if (a->len > INT64_MAX - b->len) {
        gs_fatal("sequence too long");
    }
    /* An empty operand carries no reliable kind; take it from the other. */
    int32_t kind = a->len > 0 ? a->kind : b->kind;
    GsSeq *r = gs_seq_alloc(kind, NULL, a->len + b->len);
    memcpy(r->data, a->data, (size_t)a->len);
    memcpy(r->data + a->len, b->data, (size_t)b->len);
    return r;
}

GsSeq *gs_seq_revcomp(const GsSeq *s) {
    GsSeq *r = gs_seq_alloc(GS_KIND_DNA, NULL, s->len);
    for (int64_t i = 0; i < s->len; ++i) {
        char c = s->data[s->len - 1 - i];
        switch (c) {
            case 'A': c = 'T'; break;
            case 'T': c = 'A'; break;
            case 'C': c = 'G'; break;
            case 'G': c = 'C'; break;
            default: break; /* unreachable for checked DNA; same as Phase 1 */
        }
        r->data[i] = c;
    }
    return r;
}

GsSeq *gs_seq_transcribe(const GsSeq *s) {
    GsSeq *r = gs_seq_alloc(GS_KIND_RNA, s->data, s->len);
    for (int64_t i = 0; i < r->len; ++i) {
        if (r->data[i] == 'T') r->data[i] = 'U';
    }
    return r;
}

int32_t gs_seq_equal(const GsSeq *a, const GsSeq *b) {
    return a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0;
}

int32_t gs_seq_compare_bases(const GsSeq *a, const GsSeq *b, int64_t line) {
    if (a->len != 1 || b->len != 1) {
        /* Same text and exit status as the interpreter's runtime_error. */
        fflush(stdout);
        fprintf(stderr,
                "Runtime Error (line %lld): ordering operators compare single bases only, "
                "but got \"%s\" and \"%s\".\n",
                (long long)line, a->data, b->data);
        exit(1);
    }
    return (int32_t)(unsigned char)a->data[0] - (int32_t)(unsigned char)b->data[0];
}
