/*
 * src/call_ui.c - terminal, camera and clock side of a video call
 */

#include "platform.h"

#include "nolibc.h"

#include "ascii.h"
#include "call.h"
#include "call_ui.h"
#include "capture.h"
#include "demo.h"
#include "netview.h"
#include "room.h"

#define UI_ENTER "\033[?1049h\033[?25l\033[2J"
#define UI_LEAVE "\033[?25h\033[?1049l"
#define UI_SYNC_BEGIN "\033[?2026h"
#define UI_SYNC_END "\033[?2026l"
#define UI_TIOCGWINSZ 0x5413

extern volatile sig_atomic_t keep_running;
void term_raw_mode(void);
void term_restore(void);

struct ui_winsize {
  unsigned short rows, cols, xpixel, ypixel;
};

static void fd_add(nl_fd_set *s, int fd) {
  unsigned long *bits = (unsigned long *)s;
  unsigned u = (unsigned)fd;
  bits[u / 64u] |= 1UL << (u % 64u);
}

// Writes all of it, but never sleeps in a full tty for good: a stopped terminal
// must not keep us from leaving when asked to (a write restarted by signal
// would not return)
static void ui_write(const char *p, size_t n) {
  while (n > 0 && keep_running) {
    nl_fd_set ws;
    nl_memset(&ws, 0, sizeof(ws));
    fd_add(&ws, STDOUT_FILENO);
    struct nl_timeval tv = {0, 100000};
    if (nl_select(STDOUT_FILENO + 1, NULL, &ws, NULL, &tv) <= 0) {
      continue;
    }

    ssize_t w = write(STDOUT_FILENO, p, n);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }

      return;
    }

    p += w;
    n -= (size_t)w;
  }
}

static long ui_now_ms(void) {
  struct timespec ts = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }

  return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static int fd_has(const nl_fd_set *s, int fd) {
  const unsigned long *bits = (const unsigned long *)s;
  unsigned u = (unsigned)fd;

  return (bits[u / 64u] >> (u % 64u)) % 2UL != 0;
}

static void term_size(int *w, int *h) {
  struct ui_winsize ws;
  nl_memset(&ws, 0, sizeof(ws));
  if (nl_ioctl(STDOUT_FILENO, UI_TIOCGWINSZ, &ws) == 0 && ws.cols > 0 &&
      ws.rows > 0) {
    *w = ws.cols;
    *h = ws.rows;
  }
}

int room_main(const char *addr) {
  uint32_t ip = 0;
  int port = 0;
  if (nv_parse_addr(addr, &ip, &port) != 0 || port == 0) {
    fprintf(stderr, "--room needs a port (7100) or an IPv4 address and port "
                    "(127.0.0.1:7100, *:7100)\n");

    return 2;
  }

  static room_t room;
  if (room_listen(&room, ip, port) != 0) {
    fprintf(stderr, "--room: cannot listen on %s (in use?)\n", addr);

    return 2;
  }

  fprintf(stderr, "room open on port %d (up to %d people)\n", room.port,
          ROOM_MAX);
  if (ip != 0x7f000001u) {
    fprintf(stderr, "--room: listening beyond this machine, no password and no "
                    "encryption: use ssh -L, see README\n");
  }

  int members = 0;
  while (keep_running) {
    if (room_step(&room, 500, ui_now_ms()) < 0) {
      fprintf(stderr, "--room: giving up\n");
      room_close(&room);

      return 1;
    }

    int now_in = room_members(&room);
    if (now_in != members) {
      fprintf(stderr, "%d in the room\n", now_in);
      members = now_in;
    }
  }

  room_close(&room);

  return 0;
}

// Typed line: printable ASCII, backspace, Enter sends, Esc clears
typedef struct {
  char buf[CALL_CHAT_MAX + 1];
  int len;
} typing_t;

