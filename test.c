/* toonwriter unit test suite.
 *
 * Drives the push API and compares TOON output byte-for-byte against
 * spec-conforming expectations (the canonical forms cross-checked against the
 * TOON spec and the json2toon reference). Also exercises the bounded-memory
 * machinery: every array form is produced once entirely in RAM and once with a
 * tiny lookahead window that forces a temp-file spill, and the two outputs must
 * be byte-identical (a spill must never change the output). Build/run: `make
 * test` (optionally ASAN=1 / DEBUG=1).
 */
#include <toonwriter.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ capture sink */

typedef struct { char *p; size_t len, cap; } sbuf;

static size_t sink(const void *restrict ptr, size_t size, size_t nmemb,
                   void *restrict arg) {
  sbuf *b = (sbuf *)arg;
  size_t total = size * nmemb;
  if (b->len + total + 1 > b->cap) {
    size_t nc = b->cap ? b->cap : 64;
    char *np;
    while (nc < b->len + total + 1) nc *= 2;
    np = (char *)realloc(b->p, nc);
    if (!np) { perror("realloc"); exit(2); }
    b->p = np; b->cap = nc;
  }
  memcpy(b->p + b->len, ptr, total);
  b->len += total;
  b->p[b->len] = 0;
  return nmemb;
}

static int g_pass = 0, g_fail = 0;

static void show(const char *s) {
  for (; *s; s++) {
    if (*s == '\n') printf("\\n");
    else if ((unsigned char)*s < 0x20) printf("\\x%02x", (unsigned char)*s);
    else putchar(*s);
  }
}

static void check(const char *name, const char *got, const char *exp) {
  if (strcmp(got, exp) == 0) { g_pass++; return; }
  g_fail++;
  printf("FAIL %s\n  expected: \"", name); show(exp);
  printf("\"\n  got:      \""); show(got); printf("\"\n");
}

static void check_status(const char *name, int got, int want) {
  if (got == want) { g_pass++; return; }
  g_fail++;
  printf("FAIL %s: status %d, want %d\n", name, got, want);
}

static void check_bool(const char *name, int cond) {
  if (cond) { g_pass++; return; }
  g_fail++;
  printf("FAIL %s\n", name);
}

/* ---------------------------------------------------------------- helpers */

#define NEW() sbuf b = {0,0,0}; toonwriter_handle h = toonwriter_new_stream(sink, &b, NULL)
#define DONE(name, exp) do { toonwriter_delete(h); check(name, b.p?b.p:"", exp); free(b.p); } while (0)
#define K(s) toonwriter_object_key(h, s)
#define S(s) toonwriter_cstr(h, s)

/* ============================================================ basic shapes */

static void test_basic(void) {
  /* root primitives */
  { NEW(); toonwriter_int(h, 42);            DONE("root-number", "42\n"); }
  { NEW(); toonwriter_dbl(h, -3.14L);        DONE("root-negative", "-3.14\n"); }
  { NEW(); S("hello");                       DONE("root-string", "hello\n"); }
  { NEW(); toonwriter_bool(h, 1);            DONE("root-true", "true\n"); }
  { NEW(); toonwriter_bool(h, 0);            DONE("root-false", "false\n"); }
  { NEW(); toonwriter_null(h);               DONE("root-null", "null\n"); }

  /* objects */
  { NEW(); toonwriter_start_object(h); K("name"); S("Alice"); K("age");
    toonwriter_int(h, 30); toonwriter_end(h);
    DONE("flat-object", "name: Alice\nage: 30\n"); }
  { NEW(); toonwriter_start_object(h); K("user"); toonwriter_start_object(h);
    K("name"); S("Alice"); toonwriter_end(h); toonwriter_end(h);
    DONE("nested-object", "user:\n  name: Alice\n"); }
  { NEW(); toonwriter_start_object(h); toonwriter_end(h);
    DONE("empty-object-root", ""); }
  { NEW(); toonwriter_start_object(h); K("meta"); toonwriter_start_object(h);
    toonwriter_end(h); toonwriter_end(h);
    DONE("empty-object-field", "meta:\n"); }
  { NEW(); toonwriter_start_object(h); K("a"); toonwriter_bool(h, 1); K("b");
    toonwriter_null(h); toonwriter_end(h);
    DONE("bool-null-fields", "a: true\nb: null\n"); }
  { NEW(); toonwriter_start_object(h); K("a"); toonwriter_start_object(h);
    K("b"); toonwriter_start_object(h); K("c"); toonwriter_int(h, 1);
    toonwriter_end(h); toonwriter_end(h); toonwriter_end(h);
    DONE("deep-object", "a:\n  b:\n    c: 1\n"); }
}

