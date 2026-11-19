/*
 * tests/test_nl_ramfs.c - Archive format of lib/nl_ramfs.h
 *
 *   1. pack -> init -> find round trip, alignment, empty archive
 *   2. packer refuses unusable input
 *   3. reader refuses each kind of damaged header / entry
 *   4. mutation fuzz: random damage to a valid archive must either be refused
 *      or leave every entry readable inside bytes handed to init (archive copy
 *      is placed at very end of its buffer, so a read past it is caught by ASan
 *      builds too)
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_nl_ramfs.c lib/nl_mem.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c -Iinclude -Ilib -o /tmp/rf
 */

#include "../lib/nolibc.h"

#include "../lib/nl_ramfs.h"

#define BUF 8192

static uint8_t arc[BUF];
static uint8_t mut_buf[BUF];

static int fails, total;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (!(c)) {                                                                \
      fails++;                                                                 \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

static unsigned long long rng_state;
static unsigned long long rnd(void) {
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;

  return rng_state * 0x2545F4914F6CDD1DULL;
}
static unsigned rnd_n(unsigned n) { return (unsigned)(rnd() % n); }

static int same(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) {
      return 0;
    }
  }

  return 1;
}

static const uint8_t blob_a[] = {1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
                                 11, 12, 13, 14, 15, 16, 17, 18, 19};
static const uint8_t blob_c[] = {0xde, 0xad, 0xbe, 0xef};

static const nl_ramfs_src_t files[] = {
    {"plugins/invert.so", blob_a, sizeof(blob_a)},
    {"plugins/empty.so", (const void *)0, 0},
    {"charsets/ascii.txt", blob_c, sizeof(blob_c)},
};
#define NFILES (sizeof(files) / sizeof(files[0]))

static size_t build_valid(void) {
  size_t need = nl_ramfs_pack((uint8_t *)0, 0, files, NFILES);
  size_t got = nl_ramfs_pack(arc, sizeof(arc), files, NFILES);
  CHECK(need > 0 && need == got, "size query %ld != packed %ld", (long)need,
        (long)got);

  return got;
}

static void roundtrip(void) {
  size_t n = build_valid();
  nl_ramfs_t fs;
  CHECK(nl_ramfs_init(&fs, arc, n) == 0, "valid archive refused");
  CHECK(fs.count == NFILES, "count %d", (int)fs.count);
  CHECK(fs.size == n, "size %ld != %ld", (long)fs.size, (long)n);

  for (size_t i = 0; i < NFILES; i++) {
    const void *d = (const void *)0;
    size_t len = 0;
    CHECK(nl_ramfs_find(&fs, files[i].name, &d, &len) == 0, "'%s' not found",
          files[i].name);
    CHECK(len == files[i].len, "'%s' len %ld", files[i].name, (long)len);
    CHECK(same(d, files[i].data, len), "'%s' content differs", files[i].name);
    CHECK((((const uint8_t *)d - arc) % NL_RAMFS_ALIGN) == 0,
          "'%s' data not 16-byte aligned", files[i].name);
  }

  const void *d;
  size_t len;
  CHECK(nl_ramfs_find(&fs, "plugins/invert.s", &d, &len) < 0, "prefix matched");
  CHECK(nl_ramfs_find(&fs, "plugins/invert.so2", &d, &len) < 0,
        "name+x matched");
  CHECK(nl_ramfs_find(&fs, "", &d, &len) < 0, "empty name matched");
  CHECK(nl_ramfs_entry(&fs, (uint32_t)NFILES, (const char **)0, &d, &len) < 0,
        "entry past count");

  // Too small a buffer: size reported, nothing written
  uint8_t small[8] = {0};
  CHECK(nl_ramfs_pack(small, sizeof(small), files, NFILES) == n,
        "small buffer: wrong size");
  CHECK(small[0] == 0, "small buffer was written");

  // Empty archive
  size_t en = nl_ramfs_pack(arc, sizeof(arc), (const nl_ramfs_src_t *)0, 0);
  CHECK(en == NL_RAMFS_HDR_SIZE, "empty archive size %ld", (long)en);
  CHECK(nl_ramfs_init(&fs, arc, en) == 0 && fs.count == 0, "empty refused");
  CHECK(nl_ramfs_find(&fs, "x", &d, &len) < 0, "found in empty archive");

  // Trailing bytes after total size are allowed
  n = build_valid();
  CHECK(nl_ramfs_init(&fs, arc, n + 100) == 0, "trailing bytes refused");

  // None linked into this test binary
  CHECK(nl_ramfs_builtin(&fs) < 0 && fs.count == 0, "builtin without blob");
}

