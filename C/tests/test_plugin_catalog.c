/*
 * tests/test_plugin_catalog.c - runtime plugin catalog, background builds and
 * search picker
 *
 *   FLAGS="-D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc \
 *     -print-file-name=include) -ffreestanding -fno-builtin \
 *     -fno-stack-protector -nostdlib -static -Iinclude -Ilib"
 *   LIBS="lib/nl_printf.c lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c"
 *   gcc $FLAGS tests/test_plugin_catalog.c src/plugin_catalog.c \
 *     src/plugin_picker.c src/plugins.c lib/nl_dlfcn.c $LIBS -o /tmp/t_cat
 *   /tmp/t_cat
 */
#include "../lib/nolibc.h"

#include "plugin_catalog.h"
#include "plugin_picker.h"
#include "plugins.h"

static int fails, total;
#define CHECK(c, msg)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (!(c)) {                                                                \
      fails++;                                                                 \
      fprintf(stderr, "FAIL [%d] %s\n", __LINE__, msg);                        \
    }                                                                          \
  } while (0)

#define SRC_DIR ".cache/test_src"
#define CACHE_DIR ".cache/test_plugins"

static void mkdirs(void) {
  __sc2(SYS_mkdir, (long)".cache", 0755);
  __sc2(SYS_mkdir, (long)SRC_DIR, 0755);
  __sc2(SYS_mkdir, (long)CACHE_DIR, 0755);
}

static void write_file(const char *path, const char *text) {
  int fd = nl_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    nl_write(fd, text, nl_strlen(text));
    nl_close(fd);
  }
}

static void rm(const char *path) { nl_unlink(path); }

static int contains(const char *hay, const char *needle) {
  size_t n = nl_strlen(needle);
  for (; *hay; hay++) {
    size_t k = 0;
    while (k < n && hay[k] == needle[k]) {
      k++;
    }

    if (k == n) {
      return 1;
    }
  }

  return 0;
}

// Wait up to ~secs for build of entry idx to finish
static int wait_build(plugin_catalog_t *c, int idx, int secs) {
  for (int i = 0; i < secs * 100; i++) {
    int fi;
    while (catalog_poll(c, &fi)) {
      if (fi == idx) {
        return 1;
      }
    }

    nl_usleep(10000);
  }

  return 0;
}

static const char GOOD_SRC[] = "#include \"nolibc.h\"\n"
                               "#include \"plugins.h\"\n"
                               "static void p(uint8_t *g, int w, int h, "
                               "void *c) { (void)g; (void)w; (void)h; "
                               "(void)c; }\n"
                               "static filter_plugin_t self = {p, \"good\"};\n"
                               "filter_plugin_t *plugin_get(void) { return "
                               "&self; }\n";

static void test_match(void) {
  CHECK(picker_name_match("edge_detect", ""), "empty query matches all");
  CHECK(picker_name_match("edge_detect", "edge"), "prefix");
  CHECK(picker_name_match("edge_detect", "DETECT"), "case-insensitive suffix");
  CHECK(picker_name_match("edge_detect", "ge_d"), "middle substring");
  CHECK(!picker_name_match("edge_detect", "xyz"), "no match");
  CHECK(!picker_name_match("ab", "abc"), "query longer than name");
}

static void fake_catalog(plugin_catalog_t *c) {
  catalog_init(c, "x", "y", (char **)0);
  const char *names[] = {"edge_detect", "invert", "threshold", "invert2"};

  for (int i = 0; i < 4; i++) {
    nl_strncpy_safe(c->e[i].name, names[i], CATALOG_NAME_LEN);

    c->e[i].state = PC_AVAILABLE;
  }

  c->count = 4;
}