/* ============================================================ arrays */

static void test_arrays(void) {
  /* inline */
  { NEW(); toonwriter_start_object(h); K("tags"); toonwriter_start_array(h);
    S("a"); S("b"); S("c"); toonwriter_end(h); toonwriter_end(h);
    DONE("inline-array", "tags[3]: a,b,c\n"); }
  { NEW(); toonwriter_start_array(h); toonwriter_int(h, 1); toonwriter_int(h, 2);
    toonwriter_int(h, 3); toonwriter_end(h);
    DONE("root-inline-array", "[3]: 1,2,3\n"); }
  { NEW(); toonwriter_start_object(h); K("tags"); toonwriter_start_array(h);
    toonwriter_end(h); toonwriter_end(h);
    DONE("empty-array-field", "tags: []\n"); }
  { NEW(); toonwriter_start_array(h); toonwriter_end(h);
    DONE("empty-array-root", "[]\n"); }

  /* empty-string key */
  { NEW(); toonwriter_start_object(h); K(""); toonwriter_start_array(h);
    toonwriter_end(h); toonwriter_end(h);
    DONE("empty-key-empty-array", "\"\": []\n"); }
  { NEW(); toonwriter_start_object(h); K(""); toonwriter_start_array(h);
    toonwriter_int(h, 1); toonwriter_int(h, 2); toonwriter_end(h); toonwriter_end(h);
    DONE("empty-key-inline-array", "\"\"[2]: 1,2\n"); }
  { NEW(); toonwriter_start_object(h); K(""); toonwriter_start_array(h);
    toonwriter_start_object(h); K("x"); toonwriter_int(h, 1); toonwriter_end(h);
    toonwriter_end(h); toonwriter_end(h);
    DONE("empty-key-tabular-array", "\"\"[1]{x}:\n  1\n"); }
  /* empty key *inside* a captured object (tabular + list); the column / member
   * key is an empty string. Regression for NULL+0 UB on a 0-length key buffer. */
  { NEW(); toonwriter_start_array(h);
    toonwriter_start_object(h); K(""); toonwriter_int(h, 1); toonwriter_end(h);
    toonwriter_start_object(h); K(""); toonwriter_int(h, 2); toonwriter_end(h);
    toonwriter_end(h);
    DONE("empty-key-in-tabular", "[2]{\"\"}:\n  1\n  2\n"); }
  { NEW(); toonwriter_start_array(h);
    toonwriter_start_object(h); K(""); toonwriter_int(h, 1); K("a"); toonwriter_int(h, 2); toonwriter_end(h);
    toonwriter_start_object(h); K("a"); toonwriter_int(h, 3); toonwriter_end(h);
    toonwriter_end(h);
    DONE("empty-key-in-list", "[2]:\n  - \"\": 1\n    a: 2\n  - a: 3\n"); }

  /* tabular */
  { NEW(); toonwriter_start_object(h); K("users"); toonwriter_start_array(h);
    toonwriter_start_object(h); K("id"); toonwriter_int(h, 1); K("name"); S("Alice");
    K("role"); S("admin"); toonwriter_end(h);
    toonwriter_start_object(h); K("id"); toonwriter_int(h, 2); K("name"); S("Bob");
    K("role"); S("user"); toonwriter_end(h);
    toonwriter_end(h); toonwriter_end(h);
    DONE("tabular", "users[2]{id,name,role}:\n  1,Alice,admin\n  2,Bob,user\n"); }
  { NEW(); toonwriter_start_array(h); toonwriter_start_object(h); K("id");
    toonwriter_int(h, 1); K("url"); S("http://a:b"); toonwriter_end(h);
    toonwriter_end(h);
    DONE("tabular-quoted-cell", "[1]{id,url}:\n  1,\"http://a:b\"\n"); }
  /* reordered keys -> still tabular (same key SET, template order) */
  { NEW(); toonwriter_start_array(h);
    toonwriter_start_object(h); K("a"); toonwriter_int(h, 1); K("b"); toonwriter_int(h, 2); toonwriter_end(h);
    toonwriter_start_object(h); K("b"); toonwriter_int(h, 4); K("a"); toonwriter_int(h, 3); toonwriter_end(h);
    toonwriter_end(h);
    DONE("tabular-reordered-keys", "[2]{a,b}:\n  1,2\n  3,4\n"); }

  /* not tabular -> list */
  { NEW(); toonwriter_start_array(h);
    toonwriter_start_object(h); K("a"); toonwriter_int(h, 1); toonwriter_end(h);
    toonwriter_start_object(h); K("b"); toonwriter_int(h, 2); toonwriter_end(h);
    toonwriter_end(h);
    DONE("not-tabular-different-keys", "[2]:\n  - a: 1\n  - b: 2\n"); }
  { NEW(); toonwriter_start_array(h);
    toonwriter_start_object(h); K("a"); toonwriter_int(h, 1); toonwriter_end(h);
    toonwriter_start_object(h); K("a"); toonwriter_int(h, 1); K("b"); toonwriter_int(h, 2); toonwriter_end(h);
    toonwriter_end(h);
    DONE("not-tabular-extra-key", "[2]:\n  - a: 1\n  - a: 1\n    b: 2\n"); }
  { NEW(); toonwriter_start_array(h);
    toonwriter_start_object(h); K("a"); toonwriter_int(h, 1); K("b"); toonwriter_int(h, 2); toonwriter_end(h);
    toonwriter_start_object(h); K("a"); toonwriter_int(h, 3); toonwriter_end(h);
    toonwriter_end(h);
    DONE("not-tabular-missing-key", "[2]:\n  - a: 1\n    b: 2\n  - a: 3\n"); }

  /* lists */
  { NEW(); toonwriter_start_array(h); toonwriter_int(h, 1);
    toonwriter_start_object(h); K("a"); toonwriter_int(h, 1); toonwriter_end(h);
    S("x"); toonwriter_end(h);
    DONE("list-mixed", "[3]:\n  - 1\n  - a: 1\n  - x\n"); }
  { NEW(); toonwriter_start_array(h);
    toonwriter_start_array(h); toonwriter_int(h, 1); toonwriter_int(h, 2); toonwriter_end(h);
    toonwriter_start_array(h); toonwriter_int(h, 3); toonwriter_int(h, 4); toonwriter_end(h);
    toonwriter_end(h);
    DONE("array-of-arrays", "[2]:\n  - [2]: 1,2\n  - [2]: 3,4\n"); }
  { NEW(); toonwriter_start_array(h); toonwriter_start_object(h); K("status");
    S("active"); K("details"); toonwriter_start_object(h); K("level"); S("high");
    K("count"); toonwriter_int(h, 5); toonwriter_end(h); toonwriter_end(h);
    toonwriter_end(h);
    DONE("list-nested-object",
         "[1]:\n  - status: active\n    details:\n      level: high\n      count: 5\n"); }
  { NEW(); toonwriter_start_array(h); toonwriter_start_object(h); toonwriter_end(h);
    toonwriter_end(h);
    DONE("list-empty-object-item", "[1]:\n  -\n"); }
  { NEW(); toonwriter_start_array(h); toonwriter_start_array(h); toonwriter_end(h);
    toonwriter_end(h);
    DONE("list-empty-array-item", "[1]:\n  - []\n"); }
  { NEW(); toonwriter_start_array(h); toonwriter_start_object(h); toonwriter_end(h);
    toonwriter_start_object(h); toonwriter_end(h); toonwriter_end(h);
    DONE("list-two-empty-objects", "[2]:\n  -\n  -\n"); }

  /* list item whose first field is a tabular array (nested-depth layout) */
  { NEW(); toonwriter_start_array(h); toonwriter_start_object(h);
    K("users"); toonwriter_start_array(h);
    toonwriter_start_object(h); K("id"); toonwriter_int(h, 1); K("name"); S("Ada"); toonwriter_end(h);
    toonwriter_start_object(h); K("id"); toonwriter_int(h, 2); K("name"); S("Bob"); toonwriter_end(h);
    toonwriter_end(h);
    K("status"); S("active"); toonwriter_end(h); toonwriter_end(h);
    DONE("list-obj-first-tabular",
         "[1]:\n  - users[2]{id,name}:\n      1,Ada\n      2,Bob\n    status: active\n"); }
}

