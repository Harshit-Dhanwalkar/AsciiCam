/*
 * tests/test_call.c - message framing (src/callproto.c), room relay
 * (src/room.c) and call client (src/call.c) together, over loopback sockets
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_call.c src/call.c src/room.c \
 *     src/callproto.c src/cellcodec.c src/netenc.c src/netview.c lib/nl_*.c \
 *     -Iinclude -Ilib -o /tmp/tcall
 */

#include "../lib/nolibc.h"
#include "call.h"
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

static uint32_t rng = 88172645u;

static unsigned rnd(unsigned n) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;

  return rng % n;
}

static long now_ms = 1000;

// Run room and given clients for some rounds
static void spin(room_t *room, call_t **cl, int n, int rounds) {
  for (int r = 0; r < rounds; r++) {
    now_ms += 20;
    room_step(room, 1, now_ms);
    for (int i = 0; i < n; i++) {
      if (cl[i]->fd >= 0 && !cl[i]->closed) {
        call_pump(cl[i]);
      }
    }
  }
}

// A picture as a renderer writes it
typedef struct {
  int cols, rows;
  char glyph[40 * 20];
  uint8_t colour[40 * 20][3];
  int use_colour;
} pic_t;

static void pic_random(pic_t *p, int cols, int rows, int use_colour) {
  p->cols = cols;
  p->rows = rows;
  p->use_colour = use_colour;
  for (int i = 0; i < cols * rows; i++) {
    p->glyph[i] = (char)(' ' + 1 + rnd(90));
    for (int k = 0; k < 3; k++) {
      p->colour[i][k] = (uint8_t)rnd(256);
    }
  }
}

static void pic_change(pic_t *p, int k) {
  for (int j = 0; j < k; j++) {
    int i = (int)rnd((unsigned)(p->cols * p->rows));
    p->glyph[i] = (char)(' ' + 1 + rnd(90));
    for (int c = 0; c < 3; c++) {
      p->colour[i][c] = (uint8_t)rnd(256);
    }
  }
}

static size_t pic_ansi(const pic_t *p, char *out, size_t cap) {
  size_t n = 0;
  for (int r = 0; r < p->rows; r++) {
    n += (size_t)nl_snprintf(out + n, cap - n, "\033[%d;1H", r + 1);
    for (int c = 0; c < p->cols; c++) {
      int i = r * p->cols + c;
      if (p->use_colour) {
        n += (size_t)nl_snprintf(out + n, cap - n, "\033[38;2;%d;%d;%dm",
                                 p->colour[i][0], p->colour[i][1],
                                 p->colour[i][2]);
      }

      out[n++] = p->glyph[i];
    }
  }

  n += (size_t)nl_snprintf(out + n, cap - n, "\033[0m");

  return n;
}

static int grids_equal(const cc_grid_t *a, const cc_grid_t *b) {
  if (!a->cell || !b->cell || a->cols != b->cols || a->rows != b->rows) {
    return 0;
  }

  for (int i = 0; i < a->cols * a->rows; i++) {
    if (a->cell[i].glyph != b->cell[i].glyph ||
        a->cell[i].fg != b->cell[i].fg || a->cell[i].bg != b->cell[i].bg) {
      return 0;
    }
  }

  return 1;
}

static int has_line(const call_t *c, const char *want) {
  for (int b = 0; b < CALL_LOG_LINES; b++) {
    const char *l = call_log_line(c, b);
    if (!l) {
      return 0;
    }

    if (nl_strcmp(l, want) == 0) {
      return 1;
    }
  }

  return 0;
}

static int raw_dead(int fd) {
  uint8_t b[256];
  for (int i = 0; i < 20; i++) {
    long r = nv_recv(fd, b, sizeof(b));
    if (r == 0 || (r < 0 && r != -11)) {
      return 1;
    }

    nl_usleep(1000);
  }

  return 0;
}

static char abuf[1 << 16];