static void test_picker_keys(void) {
  plugin_catalog_t c;
  fake_catalog(&c);

  picker_t p;
  int li = -1;
  nl_memset(&p, 0, sizeof(p));
  CHECK(picker_key(&p, &c, 'i', &li) == PK_NONE,
        "inactive picker ignores keys");

  picker_open(&p);
  int m[CATALOG_MAX];
  CHECK(picker_matches(&p, &c, m, CATALOG_MAX) == 4, "all listed at start");

  CHECK(picker_key(&p, &c, 'i', &li) == PK_CONSUMED, "typing consumed");
  CHECK(picker_key(&p, &c, 'n', &li) == PK_CONSUMED, "typing consumed 2");
  CHECK(picker_matches(&p, &c, m, CATALOG_MAX) == 2, "in -> invert, invert2");
  CHECK(m[0] == 1 && m[1] == 3, "match indices");

  CHECK(picker_key(&p, &c, PK_KEY_DOWN, &li) == PK_CONSUMED, "down");
  CHECK(p.sel == 1, "down moves to second match");
  picker_key(&p, &c, PK_KEY_DOWN, &li);
  CHECK(p.sel == 0, "down wraps");
  picker_key(&p, &c, PK_KEY_UP, &li);
  CHECK(p.sel == 1, "up wraps");

  CHECK(picker_key(&p, &c, 10, &li) == PK_LOAD, "enter loads");
  CHECK(li == 3, "enter returns selected catalog index");
  CHECK(!p.active, "enter closes picker");

  // backspace and hotkey letters are text while open
  picker_open(&p);
  picker_key(&p, &c, 'w', &li);
  picker_key(&p, &c, 'q', &li);
  CHECK(p.active && p.qlen == 2, "w and q are query text, not hotkeys");
  CHECK(picker_matches(&p, &c, m, CATALOG_MAX) == 0, "wq matches nothing");
  picker_key(&p, &c, 127, &li);
  picker_key(&p, &c, 8, &li);
  CHECK(p.qlen == 0, "backspace erases");
  picker_key(&p, &c, 127, &li);
  CHECK(p.qlen == 0, "backspace on empty is safe");

  // enter with no match closes without loading
  picker_key(&p, &c, 'z', &li);
  CHECK(picker_key(&p, &c, 13, &li) == PK_CLOSED, "enter on no match closes");
  CHECK(!p.active, "closed");

  picker_open(&p);
  CHECK(picker_key(&p, &c, PK_KEY_ESC, &li) == PK_CLOSED, "esc closes");

  // query overflow never writes past buffer
  picker_open(&p);
  for (int i = 0; i < 200; i++) {
    picker_key(&p, &c, 'a', &li);
  }
  CHECK(p.qlen < PICKER_QUERY_MAX && p.query[p.qlen] == '\0',
        "query is bounded and terminated");

  // control bytes are ignored, not stored
  picker_open(&p);
  picker_key(&p, &c, 1, &li);
  picker_key(&p, &c, 200, &li);
  CHECK(p.qlen == 0, "non-printable keys ignored");
}

static void test_picker_render(void) {
  plugin_catalog_t c;
  fake_catalog(&c);
  c.e[2].state = PC_FAILED;
  nl_strncpy_safe(c.e[2].err, "t.c:3:1: error: boom", CATALOG_ERR_LEN);
  c.e[1].state = PC_ACTIVE;

  picker_t p;
  picker_open(&p);
  char buf[4096];

  int frame_rows = 30;

  int n = picker_render(&p, &c, frame_rows, 1, buf, sizeof(buf));
  CHECK(n > 0 && (size_t)n < sizeof(buf), "renders");
  CHECK(buf[n] == '\0', "NUL terminated");
  CHECK(contains(buf, "edge_detect") && contains(buf, "threshold"),
        "names shown");
  CHECK(contains(buf, "[failed: t.c:3:1: error: boom]"),
        "failure reason shown");
  CHECK(contains(buf, "[active]"), "active state shown");
  CHECK(contains(buf, "\033[31;1H"),
        "header is at ascii_h + 1 (below the frame)");

  // no ANSI cursor-position escape in output should land on a frame row
  // (i.e. row <= frame_rows)
  {
    int on_frame = 0;
    for (const char *s = buf; *s; s++) {
      if (s[0] == '\033' && s[1] == '[') {
        int row = 0;
        const char *q = s + 2;
        while (*q >= '0' && *q <= '9') {
          row = row * 10 + (*q++ - '0');
        }

        if (*q == ';') {
          const char *r = q + 1;
          while (*r >= '0' && *r <= '9') {
            r++;
          }

          if ((*r == 'H' || *r == 'f') && row > 0 && row <= frame_rows) {
            on_frame = 1;
          }
        }
      }
    }

    CHECK(!on_frame, "picker never writes to a frame row");
  }

  picker_key(&p, &c, 'z', (int *)0);
  n = picker_render(&p, &c, frame_rows, 0, buf, sizeof(buf));
  CHECK(n > 0 && contains(buf, "no plugin matches"), "no-match row");

  CHECK(picker_render(&p, &c, 1, 1, buf, sizeof(buf)) > 0,
        "short frame -> renders at ascii_h + 1");

  picker_t closed;
  nl_memset(&closed, 0, sizeof(closed));
  CHECK(picker_render(&closed, &c, frame_rows, 1, buf, sizeof(buf)) == 0,
        "closed picker draws nothing");

  // tiny output buffer: truncated but never overrun
  char small[96];
  small[sizeof(small) - 1] = 'Z';
  picker_open(&p);
  n = picker_render(&p, &c, frame_rows, 1, small, 90);
  CHECK(small[sizeof(small) - 1] == 'Z', "no write past given size");
  (void)n;

  // many plugins: window scrolls with selection and stays bounded
  plugin_catalog_t big;
  catalog_init(&big, "x", "y", (char **)0);
  for (int i = 0; i < 30; i++) {
    nl_snprintf(big.e[i].name, CATALOG_NAME_LEN, "plug%d", i);
  }

  big.count = 30;
  picker_open(&p);
  for (int i = 0; i < 20; i++) {
    picker_key(&p, &big, PK_KEY_DOWN, (int *)0);
  }

  n = picker_render(&p, &big, 40, 1, buf, sizeof(buf));
  CHECK(n > 0 && contains(buf, "plug20"), "selected row is scrolled into view");
  CHECK(!contains(buf, "plug0 "), "rows above window are not drawn");

  char tbuf[512];
  n = toast_render("hello", 20, 1, tbuf, sizeof(tbuf));
  CHECK(n > 0 && contains(tbuf, "hello") && contains(tbuf, "\033[20;1H"),
        "toast on last frame row");
  CHECK(toast_render("", 20, 1, tbuf, sizeof(tbuf)) == 0, "empty toast");
}