/* ============================================================ scalars */

static void test_scalars(void) {
  /* string quoting */
#define QT(name, val, exp) do { NEW(); toonwriter_start_object(h); K("v"); S(val); \
    toonwriter_end(h); DONE(name, exp); } while (0)
  QT("empty-string", "", "v: \"\"\n");
  QT("numeric-string", "42", "v: \"42\"\n");
  QT("number-like-neg", "-1.5e3", "v: \"-1.5e3\"\n");
  QT("string-comma", "a,b", "v: \"a,b\"\n");
  QT("string-colon", "a:b", "v: \"a:b\"\n");
  QT("string-bracket", "a[b]", "v: \"a[b]\"\n");
  QT("string-brace", "a{b}", "v: \"a{b}\"\n");
  QT("string-literal-collide-true", "true", "v: \"true\"\n");
  QT("string-literal-collide-null", "null", "v: \"null\"\n");
  QT("string-leading-space", " x", "v: \" x\"\n");
  QT("string-trailing-tab", "x\t", "v: \"x\\t\"\n");
  QT("string-hyphen", "-x", "v: \"-x\"\n");
  QT("string-plain", "hello world", "v: hello world\n");
  QT("string-newline-escape", "a\nb", "v: \"a\\nb\"\n");
  QT("string-cr-escape", "a\rb", "v: \"a\\rb\"\n");
  QT("string-backspace-escape", "a\bb", "v: \"a\\u0008b\"\n");
  QT("string-formfeed-escape", "a\fb", "v: \"a\\u000cb\"\n");
  QT("string-quote-escape", "a\"b", "v: \"a\\\"b\"\n");
  QT("string-backslash-escape", "a\\b", "v: \"a\\\\b\"\n");
  QT("string-utf8-passthrough", "caf\xc3\xa9", "v: caf\xc3\xa9\n");
