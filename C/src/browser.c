#include "nolibc.h"

#include "browser.h"

// Linux dirent64 layout returned by getdents64 (syscall 217)
struct brows_dirent64 {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[];
};

#ifndef SYS_getdents64
#define SYS_getdents64 217
#endif

// One-row write buffer (used during replay to drain stdin without blocking)
// Single-byte non-blocking read with VMIN=0, poll for an abort key cheaply
// between events

void browser_init(browser_t *b) { nl_memset(b, 0, sizeof(*b)); }

static brows_ftype_t type_from_name(const char *name) {
  size_t n = nl_strlen(name);
  if (n >= 5 && nl_strcmp(name + n - 5, ".cast") == 0)
    return BROWS_CAST;
  if (n >= 5 && nl_strcmp(name + n - 5, ".ansi") == 0)
    return BROWS_ANSI;
  if (n >= 4 && nl_strcmp(name + n - 4, ".png") == 0)
    return BROWS_PNG;
  return BROWS_UNKNOWN;
}

int browser_scan(browser_t *b, const char *cap_dir) {
  b->count = 0;
  b->selected = 0;
  b->scroll = 0;

  int fd = open(cap_dir, O_RDONLY, 0);
  if (fd < 0) {
    nl_snprintf(b->status, sizeof(b->status), "cannot open dir");
    return -1;
  }

  char dbuf[8192];
  long n;
  while ((n = __sc3(SYS_getdents64, fd, (long)dbuf, (long)sizeof(dbuf))) > 0) {
    long off = 0;
    while (off < n && b->count < BROWS_MAX_FILES) {
      struct brows_dirent64 *d = (struct brows_dirent64 *)(dbuf + off);
      off += d->d_reclen;

      const char *name = d->d_name;
      if (name[0] == '.')
        continue; // skip dotfiles incl. . and ..

      brows_ftype_t t = type_from_name(name);
      if (t == BROWS_UNKNOWN)
        continue;

      brows_entry_t *e = &b->e[b->count];
      nl_strncpy_safe(e->name, name, BROWS_NAME_LEN);
      if (nl_snprintf(e->path, BROWS_PATH_LEN, "%s/%s", cap_dir, name) <= 0) {
        continue;
      }
      e->type = t;
      b->count++;
    }
  }
  close(fd);

  // Sort by name descending
  // asciicam-generated filenames contain an ISO 8601 UTC timestamp
  // (asciicam-YYYYMMDDThhmmssZ.<ext>), so lexicographic reverse order == newest
  // first
  for (int i = 0; i < b->count - 1; i++) {
    for (int j = i + 1; j < b->count; j++) {
      if (nl_strcmp(b->e[i].name, b->e[j].name) < 0) {
        brows_entry_t tmp = b->e[i];
        b->e[i] = b->e[j];
        b->e[j] = tmp;
      }
    }
  }

  if (b->count == 0) {
    nl_snprintf(b->status, sizeof(b->status), "no captures yet");
  } else {
    nl_snprintf(b->status, sizeof(b->status), "%d file%s", b->count,
                b->count == 1 ? "" : "s");
  }

  return 0;
}

void browser_toggle(browser_t *b) {
  b->active = !b->active;
  if (b->active) {
    b->selected = 0;
    b->scroll = 0;
  }
}