static void test_scan(char **envp) {
  mkdirs();
  rm(SRC_DIR "/good.c");
  rm(SRC_DIR "/helper.c");
  rm(SRC_DIR "/late.c");
  rm(SRC_DIR "/bad name.c");
  write_file(SRC_DIR "/good.c", GOOD_SRC);
  write_file(SRC_DIR "/helper.c", "int helper(void) { return 1; }\n");
  write_file(SRC_DIR "/bad name.c", GOOD_SRC);
  write_file(SRC_DIR "/notes.txt", "plugin_get is mentioned here\n");

  plugin_catalog_t c;
  catalog_init(&c, SRC_DIR "/", CACHE_DIR, envp);
  CHECK(catalog_supported(), "supported on Linux");
  CHECK(catalog_scan(&c) == 1, "only real plugin source is listed");
  CHECK(c.count == 1 && nl_strcmp(c.e[0].name, "good") == 0, "name is stem");
  CHECK(nl_strcmp(c.e[0].src_path, SRC_DIR "/good.c") == 0,
        "trailing slash normalised");
  CHECK(c.e[0].state == PC_AVAILABLE, "no .so yet -> available");

  // rescan keeps indices and state, appends new files
  c.e[0].state = PC_FAILED;
  write_file(SRC_DIR "/late.c", GOOD_SRC);
  CHECK(catalog_scan(&c) == 2, "rescan finds new source");
  CHECK(nl_strcmp(c.e[0].name, "good") == 0 && c.e[0].state == PC_FAILED,
        "known entry keeps index and state");
  CHECK(catalog_find(&c, "late") == 1 && catalog_find(&c, "nope") == -1,
        "find");

  // filters dir lists shipped plugins in alphabetical order
  plugin_catalog_t f;
  catalog_init(&f, "filters", CACHE_DIR, envp);
  catalog_scan(&f);
  CHECK(f.count >= 3, "shipped filters found");
  int ok = 1;
  for (int i = 1; i < f.count; i++) {
    if (nl_strcmp(f.e[i - 1].name, f.e[i].name) > 0) {
      ok = 0;
    }
  }

  CHECK(ok, "initial list is alphabetical");
  CHECK(catalog_find(&f, "invert") >= 0 && catalog_find(&f, "threshold") >= 0,
        "invert and threshold present");

  // missing dir is not an error
  plugin_catalog_t none;
  catalog_init(&none, ".cache/does_not_exist", CACHE_DIR, envp);
  CHECK(catalog_scan(&none) == 0, "missing source dir -> empty catalog");

  rm(SRC_DIR "/good.c");
  rm(SRC_DIR "/helper.c");
  rm(SRC_DIR "/late.c");
  rm(SRC_DIR "/bad name.c");
  rm(SRC_DIR "/notes.txt");
}

