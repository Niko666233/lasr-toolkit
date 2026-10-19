/* tufa_vm.c — LASR TUFA bytecode VM, clean-room C rebuild (docs/65).
 *
 * Faithful to FUN_00653880 run-loop semantics (docs/65 §3/§4/§6):
 *   - u32 value slots (int / f32 bits / 32-bit pointers)  => BUILD FOR x86
 *   - empty-stack pop silently returns 0
 *   - calls with a signature ALWAYS push a return slot ('V' -> 0/undefined)
 *   - jump base = address of the jump instruction itself
 *   - JMP_NE jumps when popped == 0; JMP_EQ jumps when popped != 0
 *   - SHORTCUT_AND/OR: skip(+5)/take(+1) the following JMP
 *   - args popped caller-TOS-first => callee locals reversed vs source order
 *     (this = locals[0], param k = locals[N+1-k])
 *   - static field idx = index in declaring class's own static group;
 *     instance idx = declaring class's super total + own-group index
 *   - runtime errors: print + continue with safe 0 (no unwinding)
 *   - DELETE(0x28)/0x0a/illegal: silent pc+=5
 *
 * First-cut scope: single thread, no GC, natives stubbed (log, pop, push 0),
 * interfaces by name-match only, inner-class segments ignored.
 *
 * Build:  cl /O2 /W3 /Fe:tufa_vm.exe tufa_vm.c        (MUST be x86 / 32-bit)
 * Usage:  tufa_vm --cp extracted --index vm/class_index.txt
 *                 --clinit java.game.Bet [--dump] [--trace]
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <wchar.h>
#include <time.h>
#include <io.h>
#include <math.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  i32;
typedef float    f32;

static u32 fu2u(f32 f) { u32 u; memcpy(&u, &f, 4); return u; }
static f32 u2fu(u32 u) { f32 f; memcpy(&f, &u, 4); return f; }
static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

#define MAX_FRAMES 512

static long g_steps = 0, g_native_calls = 0, g_errors = 0, g_oob = 0;
static long g_budget = 200000000L;
static int  g_trace = 0, g_dump = 0, g_sample = 0;

/* ------------------------------------------------------------------ vector */
typedef struct { i32 cap; u32 *base; i32 count; u32 inl[16]; } Vec;

static void vec_init(Vec *v) {
    v->cap = 16; v->base = v->inl; v->count = 0;
    memset(v->inl, 0, sizeof(v->inl));
}
static void vec_grow(Vec *v, i32 need) {
    if (need <= v->cap) return;
    i32 nc = v->cap * 2 + 2; if (nc < need) nc = need;
    u32 *nb = (u32 *)calloc((size_t)nc, 4);
    if (v->count) memcpy(nb, v->base, (size_t)v->count * 4);
    if (v->base != v->inl) free(v->base);
    v->base = nb; v->cap = nc;
}
static void vec_push(Vec *v, u32 x) { vec_grow(v, v->count + 1); v->base[v->count++] = x; }
static u32 vec_pop(Vec *v) {
    if (v->count > 0) { u32 x = v->base[--v->count]; v->base[v->count] = 0; return x; }
    return 0;                                   /* silent 0 on underflow */
}
static u32 vec_peek(Vec *v) { return v->count > 0 ? v->base[v->count - 1] : 0; }

/* ------------------------------------------------------------------ strings */
typedef struct StrNode { u16 *w; void *inst; struct StrNode *next; } StrNode;
#define STR_BUCKETS 4096
static StrNode *g_strs[STR_BUCKETS];

static u16 *wdup(const u16 *s) {
    size_t n = 0; while (s[n]) n++;
    u16 *w = (u16 *)calloc(n + 1, 2);
    memcpy(w, s, n * 2);
    return w;
}
/* String constants in the container are UTF-8 and the engine decodes them into
 * UTF-16 -- our old version widened each BYTE to one unit, so a CJK character
 * became 3 chars.  Caught by the differential probe: String.length() on "C15"
 * (U+CD15) is 1 on the real VM and was 3 in our model, and on
 * "LASR <3 CJK> line" 13 vs 19 (docs/65 10.48).  ASCII is unaffected; invalid
 * UTF-8 falls back to the old 1:1 widening so no byte is ever lost. */
static u16 *widen(const char *s) {
    size_t n = strlen(s);
    u16 *w = (u16 *)calloc(n + 1, 2);
    size_t i = 0, o = 0;
    if (!w) return NULL;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { w[o++] = c; i++; continue; }
        int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
        unsigned cp = (extra > 0) ? (unsigned)(c & (0x3F >> extra)) : 0u;
        size_t k = 1;
        if (extra > 0) {
            for (; k <= (size_t)extra; k++) {
                unsigned char d = (unsigned char)s[i + k];
                if ((d & 0xC0) != 0x80) break;
                cp = (cp << 6) | (unsigned)(d & 0x3F);
            }
        }
        if (extra > 0 && k == (size_t)extra + 1 && cp <= 0x10FFFFu &&
            !(cp >= 0xD800u && cp <= 0xDFFFu)) {
            if (cp >= 0x10000u) {
                cp -= 0x10000u;
                w[o++] = (u16)(0xD800u + (cp >> 10));
                w[o++] = (u16)(0xDC00u + (cp & 0x3FFu));
            } else {
                w[o++] = (u16)cp;
            }
            i += k;
        } else {
            w[o++] = c;
            i++;
        }
    }
    w[o] = 0;
    return w;
}
static int wcmp(const u16 *a, const u16 *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)*a - (int)*b;
}
static unsigned whash(const u16 *w) {
    unsigned h = 2166136261u;
    for (; *w; w++) { h ^= *w; h *= 16777619u; }
    return h;
}

/* --------------------------------------------------------------- structures */
typedef struct ClassS Class;
typedef struct InstS  Inst;

typedef struct { u8 tag; char *s; u32 a, b;
                 int resolved, r_is_method; Class *r_cls; i32 r_idx; void *r_m;
               } ConsEnt;

typedef struct { u32 flags, name_ref, type_ref, extra; char *name, *type; } Field;

typedef struct { u32 flags, name_ref, desc_ref; i32 tree_slot, nlocals;
                 char *name, *desc; i32 argc; char retc; } Method;

typedef struct { u32 cap; u32 *base; u32 count; } ArrData;

struct InstS { Class *cls; u32 cap; u32 *fields; u32 flags; };

struct ClassS {
    char *name;
    Class *next;                       /* registry chain */
    ConsEnt *cons; i32 ncons;
    u32 *clss; i32 nclss;
    Field *fs; i32 nfs;                /* own static fields  (group 1) */
    Field *fi; i32 nfi;                /* own instance fields(group 2) */
    Method *ms; i32 nms;               /* own static methods (group 1) */
    Method *mi; i32 nmi;               /* own instance methods(group 2) */
    u8 **recs; i32 *reclen; i32 nrec;
    Class *super;
    i32 inst_base;                     /* super instance-field total */
    i32 nfields_total;
    u32 *statics; i32 nstatics;
    int is_array, is_synth;
    int linked, clinit_done;
};

typedef struct {
    Vec loc, stk;
    u8 *pc, *code; i32 codelen;
    Class *cls; Method *m; u32 thisv;
} Frame;

typedef struct { Frame frames[MAX_FRAMES]; i32 nf; } Ctx;
static Ctx g_ctx;

#define CLS_BUCKETS 8192
static Class *g_heads[CLS_BUCKETS];
static char *g_index = NULL, *g_cp = NULL;
static Class *g_string_cls = NULL;

static unsigned shash(const char *s) {
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (u8)*s; h *= 16777619u; }
    return h;
}
static Class *cls_find(const char *name) {
    for (Class *c = g_heads[shash(name) % CLS_BUCKETS]; c; c = c->next)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

/* forward */
static Class *load_class(const char *name);
static void link_class(Class *c);
static int  run_record(Class *c, Method *m, u32 thisv, int is_static, i32 rec_idx);
static Class *member_owner(Class *c, Method *m);
static Inst *arr_build(const char *desc, i32 nd, const i32 *dims, i32 level);
static void prep_field(Field *f, Class *c);
static void prep_method(Method *m, Class *c);

/* ------------------------------------------------------------ error report */
static struct { const char *fmt; long n; } g_errk[64];
static void rt_errorf(const char *fmt, ...) {
    g_errors++;
    for (int i = 0; i < 64; i++) {
        if (g_errk[i].fmt == fmt) { g_errk[i].n++; break; }
        if (!g_errk[i].fmt) { g_errk[i].fmt = fmt; g_errk[i].n = 1; break; }
    }
    if (g_errors > 200) return;             /* throttle: count, don't drown */
    if (g_errors == 200) { fprintf(stderr, "... error output throttled ...\n"); return; }
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "Runtime error in thread 'main'\n");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    for (i32 i = g_ctx.nf - 1, k = 0; i >= 0 && k < 10; i--, k++) {
        Frame *f = &g_ctx.frames[i];
        fprintf(stderr, "  at %s.%s (offset:0x%04lx)\n",
                f->cls ? f->cls->name : "?", f->m ? f->m->name : "<clinit>",
                (unsigned long)(f->pc - f->code));
    }
}
#define RTERR(...) rt_errorf(__VA_ARGS__)

/* ------------------------------------------------------- container parsing */
static u8 *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *b = (u8 *)malloc((size_t)n + 8);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f); b[n] = 0; *len = n; return b;
}

static int parse_class_image(Class *c, const u8 *d, long start, long end) {
    long off = start;
    while (off + 8 <= end) {
        char tag[5]; memcpy(tag, d + off, 4); tag[4] = 0;
        u32 size = rd32(d + off + 4);
        const u8 *b = d + off + 8;
        if (off + 8 + (long)size > end) return 0;
        if (!memcmp(tag, "CONS", 4)) {
            c->ncons = (i32)rd32(b);
            c->cons = (ConsEnt *)calloc((size_t)(c->ncons > 0 ? c->ncons : 1),
                                        sizeof(ConsEnt));
            long p = 4;
            for (i32 i = 0; i < c->ncons && p < (long)size; i++) {
                ConsEnt *e = &c->cons[i];
                e->tag = b[p++];
                if (e->tag == 0) {
                    long s0 = p;
                    while (p < (long)size && b[p]) p++;
                    e->s = (char *)malloc((size_t)(p - s0) + 1);
                    memcpy(e->s, b + s0, (size_t)(p - s0));
                    e->s[p - s0] = 0;
                    p++;
                } else if (e->tag == 4) {
                    e->a = rd32(b + p); p += 4;
                } else if (e->tag == 5 || e->tag == 7) {
                    e->a = rd32(b + p); e->b = rd32(b + p + 4); p += 8;
                } else return 0;
            }
        } else if (!memcmp(tag, "FILD", 4) || !memcmp(tag, "MTHD", 4)) {
            int mthd = (tag[0] == 'M');
            i32 row = mthd ? 20 : 16;
            i32 n1 = (i32)rd32(b);
            long p2 = 4 + (long)row * n1;
            if (p2 + 4 > (long)size) return 0;
            i32 n2 = (i32)rd32(b + p2);
            if (mthd) {
                c->nms = n1; c->nmi = n2;
                c->ms = (Method *)calloc((size_t)(n1 ? n1 : 1), sizeof(Method));
                c->mi = (Method *)calloc((size_t)(n2 ? n2 : 1), sizeof(Method));
            } else {
                c->nfs = n1; c->nfi = n2;
                c->fs = (Field *)calloc((size_t)(n1 ? n1 : 1), sizeof(Field));
                c->fi = (Field *)calloc((size_t)(n2 ? n2 : 1), sizeof(Field));
            }
            for (i32 g = 0; g < 2; g++) {
                i32 n = g ? n2 : n1;
                const u8 *src = b + (g ? (p2 + 4) : 4);
                for (i32 i = 0; i < n; i++) {
                    const u8 *r = src + (long)row * i;
                    if (mthd) {
                        Method *t = &(g ? c->mi : c->ms)[i];
                        t->flags = rd32(r); t->name_ref = rd32(r + 4);
                        t->desc_ref = rd32(r + 8);
                        t->tree_slot = (i32)rd32(r + 12);
                        t->nlocals = (i32)rd32(r + 16);
                    } else {
                        Field *t = &(g ? c->fi : c->fs)[i];
                        t->flags = rd32(r); t->name_ref = rd32(r + 4);
                        t->type_ref = rd32(r + 8); t->extra = rd32(r + 12);
                    }
                }
            }
        } else if (!memcmp(tag, "CLSS", 4)) {
            if (!c->clss) {
                c->nclss = (i32)(size / 4);
                c->clss = (u32 *)calloc((size_t)(c->nclss ? c->nclss : 1), 4);
                for (i32 i = 0; i < c->nclss; i++) c->clss[i] = rd32(b + 4 * i);
            }
        } else if (!memcmp(tag, "TREE", 4)) {
            c->nrec = (i32)rd32(b);
            if (c->nrec < 0) return 0;
            c->recs = (u8 **)calloc((size_t)(c->nrec ? c->nrec : 1), sizeof(u8 *));
            c->reclen = (i32 *)calloc((size_t)(c->nrec ? c->nrec : 1), 4);
            long p = 4;
            for (i32 i = 0; i < c->nrec; i++) {
                u32 sz = rd32(b + p); p += 4;
                u8 *m = (u8 *)malloc((size_t)sz + 1);
                memcpy(m, b + p, sz); m[sz] = 0;
                c->recs[i] = m; c->reclen[i] = (i32)sz;
                p += (long)sz;
            }
        } else if (!memcmp(tag, "TUFA", 4)) {
            /* inner-class marker: segments after it ignored in this cut */
        }
        off += 8 + (long)size;
    }
    return 1;
}

/* ------------------------------------------------------------- CONS access */
static const char *cons_str(Class *c, u32 ref) {
    if (!c || (i32)ref < 0 || (i32)ref >= c->ncons) return NULL;
    ConsEnt *e = &c->cons[ref];
    if (e->tag == 0) return e->s;
    if (e->tag == 4) return cons_str(c, e->a);
    if (e->tag == 5) return cons_str(c, e->a);   /* NEW/CAST/INSTANCEOF classref */
    return NULL;
}

/* --------------------------------------------------------- class index/load */
static char *index_lookup(const char *name) {
    static char *cache = NULL;
    if (!cache) {
        long len;
        cache = (char *)read_file(g_index, &len);
        if (!cache) { fprintf(stderr, "cannot read index %s\n", g_index); exit(2); }
    }
    size_t nl = strlen(name);
    for (char *p = cache; *p;) {
        char *eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        char *tab = (char *)memchr(p, '\t', (size_t)(eol - p));
        if (tab && (size_t)(tab - p) == nl && !memcmp(p, name, nl)) {
            size_t n = (size_t)(eol - tab - 1);
            while (n && (tab[n] == '\r' || tab[n] == '\n')) n--;
            char *r = (char *)malloc(n + 1);
            memcpy(r, tab + 1, n); r[n] = 0;
            return r;
        }
        if (!*eol) break;
        p = eol + 1;
    }
    return NULL;
}

static Class *new_class(const char *name) {
    Class *c = (Class *)calloc(1, sizeof(Class));
    c->name = strdup(name);
    unsigned h = shash(name) % CLS_BUCKETS;
    c->next = g_heads[h];
    g_heads[h] = c;
    return c;
}

static Class *synth_class(const char *name) {
    Class *c = new_class(name);
    c->is_synth = 1;
    c->linked = 1; c->clinit_done = 1;
    return c;
}

/* Parse every class segment of a file (inner TUFA markers split segments;
 * 7 files carry 18 embedded classes — docs/64). All segments are registered
 * in the global registry, unlinked; linking happens on first use. */
static char *segment_name(const u8 *d, long start, long end) {
    Class tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (!parse_class_image(&tmp, d, start, end)) return NULL;
    char *nm = NULL;
    if (tmp.nclss >= 2) {
        const char *s = cons_str(&tmp, tmp.clss[1]);
        if (s) nm = strdup(s);
    }
    return nm;   /* tmp allocations intentionally leaked (7 files only) */
}

static void parse_file_segments(const char *path) {
    long len; u8 *d = read_file(path, &len);
    if (!d) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
    if (len < 12 || memcmp(d, "TUFA", 4)) {
        fprintf(stderr, "bad container: %s\n", path); exit(2);
    }
    long off = 12, seg = 12;
    while (off + 8 <= len) {
        char tag[5]; memcpy(tag, d + off, 4); tag[4] = 0;
        u32 size = rd32(d + off + 4);
        if (off + 8 + (long)size > len) break;
        if (!memcmp(tag, "TUFA", 4)) {
            long e = off;
            char *nm = segment_name(d, seg, e);
            if (nm && !cls_find(nm)) {
                Class *c = new_class(nm);
                parse_class_image(c, d, seg, e);
            }
            free(nm);
            seg = off + 8 + (long)size;
        }
        off += 8 + (long)size;
    }
    if (seg < len) {
        char *nm = segment_name(d, seg, len);
        if (nm && !cls_find(nm)) {
            Class *c = new_class(nm);
            parse_class_image(c, d, seg, len);
        }
        free(nm);
    }
    free(d);
}

static Class *load_class(const char *name) {
    Class *c = cls_find(name);
    if (c) { link_class(c); return c; }
    if (name[0] == '[') {
        c = synth_class(name);
        c->is_array = 1;
        c->nfields_total = 2;             /* [0]=length [1]=ArrData* */
        return c;
    }
    char *rel = index_lookup(name);
    if (rel) {
        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", g_cp, rel);
        parse_file_segments(path);        /* registers every segment */
        free(rel);
        c = cls_find(name);
    }
    if (!c) {                             /* library class (java.lang.Object…) */
        c = new_class(name);
        c->is_synth = 1;
        if (getenv("TUFA_VM_SYNTHLOG"))
            fprintf(stderr, "[synth] %s\n", name);
    }
    link_class(c);
    return c;
}

/* ------------------------------------------------------------ member lookup */
static void prep_field(Field *f, Class *c) {
    if (f->name) return;
    f->name = (char *)cons_str(c, f->name_ref);
    f->type = (char *)cons_str(c, f->type_ref);
    if (!f->name) f->name = (char *)"?";
    if (!f->type) f->type = (char *)"?";
}
static i32 sig_argc(const char *d) {
    i32 n = 0;
    if (*d != '(') return 0;
    d++;
    while (*d && *d != ')') {
        while (*d == '[') d++;               /* array prefixes: one arg */
        if (*d == 'L') { while (*d && *d != ';') d++; if (!*d) break; }
        n++; d++;
    }
    return n;
}
static void prep_method(Method *m, Class *c) {
    if (m->name) return;
    m->name = (char *)cons_str(c, m->name_ref);
    m->desc = (char *)cons_str(c, m->desc_ref);
    if (!m->name) m->name = (char *)"?";
    if (!m->desc) m->desc = (char *)"()V";
    m->argc = sig_argc(m->desc);
    const char *p = strchr(m->desc, ')');
    /* original: Method+0x20 = ptr past ')' or 0 when the signature ends at
     * ')' (e.g. "()") — 0 means NO return slot is pushed/popped at all. */
    m->retc = (p && p[1]) ? p[1] : 0;
}
static Class *member_owner(Class *c, Method *m) {
    for (Class *k = c; k; k = k->super) {
        if ((m >= k->ms && m < k->ms + k->nms) ||
            (m >= k->mi && m < k->mi + k->nmi)) return k;
    }
    return c;
}
static Method *find_method(Class *c, const char *name, const char *desc) {
    for (Class *k = c; k; k = k->super) {
        for (i32 g = 0; g < 2; g++) {
            Method *arr = g ? k->mi : k->ms;
            i32 n = g ? k->nmi : k->nms;
            for (i32 i = 0; i < n; i++) {
                prep_method(&arr[i], k);
                if (!strcmp(arr[i].name, name) && !strcmp(arr[i].desc, desc))
                    return &arr[i];
            }
        }
    }
    return NULL;
}
static int find_field(Class *c, const char *name, int group, Class **decl) {
    for (Class *k = c; k; k = k->super) {
        Field *arr = group ? k->fi : k->fs;
        i32 n = group ? k->nfi : k->nfs;
        for (i32 i = 0; i < n; i++) {
            prep_field(&arr[i], k);
            if (!strcmp(arr[i].name, name)) {
                *decl = k;
                return group ? (k->inst_base + i) : i;
            }
        }
        if (!group) break;              /* statics do not climb (observed) */
    }
    *decl = NULL;
    return -1;
}

static ConsEnt *resolve_member(Class *from, u32 ref) {
    if (!from || (i32)ref < 0 || (i32)ref >= from->ncons) return NULL;
    ConsEnt *e = &from->cons[ref];
    if (e->tag != 5) return NULL;
    if (e->resolved) return e;
    e->resolved = 1;
    const char *clsname = cons_str(from, e->a);
    if ((i32)e->b < 0 || (i32)e->b >= from->ncons) return e;
    ConsEnt *nat = &from->cons[e->b];
    if (!clsname || nat->tag != 7) return e;
    const char *mname = cons_str(from, nat->a);
    const char *mdesc = cons_str(from, nat->b);
    if (!mname || !mdesc) return e;
    Class *tc = load_class(clsname);
    e->r_cls = tc;
    if (mdesc[0] == '(') {
        e->r_is_method = 1;
        e->r_m = find_method(tc, mname, mdesc);
    } else {
        Class *decl = NULL;
        i32 si = find_field(tc, mname, 0, &decl);
        if (si >= 0) { e->r_idx = si; e->r_cls = decl; }
        else {
            i32 ii = find_field(tc, mname, 1, &decl);
            if (ii >= 0) { e->r_idx = ii; e->r_cls = decl; }
            else if (tc->is_array && !strcmp(mname, "length")) {
                e->r_idx = 0; e->r_cls = tc;   /* arrays: fields[0] = length */
            }
            else RTERR("Referenced field '%s' no longer exists in class '%s'. "
                       "Recompile all classes if possible!", mname, clsname);
        }
    }
    return e;
}

/* ------------------------------------------------------------------ linking */
static void link_class(Class *c) {
    if (c->linked) return;
    c->linked = 1;
    if (!c->is_synth && c->nclss >= 4 && (i32)c->clss[3] >= 0) {
        const char *sn = cons_str(c, c->clss[3]);
        if (sn && strcmp(sn, c->name)) c->super = load_class(sn);
    }
    c->inst_base = c->super ? c->super->nfields_total : 0;
    c->nfields_total = c->inst_base + c->nfi;
    c->nstatics = c->nfs;
    if (c->nstatics > 0) c->statics = (u32 *)calloc((size_t)c->nstatics, 4);
    /* <clinit> record index = CLSS[5] (verified: matches out_init.json's
     * <clinit> record number for 1058/1058 classes, incl. the 31 whose
     * field-init block occupies record 0 and pushes <clinit> to #1/#8/#13).
     * Fall back to the old "record 0 unclaimed" heuristic when CLSS is short. */
    i32 clinit_rec = -1;
    if (!c->is_synth && c->nclss >= 6) clinit_rec = (i32)c->clss[5];
    if (clinit_rec < 0) {
        int claimed0 = 0;
        for (i32 i = 0; i < c->nms; i++) if (c->ms[i].tree_slot == 0) claimed0 = 1;
        for (i32 i = 0; i < c->nmi; i++) if (c->mi[i].tree_slot == 0) claimed0 = 1;
        if (!claimed0 && c->nrec > 0 && c->reclen[0] > 0) clinit_rec = 0;
    }
    if (clinit_rec >= 0 && clinit_rec < c->nrec &&
        c->reclen[clinit_rec] > 0 && !c->clinit_done) {
        c->clinit_done = 1;
        run_record(c, NULL, 0, 1, clinit_rec);
    } else {
        c->clinit_done = 1;
    }
}

/* ---------------------------------------------------------------- instances */
/* --- instance pointer validity table (crash guard vs native-tainted refs) --- */
#define INST_TAB_BITS 18
#define INST_TAB_MASK ((1u << INST_TAB_BITS) - 1)
static Inst **g_inst_tab;
static void inst_register(Inst *o) {
    if (!g_inst_tab) g_inst_tab = (Inst **)calloc(1u << INST_TAB_BITS, sizeof(Inst *));
    if (!g_inst_tab) return;
    size_t h = ((uintptr_t)o >> 4) & INST_TAB_MASK;
    while (g_inst_tab[h] && g_inst_tab[h] != o) h = (h + 1) & INST_TAB_MASK;
    g_inst_tab[h] = o;
}
static int inst_valid(const void *p) {
    if (!p || !g_inst_tab) return 0;
    size_t h = ((uintptr_t)p >> 4) & INST_TAB_MASK;
    while (g_inst_tab[h]) {
        if (g_inst_tab[h] == p) return 1;
        h = (h + 1) & INST_TAB_MASK;
    }
    return 0;
}

static Inst *inst_new(Class *c) {
    Inst *o = (Inst *)calloc(1, sizeof(Inst));
    o->cls = c;
    o->cap = (u32)(c->nfields_total > 0 ? c->nfields_total : 0);
    if (o->cap) o->fields = (u32 *)calloc(o->cap, 4);
    inst_register(o);
    return o;
}
static u32 *inst_field(Inst *o, i32 idx) {
    if (!inst_valid(o) || idx < 0 || (u32)idx >= o->cap) {
        RTERR("Instance::getInstanceField: index (%d) out of bounds (%u)",
              idx, inst_valid(o) ? o->cap : 0);
        return NULL;
    }
    return &o->fields[idx];
}
/* `<i>()` = the class's field-initializer block, and it is invoked BY THE
 * CONSTRUCTOR: 125 INVOKESPECIAL sites in the corpus name `<i>()`, one per
 * constructor of every class that has one, always as the first call after the
 * super-ctor (`Bot.<init>(Ljava.lang.String;...)` +0x0f, ...).  So the
 * initializers run through the bytecode whenever `<init>` runs — the engine's
 * allocation path does NOT run them (tools/uni_inst_probe.py: FUN_00656100,
 * the helper the NEW opcode and Class.newInstance both use, leaves
 * loadInProgress' fields all zero), and running them here as well would
 * double-initialize every object field.  docs/65 §10.21 records the full
 * evidence chain, including the false "0 bytecode refs to `<i>`" claim that
 * this replaced (a member-ref resolution bug in the scan).
 *
 * Cross-VM check: probe/spec_i_probe.json — `new HUD_Interface().getPosition()`
 * (HUD_Interface.<i> writes position=1, no other method writes it) = 1 on both
 * the real VM and this one; control PointPairSeries.Count() = 3 on both. */