// Index of needle in hay, -1 when absent
static int nl_strstr_pos(const char *hay, const char *needle) {
  size_t nn = nl_strlen(needle);
  size_t hn = nl_strlen(hay);
  for (size_t i = 0; i + nn <= hn; i++) {
    size_t k = 0;
    while (k < nn && hay[i + k] == needle[k]) {
      k++;
    }

    if (k == nn) {
      return (int)i;
    }
  }

  return -1;
}

int main(void) {
  // Framing
  msgbuf_t mb;
  CHECK(mb_init(&mb) == 0, "msgbuf");
  int type = 0;
  const uint8_t *p = NULL;
  size_t len = 0;
  CHECK(mb_next(&mb, &type, &p, &len) == 0, "empty: nothing");
  uint8_t m1[] = {5, 0, 0, 3, 'a', 'b', 'c', 7, 0, 0, 0, 9, 0, 0, 1, 'z'};
  mb_feed(&mb, m1, 6); // header and 2 of 3 payload bytes
  CHECK(mb_next(&mb, &type, &p, &len) == 0, "partial: wait");
  mb_feed(&mb, m1 + 6, 10);
  CHECK(mb_next(&mb, &type, &p, &len) == 1 && type == 5 && len == 3 &&
            p[0] == 'a' && p[2] == 'c',
        "first message whole");
  mb_pop(&mb);
  CHECK(mb_next(&mb, &type, &p, &len) == 1 && type == 7 && len == 0,
        "second: no payload");
  mb_pop(&mb);
  CHECK(mb_next(&mb, &type, &p, &len) == 1 && type == 9 && len == 1 &&
            p[0] == 'z',
        "third");
  mb_pop(&mb);
  CHECK(mb.n == 0 && mb_next(&mb, &type, &p, &len) == 0, "drained");
  uint8_t zero[] = {0, 0, 0, 0};
  mb_feed(&mb, zero, 4);
  CHECK(mb_next(&mb, &type, &p, &len) == -1, "type 0 is invalid");
  mb.n = 0;
  uint8_t big[] = {2, 0xFF, 0xFF, 0xFF};
  mb_feed(&mb, big, 4);
  CHECK(mb_next(&mb, &type, &p, &len) == -1, "length beyond limit is invalid");
  mb.n = 0;
  uint8_t edge[] = {2, 0, (CALL_MAX_PAYLOAD + 1) / 256,
                    (CALL_MAX_PAYLOAD + 1) % 256};
  mb_feed(&mb, edge, 4);
  CHECK(mb_next(&mb, &type, &p, &len) == -1, "one over the limit is invalid");
  mb_free(&mb);

  outq_t q;
  oq_init(&q);
  CHECK(oq_put(&q, 0, NULL, 0, NULL, 0) == -1 &&
            oq_put(&q, 300, NULL, 0, NULL, 0) == -1,
        "bad message type refused");
  CHECK(oq_put(&q, 3, "ab", 2, "cde", 3) == 0 &&
            oq_pending(&q) == CALL_HDR + 5 && q.buf[0] == 3 && q.buf[3] == 5 &&
            q.buf[4] == 'a' && q.buf[6] == 'c',
        "head and body joined");
  size_t fill = 0;
  static uint8_t blob[10000];
  nl_memset(blob, 1, sizeof(blob));
  while (oq_put(&q, 4, NULL, 0, blob, sizeof(blob)) == 0) {
    fill++;
  }

  CHECK(fill >= 5 && fill <= 6, "queue takes about its size (%zu)", fill);
  CHECK(oq_put(&q, 4, NULL, 0, blob, CALL_MAX_PAYLOAD + 1) == -1,
        "oversized message refused");
  oq_free(&q);

  char clean[64];
  CHECK(call_clean_text(clean, (const uint8_t *)"  hi\x01\xC3\xA9!  ", 9, 20) ==
                6 &&
            nl_strcmp(clean, "hi??"
                             "?!") == 0,
        "text cleaned: %s", clean);
  CHECK(call_clean_text(clean, (const uint8_t *)"abcdefghij", 10, 4) == 4 &&
            nl_strcmp(clean, "abcd") == 0,
        "text clipped");
  CHECK(call_clean_text(clean, (const uint8_t *)"   ", 3, 9) == 0 &&
            clean[0] == 0,
        "blank text is empty");

  // Room
  room_t room;
  if (room_listen(&room, 0x7f000001u, 0) != 0) {
    fprintf(stderr, "no loopback sockets here: call tests skipped\n");
    fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

    return fail_n ? 1 : 0;
  }

  char addr[32];
  nl_snprintf(addr, sizeof(addr), "127.0.0.1:%d", room.port);
  static call_t ca, cb, cc;
  call_t *all[3] = {&ca, &cb, &cc};

  CHECK(call_open(&ca, addr, "Alice") == 0, "Alice connects");
  call_send_picture(&ca, "x", 1);
  CHECK(call_send_picture(&ca, "\033[1;1Hab", 8) == 1,
        "before WELCOME: nothing sent");
  spin(&room, all, 1, 10);
  CHECK(ca.id >= 0 && room_members(&room) == 1 && call_members(&ca) == 1,
        "Alice is in (id %d)", ca.id);
  CHECK(call_open(&cb, addr, "Bob\x07") == 0, "Bob connects");
  spin(&room, all, 2, 15);
  CHECK(cb.id >= 0 && cb.id != ca.id && room_members(&room) == 2, "Bob is in");
  int bob_id = cb.id;
  CHECK(ca.peer[bob_id].present && nl_strcmp(ca.peer[bob_id].name, "Bob?") == 0,
        "Alice sees Bob, name cleaned");
  CHECK(cb.peer[ca.id].present && nl_strcmp(cb.peer[ca.id].name, "Alice") == 0,
        "Bob sees Alice");
  CHECK(has_line(&ca, "* Bob? joined"), "Alice's log notes it");
  CHECK(call_members(&ca) == 2 && call_members(&cb) == 2, "two in the call");

  // pictures, keys and deltas
  pic_t pic;
  pic_random(&pic, 24, 8, 1);
  int ok_chain = 1;
  int frames = 0;
  for (int f = 0; f < 120; f++) {
    size_t an = pic_ansi(&pic, abuf, sizeof(abuf));
    int rc = call_send_picture(&ca, abuf, an);
    frames += rc == 0;
    spin(&room, all, 2, 3);
    ok_chain = ok_chain && grids_equal(&ca.local, &cb.peer[ca.id].grid);
    pic_change(&pic, (int)rnd(f % 20 == 0 ? 100 : 6));
  }

  CHECK(frames == 120 && ok_chain,
        "120 frames: Bob always holds Alice's picture");
  CHECK(room.frames_in >= 120 && ca.frames_sent == 120, "frames counted");
  CHECK(cb.frames_received >= 100, "Bob received them (%llu)",
        (unsigned long long)cb.frames_received);
  CHECK(ca.frames_sent > 0 && ca.since_key < CALL_KEY_EVERY,
        "a key every %d frames", CALL_KEY_EVERY);

  // Bob's picture comes back other way, mono this time
  pic_t pb;
  pic_random(&pb, 16, 6, 0);
  size_t bn = pic_ansi(&pb, abuf, sizeof(abuf));
  CHECK(call_send_picture(&cb, abuf, bn) == 0, "Bob sends");
  spin(&room, all, 2, 10);
  CHECK(grids_equal(&cb.local, &ca.peer[cb.id].grid),
        "Alice holds Bob's picture");
  CHECK(call_send_picture(&cb, "\033[2J", 4) == -1,
        "unrenderable picture refused");

  // Chat
  CHECK(call_say(&ca, "hello Bob") == 0 && call_say(&ca, "   ") == -1,
        "chat queued, blank refused");
  call_say(&cb, "hi \x1b[31mred");
  spin(&room, all, 2, 10);
  CHECK(has_line(&ca, "you: hello Bob") && has_line(&cb, "Alice: hello Bob"),
        "chat reaches both, in own words");
  CHECK(has_line(&cb, "you: hi ?[31mred") && has_line(&ca, "Bob?: hi ?[31mred"),
        "escape characters cannot get through");
  char longtext[400];
  nl_memset(longtext, 'x', 399);
  longtext[399] = 0;
  call_say(&ca, longtext);
  spin(&room, all, 2, 10);
  const char *l = call_log_line(&cb, 0);
  CHECK(l && nl_strlen(l) == nl_strlen("Alice: ") + CALL_CHAT_MAX,
        "long text cut to %d", CALL_CHAT_MAX);
  int before = cb.log_count;
  for (int i = 0; i < 30; i++) {
    call_say(&ca, "flood");
  }

  spin(&room, all, 2, 20);
  CHECK(cb.log_count - before <= 6 && cb.log_count - before >= 1,
        "chat flood is limited (%d lines)", cb.log_count - before);

  // Late joiner gets a picture from next key on
  CHECK(call_open(&cc, addr, "Cy") == 0, "Cy connects");
  uint64_t before_frames = cc.frames_received;
  int c_ok = 0;
  for (int f = 0; f < 100 && !c_ok; f++) {
    size_t an = pic_ansi(&pic, abuf, sizeof(abuf));
    call_send_picture(&ca, abuf, an);
    spin(&room, all, 3, 3);
    pic_change(&pic, 5);
    c_ok = cc.peer[ca.id].have_picture &&
           grids_equal(&ca.local, &cc.peer[ca.id].grid);
  }

  CHECK(c_ok && cc.frames_received > before_frames,
        "late joiner got Alice's picture (%llu frames)",
        (unsigned long long)cc.frames_received);
  CHECK(call_members(&cc) == 3 && room_members(&room) == 3,
        "three in the room");

  // A receiver without a key of sender gets no deltas
  call_t *three[3] = {&ca, &cb, &cc};
  spin(&room, three, 3, 10);
  room.cl[cc.id].synced[ca.id] = 0;
  uint64_t gate_dropped = room.frames_dropped;
  uint64_t gate_cy = cc.frames_received;
  uint64_t gate_bob = cb.frames_received;
  pic_change(&pic, 3);
  {
    size_t an = pic_ansi(&pic, abuf, sizeof(abuf));
    call_send_picture(&ca, abuf, an);
  }
  spin(&room, three, 3, 4);
  CHECK(room.frames_dropped == gate_dropped + 1 &&
            cc.frames_received == gate_cy,
        "no delta for a receiver without a key (%llu dropped)",
        (unsigned long long)(room.frames_dropped - gate_dropped));
  CHECK(cb.frames_received == gate_bob + 1, "others still get it");

  // Compose
  static char scr[1 << 17];
  size_t sn = call_compose(&cc, NULL, 100, 40, scr, sizeof(scr));
  CHECK(sn > 0 && sn <= call_max_compose(&cc, 100, 40),
        "screen fits its bound (%zu)", sn);
  scr[sn] = 0;
  CHECK(nl_strstr_pos(scr, "Alice") >= 0 && nl_strstr_pos(scr, "Bob") >= 0 &&
            nl_strstr_pos(scr, "you") >= 0 && nl_strstr_pos(scr, "3 here") >= 0,
        "names and count on screen");
  CHECK(nl_strstr_pos(scr, "\033[2J") >= 0, "first compose clears");
  sn = call_compose(&cc, "typing now", 100, 40, scr, sizeof(scr));
  scr[sn] = 0;
  CHECK(sn > 0 && nl_strstr_pos(scr, "\033[2J") < 0 &&
            nl_strstr_pos(scr, "> typing now_") >= 0,
        "same layout: no clear; typed text shown");
  sn = call_compose(&cc, NULL, 70, 30, scr, sizeof(scr));
  scr[sn] = 0;
  CHECK(nl_strstr_pos(scr, "\033[2J") >= 0, "new terminal size: cleared again");

  // Title: traffic and camera state
  cc.rate_out = 12345;
  cc.rate_in = 987;
  cc.video_off = 1;
  sn = call_compose(&cc, NULL, 100, 40, scr, sizeof(scr));
  scr[sn] = 0;
  CHECK(nl_strstr_pos(scr, "up 12.3 down 0.9 kB/s") >= 0 &&
            nl_strstr_pos(scr, "^V cam on") >= 0,
        "title shows rates and the camera switch");
  cc.video_off = 0;
  sn = call_compose(&cc, NULL, 100, 40, scr, sizeof(scr));
  scr[sn] = 0;
  CHECK(nl_strstr_pos(scr, "^V cam off") >= 0, "title offers camera off");
  CHECK(cc.out.total > 0 && cc.bytes_in > 0,
        "byte counters moved (%llu out, %llu in)",
        (unsigned long long)cc.out.total, (unsigned long long)cc.bytes_in);

  // Card shown in place of the camera
  static char card[8192];
  size_t cn = call_off_card(card, sizeof(card), 20, 6);
  cc_grid_t cg = {0};
  CHECK(cn > 0 && cc_grid_from_ansi(&cg, card, cn) == 0 && cg.cols == 20 &&
            cg.rows == 6,
        "camera-off card is a 20x6 picture");
  int mid_ok = 1;
  for (int i = 0; i < 10; i++) {
    mid_ok = mid_ok && cg.cell[3 * 20 + 5 + i].glyph == "camera off"[i];
  }

  CHECK(mid_ok, "card text sits centred on the middle row");
  int outside = 0;
  for (int i = 0; i < 20 * 6; i++) {
    int in_text = i >= 3 * 20 + 5 && i < 3 * 20 + 15;
    outside += !in_text && cg.cell[i].glyph != ' ';
  }

  CHECK(outside == 0, "card is blank except the text");
  cn = call_off_card(card, sizeof(card), 6, 1);
  cc_grid_free(&cg);
  CHECK(cn > 0 && cc_grid_from_ansi(&cg, card, cn) == 0 && cg.cols == 6,
        "card narrower than its text is clipped, not rejected");
  CHECK(call_off_card(card, 50, 20, 6) == 0, "card: buffer too small");
  CHECK(call_off_card(card, sizeof(card), 0, 6) == 0 &&
            call_off_card(card, sizeof(card), CC_MAX_COLS + 1, 6) == 0 &&
            call_off_card(card, sizeof(card), 20, CC_MAX_ROWS + 1) == 0,
        "card: bad size");
  cc_grid_free(&cg);

  CHECK(call_compose(&cc, NULL, 10, 3, scr, sizeof(scr)) == 0,
        "terminal too small: nothing");
  CHECK(call_compose(&cc, NULL, 100, 40, scr, 50) == 0, "buffer too small: 0");
  sn = call_compose(&ca, NULL, 100, 40, scr, sizeof(scr));
  scr[sn] = 0;
  CHECK(nl_strstr_pos(scr, "you: flood") >= 0, "chat on screen");

  // Leave
  call_close(&cb);
  spin(&room, all, 1, 5);
  call_t *rest[2] = {&ca, &cc};
  spin(&room, rest, 2, 20);
  CHECK(room_members(&room) == 2 && !ca.peer[bob_id].present &&
            has_line(&ca, "* Bob? left"),
        "Bob left: two remain, and it is in the log");
  CHECK(call_members(&ca) == 2 && call_members(&cc) == 2, "the others see it");

  // Room full
  static call_t extra[ROOM_MAX + 1];
  int connected = 0;
  for (int i = 0; i < ROOM_MAX + 1; i++) {
    char nm[8];
    nl_snprintf(nm, sizeof(nm), "e%d", i);
    connected += call_open(&extra[i], addr, nm) == 0;
  }

  CHECK(connected == ROOM_MAX + 1, "TCP connects for all (%d)", connected);
  for (int r = 0; r < 40; r++) {
    now_ms += 20;
    room_step(&room, 1, now_ms);
    for (int i = 0; i < ROOM_MAX + 1; i++) {
      if (extra[i].fd >= 0 && !extra[i].closed) {
        call_pump(&extra[i]);
      }
    }
  }

  int fulls = 0;
  for (int i = 0; i < ROOM_MAX + 1; i++) {
    fulls += extra[i].full;
  }

  CHECK(fulls >= 1 && room_members(&room) <= ROOM_MAX,
        "a full room says so (%d)", fulls);
  for (int i = 0; i < ROOM_MAX + 1; i++) {
    call_close(&extra[i]);
  }

  for (int r = 0; r < 20; r++) {
    now_ms += 20;
    room_step(&room, 1, now_ms);
  }

  // hostile clients
  struct {
    const char *what;
    uint8_t bytes[64];
    size_t n;
  } bad[] = {
      {"type 0", {0, 0, 0, 0}, 4},
      {"frame before hello", {CALL_FRAME, 0, 0, 6, 1, 0, 1, 0, 1, 'x'}, 10},
      {"chat before hello", {CALL_CHAT, 0, 0, 1, 'x'}, 5},
      {"wrong version", {CALL_HELLO, 0, 0, 3, 9, 'a', 'b'}, 7},
      {"empty hello", {CALL_HELLO, 0, 0, 0}, 4},
      {"huge length", {CALL_HELLO, 0xFF, 0xFF, 0xFF}, 4},
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    int fd = nv_connect(addr);
    nv_nonblock(fd);
    nv_send(fd, bad[i].bytes, bad[i].n);
    int dead = 0;
    for (int r = 0; r < 30 && !dead; r++) {
      now_ms += 20;
      room_step(&room, 1, now_ms);
      dead = raw_dead(fd);
    }

    CHECK(dead, "dropped: %s", bad[i].what);
    close(fd);
  }

  {
    // Joined, then sends things only relay may send, or a broken frame
    uint8_t hello[] = {CALL_HELLO, 0, 0, 3, CALL_VERSION, 'q', 'q'};
    uint8_t pframe[] = {CALL_PFRAME, 0, 0, 2, 0, 1};
    uint8_t broken[] = {CALL_FRAME, 0, 0, 6, 0xF0, 0, 0, 0, 0, 0};
    uint8_t *tests[2] = {pframe, broken};
    size_t sizes[2] = {sizeof(pframe), sizeof(broken)};
    const char *names[2] = {"relay-only message from a client",
                            "frame with a bad header"};
    for (int t = 0; t < 2; t++) {
      int fd = nv_connect(addr);
      nv_nonblock(fd);
      nv_send(fd, hello, sizeof(hello));
      for (int r = 0; r < 10; r++) {
        now_ms += 20;
        room_step(&room, 1, now_ms);
      }

      int members = room_members(&room);
      nv_send(fd, tests[t], sizes[t]);
      int dead = 0;
      for (int r = 0; r < 30 && !dead; r++) {
        now_ms += 20;
        room_step(&room, 1, now_ms);
        dead = raw_dead(fd);
      }

      CHECK(members >= 3 && dead, "dropped: %s", names[t]);
      close(fd);
      for (int r = 0; r < 10; r++) {
        now_ms += 20;
        room_step(&room, 1, now_ms);
      }
    }
  }

  {
    int fd = nv_connect(addr);
    nv_nonblock(fd);
    int dead = 0;
    for (int r = 0; r < 700 && !dead; r++) {
      now_ms += 20; // 14 s with no hello
      room_step(&room, 0, now_ms);
      if (r % 10 == 0) {
        dead = raw_dead(fd);
      }
    }

    CHECK(dead, "no hello within 10 s: dropped");
    close(fd);
  }

  CHECK(room_members(&room) == 2, "good clients are still in (%d)",
        room_members(&room));
  // A screen after a small change costs a small update, not a whole screen
  {
    static char scr2[1 << 17];
    cc.sig = 0; // as after a resize: everything is drawn
    size_t first = call_compose(&cc, NULL, 100, 40, scr2, sizeof(scr2));
    pic_change(&pic, 2);
    size_t an = pic_ansi(&pic, abuf, sizeof(abuf));
    call_send_picture(&ca, abuf, an);
    spin(&room, three, 3, 6);
    size_t next = call_compose(&cc, NULL, 100, 40, scr2, sizeof(scr2));
    CHECK(next > 0 && next < first / 2,
          "a small change redraws a few cells (%zu of %zu bytes)", next, first);
    CHECK(call_compose(&cc, "x", 100, 40, scr2, sizeof(scr2)) > 0,
          "typing still draws");
  }

  spin(&room, rest, 2, 10);
  CHECK(call_say(&ca, "still here") == 0, "chat still works");
  spin(&room, rest, 2, 10);
  CHECK(has_line(&cc, "Alice: still here"), "and arrives");

  // slow client: Cy stops reading while Alice sends big colour pictures
  pic_t bigpic;
  pic_random(&bigpic, 39, 19, 1);
  uint64_t dropped_before = room.frames_dropped;
  long asked_before = room.cl[ca.id].need_key_ms;
  int worst_pending = 0;
  for (int f = 0; f < 4000; f++) {
    size_t an = pic_ansi(&bigpic, abuf, sizeof(abuf));
    call_send_picture(&ca, abuf, an);
    pic_change(&bigpic, 300);
    now_ms += 20;
    room_step(&room, 0, now_ms);
    call_pump(&ca); // Cy does not pump
    if ((int)oq_pending(&room.cl[cc.id].out) > worst_pending) {
      worst_pending = (int)oq_pending(&room.cl[cc.id].out);
    }
  }
  CHECK(worst_pending > 0 && worst_pending < CALL_OUTQ / 2 + 20000,
        "pictures give way at half a queue (%d)", worst_pending);
  CHECK(room.cl[ca.id].need_key_ms > asked_before,
        "a dropped picture makes the room ask for a key");

  CHECK(room.frames_dropped > dropped_before,
        "the room dropped frames for the slow one (%llu)",
        (unsigned long long)(room.frames_dropped - dropped_before));
  CHECK(!ca.closed && room_members(&room) >= 1, "Alice was not held up");
  // Cy wakes up: whatever it receives is exact, and it catches up
  int caught = 0;
  for (int f = 0; f < 200 && !caught; f++) {
    size_t an = pic_ansi(&bigpic, abuf, sizeof(abuf));
    call_send_picture(&ca, abuf, an);
    pic_change(&bigpic, 20);
    spin(&room, rest, 2, 4);
    caught = cc.peer[ca.id].have_picture &&
             grids_equal(&ca.local, &cc.peer[ca.id].grid);
  }

  CHECK(caught && !cc.closed, "slow client catches up and is exact");

  // A sender whose link does not drain skips pictures, queue stays small
  uint64_t skipped = ca.frames_skipped;
  int worst_out = 0;
  for (int f = 0; f < 3000; f++) {
    size_t an = pic_ansi(&bigpic, abuf, sizeof(abuf));
    call_send_picture(&ca, abuf, an);
    pic_change(&bigpic, 300);
    if ((int)oq_pending(&ca.out) > worst_out) {
      worst_out = (int)oq_pending(&ca.out);
    }
  }
  CHECK(ca.frames_skipped > skipped && worst_out < CALL_OUTQ / 4 + 20000,
        "a sender on a slow link skips pictures (%d queued)", worst_out);

  call_close(&ca);
  call_close(&cc);
  room_close(&room);
  CHECK(room.lfd == -1, "room closed");

  fprintf(stderr, "%d passed  %d failed\n", pass_n, fail_n);

  return fail_n ? 1 : 0;
}