brows_action_t browser_key(browser_t *b, brows_keykind_t kind, char ch,
                           char *out_path, size_t out_sz) {
  if (!b->active) {
    return BROWS_ACT_NONE;
  }

  switch (kind) {
  case BR_KEY_ESC:
    return BROWS_ACT_CLOSE;

  case BR_KEY_UP:
    if (b->selected > 0)
      b->selected--;
    return BROWS_ACT_NONE;

  case BR_KEY_DOWN:
    if (b->selected < b->count - 1)
      b->selected++;
    return BROWS_ACT_NONE;

  case BR_KEY_ENTER:
    if (b->selected < b->count) {
      brows_entry_t *e = &b->e[b->selected];
      nl_strncpy_safe(out_path, e->path, out_sz);
      switch (e->type) {
      case BROWS_ANSI:
        return BROWS_ACT_PREVIEW_ANSI;
      case BROWS_CAST:
        return BROWS_ACT_REPLAY_CAST;
      case BROWS_PNG:
        nl_snprintf(b->info_msg, sizeof(b->info_msg),
                    "preview the .ansi sibling (same content, no PNG decoder)");
        return BROWS_ACT_INFO;
      default:
        return BROWS_ACT_NONE;
      }
    }
    return BROWS_ACT_NONE;

  case BR_KEY_CHAR:
    // b/B closes the panel (matches the open key); q/Q also closes for
    // muscle-compatibility with the rest of the app
    if (ch == 'b' || ch == 'B' || ch == 'q' || ch == 'Q') {
      return BROWS_ACT_CLOSE;
    }
    // vim-style j/k navigation as a complement to arrow keys
    if (ch == 'k') {
      if (b->selected > 0)
        b->selected--;
      return BROWS_ACT_NONE;
    }
    if (ch == 'j') {
      if (b->selected < b->count - 1)
        b->selected++;
      return BROWS_ACT_NONE;
    }
    if (ch == 'g') { // top
      b->selected = 0;
      return BROWS_ACT_NONE;
    }
    if (ch == 'G') { // bottom
      b->selected = b->count > 0 ? b->count - 1 : 0;
      return BROWS_ACT_NONE;
    }
    return BROWS_ACT_NONE;
  }

  return BROWS_ACT_NONE;
}

// Rendering
// Panel is BROWS_PANEL_W columns wide, drawn at right edge of terminal
// Each row is built in a small local buffer so knowing its display width
// exactly (only non-ASCII glyph use is U+25B6 selection marker which is 1
// column wide), then padded with spaces so right border lines up

static int append_str(char *out, size_t outsz, int *pos, const char *s) {
  int n = nl_snprintf(out + *pos, outsz - (size_t)*pos, "%s", s);
  if (n < 0) {
    return -1;
  }

  *pos += n;

  return 0;
}

static int append_pad(char *out, size_t outsz, int *pos, int n) {
  for (int i = 0; i < n; i++) {
    if ((size_t)*pos >= outsz - 1) {
      return -1;
    }

    out[(*pos)++] = ' ';
  }

  return 0;
}