/* Constructors in this dialect carry NO return type in their descriptor —
 * `<init>` is "()" / "(I)", never "()V" (the engine's own Class.newInstance
 * calls FUN_00653820(cls, "<init>", "()", 1): the descriptor string at
 * 0x6f4d38 is literally "()").  Try the dialect form first, keep "()V" as a
 * fallback for any class that does carry it. */
static Method *find_ctor(Class *c) {
    Method *m = find_method(c, "<init>", "()");
    if (!m) m = find_method(c, "<init>", "()V");
    return m;
}
static u32 *arr_elem(Inst *a, i32 idx) {
    if (!inst_valid(a) || a->cap < 2) { RTERR("Instance::getArrayElement: null array ptr");
        return NULL; }
    i32 len = (i32)a->fields[0];
    ArrData *d = (ArrData *)(uintptr_t)a->fields[1];
    if (!d || idx < 0 || idx >= len || (u32)idx >= d->cap) {
        RTERR("Instance::getArrayElement: index (%d) out of bounds (%d)", idx, len);
        return NULL;
    }
    return &d->base[idx];
}
static Frame *cur(void);                       /* defined below; used by logs */
static Inst *arr_new(const char *desc, i32 len) {
    Class *ac = load_class(desc);               /* synthetic '['-class */
    if (getenv("UNI_ALLOCLOG")) {
        Frame *f = cur();
        fprintf(stderr, "[newarray] desc=%s len=%d cls=%p at %s.%s+%ld\n",
                desc ? desc : "(null)", len, (void *)ac,
                f && f->cls ? f->cls->name : "?", f && f->m ? f->m->name : "?",
                f && f->code ? (long)(f->pc - f->code) : -1L);
    }
    Inst *a = inst_new(ac);
    if (a->cap < 2) {                            /* safety: arrays need 2 slots */
        a->fields = (u32 *)realloc(a->fields, 8);
        if (!a->fields) { RTERR("oom"); a->cap = 0; return a; }
        memset(a->fields, 0, 8);
        a->cap = 2;
    }
    if (len > 10000000) { RTERR("array length %d absurd", len); len = 0; }
    a->fields[0] = (u32)(len > 0 ? len : 0);
    ArrData *d = (ArrData *)calloc(1, sizeof(ArrData));
    if (!d) { RTERR("oom arrdata"); return a; }
    d->cap = (u32)(len > 0 ? len : 0);
    d->count = d->cap;
    if (d->cap) {
        d->base = (u32 *)calloc(d->cap, 4);
        if (!d->base) { RTERR("oom array %u", d->cap); d->cap = 0; }
    }
    a->fields[1] = (u32)(uintptr_t)d;
    return a;
}
static Inst *arr_build(const char *desc, i32 nd, const i32 *dims, i32 level) {
    Inst *a = arr_new(desc + level, dims[level]);
    if (level + 1 < nd) {
        ArrData *d = (ArrData *)(uintptr_t)a->fields[1];
        for (u32 i = 0; i < d->cap; i++)
            d->base[i] = (u32)(uintptr_t)arr_build(desc, nd, dims, level + 1);
    }
    return a;
}

/* string objects: synthetic java.lang.String, field[0] = u16* */
static void *intern_w(u16 *w) {
    unsigned h = whash(w) % STR_BUCKETS;
    for (StrNode *n = g_strs[h]; n; n = n->next)
        if (!wcmp(n->w, w)) { free(w); return n->inst; }
    if (!g_string_cls) {
        /* prefer the REAL java.lang.String (it is a game class with its own
         * clinit at record CLSS[5]); synth only if it cannot be loaded.
         * Field layout matches: fields[0] = wide-char buffer (verified
         * against the real VM, docs/65 §10.5). */
        Class *sc = cls_find("java.lang.String");
        if (!sc) sc = load_class("java.lang.String");
        if (!sc) {
            sc = synth_class("java.lang.String");
            sc->nfields_total = 1;
        }
        if (sc->nfields_total < 1) sc->nfields_total = 1;
        g_string_cls = sc;
    }
    Inst *s = inst_new(g_string_cls);
    s->fields[0] = (u32)(uintptr_t)w;
    StrNode *n = (StrNode *)calloc(1, sizeof(StrNode));
    n->w = w; n->inst = s; n->next = g_strs[h]; g_strs[h] = n;
    return s;
}
static void *intern_n(const char *s) { return intern_w(widen(s)); }
static const u16 *str_w(u32 v) {
    Inst *s = (Inst *)(uintptr_t)v;
    if (!inst_valid(s) || !g_string_cls || s->cls != g_string_cls) return NULL;
    return (const u16 *)(uintptr_t)s->fields[0];
}

/* ------------------------------------------------------------------- frames */
static Frame *cur(void) { return g_ctx.nf ? &g_ctx.frames[g_ctx.nf - 1] : NULL; }

static int frame_push(Class *c, Method *m, u8 *code, i32 codelen, u32 thisv) {
    if (g_ctx.nf >= MAX_FRAMES) { RTERR("frame stack overflow"); return 0; }
    Frame *f = &g_ctx.frames[g_ctx.nf++];
    memset(f, 0, sizeof(*f));
    vec_init(&f->loc); vec_init(&f->stk);
    f->pc = f->code = code; f->codelen = codelen;
    f->cls = c; f->m = m; f->thisv = thisv;
    return 1;
}
static void frame_pop(void) {
    Frame *f = cur();
    if (!f) return;
    if (f->loc.base != f->loc.inl) free(f->loc.base);
    if (f->stk.base != f->stk.inl) free(f->stk.base);
    g_ctx.nf--;
}

/* kind: 0=virtual 1=special 2=static.  Returns 0 when a new frame was
 * pushed (caller must NOT advance pc), 1 when handled inline. */
typedef struct NatCnt { struct NatCnt *next; char *key; long n; } NatCnt;
static NatCnt *g_nattab[1024];
static void nat_count(const char *cls, const char *m, const char *sig) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s.%s%s", cls ? cls : "?", m, sig);
    unsigned h = 5381;
    for (char *p = buf; *p; p++) h = h * 33u ^ (unsigned char)*p;
    h &= 1023;
    for (NatCnt *e = g_nattab[h]; e; e = e->next)
        if (!strcmp(e->key, buf)) { e->n++; return; }
    NatCnt *e = (NatCnt *)calloc(1, sizeof(*e));
    e->key = strdup(buf); e->n = 1; e->next = g_nattab[h]; g_nattab[h] = e;
}
static void nat_dump(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < 1024; i++)
        for (NatCnt *e = g_nattab[i]; e; e = e->next)
            fprintf(f, "%ld\t%s\n", e->n, e->key);
    fclose(f);
}

/* ------------------------------------------------------- implemented natives
 * Semantics transcribed from the original implementations (out_native_decomp):
 *   forName   = FUN_00662410: name -> find/load class -> wrap in Class obj
 *   getClass  = FUN_006626c0: this->cls -> wrap in Class obj
 *   newInstance = FUN_00662500: alloc target + run <init>()V + return obj
 *   getName   = FUN_006625d0: class name as String
 *   ResourceRef.newNative = FUN_004115c0: fields[0]==0 -> 12B zeroed handle
 *   ResourceRef.id        = FUN_00410860: handle+8 ? *(handle+8 +0x28) : 0
 * Class-object representation: java.lang.Class instance with fields[0] =
 * native Class* (643b30(obj, dcb2c=0, cls)).                                     */
static char *narrow(const u16 *w) {
    if (!w) return NULL;
    size_t n = 0;
    while (w[n]) n++;
    char *b = (char *)malloc(n + 1);
    if (!b) return NULL;
    for (size_t i = 0; i <= n; i++) b[i] = (char)(w[i] & 0xff);
    return b;
}
static u32 *inst_field_grow(Inst *o, i32 idx) {
    if (!inst_valid(o)) return NULL;
    if ((u32)idx >= o->cap) {
        u32 nc = (u32)idx + 1;
        u32 *nb = (u32 *)realloc(o->fields, (size_t)nc * 4);
        if (!nb) return NULL;
        memset(nb + o->cap, 0, (size_t)(nc - o->cap) * 4);
        o->fields = nb; o->cap = nc;
    }
    return &o->fields[idx];
}
static int cls_valid(const Class *c);
static u32 wrap_class(Class *tc) {
    Class *cc = load_class("java.lang.Class");
    if (!cc) return 0;
    Inst *o = inst_new(cc);
    u32 *f0 = inst_field_grow(o, 0);
    if (f0) *f0 = (u32)(uintptr_t)tc;
    return (u32)(uintptr_t)o;
}
static u32 nat_forName(u32 t, u32 *a) {
    const u16 *w = str_w(a[0]);
    if (!w) return 0;
    char *n = narrow(w);
    if (!n) return 0;
    /* Class.forName must return NULL for a class that does not exist — the
     * game depends on it: java.game.Map.<init> probes
     * getName()+"_track_"+n for n=0.. and only stops after 20 consecutive
     * misses (`if (forName(name) != null) { i = 0; add; } else i++;`), so a
     * loader that always hands back a class spins forever.  load_class keeps
     * its synth fallback for the bytecode path (library classes), the Java
     * visible API does not. */
    if (!cls_find(n) && !index_lookup(n) && n[0] != '[') {
        if (getenv("UNI_NATLOG")) fprintf(stderr, "[forName] %s -> null\n", n);
        free(n); return 0;
    }
    Class *c = load_class(n);
    if (getenv("UNI_NATLOG")) fprintf(stderr, "[forName] %s -> %s\n", n, c ? "ok" : "null");
    free(n);
    if (!c) return 0;
    return wrap_class(c);
}
static u32 nat_getClass(u32 t, u32 *a) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (!inst_valid(o) || !o->cls) return 0;
    return wrap_class(o->cls);
}
static u32 nat_getName(u32 t, u32 *a) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (!inst_valid(o) || o->cap < 1) return 0;
    Class *tc = (Class *)(uintptr_t)o->fields[0];
    if (!cls_valid(tc)) return 0;
    return (u32)(uintptr_t)intern_n(tc->name);
}
static u32 nat_newInstance(u32 t, u32 *a) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (!inst_valid(o) || o->cap < 1) return 0;
    Class *tc = (Class *)(uintptr_t)o->fields[0];
    if (!cls_valid(tc)) return 0;
    Inst *n = inst_new(tc);
    Method *ini = find_ctor(tc);
    if (getenv("UNI_NATLOG"))
        fprintf(stderr, "[newInstance] %s ctor=%s%s\n", tc->name,
                ini ? ini->desc : "<none>", (ini && (ini->flags & 0x100)) ? " native" : "");
    if (ini && !(ini->flags & 0x100)) {
        Class *own = member_owner(tc, ini);
        if (own && ini->tree_slot >= 0 && ini->tree_slot < own->nrec)
            run_record(own, ini, (u32)(uintptr_t)n, 0, -1);
    } else if (ini) {
        RTERR("newInstance: native <init> of %s not run", tc->name);
    }
    return (u32)(uintptr_t)n;
}
static u32 nat_arraycopy(u32 t, u32 *a) {
    Inst *s = (Inst *)(uintptr_t)a[0], *d = (Inst *)(uintptr_t)a[2];
    i32 sp = (i32)a[1], dp = (i32)a[3], n = (i32)a[4];
    if (!inst_valid(s) || !inst_valid(d) || s->cap < 2 || d->cap < 2 || n <= 0) {
        RTERR("arraycopy: invalid operand"); return 0;
    }
    ArrData *sb = (ArrData *)(uintptr_t)s->fields[1];
    ArrData *db = (ArrData *)(uintptr_t)d->fields[1];
    if (!sb || !db || sp < 0 || dp < 0 ||
        sp + n > (i32)sb->cap || dp + n > (i32)db->cap) {
        RTERR("arraycopy: out of bounds"); return 0;
    }
    memmove(db->base + dp, sb->base + sp, (size_t)n * 4);
    return 0;
}
static u32 nat_rr_newNative(u32 t, u32 *a) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (!inst_valid(o)) return 0;
    u32 *f = inst_field_grow(o, 0);
    if (f && !*f) {
        u32 *h = (u32 *)calloc(3, 4);       /* 12B native handle, zeroed */
        *f = (u32)(uintptr_t)h;
    }
    return 0;
}
static u32 nat_rr_id(u32 t, u32 *a) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (!inst_valid(o) || o->cap < 1) return 0;
    u32 *h = (u32 *)(uintptr_t)o->fields[0];
    if (!h) return 0;
    u32 *node = (u32 *)(uintptr_t)h[2];     /* *(handle+8) */
    return node ? node[10] : 0;             /* *(node+0x28) */
}
/* duplicate = FUN_004114b0: handle==0 -> null; else new wrapper of same
 * class with a fresh zeroed 12B handle (native node clone not modeled). */
static u32 nat_duplicate(u32 t, u32 *a) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (!inst_valid(o) || o->cap < 1 || !o->fields[0]) return 0;
    Inst *n = inst_new(o->cls);
    u32 *f = inst_field_grow(n, 0);
    if (f) *f = (u32)(uintptr_t)calloc(3, 4);
    return (u32)(uintptr_t)n;
}
/* Matrix.newNative = FUN_00405530: fields[0]==0 -> 64B zeroed (16 floats). */
static u32 nat_matrix_newNative(u32 t, u32 *a) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (!inst_valid(o)) return 0;
    u32 *f = inst_field_grow(o, 0);
    if (f && !*f) *f = (u32)(uintptr_t)calloc(16, 4);
    return 0;
}
/* Matrix.set(Vector3,Vector3,Ypr) = LAB_00405590: composes T*R*S into the
 * native handle (math chain 567e00/562900/562750/563520 — NOT yet modeled);
 * returns this.  Divergence: handle contents stay zero. */
static u32 nat_matrix_set(u32 t, u32 *a) { return t; }
/* ResourceRef.set(String,I) = FUN_004103d0: resolve(name,type)->resID, bind
 * into the native handle; set(I) = FUN_004104c0: validate explicit resID.
 * Model: deterministic synthetic resID per (name,type) — real rpk ids are
 * the native-resource-module round.  Java-visible contract kept: id()!=0. */
static u32 g_next_resid = 0x10000;
typedef struct ResIdEnt { struct ResIdEnt *next; char *name; u32 type, resid; } ResIdEnt;
static ResIdEnt *g_resids;
static u32 res_resolve(const char *name, u32 type) {
    for (ResIdEnt *e = g_resids; e; e = e->next)
        if (e->type == type && !strcmp(e->name, name)) return e->resid;
    ResIdEnt *e = (ResIdEnt *)calloc(1, sizeof(*e));
    e->name = strdup(name); e->type = type; e->resid = g_next_resid++;
    e->next = g_resids; g_resids = e;
    return e->resid;
}
static void handle_bind(u32 *h, u32 resid) {
    u32 *node = (u32 *)calloc(16, 4);
    if (!node) return;
    node[10] = resid;                   /* +0x28 = resID */
    h[2] = (u32)(uintptr_t)node;        /* handle+8 = node (leaks old: ok) */
}
static u32 nat_rr_set(u32 t, u32 *a) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (!inst_valid(o)) return 0;
    u32 *f = inst_field_grow(o, 0);
    if (!f) return 0;
    if (!*f) {                          /* original: handle==0 -> conflict log */
        RTERR("Reference conflict (set)");
        return 0;
    }
    u32 *h = (u32 *)(uintptr_t)*f;
    const u16 *w = str_w(a[0]);
    char *nm = w ? narrow(w) : NULL;
    u32 resid = res_resolve(nm ? nm : "(null)", a[1]);
    free(nm);
    handle_bind(h, resid);
    return 0;
}
static u32 nat_rr_set_i(u32 t, u32 *a) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (!inst_valid(o)) return 0;
    u32 *f = inst_field_grow(o, 0);
    if (!f || !*f) return 0;
    handle_bind((u32 *)(uintptr_t)*f, a[0]);
    return 0;
}
/* Minimal resource-node model (full tree/rpk loader = native-layer phase):
 * GameRef.createNativeInstance = FUN_00424bb0 builds a node in the native
 * resource tree under the parent; Java-visible effect consumed downstream:
 * handle+8 = node, node+0x28 = resID (nonzero => GameType ctor validation
 * passes).  Here: unique synthetic resID from a flat counter. */
static u32 nat_gr_createNativeInstance(u32 t, u32 *a) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (!inst_valid(o)) return 0;
    u32 *f = inst_field_grow(o, 0);
    if (!f) return 0;
    if (!*f) *f = (u32)(uintptr_t)calloc(3, 4);
    u32 *h = (u32 *)(uintptr_t)*f;
    if (!h) return 0;
    if (!h[2]) {
        u32 *node = (u32 *)calloc(16, 4);
        if (!node) return 0;
        node[10] = g_next_resid++;        /* +0x28 = resID */
        h[2] = (u32)(uintptr_t)node;
    }
    return 0;
}
static u32 nat_file_exists(u32 t, u32 *a) {
    const u16 *w = str_w(a[0]);
    if (!w) return 0;
    char *p = narrow(w);
    if (!p) return 0;
    FILE *f = fopen(p, "rb");
    free(p);
    if (f) { fclose(f); return 1; }
    return 0;
}
static u32 nat_exit(u32 t, u32 *a) {
    const u16 *w = str_w(a[0]);
    char *p = w ? narrow(w) : NULL;
    fprintf(stderr, "[System.exit] %s (batch continues; original terminates)\n",
            p ? p : "(null)");
    free(p);
    return 0;
}

/* ---- java.io.File stream model (docs/65 §10.14) -------------------------
 * The engine hangs a private 0x164-byte native object off each Java File and
 * keeps the C stream there (open = vm_file_open 0x403f80 -> FUN_004036f0);
 * the Java side only ever holds `name` (File's single instance field).  We
 * model the observable part: a side table keyed by the Java object, 4-byte
 * little-endian words, short reads zero-padded (readInt/readFloat zero their
 * buffer first — FUN_004022a0 / FUN_004023d0), open returning native != 0.
 * Reads resolve under --file-root (the install); WRITES go under
 * --file-write-root (sandbox) so the rebuilt VM can never damage the player's
 * real config/save files. */
#define N_FILE_SLOTS 64
typedef struct { u32 obj; FILE *fp; int wmode; } FileSlot;
static FileSlot g_files[N_FILE_SLOTS];
static const char *g_file_root = NULL;
static const char *g_file_wroot = NULL;

static FileSlot *fslot(u32 obj) {
    for (int i = 0; i < N_FILE_SLOTS; i++)
        if (g_files[i].fp && g_files[i].obj == obj) return &g_files[i];
    return NULL;
}
static FileSlot *fslot_new(u32 obj) {
    for (int i = 0; i < N_FILE_SLOTS; i++)
        if (!g_files[i].fp) {
            g_files[i].obj = obj; g_files[i].wmode = 0;
            return &g_files[i];
        }
    return NULL;
}
static char *file_path(u32 obj, int wmode) {
    Inst *o = (Inst *)(uintptr_t)obj;
    if (!inst_valid(o)) return NULL;
    Class *decl = NULL;
    i32 fi = find_field(o->cls, "name", 1, &decl);
    if (fi < 0) return NULL;
    u32 *slot = inst_field_grow(o, fi);
    if (!slot || !*slot) return NULL;
    const u16 *w = str_w(*slot);
    if (!w) return NULL;
    char *rel = narrow(w);
    if (!rel) return NULL;
    const char *root = wmode ? g_file_wroot : g_file_root;
    size_t n = strlen(rel) + (root ? strlen(root) : 0) + 2;
    char *p = (char *)malloc(n);
    if (p) {
        if (root && root[0] && rel[0] != '/' && rel[0] != '\\' &&
            !(rel[0] && rel[1] == ':'))
            snprintf(p, n, "%s/%s", root, rel);
        else
            snprintf(p, n, "%s", rel);
    }
    free(rel);
    return p;
}
static u32 nat_file_open(u32 t, u32 *a) {
    u32 mode = a[0];
    FileSlot *old = fslot(t);
    if (old) { fclose(old->fp); old->fp = NULL; }
    FileSlot *s = fslot_new(t);
    if (!s) return 0;
    int wmode = (int)(mode & 1);                 /* MODE_WRITE = 1 */
    char *p = file_path(t, wmode);
    if (!p) {
        fprintf(stderr, "[file] open: cannot resolve File.name (mode=%u)\n", mode);
        return 0;
    }
    const char *m = wmode ? "wb" : "rb";
    if (!wmode && ((mode >> 1) == 2)) m = "r+b";
    s->fp = fopen(p, m);
    s->wmode = wmode;
    if (!s->fp) {
        if (wmode) s->fp = fopen(p, "wb");      /* create if absent */
        if (!s->fp) fprintf(stderr, "[file] open FAILED %s (mode=%u)\n", p, mode);
    }
    /* Header policy, transcribed from FUN_004036f0 (the open worker) and
     * verified against the player's own files: the engine reads 8 bytes; when
     * byte[6] == 3 the payload is a structured data file whose 4-byte size
     * sits at offset 8 and whose records start at offset 12 (seek 0xc at the
     * end of the branch) — `save/game/options` is exactly this shape, byte[6]
     * of the 2-byte `locale/.conf` is not (it is "en" + zero padding) and that
     * one starts at 0. */
    if (s->fp && !wmode) {
        unsigned char h[8] = {0};
        size_t got = fread(h, 1, 8, s->fp);
        long pos = 0;
        if (got >= 7 && h[6] == 3) pos = 12;
        else if (got == 8 && h[6] == 1) pos = 8;
        fseek(s->fp, pos, SEEK_SET);
    }
    if (s->fp && wmode)
        fprintf(stderr, "[file] WRITE %s (sandboxed)\n", p);
    if (s->fp && getenv("UNI_FILELOG"))
        fprintf(stderr, "[file] OPEN %s mode=%u -> pos %ld\n", p, mode, ftell(s->fp));
    free(p);
    return s->fp ? 1 : 0;
}
static u32 nat_file_close(u32 t, u32 *a) {
    FileSlot *s = fslot(t);
    if (s) { fclose(s->fp); s->fp = NULL; s->obj = 0; }
    return 0;
}
static u32 nat_file_read4(u32 t) {
    FileSlot *s = fslot(t);
    u32 v = 0;                                   /* short read => zero-padded */
    if (!s || !s->fp) return 0;
    (void)fread(&v, 1, 4, s->fp);
    if (getenv("UNI_FILELOG")) {
        Frame *f = cur();
        fprintf(stderr, "[file] read4 @%ld -> 0x%08x (%d) at %s.%s+%ld\n",
                ftell(s->fp), v, (int)v, f && f->cls ? f->cls->name : "?",
                f && f->m ? f->m->name : "?",
                f && f->code ? (long)(f->pc - f->code) : -1L);
    }
    return v;
}
static u32 nat_file_write4(u32 t, u32 v) {
    FileSlot *s = fslot(t);
    if (!s || !s->fp || !s->wmode) return 0;
    return (u32)fwrite(&v, 1, 4, s->fp);
}
/* File.write(String)I = 0x402430: unwrap, take the string's character buffer,
 * convert it to bytes and fwrite them (the counterpart of readString, which has
 * no length prefix).  Verified by round-tripping Config.save against the file
 * the game itself wrote (docs/65 10.71). */
static u32 nat_file_write_s(u32 t, u32 *a) {
    FileSlot *s = fslot(t);
    if (!s || !s->fp || !s->wmode) return 0;
    const u16 *w = str_w(a[0]);
    if (!w) return (u32)fwrite("", 1, 0, s->fp);
    char *p = narrow(w);
    if (!p) return 0;
    size_t n = fwrite(p, 1, strlen(p), s->fp);
    free(p);
    return (u32)n;
}
static u32 nat_file_write_i(u32 t, u32 *a) { return nat_file_write4(t, a[0]); }
static u32 nat_file_write_f(u32 t, u32 *a) { return nat_file_write4(t, a[0]); }
static u32 nat_file_readInt(u32 t, u32 *a) { (void)a; return nat_file_read4(t); }
static u32 nat_file_readFloat(u32 t, u32 *a) { (void)a; return nat_file_read4(t); }
/* readString: NO length prefix — the engine fills its 255-char buffer from the
 * stream position and, once the file ends, keeps emitting the FIRST byte read
 * (observable: `locale/.conf` is the 2 bytes "en", and the real VM's
 * Config.language is 'en' + 'e'*253 — verified against out_realvm_dump.jsonl).
 * Modelling it as "read a 4-byte length first" produced an empty string. */
static u32 nat_file_readString(u32 t, u32 *a) {
    (void)a;
    FileSlot *s = fslot(t);
    if (!s || !s->fp) return 0;
    static char buf[256];
    int len = 0;
    char c = 0, first = 0;
    if (fread(&c, 1, 1, s->fp) != 1)
        return 0;                    /* nothing to read => the original hands
                                      * back NULL (real VM: Config.userID and
                                      * lastRoomName stay null, not "") */
    if (c == 0)
        return 0;                    /* NUL-terminated => NULL as well: the
                                      * real VM leaves Config.userID null where
                                      * a ""-producing model diverges */
    first = c;
    buf[len++] = c;
    for (int i = 1; i < 255; i++) {
        size_t got = fread(&c, 1, 1, s->fp);
        if (got != 1) c = first;
        buf[len++] = c;
    }
    buf[len] = 0;
    void *sv = intern_n(buf);
    if (getenv("UNI_FILELOG"))
        fprintf(stderr, "[file] readString -> len=%d obj=%p first=%.8s\n",
                len, sv, buf);
    return (u32)(uintptr_t)sv;
}
/* java.sound.Sound channel volumes (docs/65 §10.15).  setVolume (FUN_0040eb60)
 * is a switch that stores the float into per-channel engine globals
 * (DAT_0074230c/…); the Java-visible contract is getVolume(i) == the last
 * setVolume(i,·), defaulting to full scale — which is exactly what
 * Track.lastEngineVolume (= Sound.getVolume(CHANNEL_ENGINE) = 1.0f) shows. */
