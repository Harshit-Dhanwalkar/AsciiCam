/*
 * tests/test_netview.c - src/netview.c: address parsing, and over loopback
 * sockets: accept, hello, broadcast, a slow viewer that misses frames without
 * tearing any, a viewer that leaves, viewer limit
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_netview.c src/netview.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c lib/nl_mem.c lib/nl_gc.c \
 *     -Iinclude -Ilib -o /tmp/tn
 */

#include "../lib/nolibc.h"
#include "netview.h"
#include "platform.h"

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

#define SYS_recvfrom_t 45

// Bytes waiting on fd (up to n), 0 when none, -1 when closed
static long rd(int fd, uint8_t *buf, size_t n) {
  for (int tries = 0; tries < 50; tries++) {
    long r = __sc6(SYS_recvfrom_t, fd, (long)buf, (long)n, 0x40, 0, 0);
    if (r > 0) {
      return r;
    }

    if (r == 0) {
      return -1;
    }

    nl_usleep(2000);
  }

  return 0;
}

static int same7(const uint8_t *b) {
  static const char w[] = "FRAME-1";
  for (int i = 0; i < 7; i++) {
    if (b[i] != (uint8_t)w[i]) {
      return 0;
    }
  }

  return 1;
}

static int parse(const char *s, uint32_t *ip, int *port) {
  return nv_parse_addr(s, ip, port);
}

