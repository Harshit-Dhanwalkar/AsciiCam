#include "nolibc.h"
#include "tokdec.h"

static int pass_n, fail_n;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    if (c) {                                                                   \
      pass_n++;                                                                \
    } else {                                                                   \
      fail_n++;                                                                \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static const char *J =
    "{\"version\":\"1.0\",\"added_tokens\":[{\"id\":0,\"content\":\"<unk>\","
    "\"special\":true},{\"id\":1,\"content\":\"<s>\",\"special\":true},"
    "{\"id\":2,\"content\":\"</s>\",\"special\":true}],"
    "\"normalizer\":{\"type\":\"Sequence\",\"pattern\":\"a\\\"b}\"},"
    "\"model\":{\"type\":\"BPE\",\"dropout\":null,\"vocab\":{"
    "\"<unk>\":0,\"<s>\":1,\"</s>\":2,\"\\u2581hello\":3,\"\\u2581wor\":4,"
    "\"ld\":5,\"<0xC3>\":6,\"<0xA9>\":7,\"\\u2581\":8,\"!\":9,\"<0xE2>\":10,"
    "\"caf\\u00e9\":11,\"\\ud83d\\ude00\":12,\"<0xFF>\":13},\"merges\":[]}}";

static size_t text(const tokdec_t *t, const int *ids, int n, char *out,
                   size_t cap) {
  return tokdec_text(t, ids, n, out, cap);
}

static int streq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }

  return *a == *b;
}

int main(void) {
  tokdec_t t;
  char err[96], out[128];
  CHECK(tokdec_parse(&t, J, nl_strlen(J), err, sizeof(err)) == 0, "parse: %s",
        err);
  CHECK(t.n == 14, "n = %d", t.n);

  int a[] = {1, 3, 4, 5, 9, 2};
  text(&t, a, 6, out, sizeof(out));
  CHECK(streq(out, "hello world!"), "plain: '%s'", out);

  // Byte fallback spells é as C3 A9
  int b[] = {3, 8, 6, 7};
  text(&t, b, 4, out, sizeof(out));
  CHECK(streq(out, "hello \xC3\xA9"), "bytes: '%s'", out);

  // Vocab keeps \u escapes and surrogate pairs
  int c[] = {11, 12};
  text(&t, c, 2, out, sizeof(out));
  CHECK(streq(out, "caf\xC3\xA9\xF0\x9F\x98\x80"), "escapes: '%s'", out);

  // Half a character becomes '?', never invalid UTF-8
  int d[] = {3, 6};
  text(&t, d, 2, out, sizeof(out));
  CHECK(streq(out, "hello?"), "half: '%s'", out);
  int e[] = {10, 13};
  text(&t, e, 2, out, sizeof(out));
  CHECK(streq(out, "??"), "bad bytes: '%s'", out);

  // Unknown or out of range ids print nothing
  int f[] = {3, 99, -1, 70000, 4};
  text(&t, f, 5, out, sizeof(out));
  CHECK(streq(out, "hello wor"), "range: '%s'", out);

  // Output is cut to buffer and stays terminated
  char small[8];
  size_t n = text(&t, a, 6, small, sizeof(small));
  CHECK(n == 6 && streq(small, "hello "), "cut: '%s' %d", small, (int)n);
  CHECK(text(&t, a, 6, out, 0) == 0, "no room");
  CHECK(text(&t, a, 0, out, sizeof(out)) == 0 && out[0] == 0, "empty");
  tokdec_free(&t);

  // Control tokens print nothing even when the file does not mark them
  const char *J2 =
      "{\"model\":{\"vocab\":{\"<s>\":0,\"</s>\":1,\"<pad>\":2,\"a\":3,"
      "\"<unk>\":4}}}";
  CHECK(tokdec_parse(&t, J2, nl_strlen(J2), err, sizeof(err)) == 0, "J2");
  int g[] = {0, 3, 1, 3, 2, 4, 3};
  text(&t, g, 7, out, sizeof(out));
  CHECK(streq(out, "aaa"), "unmarked control tokens: '%s'", out);
  tokdec_free(&t);

  // Malformed files are refused, none crash
  const char *bad[] = {
      "",
      "[]",
      "{",
      "{\"model\":{\"vocab\":{\"a\":1,\"b\":}}}",
      "{\"model\":{\"vocab\":{\"a\":1,\"a2\":1}}}",
      "{\"model\":{\"vocab\":{\"a\":99999999}}}",
      "{\"model\":{\"vocab\":{}}}",
      "{\"model\":{\"vocab\":{\"a\\u12\":1,\"b\":2}}}",
      "{\"model\":{\"vocab\":{\"\\ud800x\":1,\"b\":2}}}",
      "{\"added_tokens\":[{\"content\":\"x\"}],\"model\":{\"vocab\":{\"a\":0,"
      "\"b\":1}}}",
      "{\"model\":{\"type\":\"BPE\"}}",
  };
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    int r = tokdec_parse(&t, bad[i], nl_strlen(bad[i]), err, sizeof(err));
    CHECK(r == -1 && err[0], "bad %u refused (%d) %s", i, r, err);
    if (r == 0) {
      tokdec_free(&t);
    }
  }

  // Every cut of a good file either parses to same thing or is refused
  size_t len = nl_strlen(J);
  int parsed_early = 0;
  for (size_t k = 0; k < len; k++) {
    int r = tokdec_parse(&t, J, k, err, sizeof(err));
    if (r == 0) {
      parsed_early++;
      tokdec_free(&t);
    }
  }

  CHECK(parsed_early == 0, "%d truncated files parsed", parsed_early);

  // Damaged bytes never crash
  unsigned s = 12345u;
  char *buf = nl_malloc(len + 1);
  for (int it = 0; it < 3000; it++) {
    nl_memcpy(buf, J, len + 1);
    for (int k = 0; k < 3; k++) {
      s ^= s << 13u;
      s ^= s >> 17u;
      s ^= s << 5u;
      buf[s % len] = (char)(s >> 8u);
    }

    if (tokdec_parse(&t, buf, len, err, sizeof(err)) == 0) {
      int ids[3] = {3, 4, 5};
      text(&t, ids, 3, out, sizeof(out));
      tokdec_free(&t);
    }
  }

  nl_free(buf);
  fprintf(stderr, "tokdec: %d passed, %d failed\n", pass_n, fail_n);

  return fail_n != 0;
}