static u32 g_chan_vol[16];
static int g_chan_init = 0;
static void sound_chan_init(void) {
    if (g_chan_init) return;
    for (int i = 0; i < 16; i++) g_chan_vol[i] = 0x3f800000u;   /* 1.0f */
    g_chan_init = 1;
}
static u32 nat_sound_setVolume(u32 t, u32 *a) {
    (void)t;
    sound_chan_init();
    if ((int)a[0] >= 0 && a[0] < 16) g_chan_vol[a[0]] = a[1];
    return 0;
}
static u32 nat_sound_getVolume(u32 t, u32 *a) {
    (void)t;
    sound_chan_init();
    if ((int)a[0] >= 0 && a[0] < 16) return g_chan_vol[a[0]];
    return 0x3f800000u;
}
/* Ref/array-returning natives whose 0 return cascades: java.game.Init.<init>
 * reads the command line array right away, and 14 errors in that ctor all trace
 * back to stub-returned nulls (docs/65 §10.16).  No ground truth exists for the
 * contents at this stop point, so hand back a valid EMPTY array — non-null, and
 * nothing invented. */
static u32 nat_sys_cmdline(u32 t, u32 *a) {
    (void)t; (void)a;
    return (u32)(uintptr_t)arr_new("[Ljava.lang.String;", 0);
}
/* Monotonic clock for java.lang.System.currentTime()F.  java.game.Init.<init>
 * spins on `while (currentTime() - t0 < 3)` (the splash-screen delay), so a
 * 0-returning stub loops forever — that is exactly where the main-loop
 * experiment sat (UNI_SAMPLE showed Init.<init>+995 for every sample).  The
 * real engine's clock advances; we hand back CPU seconds since VM start. */
static u32 nat_sys_currentTime(u32 t, u32 *a) {
    (void)t; (void)a;
    static clock_t t0;
    static long n = 0;
    if (!t0) t0 = clock();
    double sec = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    n++;
    if (g_sample && (n % 200000L) == 0)
        fprintf(stderr, "[clock] call=%ld t0=%ld now=%ld sec=%.3f\n", n,
                (long)t0, (long)clock(), sec);
    return fu2u((f32)sec);
}
/* ======================================================= java.gui bridge model
 * docs/65 §10.20.  The whole java.gui layer is a native binding: essentially
 * every method of java.gui.Component / Background / Frame / Rotating / ... is
 * `native`, and the widget STATE lives in the engine's native widget object;
 * the Java object is a thin proxy.  Ground truth (out_native_decomp):
 *   Component.construct(Window;[String;String)V = FUN_005a0450 -> FUN_005d4b20
 *     pops (this,window,stringTable,layoutText) — FUN_00652ab0 args, then reads
 *     layoutText's field[0] (the wchar payload) and parses the hex-encoded
 *     layout blob to build the native widget;
 *   Component.getBackground = FUN_005ac140: read this->field[0] (the native
 *     widget pointer) and return the Java wrapper of widget+0x218 — the wrapper
 *     is created on demand by FUN_005a0120 (load_class(name from the native
 *     object's vtbl+0xc) + instantiate + associate), i.e. sub-objects such as
 *     Background/Frame have their own Java proxy created lazily;
 *   Embedded.m_object is a real Java field (index 0 of java.gui.Embedded) and
 *     Embedded.getObject() is plain Java (`return this.m_object`) — the engine
 *     fills it with an instance of the class named by the layout's `Class`
 *     property (all 29 in the corpus are java.game.frontend.*), which is what
 *     StartWindow.<init> casts to loadInProgress.
 *
 * We model exactly that shape: one record per proxy object holding the layout
 * kind/name/template + its properties (name -> value), plus lazily created
 * sub-wrappers for the `<Parent>.<Prop>` property groups (Background.*,
 * Frame.*, Image.*, ...) reached through the object-returning getter
 * get<Parent>() — the wrapper class comes from that getter's own return
 * descriptor, so subclasses need no table.
 *
 * Accessors: getXxx/setXxx are answered from the property table by name
 * (layout property "Left" <-> setLeft/getLeft, "Color_Active" <->
 * Background.setColor_Active, ...).  A property the layout never set returns 0
 * — precisely what the un-registered stub returned before — so the model can
 * only ADD fidelity, never regress an existing path. */
#define GW_MAXP   160
#define GW_MAXSUB 24
#define GW_MAXPFX 24
#define GW_SL     96
typedef struct GwProp { char name[48]; u8 type; u32 v; char sv[GW_SL]; } GwProp;
typedef struct GwSub  { char pfx[GW_MAXPFX]; Inst *obj; } GwSub;
typedef struct GwRec {
    Inst *java;
    int kind;
    char name[40], tmpl[40];
    GwProp props[GW_MAXP]; int nprops;
    GwSub subs[GW_MAXSUB]; int nsub;
    Inst *mobj;                        /* Embedded: Class-property instance */
    struct GwRec *next;
} GwRec;
static GwRec *g_grecs = NULL;
static int g_gui_log = -1;
static int gui_log(void) {
    if (g_gui_log < 0) g_gui_log = getenv("UNI_GUILOG") ? 1 : 0;
    return g_gui_log;
}
static GwRec *gw_find(Inst *o) {
    for (GwRec *r = g_grecs; r; r = r->next) if (r->java == o) return r;
    return NULL;
}
static GwRec *gw_get(Inst *o) {
    GwRec *r = gw_find(o);
    if (r) return r;
    r = (GwRec *)calloc(1, sizeof(GwRec));
    r->java = o; r->kind = -1; r->next = g_grecs; g_grecs = r;
    return r;
}
/* Layout `kind` -> proxy class.  Derived from the corpus by grouping every
 * decoded layout's kind by the declared type of the Java field it is
 * constructed into (tools/ui_layout.py kind_to_class: 1058 layouts, 0
 * conflicts beyond what is listed); the value is used for cross-checking the
 * blob against the receiver, not for creating the receiver. */
static const char *gw_kind_cls(int k) {
    static const struct { int kind; const char *cls; } t[] = {
        {2,"java.gui.Window"},{6,"java.gui.Component"},{7,"java.gui.Button"},
        {8,"java.gui.Label"},{9,"java.gui.HProportional"},{13,"java.gui.TextButton"},
        {16,"java.gui.LineEdit"},{20,"java.gui.Page"},{21,"java.gui.PageControl"},
        {29,"java.gui.Checkbox"},{30,"java.gui.Progressbar"},{34,"java.gui.ComboBox"},
        {36,"java.gui.Logview"},{37,"java.gui.IconButton"},{39,"java.gui.View3D"},
        {42,"java.gui.TabListHeaderItem"},{43,"java.gui.TabList"},
        {53,"java.gui.HSlider"},{54,"java.gui.Gauge"},{55,"java.gui.VSlider"},
        {56,"java.gui.Embedded"},{60,"java.gui.ViewVideo"},{63,"java.gui.Rotating"},
        {64,"java.gui.MasterGameList"},{0,NULL}};
    for (int i = 0; t[i].cls; i++) if (t[i].kind == k) return t[i].cls;
    return NULL;
}
static int gw_hexc(char c) { return (c >= 'A' && c <= 'P') ? c - 'A' : -1; }
/* The layout argument is ASCII text, two chars per byte, A=0..P=15. */
static int gw_hexblob(const char *s, u8 *out, int maxn) {
    int n = 0;
    for (; s[0] && s[1] && n < maxn; s += 2) {
        int hi = gw_hexc(s[0]), lo = gw_hexc(s[1]);
        if (hi < 0 || lo < 0) break;
        out[n++] = (u8)((hi << 4) | lo);
    }
    return n;
}
static u16 gw_rd16(const u8 *p) { return (u16)(p[0] | ((u16)p[1] << 8)); }
static int gw_cstr(const u8 *b, int n, int off, char *dst, int cap) {
    int j = off;
    while (j < n && b[j]) j++;
    if (j >= n) return -1;
    int len = j - off;
    if (len >= cap) len = cap - 1;
    memcpy(dst, b + off, (size_t)len); dst[len] = 0;
    return j + 1;
}
typedef struct { int kind; char name[40], tmpl[40];
                 GwProp props[GW_MAXP]; int nprops; } GwLayout;
/* Blob grammar (docs/65 §10.20, cross-checked against tools/ui_layout.py):
 *   u32 size(== len-4) | u16 version(7) | u16 0x10 | u8 kind
 *   cstr name | cstr template | prop* | 0x00 [zero padding]
 *   prop := cstr name | u16 type | payload
 *   type 1/4 -> u32, 2 -> float, 6 -> 4 floats, 3/12/13/14 -> cstr + u32,
 *   everything else -> cstr. */
static int gw_parse(const u8 *b, int n, GwLayout *L) {
    if (n < 12 || (int)rd32(b) != n - 4) return 0;
    L->kind = b[8];
    int o = gw_cstr(b, n, 9, L->name, sizeof L->name);
    if (o < 0) return 0;
    o = gw_cstr(b, n, o, L->tmpl, sizeof L->tmpl);
    if (o < 0) return 0;
    L->nprops = 0;
    while (o < n && b[o] != 0 && L->nprops < GW_MAXP) {
        GwProp *p = &L->props[L->nprops];
        p->v = 0; p->sv[0] = 0;
        o = gw_cstr(b, n, o, p->name, sizeof p->name);
        if (o < 0 || o + 2 > n) return 0;
        p->type = (u8)gw_rd16(b + o);
        o += 2;
        int extra = 0, isstr = 0;
        switch (p->type) {
        case 1: case 2: case 4:
            if (o + 4 > n) return 0;
            p->v = rd32(b + o); o += 4; break;
        case 6:
            if (o + 16 > n) return 0;
            p->v = rd32(b + o); o += 16; break;       /* keep the first float */
        case 3: case 12: case 13: case 14: isstr = 1; extra = 4; break;
        default: isstr = 1; break;
        }
        if (isstr) {
            o = gw_cstr(b, n, o, p->sv, sizeof p->sv);
            if (o < 0 || o + extra > n) return 0;
            o += extra;
        }
        L->nprops++;
    }
    return 1;
}
/* Property groups the corpus actually uses, in `<Parent>.<Prop>` form. */
static const char *const gw_objpfx[] = {
    "Background", "Frame", "Image", "Header", "Thumb", "Text", "List",
    "ContainerRoot", "ListboxContainer", "ThumbPeakMin", "ThumbPeakMax",
    "Scroller", "NormalPointer", "MinPointer", "MaxPointer", NULL };
/* Which class does `<Parent>.<Prop>` belong to?  Ask the receiver's own
 * get<Parent>() return descriptor — the same fact tools/ui_layout.py took from
 * the Java field types, but straight from the class, so it stays right for
 * subclasses (HSlider.getThumb, TabList.getHeader, ...). */
static Class *gw_sub_class(Inst *self, const char *pfx) {
    char acc[64];
    snprintf(acc, sizeof acc, "get%s", pfx);
    if (!self->cls) return NULL;
    int seen = 0;
    for (Class *k = self->cls; k; k = k->super)
        for (i32 j = 0; j < k->nmi; j++) {
            Method *m = &k->mi[j];
            prep_method(m, k);
            if (strcmp(m->name, acc)) continue;
            seen++;
            if (m->desc[0] != '(' || m->desc[1] != ')' || m->desc[2] != 'L') {
                /* NOT a hard failure: another overload named get<Pfx> may be
                 * the object getter (returning NULL here made the whole group
                 * unusable - Gauge.NormalPointer/MinPointer/MaxPointer). */
                if (gui_log())
                    fprintf(stderr, "[gui] sub_class %s.%s desc=%s not ()L...;\n",
                            k->name, acc, m->desc);
                continue;
            }
            size_t len = strlen(m->desc + 3);
            if (!len || len >= sizeof acc + 40) return NULL;
            char nm[96];
            memcpy(nm, m->desc + 3, len); nm[len - 1] = 0;
            return load_class(nm);
        }
    if (gui_log())
        fprintf(stderr, "[gui] sub_class %s.%s MISS (matched=%d)\n",
                self->cls->name, acc, seen);
    return NULL;
}
static void gw_run_ctor(Inst *o, Class *c) {
    Method *ini = find_ctor(c);
    if (!ini) return;
    if (ini->flags & 0x100) {
        fprintf(stderr, "[gui] native <init> of %s not run\n", c->name);
        return;
    }
    Class *own = member_owner(c, ini);
    if (own) run_record(own, ini, (u32)(uintptr_t)o, 0, -1);
}
/* Engine-side instantiation = <i>() (field initializers) then <init>(). */
static Inst *gw_new_obj(Class *c) {
    Inst *o = inst_new(c);
    gw_run_ctor(o, c);                       /* <init> calls <i> itself */
    return o;
}
/* Lazily create (once) the proxy object for a property group. */
static Inst *gw_sub(Inst *self, const char *pfx) {
    GwRec *r = gw_get(self);
    for (int i = 0; i < r->nsub; i++)
        if (!strcmp(r->subs[i].pfx, pfx)) return r->subs[i].obj;
    if (r->nsub >= GW_MAXSUB) return NULL;
    Class *c = gw_sub_class(self, pfx);
    if (!c) return NULL;
    Inst *o = gw_new_obj(c);
    gw_get(o)->kind = -2;               /* sub-proxy, not a layout root */
    snprintf(r->subs[r->nsub].pfx, GW_MAXPFX, "%s", pfx);
    r->subs[r->nsub].obj = o; r->nsub++;
    return o;
}
static i32 gw_field_index(Inst *o, const char *fname) {
    if (!o || !o->cls) return -1;
    for (Class *k = o->cls; k; k = k->super)
        for (i32 j = 0; j < k->nfi; j++) {
            Field *f = &k->fi[j];
            prep_field(f, k);
            if (!strcmp(f->name, fname)) return k->inst_base + j;
        }
    return -1;
}
static const char *g_nat_cls = "", *g_nat_name = "", *g_nat_desc = "";
/* getXxx / setXxx -> layout property name Xxx */
static const char *gw_propname(const char *acc) {
    if (!strncmp(acc, "get", 3) && acc[3]) return acc + 3;
    if (!strncmp(acc, "set", 3) && acc[3]) return acc + 3;
    return NULL;
}
static GwProp *gw_prop(Inst *o, const char *nm, int create) {
    if (!inst_valid(o) || !nm) return NULL;
    GwRec *r = gw_get(o);
    for (int i = 0; i < r->nprops; i++)
        if (!strcmp(r->props[i].name, nm)) return &r->props[i];
    if (!create || r->nprops >= GW_MAXP) return NULL;
    GwProp *p = &r->props[r->nprops++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof p->name, "%s", nm);
    return p;
}
/* Component.construct(Window;[String;String)V  = FUN_005a0450/FUN_005d4b20 */
static u32 nat_gui_construct(u32 t, u32 *a) {
    Inst *self = (Inst *)(uintptr_t)t;
    if (!inst_valid(self)) return 0;
    const u16 *w = str_w(a[2]);
    char *txt = w ? narrow(w) : NULL;
    if (!txt) return 0;
    u8 blob[8192];
    int n = gw_hexblob(txt, blob, sizeof blob);
    free(txt);
    GwLayout L;
    memset(&L, 0, sizeof L);
    if (n <= 0 || !gw_parse(blob, n, &L)) {
        fprintf(stderr, "[gui] construct %s: layout parse FAIL (%d bytes)\n",
                self->cls ? self->cls->name : "?", n);
        return 0;
    }
    GwRec *r = gw_get(self);
    r->kind = L.kind;
    snprintf(r->name, sizeof r->name, "%s", L.name);
    snprintf(r->tmpl, sizeof r->tmpl, "%s", L.tmpl);
    r->nprops = L.nprops;
    memcpy(r->props, L.props, sizeof(GwProp) * (size_t)L.nprops);
    for (int i = 0; i < L.nprops; i++) {
        const char *pn = L.props[i].name;
        const char *dot = strchr(pn, '.');
        if (dot && dot > pn) {
            char pfx[GW_MAXPFX];
            int len = (int)(dot - pn);
            if (len >= GW_MAXPFX) continue;
            memcpy(pfx, pn, (size_t)len); pfx[len] = 0;
            Inst *so = gw_sub(self, pfx);
            if (gui_log())
                fprintf(stderr, "[gui] prop %s.%s sub=%s\n",
                        self->cls ? self->cls->name : "?", pn,
                        so ? "ok" : "NULL");
            if (so) {                        /* same property, sub-proxy record */
                GwProp *d = gw_prop(so, dot + 1, 1);
                if (d) {
                    d->type = L.props[i].type;
                    d->v = L.props[i].v;
                    memcpy(d->sv, L.props[i].sv, GW_SL);
                }
            }
        } else if (L.kind == 56 && !strcmp(pn, "Class") && L.props[i].sv[0]) {
            Class *ec = load_class(L.props[i].sv);
            if (!ec) {
                fprintf(stderr, "[gui] embedded class not found: %s\n",
                        L.props[i].sv);
                continue;
            }
            Inst *o = gw_new_obj(ec);
            i32 fi = gw_field_index(self, "m_object");
            if (fi >= 0) {
                u32 *f = inst_field_grow(self, fi);
                if (f) *f = (u32)(uintptr_t)o;
                r->mobj = o;
            }
        }
    }
    if (gui_log())
        fprintf(stderr, "[gui] construct %s kind=%d name=%s props=%d subs=%d%s%s\n",
                self->cls ? self->cls->name : "?", L.kind, L.name, L.nprops,
                r->nsub, r->mobj ? " embedded=" : "", r->mobj ? r->mobj->cls->name : "");
    return 0;
}
/* Object-returning getters answered from the sub-proxy table. */
static u32 nat_gui_get_sub(u32 t, u32 *a) {
    Inst *self = (Inst *)(uintptr_t)t;
    (void)a;
    if (!inst_valid(self)) return 0;
    GwRec *r = gw_find(self);
    if (!r) return 0;
    const char *pn = gw_propname(g_nat_name);
    if (!pn) return 0;
    for (int i = 0; i < r->nsub; i++)
        if (!strcmp(r->subs[i].pfx, pn)) return (u32)(uintptr_t)r->subs[i].obj;
    if (gui_log())
        fprintf(stderr, "[gui] get_sub %s.%s MISS (nsub=%d)\n",
                self->cls ? self->cls->name : "?", pn, r->nsub);
    return 0;
}
/* Numeric getters/setters answered from the property table.
 *
 * The engine's widget factory zeroes the native widget and then applies the
 * layout's properties, so a property the layout does NOT set answers with the
 * FACTORY's own default.  Measured, not guessed (probe/spec_gui_state_b.json,
 * real VM vs tufa_vm, same real Window + layout blob):
 *     Width    508.0  ✓ (from the layout)
 *     Visible  0      ✓ (from the layout)
 *     Priority 1      ✓ (from the layout)
 *     Enabled  1      ✗ our 0 → the factory's default for Enabled is 1
 * A bare object (never constructed) has no native widget and answers 0 —
 * the getters all begin `if (widget == 0) return 0` (e.g. FUN_005aaff0).
 *
 * Second sweep (probe/spec_gui_defaults.json, 16 getters on a minimal Label
 * layout whose props are only Template/Left/Top/Width/Height/Align/Format, so
 * every other answer IS the factory default): Visible=1, Enabled=1,
 * CanHover=1; Priority/Zoom/LayoutType/Mutex/TabStop/Autosize/SelectedState/
 * Clipping/TabOrder/UserData/FadeFactor all 0, Width/Align = the layout's
 * values.  Both sweeps now match the real VM 16/16. */
static u32 gw_default(const char *pn) {
    if (!pn) return 0;
    if (!strcmp(pn, "Visible")) return 1;
    if (!strcmp(pn, "Enabled")) return 1;
    if (!strcmp(pn, "CanHover")) return 1;
    return 0;
}
static u32 nat_gui_get_num(u32 t, u32 *a) {
    (void)a;
    Inst *self = (Inst *)(uintptr_t)t;
    const char *pn = gw_propname(g_nat_name);
    GwProp *p = gw_prop(self, pn, 0);
    if (p) return p->v;
    GwRec *r = inst_valid(self) ? gw_find(self) : NULL;
    if (r && r->kind >= 0) return gw_default(pn);    /* layout-built widget */
    return 0;
}
static u32 nat_gui_set_num(u32 t, u32 *a) {
    GwProp *p = gw_prop((Inst *)(uintptr_t)t, gw_propname(g_nat_name), 1);
    if (p) p->v = a[0];
    return 0;
}
static u32 nat_gui_set_obj(u32 t, u32 *a) {
    Inst *self = (Inst *)(uintptr_t)t, *val = (Inst *)(uintptr_t)a[0];
    const char *pn = gw_propname(g_nat_name);
    if (!inst_valid(self) || !pn || !inst_valid(val)) return 0;
    GwRec *r = gw_get(self);
    for (int i = 0; i < r->nsub; i++)
        if (!strcmp(r->subs[i].pfx, pn)) { r->subs[i].obj = val; return 0; }
    if (r->nsub < GW_MAXSUB) {
        snprintf(r->subs[r->nsub].pfx, GW_MAXPFX, "%s", pn);
        r->subs[r->nsub].obj = val; r->nsub++;
    }
    return 0;
}
/* Fallback rule so the whole java.gui accessor family is answered by the
 * property table without listing ~120 methods per class. */
static u32 nat_file_delete(u32 t, u32 *a) {
    const u16 *w = str_w(a[0]);                  /* static: (path)I */
    char *rel = w ? narrow(w) : NULL;
    if (!rel) return 0;
    size_t n = strlen(rel) + (g_file_wroot ? strlen(g_file_wroot) : 0) + 2;
    char *p = (char *)malloc(n);
    int ok = 0;
    if (p) {
        snprintf(p, n, "%s/%s", g_file_wroot ? g_file_wroot : ".", rel);
        ok = (remove(p) == 0);
        if (!ok) fprintf(stderr, "[file] delete MISS %s\n", p);
        free(p);
    }
    free(rel);
    return (u32)ok;
}
/* ====================================== java.game.parts type-builder model
 * docs/65 §10.25.  Every vehicle part class (vehicles.classes.*, 691 of them)
 * declares its geometry through Java calls at class-init time:
 *     createTypeBegin()  configureVisual(RenderType[,F[,F,F,F,F,F]])
 *     configureType(String)  configureMesh(ResourceRef[,I[,I]])
 *     configureTexture(ResourceRef)  createTypeEnd()
 * (13,217 configureType sites in the corpus.)
 * Ground truth: out_native_decomp/004997c0.c (Part.configureType),
 * 004558f0.c (Chassis.configureType), 004afb30.c (the dispatcher):
 *   - the Java string is copied to a C buffer, then TRUNCATED at ';' / '#'
 *     ('/'), i.e. everything from the first of those is a comment
 *     (corpus: `slot ... ; lightbar` - the name after ';' is a comment);
 *   - the buffer is tokenised, the first token is looked up case-insensitively
 *     in a per-kind (name -> handler) table and the handler gets the rest;
 *   - createTypeBegin allocates a 0x1f50-byte builder context and links a
 *     12-byte node into the part's type list (part+0x38 -> node, node+8 =
 *     context); configureType walks this+0x10 -> +0x38 -> +8 to reach it.
 * We model the STATE (directives in order + per-kind counters), not the
 * geometry: the corpus only ever reads a Part back through
 * getVehicle/flap/getInfo/getParent/id, so nothing here may invent geometry. */
#define PT_MAXIT  256
#define PT_MAXKEY 20
typedef struct PtItem { char key[PT_MAXKEY]; int nf; float f[8]; char txt[40]; } PtItem;
typedef struct PtRec {
    char owner[64];                    /* the part CLASS: these builders are
                                        * invoked STATICALLY in the corpus
                                        * (vehicles.classes.X.createTypeBegin()),
                                        * so a type belongs to the class, not to
                                        * an instance (docs/65 10.25) */
    int open, nit, nvis, nmesh, ntex, ndecal;
    PtItem it[PT_MAXIT];
    struct PtRec *next;
} PtRec;
static PtRec *g_ptrecs = NULL;
static int g_pt_log = -1;
static int pt_log(void) {
    if (g_pt_log < 0) g_pt_log = getenv("UNI_PARTLOG") ? 1 : 0;
    return g_pt_log;
}
/* receiver may be 0 (INVOKESTATIC): never deref it blindly */
static const char *pt_owner(u32 t) {
    Inst *o = (Inst *)(uintptr_t)t;
    if (o && inst_valid(o) && o->cls && o->cls->name) return o->cls->name;
    return g_nat_cls && g_nat_cls[0] ? g_nat_cls : "?";
}
static PtRec *pt_get(u32 t, int create) {
    const char *nm = pt_owner(t);
    for (PtRec *r = g_ptrecs; r; r = r->next)
        if (!strcmp(r->owner, nm)) return r;
    if (!create) return NULL;
    PtRec *r = (PtRec *)calloc(1, sizeof(PtRec));
    if (!r) return NULL;
    snprintf(r->owner, sizeof r->owner, "%s", nm);
    r->next = g_ptrecs; g_ptrecs = r;
    return r;
}
/* createTypeBegin()V */
static u32 nat_pt_begin(u32 t, u32 *a) {
    (void)a;
    PtRec *r = pt_get(t, 1);
    if (!r) return 0;
    r->open = 1; r->nit = r->nvis = r->nmesh = r->ntex = r->ndecal = 0;
    if (pt_log()) fprintf(stderr, "[part] begin %s\r\n", r->owner);
    return 0;
}
/* createTypeEnd()V */
static u32 nat_pt_end(u32 t, u32 *a) {
    (void)a;
    PtRec *r = pt_get(t, 0);
    if (r) r->open = 0;
    if (pt_log()) {
        fprintf(stderr, "[part] end %s type=%d vis=%d mesh=%d tex=%d decal=%d",
                pt_owner(t), r ? r->nit : -1, r ? r->nvis : -1,
                r ? r->nmesh : -1, r ? r->ntex : -1, r ? r->ndecal : -1);
        for (int i = 0; r && i < r->nit; i++)
            fprintf(stderr, " %s:%d", r->it[i].key, r->it[i].nf);
        fprintf(stderr, "\r\n");
    }
    fflush(stderr);
    return 0;
}
/* configureType(String)V */
static u32 nat_pt_configure(u32 t, u32 *a) {
    PtRec *r = pt_get(t, 1);
    const u16 *w = str_w(a[0]);
    char *s = w ? narrow(w) : NULL;
    if (!s) return 0;
    for (char *p = s; *p; p++)                  /* ';' '#' '/' start a comment */
        if (*p == ';' || *p == '#' || *p == '/') { *p = 0; break; }
    if (!r || r->nit >= PT_MAXIT) { free(s); return 0; }
    PtItem *it = &r->it[r->nit];
    memset(it, 0, sizeof(*it));
    char *tok = strtok(s, " \t\r\n\r\n");
    if (!tok) { free(s); return 0; }
    for (int i = 0; tok[i] && i < PT_MAXKEY - 1; i++)
        it->key[i] = (char)tolower((unsigned char)tok[i]), it->key[i + 1] = 0;
    int nf = 0;
    while ((tok = strtok(NULL, " \t\r\n\r\n")) != NULL) {
        char *end = NULL;
        float v = (float)strtod(tok, &end);
        if (end && *end == 0 && end != tok) {
            if (nf < 8) it->f[nf++] = v;
        } else if (!it->txt[0]) {
            snprintf(it->txt, sizeof it->txt, "%s", tok);   /* name-ish token */
        }
    }
    it->nf = nf;
    r->nit++;
    free(s);
    return 0;
}
/* configureVisual(RenderType[,...])V, configureMesh(...)V, configureTexture(...)V,
 * applyDecal(...)V: counted; the geometry itself is engine-side. */
