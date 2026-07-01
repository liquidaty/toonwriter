/* toonwriter - fuzz harness.
 *
 * The fuzzed bytes are interpreted as a deterministic *program* of push-API
 * calls that builds one well-formed document (balanced containers, proper
 * key/value pairing). The same program is then replayed through the writer
 * under several lookahead-window sizes -- a large RAM-only window and tiny
 * windows that force a temp-file spill.
 *
 * Oracle (the writer-equivalent of json2toon's round-trip check): the output
 * MUST NOT depend on the window. A spill is a memory-vs-disk decision only; if
 * any window produces different bytes, a spill/seek/varint/boundary bug has
 * changed the output, and we abort(). Running this under ASan/UBSan also turns
 * the random structures into a memory-safety sweep of the capture, two-pass
 * shape/emit, and output-buffer paths.
 *
 * Build: `make fuzz` (libFuzzer/LLVM clang) or `make fuzz-standalone` (portable
 * replay driver, any toolchain).
 */
#include <toonwriter.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------- capture sink */

typedef struct { char *p; size_t len, cap; int oom; } cap_sink;
static size_t cap_sink_fn(const void *restrict ptr, size_t size, size_t nmemb,
                          void *restrict arg) {
  cap_sink *c = (cap_sink *)arg;
  size_t total = size * nmemb;
  if (c->len + total > c->cap) {
    size_t nc = c->cap ? c->cap : 256;
    char *nb;
    while (nc < c->len + total) nc *= 2;
    nb = (char *)realloc(c->p, nc);
    if (!nb) { c->oom = 1; return 0; }
    c->p = nb; c->cap = nc;
  }
  memcpy(c->p + c->len, ptr, total);
  c->len += total;
  return nmemb;
}

/* ------------------------------------------------- deterministic generator */

/* A cursor over the fuzz input; out-of-bytes reads return 0 so generation
 * always terminates (depth and child counts are also hard-bounded). */
typedef struct { const uint8_t *d; size_t n, pos; int budget; } cur;
static unsigned take(cur *c) { return c->pos < c->n ? c->d[c->pos++] : 0; }

#define FZ_MAXDEPTH 40
#define FZ_MAXKIDS 8

static void gen_value(toonwriter_handle h, cur *c, int depth);

/* Emit one scalar derived from the next input bytes. */
static void gen_scalar(toonwriter_handle h, cur *c) {
  unsigned k = take(c) % 7;
  switch (k) {
  case 0: toonwriter_int(h, (toonw_int64)((long long)take(c) - 128)); break;
  case 1: {
    long double d = (long double)((int)take(c) - 100) / 4.0L;
    toonwriter_dbl(h, d);
    break;
  }
  case 2: toonwriter_bool(h, (unsigned char)(take(c) & 1)); break;
  case 3: toonwriter_null(h); break;
  case 4: {                       /* string with possibly tricky bytes */
    unsigned len = take(c) % 10, i;
    unsigned char s[10];
    for (i = 0; i < len; i++) s[i] = (unsigned char)take(c);
    toonwriter_strn(h, s, len);
    break;
  }
  case 5: {                       /* unknown classifier */
    unsigned len = take(c) % 8, i;
    unsigned char s[8];
    for (i = 0; i < len; i++) s[i] = (unsigned char)take(c);
    toonwriter_unknown(h, s, len, 0);
    break;
  }
  default: {                      /* raw, kept a valid bare token */
    char s[16];
    int m = (int)(take(c)) - 128;
    int len = snprintf(s, sizeof s, "%d", m);
    toonwriter_write_raw(h, (const unsigned char *)s, (size_t)len);
    break;
  }
  }
}

/* Emit a key derived from the next input bytes (may need quoting/escaping).
 * NUL-terminate so the len==0 case is a well-defined empty key: object_keyn
 * treats len_or_zero==0 as "key is a C string", i.e. strlen(key). */
static void gen_key(toonwriter_handle h, cur *c) {
  unsigned len = take(c) % 8, i;
  char s[9];
  for (i = 0; i < len; i++) s[i] = (char)(take(c) | 1); /* avoid embedded NUL */
  s[len] = 0;
  toonwriter_object_keyn(h, s, len);
}

static void gen_object(toonwriter_handle h, cur *c, int depth) {
  int kids = 0;
  toonwriter_start_object(h);
  while (kids < FZ_MAXKIDS && c->budget > 0 && (take(c) & 3)) {
    c->budget--;
    gen_key(h, c);
    gen_value(h, c, depth + 1);
    kids++;
  }
  toonwriter_end(h);
}

