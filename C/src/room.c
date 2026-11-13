/*
 * src/room.c - video call relay, see include/room.h
 */

#include "platform.h"

#include "nolibc.h"

#include "cellcodec.h"
#include "netview.h"
#include "room.h"

#define HELLO_TIMEOUT_MS 10000
#define NEED_KEY_GAP_MS 1000
#define MAX_FRAMES_PER_SEC 40
#define MAX_CHATS_PER_SEC 5

static void drop_client(room_t *r, int i);

static room_client_t *at(room_t *r, int i) { return &r->cl[i]; }

int room_listen(room_t *r, uint32_t ip, int port) {
  nl_memset(r, 0, sizeof(*r));
  for (int i = 0; i < ROOM_MAX; i++) {
    r->cl[i].fd = -1;
  }

  nv_server_t s;
  if (nv_listen(&s, ip, port) != 0) {
    r->lfd = -1;

    return -1;
  }

  r->lfd = s.lfd;
  r->port = s.port;

  return 0;
}

int room_members(const room_t *r) {
  int n = 0;
  for (int i = 0; i < ROOM_MAX; i++) {
    n += r->cl[i].fd >= 0 && r->cl[i].joined;
  }

  return n;
}

static int is_member(const room_t *r, int i) {
  return r->cl[i].fd >= 0 && r->cl[i].joined;
}

// A message that has to arrive (chat, join, leave): a client whose queue is
// full is too slow to be in room
static void send_must(room_t *r, int to, int type, const void *head, size_t hn,
                      const void *body, size_t bn) {
  if (oq_put(&at(r, to)->out, type, head, hn, body, bn) != 0) {
    r->kicked++;
    drop_client(r, to);
  }
}

static void ask_key(room_t *r, int sender, long now_ms, int force) {
  room_client_t *s = at(r, sender);
  if (!force && now_ms - s->need_key_ms < NEED_KEY_GAP_MS) {
    return;
  }

  if (oq_put(&s->out, CALL_NEED_KEY, NULL, 0, NULL, 0) == 0) {
    s->need_key_ms = now_ms;
  }
}

static void drop_client(room_t *r, int i) {
  room_client_t *c = at(r, i);
  if (c->fd < 0) {
    return;
  }

  int was_member = c->joined;
  close(c->fd);
  mb_free(&c->in);
  oq_free(&c->out);
  c->fd = -1;
  c->joined = 0;
  for (int k = 0; k < ROOM_MAX; k++) {
    r->cl[k].synced[i] = 0;
  }

  if (was_member) {
    uint8_t id = (uint8_t)i;
    for (int k = 0; k < ROOM_MAX; k++) {
      if (is_member(r, k)) {
        send_must(r, k, CALL_LEAVE, &id, 1, NULL, 0);
      }
    }
  }
}

static void accept_all(room_t *r, long now_ms) {
  for (;;) {
    int fd = nv_accept_fd(r->lfd);
    if (fd < 0) {
      return;
    }

    int slot = -1;
    for (int i = 0; i < ROOM_MAX && slot < 0; i++) {
      if (r->cl[i].fd < 0) {
        slot = i;
      }
    }

    room_client_t *c = slot >= 0 ? at(r, slot) : NULL;
    if (!c || mb_init(&c->in) != 0) {
      uint8_t full[CALL_HDR] = {CALL_FULL, 0, 0, 0};
      (void)nv_send(fd, full, sizeof(full));
      close(fd);
      if (c) {
        c->fd = -1;
      }

      continue;
    }

    if (oq_init(&c->out) != 0) {
      mb_free(&c->in);
      close(fd);

      continue;
    }

    c->fd = fd;
    c->joined = 0;
    c->name[0] = '\0';
    nl_memset(c->synced, 0, sizeof(c->synced));
    c->since_ms = now_ms;
    c->need_key_ms = -NEED_KEY_GAP_MS;
    c->window_ms = now_ms;
    c->frames_in_window = 0;
    c->chat_in_window = 0;
  }
}

static void on_hello(room_t *r, int i, const uint8_t *p, size_t n,
                     long now_ms) {
  room_client_t *c = at(r, i);
  if (c->joined || n < 2 || p[0] != CALL_VERSION) {
    drop_client(r, i);

    return;
  }

  size_t len = call_clean_text(c->name, p + 1, n - 1, CALL_NAME_MAX);
  if (len == 0) {
    nl_strncpy_safe(c->name, "guest", sizeof(c->name));
    len = 5;
  }

  c->joined = 1;
  uint8_t id = (uint8_t)i;
  send_must(r, i, CALL_WELCOME, &id, 1, NULL, 0);
  for (int k = 0; k < ROOM_MAX && at(r, i)->fd >= 0; k++) {
    if (k == i || !is_member(r, k)) {
      continue;
    }

    uint8_t kid = (uint8_t)k;
    send_must(r, i, CALL_JOIN, &kid, 1, r->cl[k].name,
              nl_strlen(r->cl[k].name));
    send_must(r, k, CALL_JOIN, &id, 1, c->name, len);
    if (r->cl[k].fd >= 0) {
      ask_key(r, k, now_ms, 1); // newcomer has nothing of theirs yet
    }
  }
}