static void packer_refusals(void) {
  nl_ramfs_src_t dup[] = {{"a", blob_c, 4}, {"b", blob_c, 4}, {"a", blob_c, 4}};
  CHECK(nl_ramfs_pack(arc, sizeof(arc), dup, 3) == 0,
        "duplicate name accepted");

  nl_ramfs_src_t noname[] = {{(const char *)0, blob_c, 4}};
  CHECK(nl_ramfs_pack(arc, sizeof(arc), noname, 1) == 0, "NULL name accepted");

  nl_ramfs_src_t empty[] = {{"", blob_c, 4}};
  CHECK(nl_ramfs_pack(arc, sizeof(arc), empty, 1) == 0, "empty name accepted");

  char longname[NL_RAMFS_MAX_NAME + 2];
  for (size_t i = 0; i < sizeof(longname) - 1; i++) {
    longname[i] = 'x';
  }

  longname[sizeof(longname) - 1] = 0;
  nl_ramfs_src_t lng[] = {{longname, blob_c, 4}};
  CHECK(nl_ramfs_pack(arc, sizeof(arc), lng, 1) == 0, "256-char name accepted");
  longname[NL_RAMFS_MAX_NAME] = 0;
  CHECK(nl_ramfs_pack(arc, sizeof(arc), lng, 1) > 0, "255-char name refused");

  nl_ramfs_src_t nodata[] = {{"a", (const void *)0, 4}};
  CHECK(nl_ramfs_pack(arc, sizeof(arc), nodata, 1) == 0, "NULL data with len");

  nl_ramfs_src_t huge[] = {{"a", blob_c, (size_t)NL_RAMFS_MAX_SIZE + 1}};
  CHECK(nl_ramfs_pack((uint8_t *)0, 0, huge, 1) == 0,
        "oversized entry accepted");

  CHECK(nl_ramfs_pack((uint8_t *)0, 0, files, NL_RAMFS_MAX_ENTRIES + 1) == 0,
        "too many entries accepted");
}

static void put32(size_t off, unsigned v) {
  arc[off + 0] = (uint8_t)v;
  arc[off + 1] = (uint8_t)(v >> 8);
  arc[off + 2] = (uint8_t)(v >> 16);
  arc[off + 3] = (uint8_t)(v >> 24);
}

// Entry 0 field `f` (0 name off, 1 name len, 2 data off, 3 data len) = v
static void set_field(int f, unsigned v) {
  put32(NL_RAMFS_HDR_SIZE + (size_t)f * 4, v);
}

#define EXPECT_REFUSED(what, stmt)                                             \
  do {                                                                         \
    size_t n_ = build_valid();                                                 \
    nl_ramfs_t fs_;                                                            \
    stmt;                                                                      \
    CHECK(nl_ramfs_init(&fs_, arc, n_) < 0, "accepted: %s", what);             \
    CHECK(fs_.count == 0 && fs_.base == (const uint8_t *)0,                    \
          "fs not cleared after refusing: %s", what);                          \
  } while (0)

static void reader_refusals(void) {
  nl_ramfs_t fs;
  size_t n = build_valid();
  CHECK(nl_ramfs_init(&fs, arc, 0) < 0, "zero length accepted");
  CHECK(nl_ramfs_init(&fs, arc, NL_RAMFS_HDR_SIZE - 1) < 0, "short header");
  CHECK(nl_ramfs_init(&fs, (const void *)0, n) < 0, "NULL image accepted");
  CHECK(nl_ramfs_init(&fs, arc, n - 1) < 0, "truncated archive accepted");

  EXPECT_REFUSED("bad magic", arc[0] = 'X');
  EXPECT_REFUSED("version 2", arc[4] = 2);
  EXPECT_REFUSED("reserved set", arc[6] = 1);
  EXPECT_REFUSED("count too large", put32(8, NL_RAMFS_MAX_ENTRIES + 1));
  EXPECT_REFUSED("count 0xffffffff", put32(8, 0xffffffffu));
  EXPECT_REFUSED("table past total", put32(8, 400));
  EXPECT_REFUSED("total past image", put32(12, (unsigned)n + 16));
  EXPECT_REFUSED("total 0xffffffff", put32(12, 0xffffffffu));
  EXPECT_REFUSED("total below table", put32(12, 20));

  EXPECT_REFUSED("name inside header", set_field(0, 4));
  EXPECT_REFUSED("name offset huge", set_field(0, 0xfffffff0u));
  EXPECT_REFUSED("name length 0", set_field(1, 0));
  EXPECT_REFUSED("name length 256", set_field(1, NL_RAMFS_MAX_NAME + 1));
  EXPECT_REFUSED("name length huge", set_field(1, 0xffffffffu));
  EXPECT_REFUSED("name runs past total", set_field(0, (unsigned)n - 3));
  EXPECT_REFUSED("name not NUL terminated", set_field(1, 5)); // 'plugi' + 'n'
  EXPECT_REFUSED("name contains NUL", set_field(1, 40)); // Across string table

  // 256 character name: well formed in every way but length limit
  {
    static char nm[NL_RAMFS_MAX_NAME + 1];
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < NL_RAMFS_MAX_NAME; i++) {
      nm[i] = 'x';
    }

    nl_ramfs_src_t one[] = {{nm, zeros, sizeof(zeros)}};
    size_t on = nl_ramfs_pack(arc, sizeof(arc), one, 1);
    CHECK(on > 0 && nl_ramfs_init(&fs, arc, on) == 0, "255-char archive");

    // name starts at 32 and is followed by first data byte (zero)
    arc[32 + NL_RAMFS_MAX_NAME] = 'x';
    set_field(1, NL_RAMFS_MAX_NAME + 1);
    CHECK(nl_ramfs_init(&fs, arc, on) < 0, "256-char name accepted");
  }

  EXPECT_REFUSED("data offset unaligned", set_field(2, 17));
  EXPECT_REFUSED("data offset past total", set_field(2, (unsigned)n + 16));
  EXPECT_REFUSED("data length past total", set_field(3, (unsigned)n));
  EXPECT_REFUSED("data length huge", set_field(3, 0xffffffffu));
  EXPECT_REFUSED("data offset + length wraps", set_field(2, 0xfffffff0u));
}