int main(void) {
  uint32_t ip = 0;
  int port = -1;
  CHECK(parse("7000", &ip, &port) == 0 && ip == 0x7f000001u && port == 7000,
        "bare port is loopback");
  CHECK(parse(":7000", &ip, &port) == 0 && ip == 0x7f000001u && port == 7000,
        ":port");
  CHECK(parse("localhost:81", &ip, &port) == 0 && ip == 0x7f000001u &&
            port == 81,
        "localhost");
  CHECK(parse("10.1.2.3:65535", &ip, &port) == 0 && ip == 0x0a010203u &&
            port == 65535,
        "dotted quad");
  CHECK(parse("0.0.0.0:9", &ip, &port) == 0 && ip == 0 && port == 9, "any");
  CHECK(parse("*:9", &ip, &port) == 0 && ip == 0 && port == 9, "star is any");
  CHECK(parse("127.0.0.1:0", &ip, &port) == 0 && port == 0, "port 0 parses");
  const char *bad[] = {"",
                       "x",
                       "70000",
                       "65536",
                       ":65536",
                       "127.0.0.1:65536",
                       "7000x",
                       ":",
                       "1.2.3:5",
                       "1.2.3.4.5:5",
                       "256.1.1.1:5",
                       "1..3.4:5",
                       "1.2.3.4",
                       "a.b.c.d:5",
                       "localhostx:5",
                       "1.2.3.4:",
                       "1.2.3.4:-1",
                       "1111.1.1.1:5",
                       " 1.2.3.4:5",
                       "::1:5"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    CHECK(parse(bad[i], &ip, &port) != 0, "refused: \"%s\"", bad[i]);
  }

  CHECK(nv_parse_addr(NULL, &ip, &port) != 0, "NULL refused");
  CHECK(nv_connect("nonsense") == -1 && nv_connect("127.0.0.1:0") == -1,
        "connect: bad address");

  nv_server_t srv;
  if (nv_listen(&srv, 0x7f000001u, 0) != 0) {
    fprintf(stderr, "no loopback sockets here: network tests skipped\n");
    fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

    return fail_n ? 1 : 0;
  }

  CHECK(srv.port > 0 && srv.nclients == 0, "listening on port %d", srv.port);
  char addr[32];
  nl_snprintf(addr, sizeof(addr), "127.0.0.1:%d", srv.port);
  CHECK(nv_accept(&srv) == 0, "nobody there yet");
  CHECK(nv_broadcast(&srv, (const uint8_t *)"x", 1) == 0,
        "broadcast to nobody is fine");

  uint8_t buf[1 << 16];
  int a = nv_connect(addr);
  CHECK(a >= 0, "viewer A connects");
  nl_usleep(5000);
  CHECK(nv_accept(&srv) == 1 && srv.nclients == 1, "server takes it");
  long n = rd(a, buf, sizeof(buf));
  CHECK(n > 0 && buf[n - 1] == 'J', "hello clears the screen (%ld bytes)", n);
  CHECK(nv_broadcast(&srv, (const uint8_t *)"FRAME-1", 7) == 1, "one viewer");
  n = rd(a, buf, sizeof(buf));
  CHECK(n == 7 && same7(buf), "frame arrives whole");

  int b = nv_connect(addr);
  nl_usleep(5000);
  CHECK(nv_accept(&srv) == 1 && srv.nclients == 2, "viewer B joins");
  rd(b, buf, sizeof(buf)); // hello
  CHECK(nv_broadcast(&srv, (const uint8_t *)"FRAME-2", 7) == 2, "two viewers");
  n = rd(a, buf, sizeof(buf));
  CHECK(n == 7 && buf[6] == '2', "A got 2");
  n = rd(b, buf, sizeof(buf));
  CHECK(n == 7 && buf[6] == '2', "B got 2");

  // B never reads: big frames fill its queue. A keeps up. Frames are
  // "<F" seq len(4 bytes) payload; none may be torn or reordered on B
  enum { BIG = 300000 };
  uint8_t *frame = nl_malloc(BIG + 8);
  uint8_t *chunk = nl_malloc(1 << 20);
  CHECK(frame && chunk, "memory");
  int sent = 0;
  static uint8_t *all;
  size_t all_n = 0;
  all = nl_malloc((size_t)62 * (BIG + 8));
  int a_ok = 1;
  long a_total = 0;
  for (int f = 0; f < 60; f++) {
    frame[0] = '<';
    frame[1] = 'F';
    frame[2] = (uint8_t)f;
    frame[3] = 0;
    nl_memset(frame + 4, f + 1, BIG);
    nv_broadcast(&srv, frame, BIG + 4);
    sent++;
    long got;
    while ((got = rd(a, chunk, 1 << 20)) > 0) {
      a_total += got;
    }

    a_ok = a_ok && got >= 0;

    // B reads a little now and then: its queue drains in part, so a frame that
    // was half sent has to be completed before next one starts
    if (f % 3 == 2) {
      long bg = rd(b, chunk, 150000);
      if (bg > 0) {
        nl_memcpy(all + all_n, chunk, (size_t)bg);
        all_n += (size_t)bg;
      }
    }
  }

  CHECK(a_ok && a_total > 0 && srv.nclients == 2, "A is not held up by B");
  CHECK(srv.dropped > 0, "B missed frames (%llu dropped)",
        (unsigned long long)srv.dropped);
  CHECK(a_total == (long)sent * (BIG + 4) || a_total > (long)(BIG + 4) * 10,
        "A got most frames (%ld bytes)", a_total);

  // Now B reads everything there is: frames are whole, in order
  long got;
  while ((got = rd(b, chunk, 1 << 20)) > 0) {
    nl_memcpy(all + all_n, chunk, (size_t)got);
    all_n += (size_t)got;
  }

  size_t pos = 0;
  int whole = 0;
  int ordered = 1;
  int last = -1;
  int sound = 1;
  // skip B's FRAME-2 (7 bytes) already read above, so stream starts at a frame
  while (pos + 4 <= all_n && all[pos] == '<' && all[pos + 1] == 'F') {
    int seq = all[pos + 2];
    if (pos + 4 + BIG > all_n) {
      break; // tail still queued on server side
    }

    for (size_t i = 0; i < BIG; i++) {
      if (all[pos + 4 + i] != (uint8_t)(seq + 1)) {
        sound = 0;
        break;
      }
    }

    ordered = ordered && seq > last;
    last = seq;
    whole++;
    pos += 4 + BIG;
  }

  CHECK(whole >= 1 && sound && ordered,
        "B: %d whole frames, in order, none torn", whole);
  CHECK(pos == all_n || (pos < all_n && all_n - pos < (size_t)BIG + 4),
        "B: nothing but a partial tail follows (%zu left)", all_n - pos);
  CHECK(whole < sent, "B saw fewer frames than were sent (%d of %d)", whole,
        sent);

  // A leaves
  close(a);
  int gone = 0;
  for (int i = 0; i < 20 && !gone; i++) {
    nv_broadcast(&srv, (const uint8_t *)"z", 1);
    nl_usleep(2000);
    gone = srv.nclients == 1;
  }

  CHECK(gone, "a viewer that left is dropped");
  close(b);

  // Limit
  int c[NV_MAX_CLIENTS + 2];
  int made = 0;
  for (int i = 0; i < NV_MAX_CLIENTS + 2; i++) {
    c[i] = nv_connect(addr);
    made += c[i] >= 0;
  }

  nl_usleep(5000);
  nv_accept(&srv);
  CHECK(made == NV_MAX_CLIENTS + 2 && srv.nclients <= NV_MAX_CLIENTS,
        "at most %d viewers (%d)", NV_MAX_CLIENTS, srv.nclients);
  int closed = 0;
  for (int i = 0; i < NV_MAX_CLIENTS + 2; i++) {
    uint8_t t[64];
    long r;
    while ((r = rd(c[i], t, sizeof(t))) > 0) {
    }

    closed += r < 0;
  }

  CHECK(closed >= 2, "the extra viewers are turned away (%d)", closed);
  for (int i = 0; i < NV_MAX_CLIENTS + 2; i++) {
    close(c[i]);
  }

  nv_close(&srv);
  CHECK(srv.lfd == -1 && srv.nclients == 0, "closed");
  nv_close(&srv);
  CHECK(nv_connect(addr) == -1, "nobody listens any more");

  // key and delta frames: new or late viewers get key first, rest follow
  // deltas, a viewer that missed a frame is brought back with a key
  {
    nv_server_t k;
    CHECK(nv_listen(&k, 0x7f000001u, 0) == 0, "second server listens");
    char kaddr[32];
    nl_snprintf(kaddr, sizeof(kaddr), "127.0.0.1:%d", k.port);
    uint8_t t[64];
    int v1 = nv_connect(kaddr);
    nl_usleep(5000);
    nv_accept(&k);
    rd(v1, t, sizeof(t)); // hello
    CHECK(nv_wants_key(&k) == 1, "a new viewer wants a key");
    nv_broadcast2(&k, (const uint8_t *)"D1", 2, (const uint8_t *)"K1", 2);
    long r = rd(v1, t, sizeof(t));
    CHECK(r == 2 && t[0] == 'K' && nv_wants_key(&k) == 0,
          "it gets the key, then wants none");
    nv_broadcast2(&k, (const uint8_t *)"D2", 2, (const uint8_t *)"K2", 2);
    r = rd(v1, t, sizeof(t));
    CHECK(r == 2 && t[0] == 'D', "then it follows deltas");

    int v2 = nv_connect(kaddr);
    nl_usleep(5000);
    nv_accept(&k);
    rd(v2, t, sizeof(t));
    CHECK(nv_wants_key(&k) == 1, "a late viewer wants a key");
    nv_broadcast2(&k, (const uint8_t *)"D3", 2, (const uint8_t *)"K3", 2);
    r = rd(v1, t, sizeof(t));
    CHECK(r == 2 && t[0] == 'D' && t[1] == '3', "the old one still gets D");
    r = rd(v2, t, sizeof(t));
    CHECK(r == 2 && t[0] == 'K' && t[1] == '3', "the late one gets K");
    nv_want_key_all(&k);
    CHECK(nv_wants_key(&k) == 2, "everybody can be asked for a key");
    nv_broadcast2(&k, (const uint8_t *)"D4", 2, (const uint8_t *)"K4", 2);
    r = rd(v1, t, sizeof(t));
    CHECK(r == 2 && t[0] == 'K', "and gets one");
    rd(v2, t, sizeof(t));

    // v2 stops reading: it misses frames and must get a key afterwards
    for (int f = 0; f < 40 && nv_wants_key(&k) == 0; f++) {
      nv_broadcast2(&k, frame, BIG, (const uint8_t *)"KEY!", 4);
      while (rd(v1, chunk, 1 << 20) > 0) {
      }
    }

    CHECK(nv_wants_key(&k) >= 1, "a viewer that fell behind wants a key");
    long drained;
    while ((drained = rd(v2, chunk, 1 << 20)) > 0) {
    }

    int back = 0;
    for (int f = 0; f < 40 && !back; f++) {
      nv_broadcast2(&k, (const uint8_t *)"dd", 2, (const uint8_t *)"KEY!", 4);
      back = nv_wants_key(&k) == 0;
      nl_usleep(1000);
    }

    long tail = rd(v2, chunk, 1 << 20);
    long more;
    while ((more = rd(v2, chunk + (tail > 0 ? tail : 0), 1000)) > 0) {
      tail += more;
    }

    CHECK(back && tail >= 4 && chunk[tail - 4] == 'K' && chunk[tail - 1] == '!',
          "it is brought back with a key (%ld bytes)", tail);
    close(v1);
    close(v2);
    nv_close(&k);
  }

  nl_free(frame);
  nl_free(chunk);
  nl_free(all);

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