static u32 nat_pt_visual(u32 t, u32 *a) {
    (void)a;
    PtRec *r = pt_get(t, 1);
    if (r) r->nvis++;
    return 0;
}
static u32 nat_pt_mesh(u32 t, u32 *a) {
    (void)a;
    PtRec *r = pt_get(t, 1);
    if (r) r->nmesh++;
    return 0;
}
static u32 nat_pt_texture(u32 t, u32 *a) {
    (void)a;
    PtRec *r = pt_get(t, 1);
    if (r) r->ntex++;
    return 0;
}
static u32 nat_pt_decal(u32 t, u32 *a) {
    (void)a;
    PtRec *r = pt_get(t, 1);
    if (r) r->ndecal++;
    return 0;
}
/* =================== pure-computation natives (docs/65 10.28) ============
 * Three that can be EXACT rather than guessed -- all read out of the binary:
 *   String.intValue()I     FUN_006615a0: _swscanf(text, L"%d", &out)
 *                          (format at 0x6e7c54; scanner wrapper FUN_00567cf0)
 *   String.floatValue()F   FUN_006615e0: _swscanf(text, L"%f", &out)  (0x703b1c)
 *   Math.random()F         FUN_00405390: fild(rand() & 0x7fff, 0x7fff->0x7ffe)
 *                          * (float)3.0518509447574615e-05  [= 1/32767 as f32]
 *                          i.e. the MSVC CRT LCG (s*214013+2531011 >>16 &0x7fff).
 * The originals leave `out` uninitialised when nothing matches; we zero it and
 * say so (uninitialised stack garbage is not reproducible). */
static u32 g_rand_seed = 1;                     /* CRT default seed */
static u32 nat_str_intValue(u32 t, u32 *a) {
    (void)a;
    const u16 *w = str_w(t);
    char *s = w ? narrow(w) : NULL;
    int v = 0;
    if (s) { sscanf(s, "%d", &v); free(s); }
    return (u32)v;
}
static u32 nat_str_floatValue(u32 t, u32 *a) {
    (void)a;
    const u16 *w = str_w(t);
    char *s = w ? narrow(w) : NULL;
    float f = 0.0f;
    if (s) { sscanf(s, "%f", &f); free(s); }
    return fu2u(f);
}
static u32 nat_math_random(u32 t, u32 *a) {
    (void)t; (void)a;
    g_rand_seed = g_rand_seed * 214013u + 2531011u;
    u32 r = (g_rand_seed >> 16) & 0x7fff;
    if (r == 0x7fff) r = 0x7ffe;
    /* the engine does fild(int); fmul dword[0x6e7cf4] (a FLOAT32) -> double,
     * then the wrapper narrows to f32.  Use the exact bit pattern 0x38000100
     * (= 1/32767 as float32) instead of a double literal: the double literal is
     * NOT the same number and can flip the last result bit. */
    f32 scale = u2fu(0x38000100u);
    double d = (double)(int)r * (double)scale;
    return fu2u((f32)d);
}
/* ================= natives whose whole body is an ENGINE call ============
 * (docs/65 10.29)  Every one of these was disassembled down to its RET first.
 * They share one shape:
 *     unwrap args -> FUN_00643bf0(this, 0x8dcb2c) [= the receiver's NATIVE obj]
 *     -> if non-null, ONE call into the engine (vtable slot or a singleton)
 *     -> ret, reading/writing NO Java field.
 * None returns a value and none can touch state this model owns, so the
 * faithful Java-level behaviour is "nothing happens".  The stops are recorded
 * so a future native-side model resumes exactly here:
 *   gui.Object.initBegin()V          0x5a0360  call [native + 0x24]
 *   gui.Object.initEnd()V            0x5a0390  call [native + 0x28]
 *   gui.TabList.refresh()V           0x5a0e40  call [native + 0x164]
 *   gui.Component.postCustomEvent(I)V 0x5a0410 FUN_005dce00(native, eventId)
 *   gfx.Viewport.deactivate()V       0x40ae30  FUN_004c3140(gfx_singleton, native)
 *   gfx.Animation.finalize()V        0x40aec0  call [native] + name lookup +
 *                                              clears a native slot (engine side)
 *   ResourceRef.unload(I)V           0x4107d0  FUN_0042b450(native, handle) =
 *                                              releases the PASSED handle; the
 *                                              receiver itself is untouched
 */
/* Chassis.getSfxTable(I)L.../SfxTable; = 0x4884e0.  Body read to the RET:
 *   i < 0            -> NULL
 *   native = FUN_00643bf0(this, 0x8dcb2c); node = native[+8]; NULL -> NULL
 *   FUN_0055a230(node, 0xa0000000, 0) < 0 -> NULL
 *   kind = node[+0x24]; obj = node + per_kind_offset_table[0x7439b0][kind]
 * i.e. the engine hands back a pointer INTO its own chassis struct.  The rest
 * of the body (0x488512.., read 2026-10-18 on the live game, docs/65 10.54):
 *   q = [sub + 0x64];  if (i > 3) -> NULL         <-- ONLY 0..3 ARE VALID
 *   switch (i) { 0: esi = [q+0x24d4] + 0x128;  1: + 0x370;
 *                2: + 0x5b8;                  3: + 0x804; }
 * i.e. one SfxTable per WHEEL, hanging off the axle/suspension struct
 * (q+0x24d4, the same field Chassis.setSteerWheel uses).  This model has no
 * chassis struct, so it returns one cached, fully constructed
 * java.game.parts.SfxTable per (receiver, index) -- but it MUST reproduce the
 * null path exactly: i < 0 and i > 3 both give NULL in the engine, and the
 * earlier version returned an object for i > 3 (real difference, found by
 * reading the whole body, not by probing: in the sandbox both sides are null). */
#define NST_SFX 8
static struct { u32 self; i32 idx; Inst *obj; } g_sfx[NST_SFX];
static u32 nat_chassis_getSfxTable(u32 t, u32 *a) {
    i32 i = (i32)a[0];
    if (i < 0 || i > 3 || !t) return 0;   /* engine: i>3 -> NULL (wheel tables) */
    for (int k = 0; k < NST_SFX; k++)
        if (g_sfx[k].obj && g_sfx[k].self == t && g_sfx[k].idx == i)
            return (u32)(uintptr_t)g_sfx[k].obj;
    Class *c = cls_find("java.game.parts.SfxTable");
    if (!c) c = load_class("java.game.parts.SfxTable");
    if (!c) return 0;
    Inst *o = gw_new_obj(c);
    for (int k = 0; k < NST_SFX; k++)
        if (!g_sfx[k].obj) { g_sfx[k].self = t; g_sfx[k].idx = i; g_sfx[k].obj = o; break; }
    return (u32)(uintptr_t)o;
}
/* ======== field-getter bucket: the two members that are NOT engine-state ===
 * (docs/65 10.34)  The bucket mixed three different things; reading all 13 rows
 * separated them:
 *   exact/ implementable:  java.net.Network.Leave()I  = 0x41a750  mov eax,1; ret
 *                          -> a constant 1 ("true"); no state at all.
 *   engine-memory store:   java.gfx.GfxEngine.setFadeParams(II)V = 0x40b130
 *                          -> writes [0x7432c8]+0x1b0/+0x1b4 (the gfx
 *                          singleton, no null check in the engine) => no-op.
 *                          java.sound.Sound.nextTrack()V/prevTrack()V =
 *                          0x40ea10/0x40ea20 -> read the global current-music-set
 *                          pointer [0x75df08] (the same one changeMusicSet
 *                          writes) and tail-jump into the engine's next/prev
 *                          track routine => engine-side, no Java state.
 *   engine-state READS (stay in the boundary): Replay.length()F 0x4280e0,
 *                          Replay.pos()F 0x428110, Replay.native_mode()I
 *                          0x4280b0 (all read the global replay object
 *                          [0x770fc0]), System.simTime()F 0x4c0f70 (reads the
 *                          live frame clock singleton [0x743230]),
 *                          Network.getIndex(I)I 0x4098b0 (reads the global net
 *                          object [0x74233c]; returns 0 when it is null, which is
 *                          what this model's stub already does).               */
/* java.io.Input.createHotkey(Ljava.io.Hotkey;)V = 0x404970: scans the engine's
 * GLOBAL hotkey table (0x75d400 .. 0x75db00, stride 0x1c) for the entry holding
 * this hotkey object, clears a flag for it (0x75d3f8 + i*0x1c = 0), then looks
 * for a free slot.  Pure engine-global state, no Java field, no return value
 * => Java-visible no-op.  (The same global block continues with the music-set
 * table at 0x75db00 that changeMusicSet indexes - docs/65 10.34.) */
/* java.util.resource.GameRef.command(Ljava.lang.String;)V = 0x4209f0  (88 sites)
 * Body (decompiled): vm_get_int_field(this) -> the GameRef's engine object;
 * if the String arg is non-null it is COPIED into a malloc'd C buffer; then
 *   native == 0  ->  log "Reference conflict (%s@%d)" (error path only)
 *   else         ->  FUN_00435650(native, 0, 0x10, text, 0)
 * FUN_00435650 is the engine's generic NOTIFICATION DISPATCH: it walks the
 * object node (testing node[0x88] against the mask), calls the node's vtable
 * handler ((*(node+0xc)+0x3c)(node[100], 0, 0x10, text)), then walks the
 * handler list at node[0x74] calling every entry whose mask matches, and hands
 * the text payload to FUN_004351d0.  Nothing on this path writes a Java field,
 * so the Java-visible behaviour of command() is a no-op.
 * KNOWN LIMITATION (documented, not hidden): the engine's handlers can be Java
 * callbacks registered through GameRef.registerCallback(I) -- callbacks are not
 * modelled here (that native is a no-op too), so a command whose only visible
 * effect would be firing a Java callback is not reproduced.  Everything else it
 * can do is engine-side state. */
static u32 nat_net_leave(u32 t, u32 *a) { (void)t; (void)a; return 1u; }
/* java.lang.Float.toString(F,Ljava.lang.String;)Ljava.lang.String; = 0x404e60
 * Body read: vm_get_int_field(fmt) -> the WIDE buffer of the format String;
 * fld arg1; fstp qword (the f32 is widened to double); then
 * FUN_00567cc0(dst,n) whose own body is exactly `_vswprintf(dst,n,fmt,va)`;
 * the buffer is finally turned into a Java String.  So the Java-visible
 * semantics is: swprintf(fmt, (double)value) -- WIDE specifiers, DOUBLE
 * argument -- which is what this model does with the host C library.
 * Differentially verified (probe/spec_floatstr.json); that probe is also the
 * test of whether the host libm formats floats the way the engine's MSVC CRT
 * does (incl. tie rounding like "%.0f" of 1234.5). */
/* ---- MSVC-compatible float formatting (docs/65 10.36) -------------------
 * Two divergences from the host (glibc/MinGW) CRT were MEASURED with the
 * differential probe, not guessed:
 *   "%.0f" of 1234.5 -> engine "1235", host "1234"     (exact tie: the engine's
 *        MSVC rounds ties AWAY FROM ZERO, glibc rounds to even)
 *   "%.3e" of 1e20   -> engine "1.000e+020", host "1.000e+20" (MSVC keeps at
 *        least THREE exponent digits)
 * Both are fixed here: the value is expanded with the host at much higher
 * precision and the decimal string is then rounded with MSVC's rule (nearest,
 * ties away from zero); exponent output gets its exponent padded to 3 digits.
 * %g/%G gets the exponent padding but still inherits the host's tie rounding --
 * a KNOWN, PROBED gap (spec_floatstr case 12). */
static int f_conv(const wchar_t *f) {
    const wchar_t *p = f;
    int c = 0;
    for (; *p; p++) if (!wcschr(L"0123456789.-+ #", *p)) c = *p;
    return c;
}
static int f_prec(const wchar_t *f) {
    const wchar_t *d = wcsrchr(f, L'.');
    if (!d) return 6;
    return (int)wcstol(d + 1, NULL, 10);
}
/* flags/width; the width digits stop at the '.' (they used to clobber it) */
static void f_flags(const wchar_t *f, int *plus, int *minus, int *zero, int *width) {
    const wchar_t *p = wcschr(f, L'%');
    *plus = *minus = *zero = 0; *width = 0;
    if (!p) return;
    for (p++; *p && *p != L'.' && !wcschr(L"fFeEgG", *p); p++) {
        if (*p == L'+') *plus = 1;
        else if (*p == L'-') *minus = 1;
        else if (*p == L'0') *zero = 1;
        else if (*p >= L'1' && *p <= L'9') {
            wchar_t *e;
            long v = wcstol(p, &e, 10);
            *width = (int)v;
            p = e - 1;
        }
    }
}
static void pad_exp3(wchar_t *s) {
    wchar_t *e = NULL;
    for (wchar_t *p = s; *p; p++) if (*p == L'e' || *p == L'E') { e = p; break; }
    if (!e) return;
    wchar_t *d = e + 1;
    if (*d == L'+' || *d == L'-') d++;
    size_t nd = 0;
    while (d[nd] >= L'0' && d[nd] <= L'9') nd++;
    if (nd == 0 || nd >= 3) return;
    size_t need = 3 - nd;
    if (wcslen(s) + need >= 1500) return;
    memmove(d + need, d, (nd + 1) * sizeof(wchar_t));
    for (size_t i = 0; i < need; i++) d[i] = L'0';
}
/* digits of |v| rounded to `keep` digits: nearest, ties AWAY FROM ZERO (MSVC) */
static int round_digits_away(wchar_t *dig, int ndig, int keep) {
    int up = 0;
    if (keep < 0) keep = 0;
    if (keep < ndig) {
        if (dig[keep] > L'5') up = 1;
        else if (dig[keep] == L'5') up = 1;
    }
    int n = keep;
    if (up) {
        int i = keep - 1;
        for (; i >= 0; i--) {
            if (dig[i] < L'9') { dig[i]++; break; }
            dig[i] = L'0';
        }
        if (i < 0) {
            memmove(dig + 1, dig, (size_t)keep * sizeof(wchar_t));
            dig[0] = L'1';
            n = keep + 1;
        }
    }
    dig[n] = 0;
    return n;
}
/* magnitude of v in the requested form, sign not included.
 *   expform: d[.ddd]e+0XX   (with `strip`, trailing fraction zeros go away)
 *   fixed  : iii[.ddd]                                                       */
static void fmt_mag(double v, int N, int expform, int strip, wchar_t *out, size_t cap) {
    double av = v < 0 ? -v : v;
    if (N < 0) N = 0;
    int xp = N + 60;
    if (xp > 700) xp = 700;
    if (expform) {
        wchar_t xf[32], tmp[4096];
        swprintf(xf, 32, L"%%.%de", xp);
        tmp[0] = 0;
        swprintf(tmp, 4096, xf, av);
        wchar_t *e = NULL;
        for (wchar_t *q = tmp; *q; q++) if (*q == L'e' || *q == L'E') { e = q; break; }
        long ex = e ? wcstol(e + 1, NULL, 10) : 0;
        wchar_t dig[1500];
        int ndig = 0;
        for (const wchar_t *q = tmp; e && q < e; q++)
            if (*q >= L'0' && *q <= L'9' && ndig < 1490) dig[ndig++] = *q;
        dig[ndig] = 0;
        int keep = 1 + N;
        if (keep > ndig) keep = ndig;
        if (keep < 1) keep = 1;
        int n2 = round_digits_away(dig, ndig, keep);
        if (n2 > keep) ex++;
        int last = N;
        if (strip) { while (last > 0 && dig[last] == L'0') last--; }
        size_t q2 = 0;
        out[q2++] = dig[0];
        if (last > 0) {
            out[q2++] = L'.';
            for (int i = 1; i <= last; i++) out[q2++] = (i < n2) ? dig[i] : L'0';
        }
        /* MSVC prints at least THREE exponent digits (e+020, not e+20); %+03ld
         * would only guarantee three characters INCLUDING the sign. */
        q2 += (size_t)swprintf(out + q2, (int)(cap - q2), L"e%lc%03ld",
                               (wchar_t)(ex < 0 ? L'-' : L'+'),
                               (long)(ex < 0 ? -ex : ex));
        out[cap - 1] = 0;
    } else {
        wchar_t xf[32], mag[1400];
        swprintf(xf, 32, L"%%.%df", xp);
        mag[0] = 0;
        swprintf(mag, 1400, xf, av);
        wchar_t dig[1500];
        int ndig = 0, ip = 0;
        for (const wchar_t *q = mag; *q; q++) {
            if (*q == L'.') { ip = ndig; continue; }
            if (ndig < 1490) dig[ndig++] = *q;
        }
        dig[ndig] = 0;
        int keep = ip + N;
        if (keep < 0) keep = 0;
        if (keep > ndig) keep = ndig;
        int n2 = round_digits_away(dig, ndig, keep);
        int newip = ip + (n2 - keep);
        wchar_t body[1600];
        int bp = 0;
        for (int i = 0; i < newip - n2 && bp < 1500; i++) body[bp++] = L'0';
        for (int i = 0; i < n2 && bp < 1500; i++) {
            if (i == newip) body[bp++] = L'.';
            body[bp++] = dig[i];
        }
        body[bp] = 0;
        if (strip) {                                   /* %g: drop fraction zeros */
            wchar_t *dot = wcschr(body, L'.');
            if (dot) {
                wchar_t *end2 = body + wcslen(body);
                while (end2 > dot + 1 && end2[-1] == L'0') *--end2 = 0;
                if (end2 == dot + 1) *dot = 0;
            }
        }
        wcsncpy(out, body, cap - 1);
        out[cap - 1] = 0;
    }
}
static u32 nat_float_toString(u32 t, u32 *a) {
    (void)t;
    const u16 *wfmt = str_w(a[1]);
    double v = (double)u2fu(a[0]);
    wchar_t out[4096];
    out[0] = 0;
    if (!wfmt) {
        u16 *w0 = (u16 *)calloc(1, sizeof(u16));
        return (u32)(uintptr_t)intern_w(w0);
    }
    int conv = f_conv(wfmt);
    int N = f_prec(wfmt);
    int plus, minus, zero, width;
    f_flags(wfmt, &plus, &minus, &zero, &width);
    wchar_t body[2000];
    body[0] = 0;
    if (conv == L'g' || conv == L'G') {
        int P = (N <= 0) ? 1 : N;
        wchar_t tmp[64];
        swprintf(tmp, 64, L"%.17e", v < 0 ? -v : v);
        wchar_t *e = wcschr(tmp, L'e');
        long E = e ? wcstol(e + 1, NULL, 10) : 0;
        int expform = (E < -4 || E >= P);
        int Nd = expform ? P - 1 : (P - 1 - (int)E);
        fmt_mag(v, Nd, expform, 1, body, 2000);
        if (conv == L'G') for (wchar_t *q = body; *q; q++) if (*q == L'e') *q = L'E';
    } else if (conv == L'e' || conv == L'E') {
        fmt_mag(v, N, 1, 0, body, 2000);
        if (conv == L'E') for (wchar_t *q = body; *q; q++) if (*q == L'e') *q = L'E';
    } else {
        fmt_mag(v, N, 0, 0, body, 2000);
    }
    wchar_t sign = (v < 0) ? L'-' : (plus ? L'+' : 0);
    int total = (int)wcslen(body) + (sign ? 1 : 0);
    int padn = (width > total) ? width - total : 0;
    int q = 0;
    if (!minus && padn && zero) {
        if (sign) out[q++] = sign;
        for (int i = 0; i < padn && q < 4000; i++) out[q++] = L'0';
    } else {
        if (!minus) for (int i = 0; i < padn && q < 4000; i++) out[q++] = L' ';
        if (sign && q < 4000) out[q++] = sign;
    }
    wcsncpy(out + q, body, (size_t)(4095 - q));
    out[4095] = 0;
    q = (int)wcslen(out);
    if (minus) for (int i = 0; i < padn && q < 4090; i++) out[q++] = L' ';
    out[q] = 0;
    size_t n = wcslen(out) + 1;
    u16 *w = (u16 *)malloc(n * sizeof(u16));
    if (!w) return 0;
    memcpy(w, out, n * sizeof(u16));
    return (u32)(uintptr_t)intern_w(w);
}
/* ============ java.gui.WStringList: the whole family (docs/65 10.37) ======
 * Every member is a native (create/destroy/add/insert/remove/set/clear/
 * getCount/toString), so the list itself lives in the engine -- but every
 * mutation arrives as a JAVA-visible argument, so the Java-level behaviour is
 * reproducible and testable:
 *   getCount()I  0x5a07b0  return native[+0x10]              -> item count
 *   add(String)  0x5a0850  append (count++ + append)
 *   insert(String,I) 0x5a0920  insert at index
 *   remove(I)    0x5bb410  FUN_005bae50(list, index)
 *   clear()      0x5bb350  FUN_005baa80(list)
 *   create()     0x5a07e0  allocate the engine list (=> empty)
 *   destroy()    0x5bb2f0  free it (=> empty here)
 *   set(WStringList) 0x5bb450  FUN_005baaf0(dst, src) = copy contents
 *   toString()   0x5bb380  FUN_005bad20 = `for i { if (i) append(SEP); append
 *                          item_i }` with SEP = the wide constant at 0x6ff81c
 *                          == L"\n" (read out of the image) -> join with "\n"
 * NOT modelled (1 call site): set(String)V 0x5bb4a0 -- its body reads the
 * String's native object and calls a helper whose contract is not pinned down;
 * a guess could not be caught by any test, so it stays a documented stub. */
#define WSL_MAX 8
#define WSL_ITEMS 64
static struct { Inst *self; Inst *it[WSL_ITEMS]; int n; } g_wsl[WSL_MAX];
static void *wsl_of(Inst *o) {
    if (!o) return NULL;
    for (int i = 0; i < WSL_MAX; i++)
        if (g_wsl[i].self == o) return &g_wsl[i];
    for (int i = 0; i < WSL_MAX; i++)
        if (!g_wsl[i].self) {
            g_wsl[i].self = o; g_wsl[i].n = 0;
            return &g_wsl[i];
        }
    return NULL;
}
static u32 nat_wsl_create(u32 t, u32 *a) {
    (void)a; void *l = wsl_of((Inst *)(uintptr_t)t);
    if (l) ((struct { Inst *self; Inst *it[WSL_ITEMS]; int n; } *)l)->n = 0;
    return 0;
}
static u32 nat_wsl_clear(u32 t, u32 *a) {
    (void)a; void *l = wsl_of((Inst *)(uintptr_t)t);
    if (l) ((struct { Inst *self; Inst *it[WSL_ITEMS]; int n; } *)l)->n = 0;
    return 0;
}
static u32 nat_wsl_add(u32 t, u32 *a) {
    void *l = wsl_of((Inst *)(uintptr_t)t);
    if (!l) return 0;
    struct WSL { Inst *self; Inst *it[WSL_ITEMS]; int n; };
    struct WSL *w = (struct WSL *)l;
    if (w->n < WSL_ITEMS) w->it[w->n++] = (Inst *)(uintptr_t)a[0];
    return 0;
}
static u32 nat_wsl_insert(u32 t, u32 *a) {
    struct WSL { Inst *self; Inst *it[WSL_ITEMS]; int n; };
    struct WSL *w = (struct WSL *)wsl_of((Inst *)(uintptr_t)t);
    if (!w) return 0;
    i32 i = (i32)a[1];
    if (i < 0 || i > w->n || w->n >= WSL_ITEMS) return 0;
    for (int k = w->n; k > i; k--) w->it[k] = w->it[k - 1];
    w->it[i] = (Inst *)(uintptr_t)a[0];
    w->n++;
    return 0;
}
static u32 nat_wsl_remove(u32 t, u32 *a) {
    struct WSL { Inst *self; Inst *it[WSL_ITEMS]; int n; };
    struct WSL *w = (struct WSL *)wsl_of((Inst *)(uintptr_t)t);
    if (!w) return 0;
    i32 i = (i32)a[0];
    if (i < 0 || i >= w->n) return 0;
    for (int k = i; k + 1 < w->n; k++) w->it[k] = w->it[k + 1];
    w->n--;
    return 0;
}
static u32 nat_wsl_count(u32 t, u32 *a) {
    (void)a;
    struct WSL { Inst *self; Inst *it[WSL_ITEMS]; int n; };
    struct WSL *w = (struct WSL *)wsl_of((Inst *)(uintptr_t)t);
    return w ? (u32)w->n : 0;
}
static u32 nat_wsl_setlist(u32 t, u32 *a) {
    struct WSL { Inst *self; Inst *it[WSL_ITEMS]; int n; };
    struct WSL *dst = (struct WSL *)wsl_of((Inst *)(uintptr_t)t);
    struct WSL *src = (struct WSL *)wsl_of((Inst *)(uintptr_t)a[0]);
    if (!dst) return 0;
    dst->n = 0;
    if (src) for (int i = 0; i < src->n && dst->n < WSL_ITEMS; i++) dst->it[dst->n++] = src->it[i];
    return 0;
}
static u32 nat_wsl_toString(u32 t, u32 *a) {
    (void)a;
    struct WSL { Inst *self; Inst *it[WSL_ITEMS]; int n; };
    struct WSL *w = (struct WSL *)wsl_of((Inst *)(uintptr_t)t);
    /* the engine's list->string builder yields NOTHING for an empty
     * list: the real VM hands back a null String (probe spec_wsl r2). */
    if (!w || w->n == 0) return 0;
    wchar_t buf[4096];
    int q = 0;
    buf[0] = 0;
    if (w) for (int i = 0; i < w->n; i++) {
        if (i) { if (q < 4090) buf[q++] = (wchar_t)10; }   /* SEP = L"\n" (0x6ff81c) */
        const u16 *txt = str_w((u32)(uintptr_t)w->it[i]);
        if (txt) for (const u16 *c = txt; *c && q < 4090; c++) buf[q++] = (wchar_t)*c;
    }
    buf[q] = 0;
    size_t n = wcslen(buf) + 1;
    u16 *out = (u16 *)malloc(n * sizeof(u16));
    if (!out) return 0;
    memcpy(out, buf, n * sizeof(u16));
    return (u32)(uintptr_t)intern_w(out);
}
/* ===== integer -> string + string append: pure formatting (docs/65 10.40) ==
 * Found by the v3 registry scan (their entries were missing before, so they sat
 * in the backlog without an address).
 *   java.lang.Integer.toString(I)Ljava.lang.String; = 0x404f30
 *     body: unwrap -> the int; then FUN_00567cc0(dst, fmt@0x6e7c5c, value),
 *     whose own body is _vswprintf -> decimal text -> Java String (FUN_00644cf0).
 *   java.lang.String.append(I)Ljava.lang.String; = 0x661620
 *     body: the receiver's wide text (vm_get_int_field + FUN_006675de, a wide
 *     strlen) followed by the decimal text of the int argument -> new String.
 * Both are pure formatting, so they are verified bit-for-bit by the probe
 * (probe/spec_intstr.json) instead of being assumed. */
