/*
 * src/slides.c - folder slideshow, see include/slides.h
 */

#include "platform.h"

#if !defined(__LINUX_NOLIBC__)
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "nolibc.h"

#include "slides.h"

#define NAME_LEN 128
#define DIR_LEN 400

#if defined(__LINUX_NOLIBC__)
#ifndef SYS_getdents64
#define SYS_getdents64 217
#endif

// open(2) flags: O_RDONLY | O_DIRECTORY
#define SLIDES_OPEN_DIR 0200000

struct slides_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};
#endif

static struct {
  char dir[DIR_LEN];
  char name[SLIDES_MAX][NAME_LEN];
  unsigned char bad[SLIDES_MAX];
  int count;
  int cur;
  long start_ms; // when current picture appeared, -1 = not yet known
  int req;
  int changed;
  int open;
} S;

static int g_secs = SLIDES_DEFAULT_SECS;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int is_digit(int c) { return c >= '0' && c <= '9'; }

int slides_is_picture(const char *name) {
  size_t n = nl_strlen(name);
  size_t dot = n;
  while (dot > 0 && name[dot - 1] != '.') {
    dot--;
  }

  if (dot == 0 || dot == n) {
    return 0;
  }

  char ext[6];
  size_t len = n - dot;
  if (len >= sizeof(ext)) {
    return 0;
  }

  for (size_t i = 0; i < len; i++) {
    ext[i] = (char)lower(name[dot + i]);
  }

  ext[len] = '\0';
  static const char *const known[] = {"png", "jpg", "jpeg",
                                      "pgm", "ppm", "pnm"};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (nl_strcmp(ext, known[i]) == 0) {
      return 1;
    }
  }

  return 0;
}

int slides_natural_cmp(const char *a, const char *b) {
  const char *a0 = a;
  const char *b0 = b;
  while (*a && *b) {
    if (is_digit(*a) && is_digit(*b)) {
      const char *ra = a;
      const char *rb = b;
      while (*a == '0') {
        a++;
      }

      while (*b == '0') {
        b++;
      }

      size_t la = 0;
      size_t lb = 0;
      while (is_digit(a[la])) {
        la++;
      }

      while (is_digit(b[lb])) {
        lb++;
      }

      if (la != lb) {
        return la < lb ? -1 : 1;
      }

      for (size_t i = 0; i < la; i++) {
        if (a[i] != b[i]) {
          return a[i] < b[i] ? -1 : 1;
        }
      }

      // Same value: fewer leading zeros first, so order is total
      size_t za = (size_t)(a - ra);
      size_t zb = (size_t)(b - rb);
      if (za != zb) {
        return za < zb ? -1 : 1;
      }

      a += la;
      b += lb;

      continue;
    }

    int ca = lower(*a);
    int cb = lower(*b);
    if (ca != cb) {
      return ca < cb ? -1 : 1;
    }

    a++;
    b++;
  }

  if (*a || *b) {
    return *a ? 1 : -1;
  }

  // Equal apart from case
  return nl_strcmp(a0, b0);
}

static void sort_names(void) {
  char tmp[NAME_LEN];
  for (int i = 1; i < S.count; i++) {
    nl_memcpy(tmp, S.name[i], NAME_LEN);
    int j = i - 1;
    while (j >= 0 && slides_natural_cmp(S.name[j], tmp) > 0) {
      nl_memcpy(S.name[j + 1], S.name[j], NAME_LEN);
      j--;
    }

    nl_memcpy(S.name[j + 1], tmp, NAME_LEN);
  }
}

static void add_name(const char *name, int is_dir) {
  if (is_dir || name[0] == '.' || S.count >= SLIDES_MAX ||
      nl_strlen(name) >= NAME_LEN || !slides_is_picture(name)) {
    return;
  }

  nl_strncpy_safe(S.name[S.count], name, NAME_LEN);
  S.count++;
}