#undef QT
  /* control char -> \u00XX (value 0x01 inside, forces quoting) */
  { NEW(); toonwriter_start_object(h); K("v");
    toonwriter_strn(h, (const unsigned char *)"a\x01" "b", 3); toonwriter_end(h);
    DONE("string-control-escape", "v: \"a\\u0001b\"\n"); }

  /* key quoting */
  { NEW(); toonwriter_start_object(h); K("a-b"); toonwriter_int(h, 1); toonwriter_end(h);
    DONE("key-dash", "\"a-b\": 1\n"); }
  { NEW(); toonwriter_start_object(h); K("full name"); S("Ada"); toonwriter_end(h);
    DONE("key-space", "\"full name\": Ada\n"); }
  { NEW(); toonwriter_start_object(h); K("_ok9"); toonwriter_int(h, 1); toonwriter_end(h);
    DONE("key-bare-underscore", "_ok9: 1\n"); }
  { NEW(); toonwriter_start_object(h); K("9bad"); toonwriter_int(h, 1); toonwriter_end(h);
    DONE("key-leading-digit", "\"9bad\": 1\n"); }

  /* number formatting */
  { NEW(); toonwriter_start_object(h); K("a"); toonwriter_int(h, -9223372036854775807LL - 1);
    K("b"); toonwriter_int(h, 9223372036854775807LL); toonwriter_end(h);
    DONE("int-extremes", "a: -9223372036854775808\nb: 9223372036854775807\n"); }
  { NEW(); toonwriter_start_object(h); K("a"); toonwriter_size_t(h, (size_t)0);
    K("b"); toonwriter_size_t(h, (size_t)123456); toonwriter_end(h);
    DONE("size_t-fields", "a: 0\nb: 123456\n"); }
  { NEW(); toonwriter_start_object(h); K("a"); toonwriter_dbl(h, 1.5L);
    K("b"); toonwriter_dbl(h, 1000.0L); K("c"); toonwriter_dbl(h, 0.0L);
    toonwriter_end(h);
    DONE("dbl-trim", "a: 1.5\nb: 1000\nc: 0\n"); }

  /* unknown classification: numbers/bools verbatim, else quoted string */
  { NEW(); toonwriter_start_object(h); K("x");
    toonwriter_unknown(h, (const unsigned char *)"1.50", 4, 0); K("z");
    toonwriter_unknown(h, (const unsigned char *)"1e3", 3, 0); K("t");
    toonwriter_unknown(h, (const unsigned char *)"true", 4, 0); K("s");
    toonwriter_unknown(h, (const unsigned char *)"hi:there", 8, 0); toonwriter_end(h);
    DONE("unknown-passthrough", "x: 1.50\nz: 1e3\nt: true\ns: \"hi:there\"\n"); }

  /* write_raw: verbatim, no quoting */
  { NEW(); toonwriter_start_object(h); K("v");
    toonwriter_write_raw(h, (const unsigned char *)"0xFF", 4); toonwriter_end(h);
    DONE("write-raw", "v: 0xFF\n"); }

  /* null string -> null literal */
  { NEW(); toonwriter_start_object(h); K("v"); toonwriter_str(h, NULL); toonwriter_end(h);
    DONE("null-string", "v: null\n"); }
}