static void gen_array(toonwriter_handle h, cur *c, int depth) {
  int kids = 0;
  toonwriter_start_array(h);
  while (kids < FZ_MAXKIDS && c->budget > 0 && (take(c) & 3)) {
    c->budget--;
    gen_value(h, c, depth + 1);
    kids++;
  }
  toonwriter_end(h);
}

static void gen_value(toonwriter_handle h, cur *c, int depth) {
  unsigned k;
  if (depth >= FZ_MAXDEPTH || c->budget <= 0) { gen_scalar(h, c); return; }
  k = take(c) % 5;
  if (k == 0) gen_object(h, c, depth);
  else if (k == 1) gen_array(h, c, depth);
  else gen_scalar(h, c);
}

/* Run the whole program once with the given options, capturing the output. */
static char *run_program(const uint8_t *data, size_t len,
                         const struct toonwriter_opts *opts, size_t *out_len) {
  cap_sink cs = {0, 0, 0, 0};
  cur c;
  toonwriter_handle h = toonwriter_new_stream(cap_sink_fn, &cs,
                                              (struct toonwriter_opts *)opts);
  if (!h) { *out_len = 0; return NULL; }
  c.d = data; c.n = len; c.pos = 0; c.budget = 4000;
  gen_value(h, &c, 0);
  toonwriter_end_all(h);
  toonwriter_delete(h);
  *out_len = cs.len;
  return cs.p;
}

/* Compare two outputs; an inequality means a spill/window-dependent bug. */
static void same_or_abort(const char *a, size_t an, const char *b, size_t bn,
                          size_t window) {
  if (an == bn && (an == 0 || memcmp(a, b, an) == 0))
    return;
#ifdef TOONW_FUZZ_STANDALONE
  {
    size_t i = 0, m = an < bn ? an : bn;
    while (i < m && a[i] == b[i]) i++;
    fprintf(stderr,
            "DIVERGE window=%zu ref_len=%zu cur_len=%zu first_diff=%zu\n",
            window, an, bn, i);
  }
#else
  (void)window;
#endif
  abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
  /* Window sizes spanning RAM-only down to byte-at-a-time spilling. */
  static const size_t windows[] = {1u << 20, 0 /*default 1MiB*/, 4096, 257, 64, 13, 7, 1};
  size_t i, ref_len = 0, len_i;
  struct toonwriter_opts o;
  char *ref, *cur_out;

  memset(&o, 0, sizeof o);
  o.max_buffer_size = windows[0];
  ref = run_program(data, len, &o, &ref_len);
  if (!ref && ref_len == 0) ref = (char *)calloc(1, 1); /* normalize NULL/empty */

  for (i = 1; i < sizeof windows / sizeof windows[0]; i++) {
    memset(&o, 0, sizeof o);
    o.max_buffer_size = windows[i];
    cur_out = run_program(data, len, &o, &len_i);
    if (!cur_out && len_i == 0) cur_out = (char *)calloc(1, 1);
    same_or_abort(ref, ref_len, cur_out, len_i, windows[i]);
    free(cur_out);
  }
  free(ref);
  return 0;
}

#ifdef TOONW_FUZZ_STANDALONE
/* Minimal libFuzzer-compatible standalone driver: run each argv file (or stdin
 * if none) through LLVMFuzzerTestOneInput exactly once. Buildable/runnable with
 * any toolchain (no libFuzzer), for CI smoke coverage and crash replay. */
#include <stdio.h>

static int run_stream(FILE *f) {
  size_t cap = 4096, len = 0;
  unsigned char *buf = (unsigned char *)malloc(cap);
  if (!buf) return -1;
  for (;;) {
    size_t got;
    if (len == cap) {
      unsigned char *nb = (unsigned char *)realloc(buf, cap * 2);
      if (!nb) { free(buf); return -1; }
      buf = nb; cap *= 2;
    }
    got = fread(buf + len, 1, cap - len, f);
    len += got;
    if (got == 0) break;
  }
  LLVMFuzzerTestOneInput(buf, len);
  free(buf);
  return 0;
}

int main(int argc, char **argv) {
  int i;
  if (argc < 2)
    return run_stream(stdin) == 0 ? 0 : 1;
  for (i = 1; i < argc; i++) {
    FILE *f = fopen(argv[i], "rb");
    if (!f) { fprintf(stderr, "fuzz: cannot open '%s'\n", argv[i]); return 1; }
    if (run_stream(f) != 0) { fclose(f); return 1; }
    fclose(f);
  }
  return 0;
}
#endif /* TOONW_FUZZ_STANDALONE */