static void test_build_and_load(char **envp) {
  mkdirs();
  rm(CACHE_DIR "/invert.so");

  plugin_catalog_t c;
  catalog_init(&c, "filters", CACHE_DIR, envp);
  catalog_scan(&c);
  int idx = catalog_find(&c, "invert");
  CHECK(idx >= 0, "invert in catalog");
  if (idx < 0) {
    return;
  }

  CHECK(catalog_start_build(&c, idx) == 0, "build starts");
  CHECK(c.e[idx].state == PC_COMPILING && c.e[idx].pid > 0,
        "compiling with a child pid");
  CHECK(catalog_start_build(&c, idx) == 0 && c.e[idx].state == PC_COMPILING,
        "starting again while compiling is a no-op");

  CHECK(wait_build(&c, idx, 60), "build finishes");
  CHECK(c.e[idx].state == PC_READY, "build succeeded -> ready");

  // artifact is a real, loadable plugin
  plugin_loader_t pl;
  nl_memset(&pl, 0, sizeof(pl));
  pl.inotify_fd = -1;

  plugin_log_stderr = 0;
  CHECK(plugin_load(&pl, c.e[idx].so_path) == 0, "built .so loads");
  if (pl.plugin) {
    uint8_t px[4] = {0, 100, 200, 255};

    int strength = 255;
    pl.plugin->process(px, 4, 1, &strength);
    CHECK(px[0] == 255 && px[1] == 155 && px[2] == 55 && px[3] == 0,
          "loaded plugin actually inverts");
  }

  plugin_cleanup(&pl);

  // second build of an up-to-date plugin still reports ready
  c.e[idx].state = PC_READY;
  CHECK(catalog_start_build(&c, idx) == 0, "rebuild starts");
  CHECK(wait_build(&c, idx, 60) && c.e[idx].state == PC_READY,
        "no-op rebuild ends ready");

  // active entries are not rebuilt
  c.e[idx].state = PC_ACTIVE;
  CHECK(catalog_start_build(&c, idx) == 0 && c.e[idx].state == PC_ACTIVE,
        "active entry untouched");
}

static void test_failure_and_cancel(char **envp) {
  mkdirs();
  rm(SRC_DIR "/broken.c");
  rm(CACHE_DIR "/broken.so");
  write_file(SRC_DIR "/broken.c",
             "#include \"nolibc.h\"\n#include \"plugins.h\"\n"
             "int plugin_get(void) { return undeclared_thing; }\n");

  plugin_catalog_t c;
  catalog_init(&c, SRC_DIR, CACHE_DIR, envp);
  catalog_scan(&c);
  int idx = catalog_find(&c, "broken");
  CHECK(idx >= 0, "broken source is listed");
  if (idx < 0) {
    return;
  }

  CHECK(catalog_start_build(&c, idx) == 0, "build starts");
  CHECK(wait_build(&c, idx, 60), "failing build finishes");
  CHECK(c.e[idx].state == PC_FAILED, "state failed");
  CHECK(contains(c.e[idx].err, "error"), "first error line captured");
  CHECK(!contains(c.e[idx].err, SRC_DIR "/"), "directory prefix stripped");
  CHECK(c.e[idx].pid == 0, "pid cleared");

  // no make on PATH: report it instead of hanging or claiming success
  char *noenv[] = {"PATH=/nonexistent", (char *)0};
  plugin_catalog_t n;
  catalog_init(&n, SRC_DIR, CACHE_DIR, noenv);
  catalog_scan(&n);
  idx = catalog_find(&n, "broken");
  CHECK(catalog_start_build(&n, idx) == 0, "spawn itself succeeds");
  CHECK(wait_build(&n, idx, 10), "finishes");
  CHECK(n.e[idx].state == PC_FAILED, "missing make -> failed");
  CHECK(n.e[idx].err[0] != '\0', "reason recorded");

  // cancelling kills and reaps child (a second wait finds no child)
  rm(CACHE_DIR "/threshold.so");
  plugin_catalog_t k;
  catalog_init(&k, "filters", CACHE_DIR, envp);
  catalog_scan(&k);
  int ti = catalog_find(&k, "threshold");
  CHECK(catalog_start_build(&k, ti) == 0, "build starts");
  long pid = k.e[ti].pid;
  catalog_cleanup(&k);
  CHECK(k.e[ti].state == PC_FAILED && k.e[ti].pid == 0, "cancelled");
  int status;
  long r = __sc4(SYS_wait4, pid, (long)&status, 1, 0);
  CHECK(r < 0, "child was reaped (no zombie left)");

  rm(SRC_DIR "/broken.c");
}

int main(int argc, char **argv) {
  char **envp = argv + argc + 1;

  test_match();
  test_picker_keys();
  test_picker_render();
  test_scan(envp);
  test_build_and_load(envp);
  test_failure_and_cancel(envp);

  fprintf(stderr, "%d/%d checks passed\n", total - fails, total);

  return fails ? 1 : 0;
}