/* ============================================================ error paths */

static void test_errors(void) {
  { NEW(); check_status("end-nothing-open", toonwriter_end(h),
                        toonwriter_status_invalid_end); toonwriter_delete(h); free(b.p); }
  { NEW(); toonwriter_start_object(h);
    check_status("end-array-on-object", toonwriter_end_array(h),
                 toonwriter_status_invalid_end);
    check_status("end-object-ok", toonwriter_end_object(h), toonwriter_status_ok);
    toonwriter_delete(h); free(b.p); }
  { NEW(); toonwriter_start_object(h); toonwriter_end(h);
    check_status("double-end", toonwriter_end(h), toonwriter_status_invalid_end);
    toonwriter_delete(h); free(b.p); }
  /* end_all closes every open container */
  { NEW(); toonwriter_start_object(h); K("a"); toonwriter_start_object(h);
    K("b"); toonwriter_start_object(h); K("c"); toonwriter_int(h, 1);
    toonwriter_end_all(h);
    DONE("end-all", "a:\n  b:\n    c: 1\n"); }
  /* variant handler not set -> misconfiguration */
  { NEW(); check_status("variant-unset", toonwriter_variant(h, NULL),
                        toonwriter_status_misconfiguration);
    toonwriter_delete(h); free(b.p); }
}

/* ============================================================ variant */

static struct toonwriter_variant my_variant(void *data) {
  struct toonwriter_variant v;
  int tag = *(int *)data;
  memset(&v, 0, sizeof v);
  if (tag == 0) { v.type = toonwriter_datatype_integer; v.value.i = 99; }
  else if (tag == 1) { v.type = toonwriter_datatype_string;
                       v.value.str = (unsigned char *)"hi there"; }
  else { v.type = toonwriter_datatype_bool; v.value.b = 1; }
  return v;
}
static int g_cleanups = 0;
static void my_cleanup(void *data, struct toonwriter_variant *v) {
  (void)data; (void)v; g_cleanups++;
}

static void test_variant(void) {
  int t0 = 0, t1 = 1, t2 = 2;
  NEW();
  g_cleanups = 0;
  check_status("variant-set",
               toonwriter_set_variant_handler(h, my_variant, my_cleanup),
               toonwriter_status_ok);
  toonwriter_start_object(h);
  K("a"); toonwriter_variant(h, &t0);
  K("b"); toonwriter_variant(h, &t1);
  K("c"); toonwriter_variant(h, &t2);
  toonwriter_end(h);
  DONE("variant", "a: 99\nb: hi there\nc: true\n");
  check_bool("variant-cleanup-called", g_cleanups == 3);
}

/* ============================================================ bounded memory */

/* Build a representative document (each array form, deeply nested, large) into
 * a fresh writer with the given options. Reused to prove the output is
 * independent of the lookahead window (RAM vs spill). */