// Fill S.name from dir. 0 on success, -1 when dir cannot be read as a folder
static int read_dir(const char *dir) {
  S.count = 0;
#if defined(__LINUX_NOLIBC__)
  int fd = open(dir, SLIDES_OPEN_DIR, 0);
  if (fd < 0) {
    return -1;
  }

  char dbuf[8192] = {0};
  int first = 1;
  for (;;) {
    long n = __sc3(SYS_getdents64, fd, (long)dbuf, (long)sizeof(dbuf));
    if (n < 0 && first) {
      close(fd);

      return -1;
    }

    if (n <= 0) {
      break;
    }

    first = 0;
    for (long off = 0; off < n;) {
      const struct slides_dirent64 *d =
          (const struct slides_dirent64 *)(dbuf + off);
      if (d->d_reclen == 0) {
        break;
      }

      off += d->d_reclen;
      add_name(d->d_name, d->d_type == 4); // DT_DIR
    }
  }

  close(fd);
#else
  DIR *dp = opendir(dir);
  if (!dp) {
    return -1;
  }

  struct dirent *d;
  while ((d = readdir(dp)) != NULL) {
    add_name(d->d_name, 0);
  }

  closedir(dp);
#endif

  sort_names();

  return 0;
}

int slides_open(const char *dir) {
  if (S.open && nl_strcmp(S.dir, dir) == 0) {
    return S.count;
  }

  if (nl_strlen(dir) >= DIR_LEN) {
    return -1;
  }

  slides_close();
  if (read_dir(dir) != 0) {
    return -1;
  }

  nl_strncpy_safe(S.dir, dir, DIR_LEN);
  S.start_ms = -1;
  S.open = 1;
  __atomic_store_n(&S.changed, 0, __ATOMIC_RELEASE);

  return S.count;
}

void slides_close(void) { nl_memset(&S, 0, sizeof(S)); }

int slides_active(void) { return S.open && S.count > 0; }

int slides_count(void) { return S.count; }

int slides_index(void) { return __atomic_load_n(&S.cur, __ATOMIC_ACQUIRE); }

const char *slides_name(int i) {
  return i >= 0 && i < S.count ? S.name[i] : NULL;
}

int slides_path(char *out, size_t cap) {
  if (!slides_active()) {
    return -1;
  }

  size_t dl = nl_strlen(S.dir);
  const char *sep = dl > 0 && S.dir[dl - 1] == '/' ? "" : "/";
  const char *name = S.name[slides_index()];
  if (dl + nl_strlen(sep) + nl_strlen(name) >= cap) {
    return -1;
  }

  nl_snprintf(out, cap, "%s%s%s", S.dir, sep, name);

  return 0;
}

void slides_set_secs(int secs) { g_secs = secs < 0 ? 0 : secs; }

void slides_request(int delta) {
  __atomic_fetch_add(&S.req, delta, __ATOMIC_ACQ_REL);
}

// Move by delta over pictures that are not bad. 1 when it moved
static int step(int delta, long now_ms) {
  int n = S.count;
  int dir = delta < 0 ? -1 : 1;
  int left = delta < 0 ? -delta : delta;
  int at = S.cur;
  int moved = 0;
  while (left > 0) {
    int tries = 0;
    int next = at;
    do {
      next = ((next + dir) % n + n) % n;
      tries++;
    } while (S.bad[next] && tries < n);

    if (S.bad[next]) {
      break;
    }

    at = next;
    moved = 1;
    left--;
  }

  S.start_ms = now_ms;
  if (!moved || at == S.cur) {
    return 0;
  }

  __atomic_store_n(&S.cur, at, __ATOMIC_RELEASE);
  __atomic_store_n(&S.changed, 1, __ATOMIC_RELEASE);

  return 1;
}

int slides_tick(long now_ms) {
  if (!slides_active()) {
    return 0;
  }

  if (S.start_ms < 0) {
    S.start_ms = now_ms;
  }

  int d = __atomic_exchange_n(&S.req, 0, __ATOMIC_ACQ_REL);
  if (d != 0) {
    return step(d % S.count, now_ms);
  }

  if (g_secs > 0 && now_ms - S.start_ms >= (long)g_secs * 1000L) {
    return step(1, now_ms);
  }

  return 0;
}

int slides_mark_bad(long now_ms) {
  if (!slides_active()) {
    return 0;
  }

  S.bad[S.cur] = 1;

  return step(1, now_ms);
}

int slides_take_changed(void) {
  return __atomic_exchange_n(&S.changed, 0, __ATOMIC_ACQ_REL);
}