// Returns 1 when user wants out
static int handle_keys(call_t *c, typing_t *t, const char *k, ssize_t n) {
  for (ssize_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)k[i];
    if (ch == 3 || ch == 4) {
      return 1;
    }

    if (ch == 22) { // Ctrl-V
      c->video_off = !c->video_off;
      c->need_key = 1;
      c->dirty = 1;

      continue;
    }

    if (ch == 27) {
      if (i + 1 < n) {
        // An escape sequence (arrow, ...): skip it
        i = n;
      } else {
        t->len = 0;
        t->buf[0] = 0;
        c->dirty = 1;
      }
    } else if (ch == '\r' || ch == '\n') {
      if (t->len > 0) {
        t->buf[t->len] = 0;
        call_say(c, t->buf);
        t->len = 0;
        t->buf[0] = 0;
        c->dirty = 1;
      }
    } else if (ch == 127 || ch == 8) {
      if (t->len > 0) {
        t->buf[--t->len] = 0;
        c->dirty = 1;
      }
    } else if (ch >= 32 && ch < 127 && t->len < CALL_CHAT_MAX) {
      t->buf[t->len++] = (char)ch;
      t->buf[t->len] = 0;
      c->dirty = 1;
    }
  }

  return 0;
}

int call_main(const char *addr, const char *name, const char *device, int cap_w,
              int cap_h, int fps, int tile_w, int tile_h,
              const ascii_opts_t *opts_in) {
  if (tile_w > CC_MAX_COLS) {
    tile_w = CC_MAX_COLS;
  }

  if (tile_h > CC_MAX_ROWS) {
    tile_h = CC_MAX_ROWS;
  }

  static call_t call;
  if (call_open(&call, addr, name) != 0) {
    fprintf(stderr,
            "--call: cannot connect to %s (is --room running there? over "
            "ssh: ssh -L 7100:localhost:7100 host)\n",
            addr);

    return 1;
  }

  webcam_t cam = {.fd = -1, .buffer = MAP_FAILED};
  if (webcam_init(&cam, device, cap_w, cap_h) < 0) {
    nl_perror("webcam_init");
    if (!demo_is_device(device)) {
      fprintf(stderr, "no usable camera at %s: try -d demo\n", device);
    }

    call_close(&call);

    return 1;
  }

  ascii_opts_t opts = *opts_in;
  opts.origin_x = 1; // positions in picture start at its own corner
  opts.origin_y = 1;
  opts.mirror = 0; // peers must not get mirrored text
  opts.tone_lut = NULL;
  opts.tone_hist = NULL;
  opts.color_mode = COLOR_TRUECOLOR;
  if (demo_is_device(device) && opts.render_mode == RENDER_DOTS) {
    opts.render_mode = RENDER_ASCII_RAMP;
  }

  int pixels = cam.width * cam.height;
  uint8_t *gray = nl_malloc((size_t)pixels + 16);
  uint8_t *rgb = opts.color ? nl_malloc((size_t)pixels * 3) : NULL;
  size_t out_size = 0;
  for (render_mode_t rm = 0; rm < RENDER_MODE_COUNT; rm++) {
    size_t s = ascii_out_size_for_mode(tile_w * 2, tile_h * 4, opts.color, rm);
    if (s > out_size) {
      out_size = s;
    }
  }

  char *pic = nl_malloc(out_size);
  if (!gray || (opts.color && !rgb) || !pic) {
    nl_perror("malloc");
    nl_free(gray);
    nl_free(rgb);
    nl_free(pic);
    webcam_cleanup(&cam);
    call_close(&call);

    return 1;
  }

  int sub_w = tile_w * 2;
  int sub_h = tile_h * 4;
  if (opts.render_mode == RENDER_HALF_BLOCK) {
    sub_w = tile_w;
    sub_h = tile_h * 2;
  }

  char *screen = NULL;
  size_t screen_cap = 0;
  int tw = 80;
  int th = 24;
  int last_tw = 0;
  int last_th = 0;
  typing_t typing = {.len = 0, .buf = {0}};
  int period = 1000 / (fps > 0 ? fps : 15);
  long next_frame = ui_now_ms();
  int quit = 0;
  long rate_at = ui_now_ms();
  uint64_t last_out = 0;
  uint64_t last_in = 0;

  ui_write(UI_ENTER, sizeof(UI_ENTER) - 1);
  term_raw_mode();
  nv_set_blocking(STDOUT_FILENO, 0);
  while (keep_running && !quit && !call.closed) {
    long now = ui_now_ms();
    int wait = (int)(next_frame - now);
    if (wait < 0) {
      wait = 0;
    }

    if (wait > 50) {
      wait = 50;
    }

    nl_fd_set rs;
    nl_memset(&rs, 0, sizeof(rs));
    fd_add(&rs, STDIN_FILENO);
    fd_add(&rs, call.fd);
    struct nl_timeval tv = {0, (long)wait * 1000L};
    int maxfd = call.fd > STDIN_FILENO ? call.fd : STDIN_FILENO;
    int ready = nl_select(maxfd + 1, &rs, NULL, NULL, &tv);
    if (ready > 0 && fd_has(&rs, STDIN_FILENO)) {
      char kb[64] = {0};
      ssize_t kn = read(STDIN_FILENO, kb, sizeof(kb));
      if (kn == 0) {
        break; // terminal gone
      }

      if (kn > 0) {
        quit = handle_keys(&call, &typing, kb, kn);
      }
    }

    if (call_pump(&call) < 0) {
      break;
    }

    // Traffic, per second
    now = ui_now_ms();
    if (now - rate_at >= 1000) {
      uint64_t out_now = call.out.total;
      call.rate_out =
          (int)((out_now - last_out) * 1000 / (uint64_t)(now - rate_at));
      call.rate_in =
          (int)((call.bytes_in - last_in) * 1000 / (uint64_t)(now - rate_at));
      last_out = out_now;
      last_in = call.bytes_in;
      rate_at = now;
      call.dirty = 1;
    }

    now = ui_now_ms();
    if (now >= next_frame) {
      next_frame = now + period;
      if (webcam_wait_frame(&cam, 0) >= 0) {
        int cap = webcam_capture_frame(&cam, gray);
        if (cap < 0) {
          nl_perror("capture_frame");

          break;
        }

        if (cap == 0) {
          const uint8_t *raw = NULL;
          if (cam.buffer && cam.buffer != MAP_FAILED) {
            raw = (const uint8_t *)cam.buffer;
          }

          const uint8_t *colour = NULL;
          if (opts.color && rgb && raw) {
            yuyv_to_rgb_ex(raw, cam.stride, gray, rgb, cam.width, cam.height);
            colour = rgb;
          }

          ascii_opts_t o = opts;
          o.color = colour != NULL;
          int len = grayscale_to_ascii(gray, colour, cam.width, cam.height,
                                       sub_w, sub_h, pic, out_size, &o);

          if (call.video_off) {
            len = (int)call_off_card(pic, out_size, tile_w, tile_h);
          }

          if (len > 0) {
            call_send_picture(&call, pic, (size_t)len);
          }

          if (webcam_requeue_buffer(&cam) < 0) {
            nl_perror("requeue_buffer");

            break;
          }
        }
      }
    }

    term_size(&tw, &th);
    if (tw != last_tw || th != last_th) {
      call.dirty = 1;
      last_tw = tw;
      last_th = th;
    }

    if (call.dirty) {
      size_t need = call_max_compose(&call, tw, th) + 64;
      if (need > screen_cap) {
        nl_free(screen);
        screen = nl_malloc(need);
        screen_cap = screen ? need : 0;
      }

      if (screen) {
        size_t n = call_compose(&call, typing.len > 0 ? typing.buf : NULL, tw,
                                th, screen, screen_cap);
        if (n > 0) {
          ui_write(UI_SYNC_BEGIN, sizeof(UI_SYNC_BEGIN) - 1);
          ui_write(screen, n);
          ui_write(UI_SYNC_END, sizeof(UI_SYNC_END) - 1);
        }
      }
    }
  }

  nv_set_blocking(STDOUT_FILENO, 1);
  term_restore();
  (void)write(STDOUT_FILENO, UI_LEAVE, sizeof(UI_LEAVE) - 1);
  int rc = 0;
  if (call.full) {
    fprintf(stderr, "--call: the room is full\n");
    rc = 1;
  } else if (call.closed) {
    fprintf(stderr, "--call: the room closed the connection\n");
    rc = 1;
  }

  nl_free(screen);
  nl_free(gray);
  nl_free(rgb);
  nl_free(pic);
  webcam_cleanup(&cam);
  call_close(&call);

  return rc;
}