static char *build_doc(const struct toonwriter_opts *opts, int n) {
  sbuf b = {0,0,0};
  toonwriter_handle h = toonwriter_new_stream(sink, &b, (struct toonwriter_opts *)opts);
  int i;
  char namebuf[32];
  toonwriter_start_object(h);
  K("rows");
  toonwriter_start_array(h); /* large tabular */
  for (i = 0; i < n; i++) {
    toonwriter_start_object(h);
    K("id"); toonwriter_int(h, i);
    K("name"); snprintf(namebuf, sizeof namebuf, "user_%d", i); S(namebuf);
    K("note"); S("a fairly long, comma,ridden: value [with] {brackets}");
    toonwriter_end(h);
  }
  toonwriter_end(h);
  K("nums");
  toonwriter_start_array(h); /* large inline */
  for (i = 0; i < n; i++) toonwriter_int(h, i * 7 - 3);
  toonwriter_end(h);
  K("mixed");
  toonwriter_start_array(h); /* list with nesting */
  for (i = 0; i < 8; i++) {
    if (i % 2) { toonwriter_start_array(h); toonwriter_int(h, i); toonwriter_int(h, -i); toonwriter_end(h); }
    else { toonwriter_start_object(h); K("k"); toonwriter_int(h, i);
           K("deep"); toonwriter_start_object(h); K("x"); S("y"); toonwriter_end(h);
           toonwriter_end(h); }
  }
  toonwriter_end(h);
  toonwriter_end(h);
  toonwriter_delete(h);
  if (!b.p) { b.p = (char *)malloc(1); if (b.p) b.p[0] = 0; }
  return b.p;
}

static void test_bounded(void) {
  struct toonwriter_opts ram, spill;
  char *a, *c;
  memset(&ram, 0, sizeof ram);
  memset(&spill, 0, sizeof spill);
  ram.max_buffer_size = 1u << 20;  /* all in RAM */
  spill.max_buffer_size = 8;       /* forces a temp-file spill */

  a = build_doc(&ram, 200);
  c = build_doc(&spill, 200);
  check_bool("spill-determinism-200", strcmp(a, c) == 0);
  free(a); free(c);

  /* large enough to cross the 64 KiB output buffer many times */
  a = build_doc(&ram, 4000);
  c = build_doc(&spill, 4000);
  check_bool("spill-determinism-4000", strcmp(a, c) == 0);
  free(a); free(c);

  /* deep nesting within the bounded depth limit */
  {
    sbuf b1 = {0,0,0}, b2 = {0,0,0};
    toonwriter_handle h1 = toonwriter_new_stream(sink, &b1, &ram);
    toonwriter_handle h2 = toonwriter_new_stream(sink, &b2, &spill);
    int d;
    /* an array nested 60 deep, then a scalar */
    for (d = 0; d < 60; d++) { toonwriter_start_array(h1); toonwriter_start_array(h2); }
    toonwriter_int(h1, 7); toonwriter_int(h2, 7);
    toonwriter_end_all(h1); toonwriter_end_all(h2);
    toonwriter_delete(h1); toonwriter_delete(h2);
    check_bool("deep-array-spill-determinism", b1.p && b2.p && strcmp(b1.p, b2.p) == 0);
    free(b1.p); free(b2.p);
  }

  /* a single very large scalar string (exercises the >64 KiB write path) */
  {
    size_t big = 200000, i;
    char *bigstr = (char *)malloc(big + 1);
    sbuf b1 = {0,0,0};
    toonwriter_handle h1;
    for (i = 0; i < big; i++) bigstr[i] = 'x';
    bigstr[big] = 0;
    h1 = toonwriter_new_stream(sink, &b1, NULL);
    toonwriter_start_object(h1); toonwriter_object_key(h1, "big");
    toonwriter_cstrn(h1, bigstr, big);
    toonwriter_end(h1); toonwriter_delete(h1);
    check_bool("huge-scalar", b1.len == strlen("big: ") + big + 1);
    free(bigstr); free(b1.p);
  }
}