int browser_render(const browser_t *b, int term_h, int term_w, int color,
                   char *out, size_t outsz) {
  if (!b->active) {
    return 0;
  }
  if (term_w < BROWS_PANEL_W + 10) {
    return 0; // terminal too narrow for side panel
  }

  int place_col = term_w - BROWS_PANEL_W + 1;
  int panel_h =
      term_h - 4 - 1; // leave PANEL_ROWS (4) for status bar + 1 margin
  if (panel_h < 8) {
    return 0; // terminal too short - don't draw, don't overflow
  }

  int list_rows = panel_h - 4; // top border, divider+hint, bottom border
  if (list_rows < 3) {
    return 0; // need at least 3 list rows to be useful
  }

  // Keep selected visible (cast away const for scroll adjustment - it's a
  // display-only cache, not real state)
  browser_t *bm = (browser_t *)b;
  if (bm->selected < bm->scroll)
    bm->scroll = bm->selected;
  if (bm->selected >= bm->scroll + list_rows)
    bm->scroll = bm->selected - list_rows + 1;
  if (bm->scroll < 0)
    bm->scroll = 0;

  // Colour palette (truecolor when color, empty strings otherwise)
  const char *border_fg = color ? "\033[38;2;120;140;180m" : "";
  const char *title_fg = color ? "\033[38;2;180;200;255m\033[1m" : "";
  const char *norm_bg =
      color ? "\033[48;2;18;18;28m\033[38;2;170;170;200m" : "";
  const char *sel_bg =
      color ? "\033[48;2;50;70;110m\033[38;2;255;255;255m\033[1m" : "\033[7m";
  const char *cast_fg = color ? "\033[38;2;255;80;80m" : "";
  const char *png_fg = color ? "\033[38;2;120;220;120m" : "";
  const char *ansi_fg = color ? "\033[38;2;220;200;100m" : "";
  const char *hint_fg = color ? "\033[38;2;140;140;160m" : "";
  const char *reset = "\033[0m";

  int pos = 0;
  out[0] = '\0';

  // Top border: "┌─ Captures ──...──┐"
  pos += nl_snprintf(out + pos, outsz - (size_t)pos,
                     "\033[1;%dH%s%s┌─ Captures %s%s", place_col, border_fg,
                     title_fg, reset, border_fg);
  for (int i = 12; i < BROWS_PANEL_W - 1; i++) {
    append_str(out, outsz, &pos, "─");
  }
  append_str(out, outsz, &pos, "┐");

  // File rows
  int max_name_w =
      BROWS_PANEL_W - 5; // "│" + " " + marker + " " + name + " " + "│"
  for (int row = 0; row < list_rows; row++) {
    int idx = b->scroll + row;
    int row_num = row + 2;

    pos += nl_snprintf(out + pos, outsz - (size_t)pos, "\033[%d;%dH", row_num,
                       place_col);

    if (idx < b->count) {
      brows_entry_t *e = &b->e[idx];
      const char *marker = (idx == b->selected) ? "▶" : " ";
      const char *row_bg = (idx == b->selected) ? sel_bg : norm_bg;
      const char *type_fg = e->type == BROWS_CAST   ? cast_fg
                            : e->type == BROWS_PNG  ? png_fg
                            : e->type == BROWS_ANSI ? ansi_fg
                                                    : "";

      // Truncate name to max_name_w columns
      // Names are ASCII (asciicam generates ASCII filenames) so byte count ==
      // display width
      char nm[BROWS_NAME_LEN];
      nl_strncpy_safe(nm, e->name, BROWS_NAME_LEN);
      size_t nl_ = nl_strlen(nm);
      if ((int)nl_ > max_name_w) {
        nm[max_name_w - 1] = '~';
        nm[max_name_w] = '\0';
        nl_ = (size_t)max_name_w;
      }

      // Layout: │ <marker> <name><pad> │
      // Width:  1 + 1   + 1      + nl_ + pad + 1 = BROWS_PANEL_W
      // So pad = BROWS_PANEL_W - 4 - nl_
      int pad = BROWS_PANEL_W - 4 - (int)nl_;
      if (pad < 0) {
        pad = 0;
      }

      pos += nl_snprintf(out + pos, outsz - (size_t)pos, "%s│ %s%s ", row_bg,
                         marker, type_fg);
      append_str(out, outsz, &pos, nm);
      // Reset type colour so padding uses row bg
      pos += nl_snprintf(out + pos, outsz - (size_t)pos, "%s%s", reset, row_bg);
      append_pad(out, outsz, &pos, pad);
      pos += nl_snprintf(out + pos, outsz - (size_t)pos, "%s│%s", reset, "");
    } else {
      // Empty row: │<spaces>│
      pos += nl_snprintf(out + pos, outsz - (size_t)pos, "%s│", norm_bg);
      append_pad(out, outsz, &pos, BROWS_PANEL_W - 2);
      pos += nl_snprintf(out + pos, outsz - (size_t)pos, "│%s", reset);
    }
  }

  // Divider between list and hint
  int div_row = list_rows + 2;
  pos += nl_snprintf(out + pos, outsz - (size_t)pos, "\033[%d;%dH%s├", div_row,
                     place_col, border_fg);
  for (int i = 1; i < BROWS_PANEL_W - 1; i++) {
    append_str(out, outsz, &pos, "─");
  }
  append_str(out, outsz, &pos, "┤");

  // Hint row: "│ ↑↓ jk move  enter open  esc close │"
  // Display width budget: BROWS_PANEL_W - 2 = 32 cols inside borders
  // "jk move  enter open  esc close" = 32 cols, exact fit
  int hint_row = list_rows + 3;
  const char *hint = "↑↓ jk move  enter open  esc close";
  pos += nl_snprintf(out + pos, outsz - (size_t)pos,
                     "\033[%d;%dH%s│ %s%s%s%s│%s", hint_row, place_col, norm_bg,
                     hint_fg, hint, reset, norm_bg, reset);
  // (No padding needed: hint is exactly BROWS_PANEL_W - 2 cols wide)

  // Bottom border with status text: "└ <status> ──...──┘"
  int bot_row = list_rows + 4;
  pos += nl_snprintf(out + pos, outsz - (size_t)pos, "\033[%d;%dH%s└ %s%s",
                     bot_row, place_col, border_fg, reset, border_fg);
  pos += nl_snprintf(out + pos, outsz - (size_t)pos, "%s", b->status);
  int used = 2 + (int)nl_strlen(b->status); // "└ " + status
  for (int i = used; i < BROWS_PANEL_W - 1; i++) {
    append_str(out, outsz, &pos, "─");
  }
  append_str(out, outsz, &pos, "┘");
  append_str(out, outsz, &pos, reset);

  return pos;
}