static void forward_frame(room_t *r, int from, const uint8_t *p, size_t n,
                          long now_ms) {
  int key = cc_is_key(p, n);
  if (key < 0) {
    drop_client(r, from);

    return;
  }

  r->frames_in++;
  uint8_t id = (uint8_t)from;
  for (int to = 0; to < ROOM_MAX; to++) {
    if (to == from || !is_member(r, to)) {
      continue;
    }

    room_client_t *t = at(r, to);
    if (!key && !t->synced[from]) {
      r->frames_dropped++;

      continue;
    }

    // Leave room in queue for chat: pictures give way first
    int room_for_frame = oq_pending(&t->out) < CALL_OUTQ / 2;
    if (room_for_frame && oq_put(&t->out, CALL_PFRAME, &id, 1, p, n) == 0) {
      t->synced[from] = 1;
      r->frames_out++;
    } else {
      t->synced[from] = 0;
      r->frames_dropped++;
      ask_key(r, from, now_ms, 0);
    }
  }
}

static void on_chat(room_t *r, int from, const uint8_t *p, size_t n) {
  char text[CALL_CHAT_MAX + 1];
  size_t len = call_clean_text(text, p, n, CALL_CHAT_MAX);
  if (len == 0) {
    return;
  }

  r->chats++;
  uint8_t id = (uint8_t)from;
  for (int to = 0; to < ROOM_MAX; to++) {
    if (is_member(r, to)) {
      send_must(r, to, CALL_PCHAT, &id, 1, text, len);
    }
  }
}

static int handle(room_t *r, int i, long now_ms) {
  room_client_t *c = at(r, i);
  int handled = 0;
  for (;;) {
    int type;
    const uint8_t *p;
    size_t n;
    int got = mb_next(&c->in, &type, &p, &n);
    if (got < 0) {
      drop_client(r, i);

      return handled;
    }

    if (got == 0) {
      return handled;
    }

    handled++;
    // Rate window of one second
    if (now_ms - c->window_ms >= 1000) {
      c->window_ms = now_ms;
      c->frames_in_window = 0;
      c->chat_in_window = 0;
    }

    if (!c->joined) {
      if (type != CALL_HELLO) {
        drop_client(r, i);

        return handled;
      }

      on_hello(r, i, p, n, now_ms);
    } else if (type == CALL_FRAME) {
      if (++c->frames_in_window <= MAX_FRAMES_PER_SEC) {
        forward_frame(r, i, p, n, now_ms);
      } else {
        r->frames_dropped++;
      }
    } else if (type == CALL_CHAT) {
      if (++c->chat_in_window <= MAX_CHATS_PER_SEC) {
        on_chat(r, i, p, n);
      }
    } else {
      drop_client(r, i); // nothing else is allowed from a client
    }

    if (c->fd < 0) {
      return handled;
    }

    mb_pop(&c->in);
  }
}

int room_step(room_t *r, int timeout_ms, long now_ms) {
  if (r->lfd < 0) {
    return -1;
  }

  int fds[ROOM_MAX + 1];
  uint8_t ready[ROOM_MAX + 1];
  fds[0] = r->lfd;
  for (int i = 0; i < ROOM_MAX; i++) {
    fds[i + 1] = r->cl[i].fd;
  }

  if (nv_wait(fds, ROOM_MAX + 1, timeout_ms, ready) < 0) {
    return -1;
  }

  if (ready[0]) {
    accept_all(r, now_ms);
  }

  int handled = 0;
  for (int i = 0; i < ROOM_MAX; i++) {
    if (r->cl[i].fd < 0 || !ready[i + 1]) {
      continue;
    }

    // Read until socket is dry (and messages stay whole)
    for (;;) {
      long got = mb_fill(&r->cl[i].in, r->cl[i].fd);
      if (got > 0) {
        handled += handle(r, i, now_ms);
        if (r->cl[i].fd < 0) {
          break;
        }

        continue;
      }

      if (got == -2) {
        break;
      }

      drop_client(r, i); // closed, error, or an oversized message
      break;
    }
  }

  for (int i = 0; i < ROOM_MAX; i++) {
    room_client_t *c = &r->cl[i];
    if (c->fd < 0) {
      continue;
    }

    if (!c->joined && now_ms - c->since_ms > HELLO_TIMEOUT_MS) {
      drop_client(r, i);

      continue;
    }

    if (oq_flush(&c->out, c->fd) != 0) {
      drop_client(r, i);
    }
  }

  return handled;
}

void room_close(room_t *r) {
  for (int i = 0; i < ROOM_MAX; i++) {
    if (r->cl[i].fd >= 0) {
      close(r->cl[i].fd);
      mb_free(&r->cl[i].in);
      oq_free(&r->cl[i].out);
      r->cl[i].fd = -1;
    }
  }

  if (r->lfd >= 0) {
    close(r->lfd);
  }

  r->lfd = -1;
}