static u16 *w_dup16(const wchar_t *src, int n) {
    if (n < 0) n = 0;
    u16 *w = (u16 *)malloc((size_t)(n + 1) * sizeof(u16));
    if (!w) return NULL;
    for (int i = 0; i < n; i++) w[i] = (u16)src[i];
    w[n] = 0;
    return w;
}
static u32 nat_int_toString(u32 t, u32 *a) {
    (void)t;
    wchar_t buf[80];
    swprintf(buf, 80, L"%d", (int)a[0]);
    u16 *w = w_dup16(buf, (int)wcslen(buf));
    return w ? (u32)(uintptr_t)intern_w(w) : 0;
}
static u32 nat_str_append_i(u32 t, u32 *a) {
    /* Semantics settled by the differential probe, NOT by my first reading of
     * the body (docs/65 10.40): the engine appends the wide CHARACTER whose
     * code is the argument, it does NOT format the number.
     * Evidence: 42 -> '*', 123456789 -> U+CD15 (0xCD15 = 123456789 & 0xFFFF),
     * 0 -> a NUL that terminates the text, negatives -> the low 16 bits as a
     * (control/high) character.  My "text + decimal" first version failed all
     * five probe cases; this one is the rule that fits every observed value. */
    const u16 *src = str_w(t);
    wchar_t buf[4096];
    int q = 0;
    if (src) for (const u16 *p2 = src; *p2 && q < 4090; p2++) buf[q++] = (wchar_t)*p2;
    buf[q++] = (wchar_t)(a[0] & 0xFFFFu);
    buf[q] = 0;
    u16 *w = w_dup16(buf, (int)wcslen(buf));
    return w ? (u32)(uintptr_t)intern_w(w) : 0;
}
/* ===== java.lang.Vector3 pure math (docs/65 10.42) ========================
 * All four float-valued members are x87 math over the receiver's JAVA fields,
 * read BY NAME through the engine's own accessor (FUN_006435f0 =
 * vm_get_float_field: "x"@0x6e7d58 / "y"@0x6e7d54 / "z"@0x6e7d50) and written
 * back through FUN_00643a90 = vm_set_float_field.  No engine state is touched,
 * so these can be (and are) settled by the differential probe on BIT PATTERNS:
 *   length()F    = fsqrt(x*x + y*y + z*z)          0x406810
 *   sqrlength()F =        x*x + y*y + z*z          0x406880
 *   distance(v)F = fsqrt(sum of squared deltas)    0x4068f0
 *   normalize()V = if (len != 0) v *= (1.0f/len)   0x406960   (SSE float32)
 * The x87 chain keeps 80-bit intermediates and rounds only when it stores into
 * a float32 slot, so the sums are done in `long double` here and rounded once;
 * normalize's divide/multiply really are float32 (divss/mulss) and are done in
 * float.  Field slots carry f32 BITS (u2fu/fu2u). */
static f32 *v3slot(u32 t, const char *name) {
    Inst *o = (Inst *)(uintptr_t)t;
    Class *decl = NULL;
    int idx;
    if (!inst_valid(o) || !o->cls) return NULL;
    idx = find_field(o->cls, name, 1, &decl);
    if (idx < 0) return NULL;
    return (f32 *)inst_field(o, idx);
}
static f32 v3get(u32 t, const char *n) {
    f32 *p = v3slot(t, n);
    return p ? u2fu(*(u32 *)p) : 0.0f;
}
static void v3put(u32 t, const char *n, f32 v) {
    f32 *p = v3slot(t, n);
    if (p) *(u32 *)p = fu2u(v);
}
static long double v3sq(u32 t) {
    long double x = v3get(t, "x"), y = v3get(t, "y"), z = v3get(t, "z");
    return x * x + y * y + z * z;
}
static u32 nat_v3_sqrlength(u32 t, u32 *a) { (void)a; return fu2u((f32)v3sq(t)); }
static u32 nat_v3_length(u32 t, u32 *a) { (void)a; return fu2u((f32)sqrtl(v3sq(t))); }
static u32 nat_v3_distance(u32 t, u32 *a) {
    long double dx = (long double)v3get(t, "x") - (long double)v3get(a[0], "x");
    long double dy = (long double)v3get(t, "y") - (long double)v3get(a[0], "y");
    long double dz = (long double)v3get(t, "z") - (long double)v3get(a[0], "z");
    return fu2u((f32)sqrtl(dx * dx + dy * dy + dz * dz));
}
static u32 nat_v3_normalize(u32 t, u32 *a) {
    (void)a;
    f32 len = (f32)sqrtl(v3sq(t));
    if (len != 0.0f) {                       /* SSE: divss by the len, then mulss */
        f32 inv = 1.0f / len;
        f32 x = v3get(t, "x") * inv, y = v3get(t, "y") * inv, z = v3get(t, "z") * inv;
        v3put(t, "x", x); v3put(t, "y", y); v3put(t, "z", z);
    }
    return 0;
}
/* ===== Ypr <-> Vector3 (docs/65 10.43) =================================
 * 0x562900 Matrix.setFromYpr(out3x4, ypr*)  -- pure x87 fsin/fcos + SSE in a
 *   fixed order; transcribed literally, rows are 16 bytes apart with 12 used.
 *   0x406540 Vector3.toYpr():  yaw = fpatan(-x,-z); pitch = fpatan(y,sqrt(x2+z2));
 *                             roll = 0   (x==0 && z==0 -> yaw=0, pitch=+/-pi/2)
 *   0x406bf0 Ypr.toVector3():  v = -(M[2], M[5], M[8]) = -(third column)
 * Both build their result the way the engine does: load_class(name) ->
 * newInstance (which runs <init>) -> write the fields BY NAME.
 * Object results are verified through the Java toString() of the result
 * (probe case rtype Ljava.lang.String;) plus a toYpr->toVector3 round trip. */
static void mtx_from_ypr(f32 m[12], const f32 y[3]) {
    long double a0 = y[0], a1 = y[1], a2 = y[2];      /* yaw, pitch, roll */
    f32 S0 = (f32)sinl(a0), C0 = (f32)cosl(a0);
    f32 S1 = (f32)sinl(a1), C1 = (f32)cosl(a1);
    f32 S2 = (f32)sinl(a2), C2 = (f32)cosl(a2);
    /* every operation in the order the listing performs it, all in f32 */
    f32 t0 = S2 * S1;
    f32 u1 = t0 * S0, t1 = t0 * C0;
    f32 t2 = C2 * C0;
    u1 = u1 + t2;                                     /* [ecx+0] */
    f32 m2 = C1 * S0;                                 /* [ecx+8]  */
    f32 m4 = S2 * C1;                                 /* [ecx+0x10] */
    f32 x6 = C2 * S1;
    f32 x7 = S2 * C0;
    f32 m1 = x6 * S0 - x7;                            /* [ecx+4]  */
    f32 m5 = C2 * C1;                                 /* [ecx+0x14] */
    f32 x0 = t1 - C2 * S0;                            /* [ecx+0x20] */
    f32 m6 = 0.0f - S1;                               /* [ecx+0x18] */
    f32 m9 = x6 * C0 + S2 * S0;                       /* [ecx+0x24] */
    f32 m10 = C1 * C0;                                /* [ecx+0x28] */
    m[0] = u1;  m[1] = m1;  m[2] = m2;  m[3] = 0.0f;
    m[4] = m4;  m[5] = m5;  m[6] = m6;  m[7] = 0.0f;
    m[8] = x0;  m[9] = m9;  m[10] = m10; m[11] = 0.0f;
}
/* engine-style object construction: load_class + newInstance (runs <init>) */
static u32 mk_inst(const char *name) {
    Class *c = cls_find(name);
    if (!c) c = load_class(name);
    if (!c) return 0;
    Inst *o = inst_new(c);
    if (!o) return 0;
    Method *ini = find_ctor(c);
    if (ini && !(ini->flags & 0x100)) {
        Class *own = member_owner(c, ini);
        if (own && ini->tree_slot >= 0 && ini->tree_slot < own->nrec)
            run_record(own, ini, (u32)(uintptr_t)o, 0, -1);
    }
    return (u32)(uintptr_t)o;
}
/* 0x406540 Vector3.toYpr()Ljava.lang.Ypr; */
static u32 nat_v3_toYpr(u32 t, u32 *a) {
    (void)a;
    f32 x = v3get(t, "x"), y = v3get(t, "y"), z = v3get(t, "z");
    f32 yy, pp, rr;
    if (z == 0.0f && x == 0.0f) {                      /* degenerate axis */
        yy = 0.0f;
        pp = (y > 0.0f) ? u2fu(0x3FC90FDBu) : u2fu(0xBFC90FDBu);   /* +/-pi/2 */
        rr = 0.0f;
    } else {
        yy = (f32)atan2l(-(long double)x, -(long double)z);
        long double h = sqrtl((long double)x * x + (long double)z * z);
        pp = (f32)atan2l((long double)y, h);           /* fpatan(st1=y, st0=h) */
        rr = 0.0f;
        /* x87 FPATAN returns +0 where the C atan2 keeps the numerator's sign:
         * Vector3(0,0,-1).toYpr() is +0.000 on the real VM but -0.000 with a
         * plain atan2l (probe case 5 of spec_ypr.json) -- adding +0.0 turns
         * -0.0 into +0.0 and leaves every other value untouched. */
        if (yy == 0.0f) yy = yy + 0.0f;
        if (pp == 0.0f) pp = pp + 0.0f;
    }
    u32 o = mk_inst("java.lang.Ypr");
    if (!o) return 0;
    v3put(o, "y", yy); v3put(o, "p", pp); v3put(o, "r", rr);
    return o;
}
/* 0x406bf0 Ypr.toVector3()Ljava.lang.Vector3; */
static u32 nat_ypr_toVector3(u32 t, u32 *a) {
    (void)a;
    f32 ypr[3], m[12];
    ypr[0] = v3get(t, "y"); ypr[1] = v3get(t, "p"); ypr[2] = v3get(t, "r");
    mtx_from_ypr(m, ypr);
    u32 o = mk_inst("java.lang.Vector3");
    if (!o) return 0;
    /* The three reads in the listing are 0x10 apart ([esp+0x30],[esp+0x40],
     * [esp+0x50]) while the matrix base is [esp+0x28] and rows are 0x10 bytes:
     * so they are M[2], M[6], M[10] = the third COLUMN, not a row.  Self-check
     * that settles it: the result must be a unit vector (length() == 1.0 for
     * any Ypr) -- reading it as a row gives 1.14 for Ypr(0.5,-0.4,0.2). */
    v3put(o, "x", 0.0f - m[2]);                        /* column 2 */
    v3put(o, "y", 0.0f - m[6]);
    v3put(o, "z", 0.0f - m[10]);
    return o;
}
/* 0x4065a0 Vector3.rotate(Ypr)Ljava.lang.Vector3; -- rotate this by the Ypr's
 * matrix and write the result back INTO the receiver (the tail returns `this`,
 * not a new object).  Body shape, read to the final ret:
 *   read the Ypr's y/p/r by name -> 0x567e00 packs them -> 0x562900 builds the
 *   3x4 matrix -> 0x643a40 reads this x/y/z -> three SSE dot products
 *   (mulss/addss in the listing's order) -> 0x643a90 writes x/y/z back.
 * The three dot products consume the matrix rows at [esp+0x34..0x3c],
 * [0x44..0x4c], [0x54..0x5c]: contiguous triples 0x10 apart = the same row
 * layout mtx_from_ypr writes, so the result is plain M*v.  Verified against the
 * real VM through the Java toString() of the receiver (spec_rotate.json). */
static u32 nat_v3_rotate_ypr(u32 t, u32 *a) {
    f32 ypr[3], m[12];
    ypr[0] = v3get(a[0], "y"); ypr[1] = v3get(a[0], "p"); ypr[2] = v3get(a[0], "r");
    mtx_from_ypr(m, ypr);
    f32 x = v3get(t, "x"), y = v3get(t, "y"), z = v3get(t, "z");
    f32 r0 = m[0] * x + m[1] * y + m[2] * z;
    f32 r1 = m[4] * x + m[5] * y + m[6] * z;
    f32 r2 = m[8] * x + m[9] * y + m[10] * z;
    v3put(t, "x", r0); v3put(t, "y", r1); v3put(t, "z", r2);
    return t;                                   /* the receiver, mutated */
}
/* 0x562b20 Matrix.setFromAxisAngle(out, ax, ay, az, angle) -- Rodrigues.
 * Guards, both verified from the listing:
 *   n2 = ax^2+ay^2+az^2 ; if (1e-6 > n2) -> EARLY RETURN, `out` untouched
 *   else if (|n2 - 1.0| > 1e-6) normalize (divide by sqrt(n2))
 *   c = fcos(angle), s = fsin(angle), t = 1 - c, rows at stride 0x10:
 *     [ t*ax*ax+c   t*ax*ay-s*az  t*ax*az+s*ay ]
 *     [ t*ax*ay+s*az t*ay*ay+c    t*ay*az-s*ax ]
 *     [ t*ax*az-s*ay t*ay*az+s*ax t*az*az+c   ]
 * The caller (Vector3.rotate(Vector3;F) = 0x4066f0) keeps the matrix in an
 * UNINITIALISED stack frame, so the degenerate-axis path is undefined in the
 * engine as well; we leave our zeroed local as-is (documented, not invented). */
static void mtx_from_axis_angle(f32 m[12], f32 ax, f32 ay, f32 az, f32 angle) {
    f32 n2 = ax * ax + ay * ay + az * az;
    if (n2 < 1.0e-6f) return;
    if ((float)fabsl((long double)n2 - 1.0L) > 1.0e-6f) {   /* x87 fabs + fdivr */
        f32 inv = (f32)(1.0L / sqrtl((long double)n2));
        ax *= inv; ay *= inv; az *= inv;
    }
    f32 c = (f32)cosl(angle), sn = (f32)sinl(angle), t = 1.0f - c;
    f32 t1 = t * ax;
    m[0] = t1 * ax + c;
    f32 a4 = t1 * ay, a1 = t1 * az;
    m[1] = a4 - sn * az;
    m[4] = sn * az + a4;
    f32 a2 = t * ay;
    m[2] = sn * ay + a1;
    f32 a7 = a2 * ay + c;
    f32 a5 = a2 * az;
    m[6] = a5 - sn * ax;
    f32 a0 = t * az * az + c;
    m[8] = a1 - sn * ay;
    m[9] = sn * ax + a5;
    m[10] = a0;
    m[5] = a7;
    m[3] = m[7] = m[11] = 0.0f;
}
/* 0x4066f0 Vector3.rotate(Vector3;F)Ljava.lang.Vector3; -- same shape as the
 * Ypr overload: read this xyz, read the axis xyz, build the axis-angle matrix,
 * M*v, write the three components back into the receiver, return `this`. */
static u32 nat_v3_rotate_axis(u32 t, u32 *a) {
    f32 m[12];
    mtx_from_axis_angle(m, v3get(a[0], "x"), v3get(a[0], "y"), v3get(a[0], "z"),
                        u2fu(a[1]));
    f32 x = v3get(t, "x"), y = v3get(t, "y"), z = v3get(t, "z");
    f32 r0 = m[0] * x + m[1] * y + m[2] * z;
    f32 r1 = m[4] * x + m[5] * y + m[6] * z;
    f32 r2 = m[8] * x + m[9] * y + m[10] * z;
    v3put(t, "x", r0); v3put(t, "y", r1); v3put(t, "z", r2);
    return t;
}
/* engine time-warp state; a fresh process starts at 1.0 (probe-measured) */
static f32 g_timewarp_scale = 1.0f;
static u32 nat_sys_timeWarp(u32 t, u32 *a) {
    (void)t;
    f32 f = u2fu(a[0]);
    f32 ret = g_timewarp_scale;
    if (!(f < 0.0f)) g_timewarp_scale = f;     /* engine 0x4338e0(0x771050, f) */
    return fu2u(ret);
}
/* 0x661730 java.lang.String.length()I -- vm_get_int_field(this) gives the wide
 * buffer, then FUN_006675de walks it two bytes at a time and returns
 * (bytes/2) - 1 = the character count (the -1 compensates the NUL it counted).
 * Exactly implementable; the engine has NO null guard here (a null String object
 * would deref 0), our version returns 0 for that case.  Counts UTF-16 units, so
 * a CJK char counts as 1 -- the probe settles it (spec_strlen.json). */
static u32 nat_str_length(u32 t, u32 *a) {
    (void)a;
    const u16 *w = str_w(t);
    u32 n = 0;
    if (w) while (w[n]) n++;
    return n;
}
/* Return-a-constant natives: the body calls an engine routine and then loads a
 * literal into eax.  Read to the final ret (docs/65 10.51):
 *   0x40ed20 Sound.initRooms(I)I      -> 0x565c00(0x897380, arg); mov eax,1; ret
 *   0x40ef00 Sound.initReverb()I      -> 0x565c60(0x897380);      mov eax,1; ret
 * The engine calls are not modelled, but the RETURN does not depend on them, so
 * "always 1" is exactly reproducible (and probe-verified). */
static u32 nat_ret1(u32 t, u32 *a) { (void)t; (void)a; return 1; }
/* int fields by name -- same by-name machinery as the Vector3 float helpers
 * (v3slot resolves the field through find_field/inst_field, exactly like the
 * engine's FUN_006435f0 / FUN_006431f0 pair). */
static u32 inst_get_i_named(u32 t, const char *name) {
    f32 *p = v3slot(t, name);
    return p ? *(u32 *)p : 0u;
}
static void inst_set_i_named(u32 t, const char *name, u32 v) {
    f32 *p = v3slot(t, name);
    if (p) *(u32 *)p = v;
}
/* 0x404510 java.io.FindFile.close()V -- the ONLY Java-visible effect is the
 * field write; the handle itself belongs to the engine.  Body read to ret:
 *   h = vm_get_int_field_by_name(this, "handle")     (0x6434f0, name 0x6e7974)
 *   vm_set_int_field_by_name(this, "handle", -1)     (0x6432f0)
 *   if (h >= 0) engine_close(h)                      (0x560390, not modelled)
 * The class really has `handle`/`flags` fields (out_tasm: "F 4 11 4 0 ; handle"),
 * so a no-op stub leaves a stale handle where the engine clears it -- a
 * Java-visible difference for any later handle check.  The write happens BEFORE
 * the guard, so it runs even when the handle is already negative. */
static u32 nat_findfile_close(u32 t, u32 *a) {
    (void)a;
    i32 h = (i32)inst_get_i_named(t, "handle");
    inst_set_i_named(t, "handle", (u32)-1);
    /* if (h >= 0) engine 0x560390(h) -- the engine owns the handle */
    return 0;
}
/* ==== java.io.FindFile: first / next (docs/65 10.60) ====================
 * Recovered addresses (the class is missing from the registry and its class
 * string is unreferenced): first = 0x4042c5 (117 ins, ends 0x404413),
 * close = 0x404510.  Body of `first`:
 *   h = engine 0x560a70(pattern, &name, &is_dir_buffer)   find-first
 *   write this."handle" = h ;  write this."flags" = <2nd arg>
 *   if (h < 0) -> return NULL
 *   loop: skip entries whose name is "." (0x6e7968) or ".." (0x6e7964)
 *         accept per the flags rule below, else engine 0x560310 (=find-next)
 *   -> build a Java String from the name (0x644c40) and return it
 * Flags are the class constants (from the real dump): FILES_DIRS=0,
 * FILES_ONLY=1, DIRS_ONLY=2; the listing's branches give exactly:
 *   file : accept iff flags <= 1      (FILES_DIRS or FILES_ONLY)
 *   dir  : accept iff flags != 1      (FILES_DIRS or DIRS_ONLY)
 * The engine keeps the find handle in `handle`; so do we (CRT find handle),
 * and close() clearing it to -1 stays correct.
 * The pattern is resolved against --file-root, like the File layer. */
#define NFF 6
typedef struct { u32 self; intptr_t fh; struct _finddata_t fd; i32 flags; int live; } FfSlot;
static FfSlot g_ff[NFF];

static FfSlot *ff_slot(u32 t, int create) {
    for (int i = 0; i < NFF; i++)
        if (g_ff[i].live && g_ff[i].self == t) return &g_ff[i];
    if (!create) return NULL;
    for (int i = 0; i < NFF; i++) {
        if (!g_ff[i].live) {
            memset(&g_ff[i], 0, sizeof g_ff[i]);
            g_ff[i].live = 1; g_ff[i].self = t; g_ff[i].fh = -1;
            return &g_ff[i];
        }
    }
    /* recycle the oldest slot */
    memmove(&g_ff[0], &g_ff[1], sizeof(FfSlot) * (NFF - 1));
    memset(&g_ff[NFF - 1], 0, sizeof g_ff[NFF - 1]);
    g_ff[NFF - 1].live = 1; g_ff[NFF - 1].self = t; g_ff[NFF - 1].fh = -1;
    return &g_ff[NFF - 1];
}
static int ff_is_dot(const char *n) {
    return (n[0] == '.' && n[1] == 0) || (n[0] == '.' && n[1] == '.' && n[2] == 0);
}
static int ff_accept(i32 flags, int isdir) {
    return isdir ? (flags != 1) : (flags <= 1);
}
static u32 ff_make_string(const char *name) {
    return (u32)(uintptr_t)intern_n(name);
}
/* walk to the next accepted entry.  advance=0: the current entry is a candidate
 * (right after find-first); advance=1: step first, because first() already
 * returned the current one (0x404424 calls engine find-next, THEN tests). */
static u32 ff_scan(u32 t, FfSlot *s, int advance) {
    int ok = 1;
    if (advance) ok = (_findnext(s->fh, &s->fd) == 0);
    while (ok) {
        if (!ff_is_dot(s->fd.name) &&
            ff_accept(s->flags, (s->fd.attrib & _A_SUBDIR) != 0))
            return ff_make_string(s->fd.name);
        ok = (_findnext(s->fh, &s->fd) == 0);
    }
    _findclose(s->fh);
    s->fh = -1; s->live = 0;
    inst_set_i_named(t, "handle", (u32)-1);
    return 0;
}
/* 0x4042c5 FindFile.first(String;I)String */
static u32 nat_findfile_first(u32 t, u32 *a) {
    i32 flags = (i32)a[1];
    const u16 *pw = str_w(a[0]);
    char *rel = pw ? narrow(pw) : NULL;
    if (!rel) { inst_set_i_named(t, "flags", (u32)flags); return 0; }
    char full[1024];
    if (g_file_root && g_file_root[0] && rel[0] != '/' && rel[0] != '\\' &&
        !(rel[0] && rel[1] == ':'))
        snprintf(full, sizeof full, "%s/%s", g_file_root, rel);
    else
        snprintf(full, sizeof full, "%s", rel);
    free(rel);
    FfSlot *old = ff_slot(t, 0);
    if (old && old->fh != -1) { _findclose(old->fh); }
    FfSlot *s = ff_slot(t, 1);
    if (!s) return 0;
    s->flags = flags;
    inst_set_i_named(t, "flags", (u32)flags);
    s->fh = _findfirst(full, &s->fd);
    if (s->fh == -1) { s->live = 0; inst_set_i_named(t, "handle", (u32)-1); return 0; }
    inst_set_i_named(t, "handle", (u32)s->fh);
    fprintf(stderr, "\n");
    return ff_scan(t, s, 0);
}
/* FindFile.next()String -- continue the enumeration (address not yet located;
 * the body is the same loop without the find-first call, see docs/65 10.59). */
static u32 nat_findfile_next(u32 t, u32 *a) {
    (void)a;
    FfSlot *s = ff_slot(t, 0);
    fprintf(stderr, "\n");
    if (!s || s->fh == -1) return 0;
    return ff_scan(t, s, 1);
}
/* ==== tiny pure natives (docs/65 10.63) ==================================
 * Input.lastKey()I   = 0x566600: `xor eax,eax; ret`        -> always 0
 * Input.flushHotkeys()V = 0x404c40: walks the static hotkey table
 *   0x75d3f8..0x75db10 (stride 0x1c) and zeroes [e+0] where [e-0x18] != 0;
 *   the table is engine-owned and has no sandbox twin -> no-op here.
 * Input.deleteHotkey(Hotkey)V = 0x404a30: same table, finds the slot whose
 *   [e+0] == the unwrapped Hotkey, then clears it -> no-op for the same reason.
 * gui.Object.create()V = 0x4da7a0: a bare `ret`. */
static u32 nat_input_lastkey(u32 t, u32 *a) { (void)t; (void)a; return 0; }
static u32 nat_input_flushhotkeys(u32 t, u32 *a) { (void)t; (void)a; return 0; }
static u32 nat_input_deletehotkey(u32 t, u32 *a) { (void)t; (void)a; return 0; }
static u32 nat_guiobject_create(u32 t, u32 *a) { (void)t; (void)a; return 0; }
/* Object.notify()V = 0x662760 / notifyAll()V = 0x662780: unwrap, then call the
 * VM monitor primitives 0x656650 / 0x656710.  The sandbox is single-threaded, so
 * there is never a waiter to wake -> no-op is exact here (same reasoning as
 * Object.wait()V = 0x662730, docs/65 10.64). */
