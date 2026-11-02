/*
 * tests/test_nl_hash.c - lib/nl_hash.h SHA-256: FIPS vectors, padding edges
 * (55/56/63/64/65 bytes), million 'a', and every two-way split of input must
 * give same digest as one call
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_nl_hash.c lib/nl_printf.c lib/nl_errno.c \
 *     lib/nl_alloc.c lib/nl_start.c -Iinclude -Ilib -o /tmp/th
 */
#include "../lib/nl_hash.h"
#include "../lib/nolibc.h"

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

static void hex_of(const void *p, size_t n, char out[NL_SHA256_HEX_LEN]) {
  nl_sha256_t h;
  uint8_t d[NL_SHA256_LEN];
  nl_sha256_init(&h);
  nl_sha256_update(&h, p, n);
  nl_sha256_final(&h, d);
  nl_sha256_hex(d, out);
}

static int same(const char *a, const char *b) { return nl_strcmp(a, b) == 0; }

static void a_run(size_t n, const char *want) {
  static char buf[200];
  char hex[NL_SHA256_HEX_LEN];
  for (size_t i = 0; i < n; i++) {
    buf[i] = 'a';
  }

  hex_of(buf, n, hex);
  CHECK(same(hex, want), "%zu x 'a': got %s", n, hex);
}

int main(void) {
  char hex[NL_SHA256_HEX_LEN];

  hex_of("", 0, hex);
  CHECK(
      same(hex,
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
      "empty");
  hex_of("abc", 3, hex);
  CHECK(
      same(hex,
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
      "abc");
  const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  hex_of(two, nl_strlen(two), hex);
  CHECK(
      same(hex,
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
      "two-block vector");

  a_run(55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  a_run(56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  a_run(63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34");
  a_run(64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  a_run(65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0");
  a_run(119,
        "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
  a_run(120,
        "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
  a_run(128,
        "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e");

  // One million 'a', fed in odd-sized pieces
  {
    static char chunk[997];
    for (size_t i = 0; i < sizeof(chunk); i++) {
      chunk[i] = 'a';
    }

    nl_sha256_t h;
    uint8_t d[NL_SHA256_LEN];
    nl_sha256_init(&h);
    size_t left = 1000000;
    while (left) {
      size_t n = left < sizeof(chunk) ? left : sizeof(chunk);
      nl_sha256_update(&h, chunk, n);
      left -= n;
    }

    nl_sha256_final(&h, d);
    nl_sha256_hex(d, hex);
    CHECK(
        same(
            hex,
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
        "million a: %s", hex);
  }

  // Every split point of a 200-byte pattern
  {
    uint8_t msg[200];
    for (int i = 0; i < 200; i++) {
      msg[i] = (uint8_t)(i * 7 + 3);
    }

    char whole[NL_SHA256_HEX_LEN];
    hex_of(msg, sizeof(msg), whole);
    int bad = 0;
    for (size_t cut = 0; cut <= sizeof(msg); cut++) {
      nl_sha256_t h;
      uint8_t d[NL_SHA256_LEN];
      nl_sha256_init(&h);
      nl_sha256_update(&h, msg, cut);
      nl_sha256_update(&h, msg + cut, sizeof(msg) - cut);
      nl_sha256_final(&h, d);
      nl_sha256_hex(d, hex);
      bad += !same(hex, whole);
    }

    CHECK(bad == 0, "%d split points disagree", bad);
  }

  // Different input, different digest; final wipes state
  {
    char x[NL_SHA256_HEX_LEN], y[NL_SHA256_HEX_LEN];
    hex_of("plugin\n", 7, x);
    hex_of("plugin \n", 8, y);
    CHECK(!same(x, y), "one extra byte changes digest");

    nl_sha256_t h;
    uint8_t d[NL_SHA256_LEN];
    nl_sha256_init(&h);
    nl_sha256_update(&h, "abc", 3);
    nl_sha256_final(&h, d);
    int zero = 1;
    for (size_t i = 0; i < sizeof(h); i++) {
      zero &= ((uint8_t *)&h)[i] == 0;
    }

    CHECK(zero, "state wiped after final");
  }

  fprintf(stderr, "%d passed %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