/* ------------------------------------------------- get_temp_filename hook */

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
static int g_tmp_calls = 0;
static char g_tmp_last[256];
static char *test_tmpname(const char *prefix) {
  size_t n;
  char *p;
  (void)prefix;
  snprintf(g_tmp_last, sizeof g_tmp_last, "/tmp/toonw_test_spill_%d.tmp", g_tmp_calls++);
  n = strlen(g_tmp_last) + 1;
  p = (char *)malloc(n);
  if (p) memcpy(p, g_tmp_last, n);
  return p;
}
static char *unwritable_tmpname(const char *prefix) {
  const char *path = "/toonwriter_no_such_dir/spill.tmp";
  size_t n = strlen(path) + 1;
  char *p = (char *)malloc(n);
  (void)prefix;
  if (p) memcpy(p, path, n);
  return p;
}
static void test_temp_hook(void) {
  struct toonwriter_opts o;
  struct stat st;
  sbuf b = {0,0,0};
  toonwriter_handle h;
  int i;
  memset(&o, 0, sizeof o);
  o.max_buffer_size = 4;            /* tiny window -> spill */
  o.get_temp_filename = test_tmpname;
  g_tmp_calls = 0;
  h = toonwriter_new_stream(sink, &b, &o);
  toonwriter_start_array(h);
  for (i = 0; i < 20; i++) toonwriter_int(h, i);
  toonwriter_end(h);
  toonwriter_delete(h);
  check_bool("temp-hook-called", g_tmp_calls > 0);
  check_bool("temp-hook-file-removed", stat(g_tmp_last, &st) != 0);
  check("temp-hook-output", b.p ? b.p : "",
        "[20]: 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19\n");
  free(b.p);

  /* an unwritable spill target surfaces as io_error (distinct from OOM) */
  {
    sbuf b2 = {0,0,0};
    toonwriter_handle h2;
    memset(&o, 0, sizeof o);
    o.max_buffer_size = 4;
    o.get_temp_filename = unwritable_tmpname;
    h2 = toonwriter_new_stream(sink, &b2, &o);
    toonwriter_start_array(h2);
    for (i = 0; i < 20; i++) toonwriter_int(h2, i);
    toonwriter_end(h2);
    check_status("spill-io-error", toonwriter_error(h2), toonwriter_status_io_error);
    toonwriter_delete(h2);
    free(b2.p);
  }
}
#else
static void test_temp_hook(void) {}
#endif

/* Configurable indent width (opts.indent). */
static void test_indent(void) {
  struct toonwriter_opts o;
  /* indent = 4 */
  { sbuf b = {0,0,0};
    toonwriter_handle h;
    memset(&o, 0, sizeof o); o.indent = 4;
    h = toonwriter_new_stream(sink, &b, &o);
    toonwriter_start_object(h); toonwriter_object_key(h, "a");
    toonwriter_start_object(h); toonwriter_object_key(h, "b"); toonwriter_int(h, 1);
    toonwriter_end(h); toonwriter_end(h); toonwriter_delete(h);
    check("indent-4-object", b.p ? b.p : "", "a:\n    b: 1\n"); free(b.p); }
  /* indent = 4 applies to tabular rows and list items too */
  { sbuf b = {0,0,0};
    toonwriter_handle h;
    memset(&o, 0, sizeof o); o.indent = 4;
    h = toonwriter_new_stream(sink, &b, &o);
    toonwriter_start_object(h); toonwriter_object_key(h, "xs");
    toonwriter_start_array(h);
    toonwriter_start_object(h); toonwriter_object_key(h, "id"); toonwriter_int(h, 1); toonwriter_end(h);
    toonwriter_start_object(h); toonwriter_object_key(h, "id"); toonwriter_int(h, 2); toonwriter_end(h);
    toonwriter_end(h); toonwriter_end(h); toonwriter_delete(h);
    check("indent-4-tabular", b.p ? b.p : "", "xs[2]{id}:\n    1\n    2\n"); free(b.p); }
  /* indent = 0 falls back to the default of 2 */
  { sbuf b = {0,0,0};
    toonwriter_handle h;
    memset(&o, 0, sizeof o); o.indent = 0;
    h = toonwriter_new_stream(sink, &b, &o);
    toonwriter_start_object(h); toonwriter_object_key(h, "a");
    toonwriter_start_object(h); toonwriter_object_key(h, "b"); toonwriter_int(h, 1);
    toonwriter_end(h); toonwriter_end(h); toonwriter_delete(h);
    check("indent-0-default-2", b.p ? b.p : "", "a:\n  b: 1\n"); free(b.p); }
}

int main(void) {
  test_basic();
  test_arrays();
  test_scalars();
  test_errors();
  test_variant();
  test_bounded();
  test_temp_hook();
  test_indent();
  printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