static u32 nat_object_notify(u32 t, u32 *a) { (void)t; (void)a; return 0; }
static u32 nat_object_notifyall(u32 t, u32 *a) { (void)t; (void)a; return 0; }
static u32 nat_noop(u32 t, u32 *a) { (void)t; (void)a; return 0; }

/* sound.Sound.changeMusicSet(I)V = 0x40e9a0: the engine bounds-checks i against
 * the music-set table count ([0x75df04] / table [0x75db00], out of range -> NULL)
 * and stores the resulting set pointer in the global "current set" [0x75df08],
 * stopping the previous set.  The model has no audio; what it does keep is the
 * Java-visible "which set is current" index (out of range -> -1, i.e. the NULL
 * the engine would store). */
static int g_music_set = -1;
static u32 nat_sound_changeMusicSet(u32 t, u32 *a) {
    (void)t;
    i32 i = (i32)a[0];
    g_music_set = i >= 0 ? (int)i : -1;
    return 0;
}
/* ======== Math.sqrt(F)F -- pure computation, exact (docs/65 10.30) ========
 * 0x4053c0.  The engine computes it with fsqrt/sqrtss; both are correctly
 * rounded for f32 inputs, so C sqrtf is bit-identical.  Differentially
 * verified (probe/spec_math.json). */
static u32 nat_math_sqrt(u32 t, u32 *a) { (void)t; return fu2u(sqrtf(u2fu(a[0]))); }

/* ======== engine config globals -- decompiled before being modelled ======
 * Each model global is initialised to the byte value the exe image holds, so
 * the state before the first call matches too (read out of the exe):
 *   0x743568 glow_threshold  0x3F266666 = 0.65f   (image bytes 66 66 26 3F)
 *   0x742608 dirt_level      0x3F800000 = 1.0f   (image bytes 00 00 80 3F)
 *   0x771ab8 dirt_type       bss -> 0 (real VM answers 0)
 *   0x763d9c time_of_day     bss -> 0
 *   0x7435a4 day_phase       0x00000001
 *   GfxEngine.setGlowThreshold(F)V 0x40aa00  g[0x743568] = f
 *   GfxEngine.setDirtType(I)I      0x40a130  if (i >= 0) g[0x771ab8] = i;
 *                                            return i >= 0 ? i : g[0x771ab8]
 *   GfxEngine.setDirtLevel(F)F     0x40a170  if (f >= 0.0f) g[0x742608] = f;
 *                                            return (f32)g[0x742608]
 *   System.syncGameTime(F)V        0x405e30  day = (int)f / 86400;  (cvttss2si)
 *                                            g[0x763d9c] = (int)f % 86400;
 *                                            g[0x7435a4] = phase ? 0 : 1;
 *                                            return day
 *     phase = (d < 72000 && d > 3599 && (d < 18000 || d > 25199))
 * NOT modelled: setShadowColor(II) 0x40a8f0, setEnvmapIntensity(FF) 0x40aa30,
 * setReflectorColor(FFF) 0x40aa70 -- the slot mapping is decompiled
 * (0x7435d4 = ~arg; 0x7434ac/0x7434b0; 0x743370/374/378) but WHICH Java arg
 * reaches which slot is unresolved, and a wrong order is invisible (no getter,
 * no clamping), so no differential test could catch it: they stay in the
 * backlog rather than shipping a guess. */
static int g_dirt_type = 0;   /* 0x771ab8 is UNINITIALISED (bss): the exe image bytes are unrelated, and the real VM answers 0 (probe spec_gfxstate case r3) */
static int g_time_of_day = 0; /* 0x763d9c: same bss caveat as above; syncGameTime always stores before anyone reads it */
static int g_day_phase = 1;
static u32 g_glow_threshold = 0x3f266666u;  /* 0.65f -- NOTE: the exe bytes 66 66 26 3F are the LITTLE-ENDIAN image; the numeric value is 0x3F266666 (this constant was byte-swapped at first and the probe caught the sibling case) */
static u32 g_dirt_level = 0x3f800000u;     /* 1.0f -- confirmed by the real VM (probe r0 = 1065353216) */
static u32 nat_gfx_setGlowThreshold(u32 t, u32 *a) { (void)t; g_glow_threshold = a[0]; return 0; }
static u32 nat_gfx_setDirtType(u32 t, u32 *a) {
    (void)t; i32 i = (i32)a[0];
    if (i >= 0) { g_dirt_type = (int)i; return (u32)i; }
    return (u32)g_dirt_type;
}
static u32 nat_gfx_setDirtLevel(u32 t, u32 *a) {
    (void)t;
    if (u2fu(a[0]) >= 0.0f) g_dirt_level = a[0];
    return g_dirt_level;
}
static u32 nat_sys_syncGameTime(u32 t, u32 *a) {
    (void)t;
    i32 secs = (i32)u2fu(a[0]);
    i32 d = secs % 86400;
    g_time_of_day = (int)d;
    g_day_phase = (d < 72000 && d > 3599 && (d < 18000 || d > 25199)) ? 0 : 1;
    return (u32)(secs / 86400);
}
/* ======== the three multi-arg gfx setters -- arg order now DERIVED =======
 * (docs/65 10.31)  The order question is settled from the engine's own helpers,
 * not guessed:
 *   FUN_00652f10  = the VM operand-stack POP (reads [stk+0x50][--[stk+0x54]])
 *                   => the FIRST pop yields the LAST Java argument.
 *   FUN_00652ab0(this, extra_out, p1, p2, ...) unwraps `argc` values into a
 *                   local array and then copies them BACKWARDS into the given
 *                   pointers: *(p1) = arr[argc-1] ... => **p1 = arg1**, p2 =
 *                   arg2, ...  (the optional extra_out is filled only when
 *                   this[0] == 0).  So the pointer list is in Java-arg order.
 * Cross-check: for the 1-arg siblings the only pointer is the last-pushed one
 * (which the decompiler names local_4) and that is where arg1 lands, exactly as
 * this rule says.  Store offsets are then calibrated on 0x40aa70, where BOTH the
 * asm store offsets ([esp+0x14]/+0x18/+0x1c) and the decompiled symbol order
 * (local_c/local_8/local_4 = arg1/2/3) are known => increasing offset = rising
 * arg index.  Same-frame functions (sub esp,8) therefore read arg1 at [esp+0x10].
 * Initial values are read out of the exe image:
 *   0x7435d4 = 0xffffffff (= ~0, consistent with the ~ below)
 *   0x7434ac = 0x40000000 = 2.0f   0x7434b0 = 0x3f000000 = 0.5f
 *   0x743370/74/78 = 0x3f800000 = 1.0f each (white reflector)
 *   GfxEngine.setShadowColor(II)V   0x40a8f0  g[0x7435d4] = ~arg1 (arg2 unused)
 *   GfxEngine.setEnvmapIntensity(FF)V 0x40aa30 g[0x7434ac]=arg1; g[0x7434b0]=arg2
 *   GfxEngine.setReflectorColor(FFF)V 0x40aa70 g[0x743370]=arg1; 0x743374=arg2;
 *                                             g[0x743378]=arg3                        */
static u32 g_shadow_color = 0xffffffffu;
static u32 g_envmap_a = 0x40000000u, g_envmap_b = 0x3f000000u;
static u32 g_reflector[3] = {0x3f800000u, 0x3f800000u, 0x3f800000u};
static u32 nat_gfx_setShadowColor(u32 t, u32 *a) { (void)t; g_shadow_color = ~a[0]; return 0; }
static u32 nat_gfx_setEnvmapIntensity(u32 t, u32 *a) {
    (void)t; g_envmap_a = a[0]; g_envmap_b = a[1]; return 0;
}
static u32 nat_gfx_setReflectorColor(u32 t, u32 *a) {
    (void)t; g_reflector[0] = a[0]; g_reflector[1] = a[1]; g_reflector[2] = a[2]; return 0;
}
typedef u32 (*NatFn)(u32, u32 *);
typedef struct { const char *cls, *name, *desc; NatFn fn; } NativeEnt;
static const NativeEnt g_natives[] = {
    {"java.lang.Class", "forName", "(Ljava.lang.String;)Ljava.lang.Class;", nat_forName},
    {"java.lang.Class", "newInstance", "()Ljava.lang.Object;", nat_newInstance},
    {"java.lang.Class", "getName", "()Ljava.lang.String;", nat_getName},
    {"java.lang.Object", "getClass", "()Ljava.lang.Class;", nat_getClass},
    {"java.lang.System", "arraycopy", "(Ljava.lang.Object;ILjava.lang.Object;II)V", nat_arraycopy},
    {"java.lang.System", "exit", "(Ljava.lang.String;)V", nat_exit},
    {"java.util.resource.ResourceRef", "newNative", "()V", nat_rr_newNative},
    {"java.util.resource.ResourceRef", "id", "()I", nat_rr_id},
    {"java.util.resource.ResourceRef", "duplicate", "()Ljava.util.resource.ResourceRef;", nat_duplicate},
    {"java.util.resource.RenderType", "duplicate", "()Ljava.util.resource.RenderType;", nat_duplicate},
    {"java.util.resource.ResourceRef", "set", "(Ljava.lang.String;I)V", nat_rr_set},
    {"java.util.resource.ResourceRef", "set", "(I)V", nat_rr_set_i},
    {"java.util.resource.GameRef", "createNativeInstance", "(Ljava.util.resource.GameRef;Ljava.util.resource.GameType;Ljava.lang.String;Ljava.lang.String;)V", nat_gr_createNativeInstance},
    {"java.lang.Matrix", "newNative", "()V", nat_matrix_newNative},
    {"java.lang.Matrix", "set", "(Ljava.lang.Vector3;Ljava.lang.Vector3;Ljava.lang.Ypr;)Ljava.lang.Matrix;", nat_matrix_set},
    {"java.io.File", "exists", "(Ljava.lang.String;)I", nat_file_exists},
    {"java.io.File", "delete", "(Ljava.lang.String;)I", nat_file_delete},
    {"java.io.File", "open", "(I)I", nat_file_open},
    {"java.io.File", "close", "()V", nat_file_close},
    {"java.io.File", "readInt", "()I", nat_file_readInt},
    {"java.io.File", "readFloat", "()F", nat_file_readFloat},
    {"java.io.File", "readResID", "()I", nat_file_readInt},
    {"java.io.File", "readString", "()Ljava.lang.String;", nat_file_readString},
    {"java.io.File", "write", "(I)I", nat_file_write_i},
    {"java.io.File", "write", "(F)I", nat_file_write_f},
    {"java.io.File", "write", "(Ljava.lang.String;)I", nat_file_write_s},
    {"java.sound.Sound", "setVolume", "(IF)V", nat_sound_setVolume},
    {"java.sound.Sound", "getVolume", "(I)F", nat_sound_getVolume},
    {"java.lang.System", "getCommandLineParameters", "()[Ljava.lang.String;", nat_sys_cmdline},
    {"java.lang.System", "currentTime", "()F", nat_sys_currentTime},
    {"java.gui.Component", "construct", "(Ljava.gui.Window;[Ljava.lang.String;Ljava.lang.String;)V", nat_gui_construct},
    {"java.gui.Component", "getBackground", "()Ljava.gui.Background;", nat_gui_get_sub},
    {"java.gui.Component", "getFrame", "()Ljava.gui.Frame;", nat_gui_get_sub},
    {"java.gui.Component", "getContainerRoot", "()Ljava.gui.Component;", nat_gui_get_sub},
    {"java.lang.String", "intValue", "()I", nat_str_intValue},
    {"java.lang.String", "floatValue", "()F", nat_str_floatValue},
    {"java.lang.Math", "random", "()F", nat_math_random},
    {"java.util.resource.ResourceRef", "unload", "(I)V", nat_noop},
    {"java.gui.Object", "initBegin", "()V", nat_noop},
    {"java.gui.Object", "initEnd", "()V", nat_noop},
    {"java.gui.TabList", "refresh", "()V", nat_noop},
    {"java.gui.Component", "postCustomEvent", "(I)V", nat_noop},
    {"java.gfx.Viewport", "deactivate", "()V", nat_noop},
    {"java.gfx.Animation", "finalize", "()V", nat_noop},
    {"java.sound.Sound", "changeMusicSet", "(I)V", nat_sound_changeMusicSet},
    {"java.game.parts.Chassis", "getSfxTable", "(I)Ljava.game.parts.SfxTable;", nat_chassis_getSfxTable},
    /* --- 26 engine-call natives, shape-verified by tools/native_shape_scan.py:
     * unwrap -> FUN_00643bf0(this, 0x8dcb2c) -> at most ONE engine action (one
     * call, or a store whose base register provably holds the native pointer)
     * -> ret, and never a Java field.  Per-row evidence (fn, action) lives in
     * out_noop_candidates.json; the 15 rows whose stores could NOT be proven to
     * target the native object were left OUT (shape engine-call-store?) and the
     * 137 engine-read rows (engine state, not modellable) are listed in
     * out_native_boundary.json.  A native-side model resumes at these addrs. */
    {"java.lang.Math", "sqrt", "(F)F", nat_math_sqrt},
    {"java.gfx.GfxEngine", "setGlowThreshold", "(F)V", nat_gfx_setGlowThreshold},
    {"java.gfx.GfxEngine", "setDirtType", "(I)I", nat_gfx_setDirtType},
    {"java.gfx.GfxEngine", "setDirtLevel", "(F)F", nat_gfx_setDirtLevel},
    {"java.lang.System", "syncGameTime", "(F)V", nat_sys_syncGameTime},
    {"java.lang.Float", "toString", "(FLjava.lang.String;)Ljava.lang.String;", nat_float_toString}, /* 0x404e60 -> _vswprintf */
    {"java.gui.WStringList", "create", "()V", nat_wsl_create},          /* 0x5a07e0 */
    {"java.gui.WStringList", "destroy", "()V", nat_wsl_create},         /* 0x5bb2f0 -> empty */
    {"java.gui.WStringList", "clear", "()V", nat_wsl_clear},            /* 0x5bb350 */
    {"java.gui.WStringList", "add", "(Ljava.lang.String;)V", nat_wsl_add},       /* 0x5a0850 */
    {"java.gui.WStringList", "insert", "(Ljava.lang.String;I)V", nat_wsl_insert},/* 0x5a0920 */
    {"java.gui.WStringList", "remove", "(I)V", nat_wsl_remove},         /* 0x5bb410 */
    {"java.gui.WStringList", "getCount", "()I", nat_wsl_count},         /* 0x5a07b0 */
    {"java.gui.WStringList", "set", "(Ljava.gui.WStringList;)V", nat_wsl_setlist},/* 0x5bb450 */
    {"java.gui.WStringList", "toString", "()Ljava.lang.String;", nat_wsl_toString},/* 0x5bb380 -> join L"\n" */
    {"java.net.Network", "Leave", "()I", nat_net_leave},                 /* 0x41a750 -> constant 1 */
    {"java.util.resource.GameRef", "command", "(Ljava.lang.String;)V", nat_noop}, /* 0x4209f0 -> engine notification dispatch */
    {"java.lang.Integer", "toString", "(I)Ljava.lang.String;", nat_int_toString},   /* 0x404f30 -> _vswprintf L"%d" */
    {"java.lang.String", "append", "(I)Ljava.lang.String;", nat_str_append_i},     /* 0x661620 -> text + decimal(arg) */
    {"java.lang.Vector3", "length", "()F", nat_v3_length},                 /* 0x406810 -> sqrt(x2+y2+z2), x87 */
    {"java.lang.Vector3", "sqrlength", "()F", nat_v3_sqrlength},           /* 0x406880 -> x2+y2+z2 */
    {"java.lang.Vector3", "distance", "(Ljava.lang.Vector3;)F", nat_v3_distance},  /* 0x4068f0 */
    {"java.lang.Vector3", "normalize", "()V", nat_v3_normalize},            /* 0x406960 -> float32 1/len scale */
    {"java.lang.Vector3", "toYpr", "()Ljava.lang.Ypr;", nat_v3_toYpr},        /* 0x406540 -> atan2 math + new Ypr */
    {"java.lang.Ypr", "toVector3", "()Ljava.lang.Vector3;", nat_ypr_toVector3}, /* 0x406bf0 -> -(matrix col 2) */
    {"java.lang.Vector3", "rotate", "(Ljava.lang.Ypr;)Ljava.lang.Vector3;", nat_v3_rotate_ypr}, /* 0x4065a0 -> M*v in place */
    {"java.lang.Vector3", "rotate", "(Ljava.lang.Vector3;F)Ljava.lang.Vector3;", nat_v3_rotate_axis}, /* 0x4066f0 -> axis-angle M*v */
    {"java.lang.System", "timeWarp", "(F)F", nat_sys_timeWarp},                 /* 0x405d60 -> returns global 0x771164 */
    {"java.lang.String", "length", "()I", nat_str_length},                      /* 0x661730 -> wide char count */
    {"java.gui.Texture", "destroy", "()V", nat_noop},                          /* 0x5bb8a0 -> engine frees the texture (3 calls), no Java field */
    /* 0x40a600 / 0x40a6b0: SEH prologue, then the arg String -> engine buffer
     * (0x567d90 + 0x6665af to free), then ONE engine call on the gfx singleton
     * g[0x7432c8]: 0x4c0f80(name) / 0x4c0f90(clear) for the shadow map, and
     * 0x4c0fa0 / 0x4c0fb0 for the lightlet.  No Java field is ever touched, so
     * the Java-visible behaviour is "nothing happens" (read to the final ret). */
    {"java.gfx.GfxEngine", "setShadowmap", "(Ljava.lang.String;)V", nat_noop},   /* 0x40a600 */
    {"java.gfx.GfxEngine", "setLightlet", "(Ljava.lang.String;)V", nat_noop},    /* 0x40a6b0 */
    {"java.gfx.GfxEngine", "preparetrack", "()V", nat_noop},                    /* 0x40b170 -> one engine call, no Java field */
    {"java.lang.Thread", "sleep", "(I)V", nat_noop},                            /* 0x661b70 -> VM sleep primitive; single-threaded model = no-op */
    {"java.lang.Thread", "sleep", "(II)V", nat_noop},                           /* 0x661b70 (same body, two Java signatures) */
    {"java.lang.Object", "wait", "()V", nat_noop},                              /* 0x662730 -> yields; no-op in the single-threaded model */
    {"java.io.FindFile", "close", "()V", nat_findfile_close},                   /* 0x404510 -> handle=-1, engine close */
    {"java.io.FindFile", "first", "(Ljava.lang.String;I)Ljava.lang.String;", nat_findfile_first}, /* 0x4042c5 */
    {"java.io.FindFile", "next", "()Ljava.lang.String;", nat_findfile_next},  /* body = same loop, addr TBD */
    {"java.io.Input", "lastKey", "()I", nat_input_lastkey},                     /* 0x566600 -> 0 */
    {"java.lang.Object", "notify", "()V", nat_object_notify},                    /* 0x662760 monitor -> no-op */
    {"java.lang.Object", "notifyAll", "()V", nat_object_notifyall},              /* 0x662780 */
    {"java.io.Input", "flushHotkeys", "()V", nat_input_flushhotkeys},            /* 0x404c40 engine table */
    {"java.io.Input", "deleteHotkey", "(Ljava.io.Hotkey;)V", nat_input_deletehotkey}, /* 0x404a30 engine table */
    {"java.gui.Object", "create", "()V", nat_guiobject_create},                  /* 0x4da7a0 bare ret */
    /* 0x4098b0 java.net.Network.getIndex(I)I: the whole body is
     * `ecx = [0x74233c]; if (!ecx) return 0; ...` -- the Network singleton is
     * null unless a net session is up, and that null path (0) IS the model's
     * behaviour, so the explicit registration is the faithful one. */
    {"java.net.Network", "getIndex", "(I)I", nat_noop},                           /* 0x4098b0 -> null singleton path = 0 */
    {"java.sound.Sound", "initRooms", "(I)I", nat_ret1},                        /* 0x40ed20 -> engine call, then mov eax,1 */
    {"java.sound.Sound", "initReverb", "()I", nat_ret1},                        /* 0x40ef00 -> engine call, then mov eax,1 */
    {"java.gfx.GfxEngine", "setFadeParams", "(II)V", nat_noop},          /* 0x40b130 -> gfx singleton +0x1b0/0x1b4 */
    {"java.sound.Sound", "nextTrack", "()V", nat_noop},                  /* 0x40ea10 -> engine next-track */
    {"java.sound.Sound", "prevTrack", "()V", nat_noop},                  /* 0x40ea20 -> engine prev-track */
    {"java.io.Input", "createHotkey", "(Ljava.io.Hotkey;)V", nat_noop}, /* 0x404970 -> global hotkey table */
    {"java.gfx.GfxEngine", "setShadowColor", "(II)V", nat_gfx_setShadowColor},      /* 0x40a8f0 */
    {"java.gfx.GfxEngine", "setEnvmapIntensity", "(FF)V", nat_gfx_setEnvmapIntensity}, /* 0x40aa30 */
    {"java.gfx.GfxEngine", "setReflectorColor", "(FFF)V", nat_gfx_setReflectorColor},  /* 0x40aa70 */
    {"java.game.parts.CarDifferential", "setTable", "(I[F[F)V", nat_noop},   /* 0x00483e30 call 0x5668d0 */
    {"java.gui.TabList", "start", "()V", nat_noop},   /* 0x005a0de0 call 0x5e5e30 */
    {"java.util.resource.GameRef", "setEventMask", "(I)V", nat_noop},   /* 0x00421700 call 0x6478f0 */
    {"java.sound.SfxRef", "stop", "(I)V", nat_noop},   /* 0x0040f2f0 call 0x42f420 */
    {"java.gui.WStringList", "clear", "()V", nat_noop},   /* 0x005bb350 call 0x5baa80 */
    {"java.sound.SfxRef", "set", "(I)V", nat_noop},   /* 0x0040f4e0 call 0x55acc0 */
    {"java.lang.Thread", "stop", "()V", nat_noop},   /* 0x00661b40 call 0x652be0 */
    {"java.util.resource.RenderRef", "setColor", "(I)V", nat_noop},   /* 0x004213e0 call 0x42e980 */
    {"java.util.resource.GameRef", "removeAllTimers", "()V", nat_noop},   /* 0x00421120 call 0x42de00 */
    {"java.util.resource.GameRef", "removeTimer", "(I)V", nat_noop},   /* 0x004210b0 call 0x42dd60 */
    {"java.util.resource.GameRef", "registerCallback", "(I)V", nat_noop},   /* 0x004217e0 call 0x435a30 */
    {"java.util.resource.GameRef", "unregisterCallback", "(I)V", nat_noop},   /* 0x00421820 call 0x434260 */
    {"java.gui.MasterGameList", "forcerefresh", "()V", nat_noop},   /* 0x005a0f20 call 0x5e2360 */
    {"java.gui.ViewVideo", "stop", "()V", nat_noop},   /* 0x005a0c40 call 0x5e75a0 */
    {"java.util.resource.GameRef", "clearEventMask", "(I)V", nat_noop},   /* 0x00421770 call 0x6478f0 */
    {"java.lang.Thread", "setPriority", "(I)V", nat_noop},   /* 0x00661ce0 native-struct store only */
    {"java.game.parts.WheelRef", "setWidth", "(F)V", nat_noop},   /* 0x00483860 native-struct store only */
    {"java.game.Replay", "record", "(Ljava.util.resource.GameRef;)V", nat_noop},   /* 0x00427f40 call 0x427ea0 */
    {"java.game.Replay", "play", "(Ljava.util.resource.GameRef;)V", nat_noop},   /* 0x004297e0 call 0x428640 */
    {"java.gfx.Viewport", "activate", "(I)V", nat_noop},   /* 0x0040ade0 call 0x4c3110 */
    {"java.gui.Component", "addChildN", "(Ljava.gui.Component;)V", nat_noop},   /* 0x005a0560 call 0x5da990 */
    {"java.gui.Component", "remChildN", "(Ljava.gui.Component;)V", nat_noop},   /* 0x005a0600 call 0x5da9b0 */
    {"java.gui.Component", "nshow", "()V", nat_noop},   /* 0x005a0660 call 0x5dc0a0 */
    {"java.gui.Component", "nhide", "()V", nat_noop},   /* 0x005a0690 call 0x5dc100 */
    {"java.util.resource.GameRef", "remNotification", "(Ljava.util.resource.GameRef;I)V", nat_noop},   /* 0x00420d70 call 0x42dc70 */
    {"java.util.resource.RenderRef", "setMatrix", "(Ljava.lang.Vector3;Ljava.lang.Ypr;)V", nat_noop},   /* 0x004247b0 call 0x6478f0 */
    /* --- 15 natives whose body stores into the ENGINE, not into Java --------
     * (docs/65 10.33)  tools/native_shape_scan.py first put these in the
     * "engine-call-store?" bucket because the store base register is not the
     * direct result of FUN_00643bf0 -- the bodies reach the target through a
     * chain: vm_get_int_field(this) -> [native+8] -> validate(FUN_0055a230)
     * -> native + per_kind_offset_table[0x7439b0][native[0x24]] -> (+0x64)
     * -> [+0x24d4] = the engine's own axle/suspension struct, or simply
     * [native+0xNN].  Every one of them was then READ to its RET (decompile
     * where available, asm otherwise) and each store provably lands in engine
     * memory, never in a Java field => Java-visible behaviour is a no-op.
     * Recorded per row: fn + the store targets, so a native-side model resumes
     * exactly there.  Examples of the confirmation reads:
     *   0x486b50 Chassis.setSteerWheel   -> *(*(native+0x24d4)+0xbdc/+0xbe0)
     *   0x4840d0 Chassis.setNitro        -> *(*(native+0x24d4)+0xcd0/+0xccc)
     *   0x4861c0 Chassis.setExhaustParams-> *(*(native+0x24d4)+0xc18/+0xc1c)
     *   0x4877f0 WheelRef.setCPatch      -> *(native+0x24)+0x6c/0x70/0x74,
     *                                       native+0x54 (uses ABS/fsin)
     *   0x4832c0 WheelRef.setBearing     -> native+0xe4 (and logs <0 via
     *                                       FUN_005668d0(string,double))
     *   0x5aba20 Component.setanchor_bottom -> native+0x1f4
     *   0x421e40 GroundRef.setFullyLoaded-> FUN_0042d670() singleton +0x9c
     *   0x483db0 CarDifferential.set*    -> native+0/4/8/0xc (abs/-abs)      */
    {"java.game.parts.CarDifferential", "set", "(IFF)V", nat_noop},   /* 0x00483db0 dword ptr [esp + 0x18], xmm0 | dword ptr [esp + 0xc] */
    {"java.game.parts.CarDifferential", "set", "(IFFF)V", nat_noop},   /* 0x00483db0 dword ptr [esp + 0x18], xmm0 | dword ptr [esp + 0xc] */
    {"java.gfx.LightType", "setLight", "(IFII)V", nat_noop},   /* 0x00422150 dword ptr [esp + 8], eax | dword ptr [esp], eax */
    {"java.util.resource.GameRef", "addTimer", "(FI)V", nat_noop},   /* 0x00421030 dword ptr [esp], xmm0 */
    {"java.game.parts.Chassis", "setSteerWheel", "(FF)V", nat_noop},   /* 0x00486b50 dword ptr [ecx + 0xbdc], xmm0 | dword ptr [edx + 0xbe0], xmm0 */
    {"java.gui.Component", "setanchor_bottom", "(I)V", nat_noop},   /* 0x005aba20 dword ptr [esp + 0xc], 0 */
    {"java.gui.ViewVideo", "play", "(I)V", nat_noop},   /* 0x005a0bf0 dword ptr [esp + 0xc], 0 */
    {"java.game.parts.WheelRef", "setBearing", "(F)V", nat_noop},   /* 0x004832c0 dword ptr [esp + 4] | qword ptr [esp] */
    {"java.game.parts.WheelRef", "setRadius", "(F)V", nat_noop},   /* 0x00487280 dword ptr [esp + 4], xmm0 | dword ptr [esp + 0x18], xmm1 */
    {"java.game.parts.WheelRef", "setCPatch", "(FFFF)V", nat_noop},   /* 0x004877f0 dword ptr [esp + 4] | dword ptr [ecx + 0x6c] */
    {"java.util.resource.GroundRef", "setFullyLoaded", "(I)V", nat_noop},   /* 0x00421e40 dword ptr [esp + 0xc], 0 | dword ptr [eax + 0x9c], edx */
    {"java.game.parts.Chassis", "setExhaustParams", "(FF)V", nat_noop},   /* 0x004861c0 dword ptr [ecx + 0xc18], xmm0 | dword ptr [edx + 0xc1c], xmm0 */
    {"java.game.parts.Chassis", "setNitro", "(FF)V", nat_noop},   /* 0x004840d0 dword ptr [esp + 0xc], xmm0 | dword ptr [esp + 0x14], xmm0 */
    {"java.gfx.LightType", "setLight", "(IIII)V", nat_noop},   /* 0x004220d0 dword ptr [esp + 8], eax | dword ptr [esp], eax */
    {"java.gfx.LightType", "setFlare", "(Ljava.util.resource.ResourceRef;IFFII)V", nat_noop},   /* 0x00425e90 dword ptr [esp + 0x40], esp | dword ptr [edx], ecx */
    /* java.game.parts type builders - model, see the section above (docs/65 10.25) */
    {"java.game.parts.Chassis", "configureType", "(Ljava.lang.String;)V", nat_pt_configure},
    {"java.game.parts.Chassis", "configureVisual", "(Ljava.util.resource.RenderType;)V", nat_pt_visual},
    {"java.game.parts.Chassis", "configureVisual", "(Ljava.util.resource.RenderType;FFF)V", nat_pt_visual},
    {"java.game.parts.Chassis", "configureVisual", "(Ljava.util.resource.RenderType;FFFFFF)V", nat_pt_visual},
    {"java.game.parts.Chassis", "createTypeBegin", "()V", nat_pt_begin},
    {"java.game.parts.Chassis", "createTypeEnd", "()V", nat_pt_end},
    {"java.game.parts.Part", "applyDecal", "(Ljava.util.resource.ResourceRef;Ljava.game.parts.PartDecal;)V", nat_pt_decal},
    {"java.game.parts.Part", "configureMesh", "(Ljava.util.resource.ResourceRef;)V", nat_pt_mesh},
    {"java.game.parts.Part", "configureMesh", "(Ljava.util.resource.ResourceRef;I)V", nat_pt_mesh},
    {"java.game.parts.Part", "configureMesh", "(Ljava.util.resource.ResourceRef;II)V", nat_pt_mesh},
    {"java.game.parts.Part", "configureTexture", "(Ljava.util.resource.ResourceRef;)V", nat_pt_texture},
    {"java.game.parts.Part", "configureType", "(Ljava.lang.String;)V", nat_pt_configure},
    {"java.game.parts.Part", "configureVisual", "(Ljava.util.resource.RenderType;)V", nat_pt_visual},
    {"java.game.parts.Part", "configureVisual", "(Ljava.util.resource.RenderType;FFF)V", nat_pt_visual},
    {"java.game.parts.Part", "configureVisual", "(Ljava.util.resource.RenderType;FFFFFF)V", nat_pt_visual},
    {"java.game.parts.Part", "createTypeBegin", "()V", nat_pt_begin},
    {"java.game.parts.Part", "createTypeEnd", "()V", nat_pt_end},
};
#define N_NATIVES ((int)(sizeof(g_natives) / sizeof(g_natives[0])))
static const NativeEnt *nat_find(const char *cls, const char *name, const char *desc) {
    for (int i = 0; i < N_NATIVES; i++)
        if (!strcmp(g_natives[i].cls, cls) && !strcmp(g_natives[i].name, name) &&
            !strcmp(g_natives[i].desc, desc))
            return &g_natives[i];
    return NULL;
}
/* Rules: the java.gui accessor family (getXxx/setXxx) is answered from the
 * layout property table by name, so the ~120 natives per gui class need no
 * entry each.  Exact table entries win (nat_find runs first). */