// Every guarantee a caller relies on after a successful nl_ramfs_init
static int usable(const nl_ramfs_t *fs, const uint8_t *lo, size_t len) {
  const uint8_t *hi = lo + len;
  for (uint32_t i = 0; i < fs->count; i++) {
    const char *name;
    const void *data;
    size_t dlen;
    if (nl_ramfs_entry(fs, i, &name, &data, &dlen) < 0) {
      return 0;
    }

    const uint8_t *p = (const uint8_t *)name;
    size_t k = 0;
    while (p + k < hi && p[k] && k <= NL_RAMFS_MAX_NAME) {
      k++;
    }

    if (p < lo || p + k >= hi || p[k] != 0 || k == 0) {
      return 0;
    }

    const uint8_t *d = (const uint8_t *)data;
    if (d < lo || d > hi || dlen > (size_t)(hi - d)) {
      return 0;
    }

    // Read it all, so a sanitizer sees every byte
    unsigned sum = 0;
    for (size_t j = 0; j < dlen; j++) {
      sum += d[j];
    }

    if (sum == 0xdeadbeefu) {
      return 0;
    }

    const void *d2;
    size_t l2;
    if (nl_ramfs_find(fs, name, &d2, &l2) < 0) {
      return 0;
    }
  }

  return 1;
}

static void fuzz(long iters) {
  size_t n = build_valid();
  static uint8_t orig[BUF];
  for (size_t i = 0; i < n; i++) {
    orig[i] = arc[i];
  }

  static const unsigned interesting[] = {
      0, 1, 15, 16, 255, 256, 0x7fffffffu, 0x80000000u, 0xffffffffu};
  long refused = 0, accepted = 0;
  for (long it = 0; it < iters; it++) {
    size_t len = n;
    for (size_t i = 0; i < n; i++) {
      mut_buf[BUF - n + i] = orig[i];
    }

    int muts = 1 + (int)rnd_n(4);
    for (int m = 0; m < muts; m++) {
      uint8_t *img = mut_buf + BUF - n;
      switch (rnd_n(4)) {
      case 0: {
        img[rnd_n((unsigned)n)] = (uint8_t)rnd();

        break;
      }
      case 1: {
        img[rnd_n((unsigned)n)] ^= (uint8_t)(1u << rnd_n(8));

        break;
      }
      case 2: { // Hostile u32 inside header / entry table / strings
        unsigned off = rnd_n(NL_RAMFS_HDR_SIZE + 3 * NL_RAMFS_ENT_SIZE);
        unsigned v =
            interesting[rnd_n(sizeof(interesting) / sizeof(*interesting))];
        for (int b = 0; b < 4 && off + (unsigned)b < n; b++) {
          img[off + (unsigned)b] = (uint8_t)(v >> (8 * b));
        }

        break;
      }
      default: { // Truncate, keeping copy flush with end of buffer
        len = rnd_n((unsigned)n + 1);

        break;
      }
      }
    }

    // Flush with end of buffer: image is last `len` bytes of mut_buf
    const uint8_t *img = mut_buf + BUF - len;
    if (len < n) {
      // Truncation: bytes beyond len must not be reachable, so re-copy
      for (size_t i = len; i-- > 0;) { // Downwards: ranges overlap
        mut_buf[BUF - len + i] = mut_buf[BUF - n + i];
      }
    }

    nl_ramfs_t fs;
    if (nl_ramfs_init(&fs, img, len) < 0) {
      refused++;
      CHECK(fs.count == 0, "refused but count set (iter %ld)", it);
    } else {
      accepted++;
      CHECK(usable(&fs, img, len), "accepted archive not usable (iter %ld)",
            it);
    }
  }

  fprintf(stderr, "fuzz: %ld refused, %ld accepted\n", refused, accepted);
  CHECK(refused > 0 && accepted > 0, "fuzz never hit one of the outcomes");
}

int main(int argc, char **argv) {
  long iters = 200000;
  if (argc > 1) {
    iters = 0;
    for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++) {
      iters = iters * 10 + (*s - '0');
    }
  }

  rng_state = 0x9E3779B97F4A7C15ULL;

  roundtrip();
  packer_refusals();
  reader_refusals();
  fuzz(iters);

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