void browser_clear_area(int term_h, int term_w) {
  if (term_w < BROWS_PANEL_W + 10) {
    return;
  }

  int place_col = term_w - BROWS_PANEL_W + 1;
  int panel_h = term_h - 4 - 1;
  if (panel_h < 8) {
    return; // terminal too short - panel was never drawn
  }

  // Bottom border sits on row panel_h + 1; clear one extra row to be safe.
  char buf[64];
  for (int row = 1; row <= panel_h + 1; row++) {
    int n = nl_snprintf(buf, sizeof(buf), "\033[%d;%dH\033[K", row, place_col);
    if (n > 0) {
      (void)write(STDOUT_FILENO, buf, (size_t)n);
    }
  }
}

// Preview / replay: these take over screen
// They clear, draw their content, and block until any key is pressed (or, for
// .cast, until recording ends)
// On return caller redraws live UI

static void write_all_fd(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, p, n);
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

// Non-blocking read: returns 1 if a byte was read into *ch, 0 otherwise
static int poll_key(char *ch) {
  ssize_t r = read(STDIN_FILENO, ch, 1);
  return r == 1 ? 1 : 0;
}

// Drain any pending input so an old keypress doesn't immediately abort
static void drain_keys(void) {
  char ch;
  while (poll_key(&ch)) { /* discard */
  }
}

// Nanosleep wrapper that doesn't depend on libc exposing nanosleep() as a
// symbol (nolibc build may not). Raw syscall via __sc2
static void my_nanosleep(long ns) {
  if (ns <= 0) {
    return;
  }

  struct timespec ts = {0};
  ts.tv_sec = ns / 1000000000L;
  ts.tv_nsec = ns % 1000000000L;
  __sc2(SYS_nanosleep, (long)&ts, 0);
}

// Wait for a keypress, polling every ~50ms
// Returns key
static char wait_for_any_key(void) {
  char ch;
  while (!poll_key(&ch)) {
    my_nanosleep(50 * 1000000L); // 50ms
  }

  return ch;
}

int browser_preview_ansi(const char *path, int *aborted) {
  *aborted = 0;
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  // Hide cursor, clear screen, home cursor
  static const char CLR[] = "\033[2J\033[H\033[?25l";
  write_all_fd(STDOUT_FILENO, CLR, sizeof(CLR) - 1);

  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) {
    write_all_fd(STDOUT_FILENO, buf, (size_t)n);
  }

  close(fd);

  // Footer prompt at bottom of screen
  static const char PROMPT[] =
      "\n\033[7m preview: press any key to return \033[0m";
  write_all_fd(STDOUT_FILENO, PROMPT, sizeof(PROMPT) - 1);

  drain_keys();
  (void)wait_for_any_key();
  *aborted = 1;

  // Clear so live UI redraws cleanly
  write_all_fd(STDOUT_FILENO, "\033[2J\033[H", 8);

  return 0;
}

// .cast replay
// asciinema v2 format:
//   line 1: JSON header {"version":2,"width":80,...}
//   line 2+: [seconds,"o","<data>"]
// data is a JSON-escaped string of bytes that were sent to the terminal
// unescape JSON \\ \" \n \r \t \b \f \/ and \uXXXX (UTF-8 encoded)

static double parse_float(const char *s, const char **end) {
  double v = 0.0;
  double frac = 0.1;
  int seen_dot = 0;
  while (*s) {
    if (*s >= '0' && *s <= '9') {
      if (seen_dot) {
        v += (*s - '0') * frac;
        frac *= 0.1;
      } else {
        v = v * 10.0 + (*s - '0');
      }
      s++;
    } else if (*s == '.') {
      seen_dot = 1;
      s++;
    } else {
      break;
    }
  }

  *end = s;

  return v;
}