static NatFn nat_gui_rule(const char *cls, const char *name, const char *desc) {
    if (strncmp(cls, "java.gui.", 9) || desc[0] != '(') return NULL;
    if (!gw_propname(name)) return NULL;
    if (!strncmp(name, "get", 3)) {
        if (desc[1] == ')' && desc[2] == 'L') return nat_gui_get_sub;
        return nat_gui_get_num;
    }
    if (!strncmp(name, "set", 3) && desc[1] != ')')
        return desc[2] == 'L' ? nat_gui_set_obj : nat_gui_set_num;
    return NULL;
}
static int cls_valid(const Class *c) {
    if (!c) return 0;
    for (int i = 0; i < CLS_BUCKETS; i++)
        for (Class *k = g_heads[i]; k; k = k->next)
            if (k == c) return 1;
    return 0;
}
static long g_native_impl = 0;

static int call_method(Class *decl, Method *m, int kind) {
    Frame *caller = cur();
    if (!m) return -1;
    if (m->flags & 0x100) {                    /* native */
        g_native_calls++;
        Class *nowner = member_owner(decl, m);
        const char *oname = nowner ? nowner->name : (decl ? decl->name : "?");
        nat_count(oname, m->name, m->desc);
        u32 argv[256]; u32 thisv = 0;
        i32 argc = m->argc > 256 ? 256 : m->argc;
        for (i32 i = m->argc - 1; i >= 0; i--) {
            u32 v = vec_pop(&caller->stk);
            if (i < argc) argv[i] = v;
        }
        if (kind != 2) thisv = vec_pop(&caller->stk);
        const NativeEnt *ne = nat_find(oname, m->name, m->desc);
        NatFn rfn = ne ? ne->fn : nat_gui_rule(oname, m->name, m->desc);
        if (g_sample && g_steps >= 1000000L && g_steps < 1000200L)
            fprintf(stderr, "[natwin] %s.%s%s impl=%d\n", oname, m->name,
                    m->desc, rfn ? 1 : 0);
        u32 rv = 0;
        g_nat_cls = oname; g_nat_name = m->name; g_nat_desc = m->desc;
        if (rfn) { g_native_impl++; rv = rfn(thisv, argv); }
        else {            static int logged = 0;
            if (logged < 60) {
                fprintf(stderr, "[native-stub] %s.%s%s (decl=%s kind=%d)\n",
                        oname, m->name, m->desc, decl ? decl->name : "?", kind);
                logged++;
            }
        }
        if (g_sample && caller && caller->cls && caller->m &&
            !strcmp(caller->cls->name, "java.game.frontend.StartWindow") &&
            !strcmp(caller->m->name, "<init>"))
            fprintf(stderr, "[guiwin] %s.%s%s -> 0x%08x impl=%d\n", oname,
                    m->name, m->desc, rv, rfn ? 1 : 0);
        if (m->retc) vec_push(&caller->stk, rv);   /* return slot iff signature */
        return 1;
    }
    /* Records live in the DECLARING class: an inherited method's tree_slot
     * indexes the OWNER's record table, and its code's NT refs resolve
     * against the OWNER's constant pool (frame symenv = declaring class). */
    Class *owner = member_owner(decl, m);
    if (!owner) owner = decl;
    if (m->tree_slot < 0 || m->tree_slot >= owner->nrec) { /* no code: path C */
        for (i32 i = 0; i < m->argc; i++) vec_pop(&caller->stk);
        if (kind != 2) vec_pop(&caller->stk);
        if (m->retc) vec_push(&caller->stk, 0);
        return 1;
    }
    i32 argc = m->argc;
    if (argc > 256) { RTERR("argc %d > 256", argc); argc = 256; }
    u32 args[256]; u32 thisv = 0;
    for (i32 i = argc - 1; i >= 0; i--) args[i] = vec_pop(&caller->stk);
    if (kind != 2) thisv = vec_pop(&caller->stk);
    if (!frame_push(owner, m, owner->recs[m->tree_slot],
                    owner->reclen[m->tree_slot], thisv)) {
        if (m->retc) vec_push(&caller->stk, 0);
        return 1;
    }
    Frame *nf = cur();
    /* original 65f450: locals capacity pre-sized from [Method+8] = n_locals */
    if (m->nlocals > 16) vec_grow(&nf->loc, m->nlocals);
    else vec_grow(&nf->loc, 16);
    if (kind != 2) vec_push(&nf->loc, thisv);
    /* original pops caller TOS (last arg) first -> reversed locals:
     * locals = [this, argN..arg1];  param k = locals[N+1-k] */
    for (i32 i = argc - 1; i >= 0; i--) vec_push(&nf->loc, args[i]);
    return 0;
}

/* --------------------------------------------------------------- run loop */
static int opw(u8 op) {                        /* width 5 set (docs/08 §5.4) */
    switch (op) {
    case 0x01: case 0x02: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0b: case 0x0d: case 0x0f: case 0x10:
    case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x17:
    case 0x18: case 0x19: case 0x1a: case 0x1f: case 0x20: case 0x21:
    case 0x22: case 0x24: case 0x25: case 0x27: case 0x28:
        return 5;
    default:
        return 1;
    }
}

static int run_steps(i32 base_nf);
static int run_record(Class *c, Method *m, u32 thisv, int is_static, i32 rec_idx) {
    u8 *code; i32 codelen;
    if (!m) {
        if (rec_idx < 0) rec_idx = 0;
        if (c->nrec <= rec_idx || c->reclen[rec_idx] <= 0) return 1;
        code = c->recs[rec_idx]; codelen = c->reclen[rec_idx];
    } else {
        if (m->tree_slot < 0 || m->tree_slot >= c->nrec) return 0;
        code = c->recs[m->tree_slot]; codelen = c->reclen[m->tree_slot];
    }
    i32 base_nf = g_ctx.nf;
    if (!frame_push(c, m, code, codelen, thisv)) return 0;
    if (m && !is_static) vec_push(&cur()->loc, thisv);
    return run_steps(base_nf);
}

static int run_steps(i32 base_nf) {
    while (g_ctx.nf > base_nf && g_steps < g_budget) {
        g_steps++;
        Frame *f = cur();
        i32 off = (i32)(f->pc - f->code);
        /* Coarse sampling profiler (UNI_SAMPLE=1): shows WHICH loop a long run
         * is stuck in — the main-loop experiment has no other handle on it. */
        if (g_sample && (g_steps % 1000000L) == 0)
            fprintf(stderr, "[sample] step=%ld %s.%s+%d frames=%d\n", g_steps,
                    f->cls ? f->cls->name : "?", f->m ? f->m->name : "?", off,
                    g_ctx.nf);
        u8 *ip = f->pc;
        u8 op; u32 pay = 0;
        if (off < 0 || off >= f->codelen) {
            op = 0x16;             /* ran off code end = normal RETURN
                                      (empty bodies, e.g. Object.<init>) */
        } else {
            op = *ip;
            if (op == 0 || op == 0x0a || op > 0x55) {
                RTERR("illegal opcode 0x%02x", op);
                f->pc = ip + 5; continue;
            }
            int w = opw(op);
            if (w == 5) {
                if (off + 5 > f->codelen) op = 0x16;   /* truncated: return */
                else pay = rd32(ip + 1);
            }
        }
        if (g_trace) {
            static const char *wc = NULL; static long w0 = -1, w1 = -1;
            if (w1 < -1) { }             /* keep the compiler quiet */
            if (w0 < 0) {
                const char *w = getenv("UNI_WIN");
                w0 = 0; w1 = -1;
                if (w) {
                    char buf[256]; snprintf(buf, sizeof(buf), "%s", w);
                    char *a = strchr(buf, ':');
                    if (a) {
                        *a = 0; wc = strdup(buf);
                        w0 = strtol(a + 1, &a, 0);
                        if (a && *a == ':') w1 = strtol(a + 1, NULL, 0);
                    }
                }
            }
            int show = (w1 >= 0)
                ? (f->cls && wc && !strcmp(f->cls->name, wc) &&
                   off >= w0 && off <= w1)
                : 1;
            if (show)
                fprintf(stderr, "[%s.%s %04x] op %02x pay %u (sp=%d)\n",
                        f->cls->name, f->m ? f->m->name : "<clinit>",
                        off, op, pay, f->stk.count);
        }
        u32 a, b; i32 ia, ib; f32 fa, fb; u32 *slot; Inst *o; ConsEnt *e;
        switch (op) {
        case 0x01: f->pc = ip + 5; continue;                 /* CAST: no-op */
        case 0x02: {                                         /* INSTANCEOF */
            o = (Inst *)(uintptr_t)vec_pop(&f->stk);
            const char *tn = cons_str(f->cls, pay);
            i32 r = 0;
            if (inst_valid(o)) for (Class *k = o->cls; k; k = k->super)
                if (tn && !strcmp(k->name, tn)) { r = 1; break; }
            vec_push(&f->stk, (u32)r);
            f->pc = ip + 5; continue; }
        case 0x03: vec_push(&f->stk, 0); f->pc = ip + 1; continue;   /* NULL */
        case 0x04: case 0x05: case 0x06: case 0x07:          /* literals */
            vec_push(&f->stk, pay); f->pc = ip + 5; continue;
        case 0x08: {                                         /* STRING */
            const char *s = cons_str(f->cls, pay);
            vec_push(&f->stk, (u32)(uintptr_t)intern_n(s ? s : ""));
            f->pc = ip + 5; continue; }
        case 0x09:                                           /* RID */
            RTERR("RID_LITERAL unimplemented (ref %u)", pay);
            vec_push(&f->stk, 0); f->pc = ip + 5; continue;
        case 0x0a: case 0x28: f->pc = ip + 5; continue;      /* reserved/DELETE */
        case 0x0b:                                           /* LOCAL_LOAD */
            if ((i32)pay < f->loc.cap) vec_push(&f->stk, f->loc.base[pay]);
            else { g_oob++; vec_push(&f->stk, 0); }   /* original pushes 0 too */
            f->pc = ip + 5; continue;
        case 0x0c: vec_push(&f->loc, 0); f->pc = ip + 1; continue;  /* LOCAL_CREATE */
        case 0x0d: {                                         /* LOCAL_STORE */
            u32 v = vec_pop(&f->stk);
            if ((i32)pay < f->loc.cap) f->loc.base[pay] = v;   /* bounds = cap! */
            else g_oob++;                    /* original silently skips */
            f->pc = ip + 5; continue; }
        case 0x0e: vec_pop(&f->loc); f->pc = ip + 1; continue;      /* LOCAL_CLEAR */
        case 0x0f: { i32 k = (i32)pay > f->loc.count ? f->loc.count : (i32)pay;
            for (i32 i = 0; i < k; i++) vec_pop(&f->loc);    /* LOCAL_CLEARN */
            f->pc = ip + 5; continue; }
        case 0x10: case 0x11: case 0x12: {                   /* INVOKE* */
            int kind = op - 0x10;
            e = resolve_member(f->cls, pay);
            Class *decl = e ? e->r_cls : NULL;
            Method *mm = (e && e->r_is_method) ? (Method *)e->r_m : NULL;
            if (!decl || !mm) {
                RTERR("unresolved method ref %u in %s.%s", pay, f->cls->name,
                      f->m ? f->m->name : "<clinit>");
                vec_push(&f->stk, 0); f->pc = ip + 5; continue;
            }
            if (kind == 0) {                                 /* virtual dispatch */
                i32 ri = f->stk.count - mm->argc - 1;
                Inst *recv = (ri >= 0) ? (Inst *)(uintptr_t)f->stk.base[ri] : NULL;
                if (!inst_valid(recv))
                    RTERR("Thread::callmethod: null.methodcall (%s.%s%s)",
                          decl->name, mm->name, mm->desc);
                else {
                    Method *v = find_method(recv->cls, mm->name, mm->desc);
                    if (v) { decl = member_owner(recv->cls, v); mm = v; }
                }
            }
            Frame *cf = f;                     /* caller frame (stable ptr) */
            call_method(decl, mm, kind);
            cf->pc = ip + 5;                   /* caller always advances past
                                                  the INVOKE, even when the
                                                  callee frame was pushed */
            f = cur(); continue; }
        case 0x13: case 0x14: case 0x15: {                   /* FIELD_REF_* */
            e = resolve_member(f->cls, pay);
            if (!e || e->r_is_method || !e->r_cls) {
                RTERR("bad field ref %u", pay);
                vec_push(&f->stk, 0); f->pc = ip + 5; continue; }
            u32 *fp;
            if (op == 0x14) {
                if (e->r_idx >= e->r_cls->nstatics) {
                    RTERR("static field oob"); fp = NULL;
                } else fp = &e->r_cls->statics[e->r_idx];
            } else {
                u32 recv = (op == 0x15) ? f->thisv : vec_pop(&f->stk);
                o = (Inst *)(uintptr_t)recv;
                if (!o) { RTERR("null.fieldref %s", e->r_cls->name);
                    vec_push(&f->stk, 0); f->pc = ip + 5; continue; }
                fp = inst_field(o, e->r_idx);
            }
            vec_push(&f->stk, fp ? *fp : 0);
            f->pc = ip + 5; continue; }
        case 0x16: {                                         /* RETURN */
            u32 rv = 0; int have = 0;
            if (f->m && f->m->retc) {          /* retc==0: no return slot at all */
                have = 1;
                if (f->m->retc != 'V') rv = vec_pop(&f->stk);
            }
            frame_pop();
            if (g_ctx.nf > base_nf && have) vec_push(&cur()->stk, rv);
            continue; }
        case 0x17: f->pc = ip + (i32)pay; continue;          /* JMP */
        case 0x18: a = vec_pop(&f->stk);                     /* JMP_NE */
            f->pc = a ? ip + 5 : ip + (i32)pay; continue;
        case 0x19: a = vec_pop(&f->stk);                     /* JMP_EQ */
            f->pc = a ? ip + (i32)pay : ip + 5; continue;
        case 0x1a: {                                         /* JMP_EQ2 */
            a = vec_pop(&f->stk); b = vec_peek(&f->stk);
            if (a != b) { f->pc = ip + 5; continue; }
            vec_pop(&f->stk);
            f->pc = ip + (i32)pay; continue; }
        case 0x1b: slot = &f->stk.base[f->stk.count - 1];    /* F2I */
            *slot = fu2u((f32)(i32)u2fu(*slot)); f->pc = ip + 1; continue;
        case 0x1c: slot = &f->stk.base[f->stk.count - 1];    /* I2F */
            *slot = fu2u((f32)(i32)*slot); f->pc = ip + 1; continue;
        case 0x1d: { slot = &f->stk.base[f->stk.count - 1];  /* I2S */
            wchar_t buf[32]; swprintf(buf, 32, L"%d", (i32)*slot);
            u16 w[32]; int i = 0;
            for (; i < 31 && buf[i]; i++) w[i] = (u16)buf[i];
            w[i] = 0;
            *slot = (u32)(uintptr_t)intern_w(wdup(w));
            f->pc = ip + 1; continue; }
        case 0x1e: { slot = &f->stk.base[f->stk.count - 1];  /* F2S */
            wchar_t buf[64]; swprintf(buf, 64, L"%.3f", (double)u2fu(*slot));
            u16 w[64]; int i = 0;
            for (; i < 63 && buf[i]; i++) w[i] = (u16)buf[i];
            w[i] = 0;
            *slot = (u32)(uintptr_t)intern_w(wdup(w));
            f->pc = ip + 1; continue; }
        case 0x1f: case 0x20: case 0x21: {                   /* PUTFIELD_* */
            e = resolve_member(f->cls, pay);
            u32 v = vec_pop(&f->stk);
            if (!e || e->r_is_method || !e->r_cls) {
                RTERR("bad putfield ref %u", pay);
                if (op != 0x20) vec_pop(&f->stk);
                f->pc = ip + 5; continue; }
            u32 *fp;
            if (op == 0x20) {
                if (e->r_idx >= e->r_cls->nstatics) { RTERR("static oob"); fp = NULL; }
                else fp = &e->r_cls->statics[e->r_idx];
            } else {
                u32 recv = (op == 0x21) ? f->thisv : vec_pop(&f->stk);
                o = (Inst *)(uintptr_t)recv;
                if (!o) { RTERR("null.putfield %s", e->r_cls->name);
                    f->pc = ip + 5; continue; }
                fp = inst_field(o, e->r_idx);
            }
            if (fp) *fp = v;
            f->pc = ip + 5; continue; }
        case 0x22: {                                         /* NEWARRAY */
            const char *dsc = cons_str(f->cls, pay);
            if (!dsc) { RTERR("NEWARRAY bad desc ref %u", pay);
                vec_push(&f->stk, 0); f->pc = ip + 5; continue; }
            i32 nd = 0; while (dsc[nd] == '[') nd++;
            if (nd < 1 || nd > 8) { RTERR("NEWARRAY desc %s", dsc);
                vec_push(&f->stk, 0); f->pc = ip + 5; continue; }
            i32 dims[8];
            for (i32 i = nd - 1; i >= 0; i--) dims[i] = (i32)vec_pop(&f->stk);
            vec_push(&f->stk, (u32)(uintptr_t)arr_build(dsc, nd, dims, 0));
            f->pc = ip + 5; continue; }
        case 0x23: {                                         /* ARRAY_STORE */
            u32 v = vec_pop(&f->stk);            /* original pop order: */
            Inst *ar = (Inst *)(uintptr_t)vec_pop(&f->stk);   /* value, array, */
            i32 idx = (i32)vec_pop(&f->stk);                  /* index (00653880 case 0x23) */
            if (getenv("UNI_ALLOCLOG"))
                fprintf(stderr, "[arraystore] arr=%p idx=%d val=%u at %s.%s+%ld\n",
                        (void *)ar, idx, v, f->cls ? f->cls->name : "?",
                        f->m ? f->m->name : "?", (long)(ip - f->code));
            u32 *p = arr_elem(ar, idx);
            if (p) *p = v;
            f->pc = ip + 1; continue; }
        case 0x24: {                                         /* ARRAY_INIT */
            Inst *ar = (Inst *)(uintptr_t)vec_pop(&f->stk);
            i32 n = (i32)pay;
            if (n > 65536) n = 65536;
            u32 *tmp = (u32 *)calloc((size_t)(n > 0 ? n : 1), 4);
            for (i32 j = 0; j < n; j++) tmp[j] = vec_pop(&f->stk);
            if (ar) for (i32 j = 0; j < n; j++) {
                u32 *p = arr_elem(ar, n - 1 - j);
                if (p) *p = tmp[j];
            }
            free(tmp);
            vec_push(&f->stk, (u32)(uintptr_t)ar);
            f->pc = ip + 5; continue; }
        case 0x25: { i32 k = (i32)pay > 4096 ? 4096 : (i32)pay;   /* EMPTYDIMS */
            for (i32 i = 0; i < k; i++) vec_push(&f->stk, 0xffffffffu);
            f->pc = ip + 5; continue; }
        case 0x26: {                                         /* ARRAY_ACCESS */
            Inst *ar = (Inst *)(uintptr_t)vec_pop(&f->stk);  /* original pop order: */
            i32 idx = (i32)vec_pop(&f->stk);                 /* array, index (case 0x26) */
            u32 *p = arr_elem(ar, idx);
            vec_push(&f->stk, p ? *p : 0);
            f->pc = ip + 1; continue; }
        case 0x27: {                                         /* NEW */
            const char *cn = cons_str(f->cls, pay);
            Class *nc = cn ? load_class(cn) : NULL;
            if (!nc) { RTERR("NEW unresolved class ref %u", pay);
                vec_push(&f->stk, 0); }
            else vec_push(&f->stk, (u32)(uintptr_t)inst_new(nc));
            f->pc = ip + 5; continue; }
        case 0x29: vec_pop(&f->stk); f->pc = ip + 1; continue;        /* POP */
        case 0x2a: vec_push(&f->stk, vec_peek(&f->stk));              /* DUP */
            f->pc = ip + 1; continue;
        case 0x2b: { a = vec_pop(&f->stk); b = vec_pop(&f->stk);      /* DUP_X1 */
            vec_push(&f->stk, a); vec_push(&f->stk, b); vec_push(&f->stk, a);
            f->pc = ip + 1; continue; }
        case 0x2c: { u32 c3 = vec_pop(&f->stk); b = vec_pop(&f->stk); /* DUP_X2 */
            a = vec_pop(&f->stk);
            /* Verified against the original handler (0x653a34): pops v1,v2,v3
             * then pushes v1, v3, v2, v1 — our old order swapped v2/v3, which
             * turned stack-shuffled array stores into null-array crashes. */
            vec_push(&f->stk, c3); vec_push(&f->stk, a);
            vec_push(&f->stk, b);  vec_push(&f->stk, c3);
            f->pc = ip + 1; continue; }
        case 0x2d: { u32 v1 = vec_pop(&f->stk); b = vec_pop(&f->stk); /* DUP2 */
            /* original (0x6539be) peeks the top two and pushes v2 then v1:
             * [.., v2, v1] -> [.., v2, v1, v2, v1]; our old code pushed v1
             * twice ([.., v2, v1, v1, v1]) — same family as the DUP_X2 bug. */
            vec_push(&f->stk, b);  vec_push(&f->stk, v1);
            vec_push(&f->stk, b);  vec_push(&f->stk, v1);
            f->pc = ip + 1; continue; }
        case 0x2e: {                                         /* SADD */
            b = vec_pop(&f->stk); a = vec_pop(&f->stk);
            const u16 *wa = str_w(a), *wb = str_w(b);
            if (!wa || !wb) { vec_push(&f->stk, 0); f->pc = ip + 1; continue; }
            size_t la = 0, lb = 0;
            while (wa[la]) la++;
            while (wb[lb]) lb++;
            u16 *w = (u16 *)calloc(la + lb + 1, 2);
            memcpy(w, wa, la * 2);
            memcpy(w + la, wb, lb * 2);
            vec_push(&f->stk, (u32)(uintptr_t)intern_w(w));
            f->pc = ip + 1; continue; }
#define TOS (f->stk.base[f->stk.count - 1])
        case 0x2f: a = vec_pop(&f->stk); TOS = (u32)((i32)TOS + (i32)a);
            f->pc = ip + 1; continue;                                  /* IADD */
        case 0x30: fa = u2fu(vec_pop(&f->stk)); TOS = fu2u(u2fu(TOS) + fa);
            f->pc = ip + 1; continue;                                  /* FADD */
        case 0x31: a = vec_pop(&f->stk); TOS = (u32)((i32)TOS - (i32)a);
            f->pc = ip + 1; continue;                                  /* ISUB */
        case 0x32: fa = u2fu(vec_pop(&f->stk)); TOS = fu2u(u2fu(TOS) - fa);
            f->pc = ip + 1; continue;                                  /* FSUB */
        case 0x33: ia = (i32)vec_pop(&f->stk);                         /* IDIV */
            if (!ia) { RTERR("integer division by zero");
                vec_push(&f->stk, 0); f->pc = ip + 1; continue; }
            TOS = (u32)((i32)TOS / ia); f->pc = ip + 1; continue;
        case 0x34: fa = u2fu(vec_pop(&f->stk));                        /* FDIV */
            if (fa == 0.0f) { RTERR("float division by zero");
                vec_push(&f->stk, 0); f->pc = ip + 1; continue; }
            TOS = fu2u(u2fu(TOS) / fa); f->pc = ip + 1; continue;
        case 0x35: a = vec_pop(&f->stk); TOS = (u32)((i32)TOS * (i32)a);
            f->pc = ip + 1; continue;                                  /* IMUL */
        case 0x36: fa = u2fu(vec_pop(&f->stk)); TOS = fu2u(u2fu(TOS) * fa);
            f->pc = ip + 1; continue;                                  /* FMUL */
        case 0x37: TOS = (u32)(-(i32)TOS); f->pc = ip + 1; continue;   /* INEG */
        case 0x38: TOS = fu2u(-u2fu(TOS)); f->pc = ip + 1; continue;   /* FNEG */
        case 0x39: TOS = (u32)((i32)TOS + 1); f->pc = ip + 1; continue;/* IINC */
        case 0x3a: TOS = fu2u(u2fu(TOS) + 1.0f); f->pc = ip + 1; continue; /* FINC */
        case 0x3b: TOS = (u32)((i32)TOS - 1); f->pc = ip + 1; continue;/* IDEC */
        case 0x3c: TOS = fu2u(u2fu(TOS) - 1.0f); f->pc = ip + 1; continue; /* FDEC */
        case 0x3d: ia = (i32)vec_pop(&f->stk);                         /* MOD */
            if (!ia) { RTERR("modulo by zero");
                vec_push(&f->stk, 0); f->pc = ip + 1; continue; }
            TOS = (u32)((i32)TOS % ia); f->pc = ip + 1; continue;
        case 0x3e: f->pc = vec_peek(&f->stk) ? ip + 6 : ip + 1; continue; /* SC_AND:
            true -> skip SC(1B)+JMP(5B) into rhs; false -> fall into JMP->end
            (original caseD_a = puVar5+1 then +4, puVar5 = ip+1) */
        case 0x3f: f->pc = vec_peek(&f->stk) ? ip + 1 : ip + 6; continue; /* SC_OR */
        case 0x40: a = vec_pop(&f->stk); TOS = (TOS && a) ? 1u : 0u;
            f->pc = ip + 1; continue;                                  /* ANDAND */
        case 0x41: a = vec_pop(&f->stk); TOS = (TOS || a) ? 1u : 0u;
            f->pc = ip + 1; continue;                                  /* OROR */
        case 0x42: a = vec_pop(&f->stk); TOS &= a; f->pc = ip + 1; continue;  /* AND */
        case 0x43: a = vec_pop(&f->stk); TOS |= a; f->pc = ip + 1; continue;  /* OR */
        case 0x44: a = vec_pop(&f->stk); TOS ^= a; f->pc = ip + 1; continue;  /* XOR */
        case 0x45: TOS = ~TOS; f->pc = ip + 1; continue;                 /* NOT */
        case 0x46: TOS = (TOS == 0) ? 1u : 0u; f->pc = ip + 1; continue; /* EXCLAM */
        case 0x47: b = vec_pop(&f->stk) & 31; TOS = (u32)((i32)TOS >> b);
            f->pc = ip + 1; continue;                                  /* ASR */
        case 0x48: b = vec_pop(&f->stk) & 31; TOS = TOS << b;
            f->pc = ip + 1; continue;                                  /* ASL */
        case 0x49: b = vec_pop(&f->stk) & 31; TOS = TOS >> b;
            f->pc = ip + 1; continue;                                  /* LSR */
        case 0x4a: ib = (i32)vec_pop(&f->stk); ia = (i32)TOS; TOS = (u32)(ia == ib);
            f->pc = ip + 1; continue;                                  /* IEQ */
        case 0x4b: ib = (i32)vec_pop(&f->stk); ia = (i32)TOS; TOS = (u32)(ia != ib);
            f->pc = ip + 1; continue;                                  /* INE */
        case 0x4c: ib = (i32)vec_pop(&f->stk); ia = (i32)TOS; TOS = (u32)(ia > ib);
            f->pc = ip + 1; continue;                                  /* IGT */
        case 0x4d: ib = (i32)vec_pop(&f->stk); ia = (i32)TOS; TOS = (u32)(ia >= ib);
            f->pc = ip + 1; continue;                                  /* IGE */
        case 0x4e: ib = (i32)vec_pop(&f->stk); ia = (i32)TOS; TOS = (u32)(ia < ib);
            f->pc = ip + 1; continue;                                  /* ILT */
        case 0x4f: ib = (i32)vec_pop(&f->stk); ia = (i32)TOS; TOS = (u32)(ia <= ib);
            f->pc = ip + 1; continue;                                  /* ILE */
        case 0x50: fb = u2fu(vec_pop(&f->stk)); fa = u2fu(TOS);
            TOS = (u32)(fa == fb); f->pc = ip + 1; continue;           /* FEQ */
        case 0x51: fb = u2fu(vec_pop(&f->stk)); fa = u2fu(TOS);
            TOS = (u32)(fa != fb); f->pc = ip + 1; continue;           /* FNE */
        case 0x52: fb = u2fu(vec_pop(&f->stk)); fa = u2fu(TOS);
            TOS = (u32)(fa > fb); f->pc = ip + 1; continue;            /* FGT */
        case 0x53: fb = u2fu(vec_pop(&f->stk)); fa = u2fu(TOS);
            TOS = (u32)(fa >= fb); f->pc = ip + 1; continue;           /* FGE */
        case 0x54: fb = u2fu(vec_pop(&f->stk)); fa = u2fu(TOS);
            TOS = (u32)(fa < fb); f->pc = ip + 1; continue;            /* FLT */
        case 0x55: fb = u2fu(vec_pop(&f->stk)); fa = u2fu(TOS);
            TOS = (u32)(fa <= fb); f->pc = ip + 1; continue;           /* FLE */
        default: RTERR("unhandled opcode 0x%02x", op);
            f->pc = ip + opw(op); continue;
        }
    }
    if (g_steps >= g_budget) RTERR("step budget exhausted");
    while (g_ctx.nf > base_nf) frame_pop();
    return 1;
}

/* ------------------------------------------------------------- statics dump */
static void dump_statics(Class *c) {
    for (i32 i = 0; i < c->nfs; i++) {
        Field *f = &c->fs[i];
        prep_field(f, c);
        u32 v = c->statics[i];
        printf("%s\t%s\t%s\t%d\t%u\t%.9g", c->name, f->name, f->type,
               i, v, (double)u2fu(v));
        if (!strcmp(f->type, "Ljava.lang.String;")) {
            const u16 *w = str_w(v);
            if (w) {
                printf("\tstr:");
                for (const u16 *p = w; *p; p++) {
                    if (*p == '\t') printf("\\t");
                    else if (*p == '\n') printf("\\n");
                    else if (*p == '\r') printf("\\r");
                    else if (*p == '\\') printf("\\\\");
                    else if (*p == ' ') printf("\\s");
                    else if (*p < 0x20 || *p > 0x7e) printf("\\u%04x", *p);
                    else putchar((char)*p);
                }
            }
        } else if (f->type[0] == '[' && v && inst_valid((void *)(uintptr_t)v)) {
            Inst *ar = (Inst *)(uintptr_t)v;
            if (ar->cap >= 2) {
                i32 len = (i32)ar->fields[0];
                ArrData *d = (ArrData *)(uintptr_t)ar->fields[1];
                printf("\tarr:%d", len);
                if (d) for (i32 j = 0; j < len && (u32)j < d->cap; j++) {
                    u32 el = d->base[j];
                    const u16 *w = str_w(el);
                    if (w) {
                        printf(" \"");
                        for (const u16 *p = w; *p; p++) {
                            if (*p == '\t') printf("\\t");
                            else if (*p == '\n') printf("\\n");
                            else if (*p == '\r') printf("\\r");
                            else if (*p == '\\') printf("\\\\");
                            else if (*p == '"') printf("\\q");
                            else if (*p == ' ') printf("\\s");
                            else if (*p < 0x20 || *p > 0x7e) printf("\\u%04x", *p);
                            else putchar((char)*p);
                        }
                        putchar('"');
                    } else printf(" %u", el);
                }
            }
        }
        putchar('\n');
    }
}

/* --------------------------------------------------------------------- main */
/* tokenizer for --args ("a|b|c") */
static const char *g_ap; static int g_ap_done;
static const char *next_tok(char *buf, size_t cap) {
    if (g_ap_done) return NULL;
    const char *s = g_ap;
    const char *bar = strchr(s, '|');
    size_t n = bar ? (size_t)(bar - s) : strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(buf, s, n); buf[n] = 0;
    if (bar) g_ap = bar + 1; else g_ap_done = 1;
    return buf;
}

/* push args onto df->stk per descriptor; tokenizer state must be set */
static int push_args(Frame *df, const char *desc) {
    char tb[4096];
    const char *d = strchr(desc, '(');
    if (!d) { fprintf(stderr, "bad desc\n"); return -1; }
    d++;
    while (*d && *d != ')') {
        const char *t = next_tok(tb, sizeof(tb));
        if (!t) { fprintf(stderr, "missing arg at '%s'\n", d); return -1; }
        if (*d == '[') {
            char ad[256]; int ai = 0;
            ad[ai++] = '['; d++;
            if (*d == 'L') {
                while (*d && *d != ';') { if (ai < 250) ad[ai++] = *d; d++; }
                if (ai < 250) ad[ai++] = *d;
                if (*d == ';') d++;
            } else { if (ai < 250) ad[ai++] = *d; d++; }
            ad[ai] = 0;
            if (!strcmp(t, "null")) { vec_push(&df->stk, 0); continue; }
            const char *colon = strchr(t, ':');
            i32 n = 0;
            if (colon && colon[1]) {
                n = 1;
                for (const char *q = colon + 1; *q; q++) if (*q == ',') n++;
            }
            Inst *ar = arr_new(ad, n);
            if (colon && colon[1] && ar->cap >= 2) {
                ArrData *dd = (ArrData *)(uintptr_t)ar->fields[1];
                const char *q = colon + 1;
                for (i32 j = 0; j < n; j++) {
                    char el[1024];
                    const char *cm = strchr(q, ',');
                    size_t L = cm ? (size_t)(cm - q) : strlen(q);
                    if (L >= sizeof(el)) L = sizeof(el) - 1;
                    memcpy(el, q, L); el[L] = 0;
                    u32 v;
                    if (ad[1] == 'F') v = fu2u((f32)strtod(el, NULL));
                    else if (ad[1] == 'L') v = (u32)(uintptr_t)intern_n(el);
                    else v = (u32)strtol(el, NULL, 0);
                    if (dd && (u32)j < dd->cap) dd->base[j] = v;
                    q = cm ? cm + 1 : q + L;
                }
            }
            vec_push(&df->stk, (u32)(uintptr_t)ar);
        } else if (*d == 'L') {
            while (*d && *d != ';') d++;
            if (*d == ';') d++;
            vec_push(&df->stk, strcmp(t, "null") ? (u32)(uintptr_t)intern_n(t) : 0);
        } else {
            if (*d == 'F') vec_push(&df->stk, fu2u((f32)strtod(t, NULL)));
            else vec_push(&df->stk, (u32)strtol(t, NULL, 0));
            d++;
        }
    }
    return 0;
}

/* str:/arr: extra dump column for a value of given type (shared format) */
static void print_extra(const char *type, u32 v) {
    if (!strcmp(type, "Ljava.lang.String;")) {
        const u16 *w = str_w(v);
        if (w) {
            printf("\tstr:");
            for (const u16 *p = w; *p; p++) {
                if (*p == '\t') printf("\\t");
                else if (*p == '\n') printf("\\n");
                else if (*p == '\r') printf("\\r");
                else if (*p == '\\') printf("\\\\");
                else if (*p == ' ') printf("\\s");
                else if (*p < 0x20 || *p > 0x7e) printf("\\u%04x", *p);
                else putchar((char)*p);
            }
        }
    } else if (type[0] == '[' && v && inst_valid((void *)(uintptr_t)v)) {
        Inst *ar = (Inst *)(uintptr_t)v;
        if (ar->cap >= 2) {
            i32 len = (i32)ar->fields[0];
            ArrData *d = (ArrData *)(uintptr_t)ar->fields[1];
            printf("\tarr:%d", len);
            if (d) for (i32 j = 0; j < len && (u32)j < d->cap; j++) {
                u32 el = d->base[j];
                const u16 *w = str_w(el);
                if (w) {
                    printf(" \"");
                    for (const u16 *p = w; *p; p++) {
                        if (*p == '\t') printf("\\t");
                        else if (*p == '\n') printf("\\n");
                        else if (*p == '\r') printf("\\r");
                        else if (*p == '\\') printf("\\\\");
                        else if (*p == '"') printf("\\q");
                        else if (*p == ' ') printf("\\s");
                        else if (*p < 0x20 || *p > 0x7e) printf("\\u%04x", *p);
                        else putchar((char)*p);
                    }
                    putchar('"');
                } else printf(" %u", el);
            }
        }
    }
}

/* Boot-driver replay (docs/65 §10.15).  The real VM's dump reflects state AFTER
 * the engine ran its startup path, so a clinit-only batch diverges by
 * construction: Config's values come from Config.init()/load(), not from its
 * <clinit> (proved with tools/field_writers.py).  UNI_DRIVERS=Class.method,...
 * replays those drivers once, before the per-class loop. */
static void run_driver(const char *spec) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *dot = strrchr(buf, '.');
    if (!dot) { fprintf(stderr, "[driver] bad spec %s\n", spec); return; }
    *dot = 0;
    const char *mname = dot + 1;
    Class *c = load_class(buf);
    if (!c) { fprintf(stderr, "[driver] no class %s\n", buf); return; }
    static const char *descs[] = {"()V", "()I", "()F", "()Z", "()", NULL};
    Method *m = NULL; Class *owner = NULL; int is_static = 0;
    for (int d = 0; descs[d] && !m; d++)
        for (Class *k = c; k && !m; k = k->super) {
            for (i32 j = 0; j < k->nms; j++) {
                prep_method(&k->ms[j], k);
                if (!strcmp(k->ms[j].name, mname) &&
                    !strcmp(k->ms[j].desc, descs[d])) {
                    m = &k->ms[j]; owner = k; is_static = 1; break; }
            }
            if (m) break;
            for (i32 j = 0; j < k->nmi; j++) {
                prep_method(&k->mi[j], k);
                if (!strcmp(k->mi[j].name, mname) &&
                    !strcmp(k->mi[j].desc, descs[d])) {
                    m = &k->mi[j]; owner = k; break; }
            }
        }
    if (!m) { fprintf(stderr, "[driver] no method %s\n", spec); return; }
    Frame *df = &g_ctx.frames[g_ctx.nf++];
    memset(df, 0, sizeof(*df));
    vec_init(&df->loc); vec_init(&df->stk);
    df->code = (u8 *)""; df->codelen = 0; df->cls = c; df->m = NULL;
    df->pc = df->code;
    if (!is_static) vec_push(&df->stk, (u32)(uintptr_t)inst_new(c));
    long s0 = g_steps, e0 = g_errors;
    long drv_budget = 20000000L;
    const char *dbo = getenv("UNI_DRIVER_BUDGET");
    if (dbo && *dbo) drv_budget = atol(dbo);
    g_budget = g_steps + drv_budget;
    call_method(owner, m, is_static ? 2 : 1);
    run_steps(0);
    fprintf(stderr, "[driver] %s steps=%ld errs=%ld\n", spec,
            g_steps - s0, g_errors - e0);
}

int main(int argc, char **argv) {
    const char *clinit = NULL;
    const char *call_spec = NULL, *call_desc = NULL, *call_args = NULL;
    const char *construct_spec = NULL;
    int g_batch = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cp") && i + 1 < argc) g_cp = argv[++i];
        else if (!strcmp(argv[i], "--index") && i + 1 < argc) g_index = argv[++i];
        else if (!strcmp(argv[i], "--clinit") && i + 1 < argc) clinit = argv[++i];
        else if (!strcmp(argv[i], "--call") && i + 1 < argc) call_spec = argv[++i];
        else if (!strcmp(argv[i], "--construct") && i + 1 < argc) construct_spec = argv[++i];
        else if (!strcmp(argv[i], "--desc") && i + 1 < argc) call_desc = argv[++i];
        else if (!strcmp(argv[i], "--args") && i + 1 < argc) call_args = argv[++i];
        else if (!strcmp(argv[i], "--file-root") && i + 1 < argc) g_file_root = argv[++i];
        else if (!strcmp(argv[i], "--file-write-root") && i + 1 < argc) g_file_wroot = argv[++i];
        else if (!strcmp(argv[i], "--batch")) g_batch = 1;
        else if (!strcmp(argv[i], "--dump")) g_dump = 1;
        else if (!strcmp(argv[i], "--trace")) g_trace = 1;
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    if (getenv("UNI_SAMPLE")) g_sample = 1;
    if (!g_cp || !g_index) {
        fprintf(stderr, "usage: tufa_vm --cp DIR --index IDX [--clinit CLASS "
                        "[--dump]] [--call CLASS.METHOD --desc D --args a|b] [--trace]\n");
        return 2;
    }
    if (call_spec) {
        char *spec = strdup(call_spec);
        char *dot = strrchr(spec, '.');
        if (!dot) { fprintf(stderr, "bad --call spec\n"); return 2; }
        *dot = 0;
        const char *mname = dot + 1;
        Class *c = load_class(spec);
        if (!c) { fprintf(stderr, "cannot load %s\n", spec); return 1; }
        const char *desc = call_desc ? call_desc : "()V";
        Method *m = NULL; Class *owner = NULL; int is_static = 0;
        for (Class *k = c; k && !m; k = k->super) {
            for (i32 j = 0; j < k->nms; j++) {
                prep_method(&k->ms[j], k);
                if (!strcmp(k->ms[j].name, mname) && !strcmp(k->ms[j].desc, desc)) {
                    m = &k->ms[j]; owner = k; is_static = 1; break; }
            }
            if (m) break;
            for (i32 j = 0; j < k->nmi; j++) {
                prep_method(&k->mi[j], k);
                if (!strcmp(k->mi[j].name, mname) && !strcmp(k->mi[j].desc, desc)) {
                    m = &k->mi[j]; owner = k; break; }
            }
        }
        if (!m) { fprintf(stderr, "method not found: %s.%s%s\n", spec, mname, desc); return 1; }
        Frame *df = &g_ctx.frames[g_ctx.nf++];
        memset(df, 0, sizeof(*df));
        vec_init(&df->loc); vec_init(&df->stk);
        df->code = (u8 *)""; df->codelen = 0; df->cls = c; df->m = NULL;
        g_ap = call_args ? call_args : "";
        g_ap_done = !call_args || !*call_args;
        char tb[4096];
        if (!is_static) {
            const char *t = next_tok(tb, sizeof(tb));
            u32 thisv = 0;
            if (t && !strncmp(t, "new:", 4)) {
                Class *tc = load_class(t + 4);
                if (tc) thisv = (u32)(uintptr_t)inst_new(tc);
            } else if (t && strcmp(t, "null")) {
                fprintf(stderr, "bad this token '%s'\n", t); return 2;
            }
            vec_push(&df->stk, thisv);
        }
        if (push_args(df, desc)) return 2;
        g_budget = g_steps + 20000000L;
        call_method(owner, m, is_static ? 2 : 1);
        run_steps(0);
        if (m->retc) {
            u32 rv = vec_pop(&df->stk);
            if (m->retc == 'F') printf("RESULT F %.9g bits=%u\n", (double)u2fu(rv), rv);
            else if (m->retc == 'L' || m->retc == '[') {
                const u16 *w = str_w(rv);
                if (w) { char *s = narrow(w); printf("RESULT L \"%s\"\n", s); free(s); }
                else printf("RESULT L obj=%u\n", rv);
            } else printf("RESULT I %d u=%u\n", (i32)rv, rv);
        } else printf("RESULT V\n");
        if (g_dump) dump_statics(owner);
        fprintf(stderr, "[call done] %s.%s%s steps=%ld errors=%ld\n",
                owner->name, m->name, m->desc, g_steps, g_errors);
        return g_errors ? 1 : 0;
    }
    if (construct_spec) {
        Class *c = load_class(construct_spec);
        if (!c) { fprintf(stderr, "cannot load %s\n", construct_spec); return 1; }
        const char *desc = call_desc ? call_desc : "()V";
        Method *m = find_method(c, "<init>", desc);
        if (!m) { fprintf(stderr, "ctor not found: %s.<init>%s\n", construct_spec, desc); return 1; }
        Class *owner = member_owner(c, m);
        if (!owner) owner = c;
        Inst *o = inst_new(c);
        Frame *df = &g_ctx.frames[g_ctx.nf++];
        memset(df, 0, sizeof(*df));
        vec_init(&df->loc); vec_init(&df->stk);
        df->code = (u8 *)""; df->codelen = 0; df->cls = c; df->m = NULL;
        vec_push(&df->stk, (u32)(uintptr_t)o);
        g_ap = call_args ? call_args : "";
        g_ap_done = !call_args || !*call_args;
        if (push_args(df, desc)) return 2;
        g_budget = g_steps + 20000000L;
        call_method(owner, m, 1);
        run_steps(0);
        printf("RESULT OBJ %u\n", (u32)(uintptr_t)o);
        for (Class *k = c; k; k = k->super) {
            for (i32 j = 0; j < k->nfi; j++) {
                Field *f = &k->fi[j];
                prep_field(f, k);
                i32 idx = k->inst_base + j;
                u32 v = ((u32)idx < o->cap) ? o->fields[idx] : 0;
                printf("FLD\t%s\t%s\t%d\t%u\t%.9g", f->name, f->type, idx, v,
                       (double)u2fu(v));
                print_extra(f->type, v);
                putchar('\n');
            }
        }
        fprintf(stderr, "[construct done] %s.<init>%s steps=%ld errors=%ld\n",
                c->name, desc, g_steps, g_errors);
        return g_errors ? 1 : 0;
    }
    if (clinit) {
        Class *c = load_class(clinit);
        if (!c) { fprintf(stderr, "cannot load %s\n", clinit); return 1; }
        if (g_dump) dump_statics(c);
        fprintf(stderr, "[done] %s: steps=%ld errors=%ld native_stubs=%ld oob_locals=%ld\n",
                clinit, g_steps, g_errors, g_native_calls, g_oob);
        return g_errors ? 1 : 0;
    }
    if (g_batch) {
        char name[512];
        long i = 0;
        const char *drv = getenv("UNI_DRIVERS");
        if (drv && *drv) {
            char *dup = strdup(drv), *tok = strtok(dup, ",");
            while (tok) { run_driver(tok); tok = strtok(NULL, ","); }
            free(dup);
        }
        while (fgets(name, sizeof(name), stdin)) {
            size_t n = strlen(name);
            while (n && (name[n-1] == '\n' || name[n-1] == '\r')) name[--n] = 0;
            if (!n) continue;
            i++;
            long s0 = g_steps, e0 = g_errors, n0 = g_native_calls;
            g_budget = g_steps + 1000000L;           /* per-class budget */
            Class *c = load_class(name);
            fprintf(stderr, "[prog %ld] %s steps=%ld errs=%ld nat=%ld\n",
                    i, name, g_steps - s0, g_errors - e0,
                    g_native_calls - n0);
            printf("### %s\n", name);
            if (c) dump_statics(c);
            fflush(stdout);
        }
        fprintf(stderr, "[batch done] steps=%ld errors=%ld native_stubs=%ld native_impl=%ld oob_locals=%ld\n",
                g_steps, g_errors, g_native_calls - g_native_impl, g_native_impl, g_oob);
        for (int i = 0; i < 64 && g_errk[i].fmt; i++)
            fprintf(stderr, "[errkind %ld] %s\n", g_errk[i].n, g_errk[i].fmt);
        nat_dump("native_calls.tsv");
        return 0;
    }
    fprintf(stderr, "nothing to do\n");
    return 2;
}