// Parse one event line "[t,\"o\",\"data\"]" into out_time and out_data
// Returns 0 on success, -1 on malformed input
static int parse_event(const char *line, size_t line_len, double *out_time,
                       char *out_data, size_t out_cap, size_t *out_len) {
  if (line_len < 7) {
    return -1; // [0,"o",""] needs at least 9, but
  }
  if (line[0] != '[') {
    return -1; // not an event line (header etc.)
  }

  const char *p = line + 1;
  const char *end = line + line_len;

  const char *time_end;
  *out_time = parse_float(p, &time_end);
  p = time_end;

  // Expect ,"o","
  if (p + 6 > end) {
    return -1;
  }
  if (p[0] != ',' || p[1] != '"' || p[2] != 'o' || p[3] != '"' || p[4] != ',' ||
      p[5] != '"') {
    return -1;
  }
  p += 6;

  size_t di = 0;
  while (p < end && *p != '"') {
    if (di >= out_cap - 1) {
      return -1; // too long for our buffer
    }

    if (*p == '\\') {
      p++;
      if (p >= end) {
        return -1;
      }
      switch (*p) {
      case '"':
        out_data[di++] = '"';
        break;
      case '\\':
        out_data[di++] = '\\';
        break;
      case '/':
        out_data[di++] = '/';
        break;
      case 'b':
        out_data[di++] = 0x08;
        break;
      case 'f':
        out_data[di++] = 0x0C;
        break;
      case 'n':
        out_data[di++] = 0x0A;
        break;
      case 'r':
        out_data[di++] = 0x0D;
        break;
      case 't':
        out_data[di++] = 0x09;
        break;
      case 'u': {
        // \uXXXX -> UTF-8 codepoint
        if (p + 4 >= end) {
          return -1;
        }
        unsigned int cp = 0;
        for (int i = 1; i <= 4; i++) {
          char c = p[i];
          unsigned int v;
          if (c >= '0' && c <= '9') {
            v = (unsigned)(c - '0');
          } else if (c >= 'a' && c <= 'f') {
            v = (unsigned)(c - 'a' + 10);
          } else if (c >= 'A' && c <= 'F') {
            v = (unsigned)(c - 'A' + 10);
          } else {
            return -1;
          }
          cp = (cp << 4) | v;
        }
        p += 4;
        if (cp < 0x80) {
          if (di >= out_cap - 1) {
            return -1;
          }
          out_data[di++] = (char)cp;
        } else if (cp < 0x800) {
          if (di >= out_cap - 2) {
            return -1;
          }
          out_data[di++] = (char)(0xC0 | (cp >> 6));
          out_data[di++] = (char)(0x80 | (cp & 0x3F));
        } else {
          if (di >= out_cap - 3) {
            return -1;
          }
          out_data[di++] = (char)(0xE0 | (cp >> 12));
          out_data[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
          out_data[di++] = (char)(0x80 | (cp & 0x3F));
        }
        break;
      }
      default:
        return -1;
      }
      p++;
    } else {
      out_data[di++] = *p++;
    }
  }

  out_data[di] = '\0';
  *out_len = di;

  return 0;
}

int browser_replay_cast(const char *path, int *aborted) {
  *aborted = 0;
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  // Streaming line reader: read into a sliding buffer, find newlines, hand each
  // complete line to parse_event. Buffer must be big enough for one frame's
  // worth of escape codes - 1 MB is generous for asciicam output
  size_t cap = 1024 * 1024;
  char *buf = nl_malloc(cap);
  if (!buf) {
    close(fd);
    return -1;
  }

  // Event data buffer: same cap, since a single event can be a full frame
  char *data = nl_malloc(cap);
  if (!data) {
    nl_free(buf);
    close(fd);
    return -1;
  }

  // Clear screen, hide cursor
  write_all_fd(STDOUT_FILENO, "\033[2J\033[H\033[?25l", 13);

  drain_keys();

  // Replay timing: track when started so can sleep until each event's scheduled
  // time, preserving original pacing
  struct timespec start_ts = {0};
  clock_gettime(CLOCK_MONOTONIC, &start_ts);
  uint64_t start_ns =
      (uint64_t)start_ts.tv_sec * 1000000000ULL + (uint64_t)start_ts.tv_nsec;

  size_t buf_len = 0;
  int eof = 0;
  int events_played = 0;
  int aborted_local = 0;

  while (!aborted_local) {
    // Refill buffer if there's room
    if (!eof && buf_len < cap) {
      ssize_t r = read(fd, buf + buf_len, cap - buf_len);
      if (r > 0) {
        buf_len += (size_t)r;
      } else if (r == 0) {
        eof = 1;
      } else if (r < 0 && errno != EINTR && errno != EAGAIN) {
        break;
      }
    }

    // Process complete lines
    size_t line_start = 0;
    for (size_t i = 0; i < buf_len; i++) {
      if (buf[i] == '\n') {
        size_t line_len = i - line_start;
        if (line_len > 0) {
          double t = 0;
          size_t dlen = 0;
          if (parse_event(buf + line_start, line_len, &t, data, cap, &dlen) ==
              0) {
            // Sleep until start_ns + t*1e9, polling for abort key
            uint64_t target_ns = start_ns + (uint64_t)(t * 1e9);
            struct timespec now_ts = {0};
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            uint64_t now_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL +
                              (uint64_t)now_ts.tv_nsec;
            while (now_ns < target_ns) {
              // Check for abort key
              char ch;
              if (poll_key(&ch)) {
                aborted_local = 1;
                break;
              }
              // Sleep up to 50ms or until target, whichever is sooner
              uint64_t remain = target_ns - now_ns;
              uint64_t step = remain > 50000000ULL ? 50000000ULL : remain;
              my_nanosleep((long)step);
              clock_gettime(CLOCK_MONOTONIC, &now_ts);
              now_ns = (uint64_t)now_ts.tv_sec * 1000000000ULL +
                       (uint64_t)now_ts.tv_nsec;
            }
            if (aborted_local) {
              break;
            }

            // Write event bytes to terminal
            if (dlen > 0) {
              write_all_fd(STDOUT_FILENO, data, dlen);
            }
            events_played++;
          }
          // If parse_event fails, line wasn't an event (JSON header or
          // malformed) - just skip it
        }
        line_start = i + 1;
      }
    }
    if (aborted_local)
      break;

    // At EOF, if there's an unprocessed tail without a trailing newline,
    // treat it as final event line
    // WARN: Without this loop forever on a file that doesn't end in \n (rare
    // but legal)
    if (eof && line_start < buf_len) {
      size_t line_len = buf_len - line_start;
      double t = 0;
      size_t dlen = 0;
      if (parse_event(buf + line_start, line_len, &t, data, cap, &dlen) == 0) {
        // No need to sleep: at EOF, just write final frame
        if (dlen > 0) {
          write_all_fd(STDOUT_FILENO, data, dlen);
        }
        events_played++;
      }
      line_start = buf_len; // mark consumed
    }

    // Move unprocessed tail to start of buffer
    if (line_start > 0) {
      if (line_start < buf_len) {
        nl_memcpy(buf, buf + line_start, buf_len - line_start);
        buf_len -= line_start;
      } else {
        buf_len = 0;
      }
    }

    // Exit conditions:
    //   - EOF and everything consumed -> done
    //   - Buffer full and no newline found -> line longer than cap, bail
    if (eof && buf_len == 0) {
      break;
    }
    if (buf_len == cap && line_start == 0) {
      break;
    }
  }

  nl_free(buf);
  nl_free(data);
  close(fd);

  // Footer prompt before returning
  if (aborted_local) {
    static const char MSG[] =
        "\n\033[7m replay aborted - press any key to return \033[0m";
    write_all_fd(STDOUT_FILENO, MSG, sizeof(MSG) - 1);
  } else {
    char msg[96];
    int n = nl_snprintf(msg, sizeof(msg),
                        "\n\033[7m replay finished (%d events) - press any "
                        "key to return \033[0m",
                        events_played);
    if (n > 0) {
      write_all_fd(STDOUT_FILENO, msg, (size_t)n);
    }
  }

  drain_keys();
  (void)wait_for_any_key();
  *aborted = 1; // always block until a keypress on way out

  // Clear so live UI redraws cleanly
  write_all_fd(STDOUT_FILENO, "\033[2J\033[H", 8);

  return 0;
}
