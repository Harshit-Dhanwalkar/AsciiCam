#include "nolibc.h"

#include "plugin_picker.h"

#define BOX_W 60

void picker_open(picker_t *p) {
  nl_memset(p, 0, sizeof(*p));
  p->active = 1;
}

void picker_close(picker_t *p) { p->active = 0; }

static char lower(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
}

int picker_name_match(const char *name, const char *query) {
  if (!query[0]) {
    return 1;
  }

  for (const char *n = name; *n; n++) {
    const char *a = n;
    const char *b = query;
    while (*a && *b && lower(*a) == lower(*b)) {
      a++;
      b++;
    }

    if (!*b) {
      return 1;
    }
  }

  return 0;
}

int picker_matches(const picker_t *p, const plugin_catalog_t *c, int *idx_out,
                   int max) {
  int n = 0;
  for (int i = 0; i < c->count && n < max; i++) {
    if (picker_name_match(c->e[i].name, p->query)) {
      idx_out[n++] = i;
    }
  }

  return n;
}

picker_action_t picker_key(picker_t *p, const plugin_catalog_t *c, int key,
                           int *load_idx) {
  if (!p->active) {
    return PK_NONE;
  }

  int m[CATALOG_MAX];
  int count;

  switch (key) {
  case PK_KEY_ESC:
    picker_close(p);

    return PK_CLOSED;
  case PK_KEY_UP:
    count = picker_matches(p, c, m, CATALOG_MAX);
    if (count > 0) {
      p->sel = (p->sel - 1 + count) % count;
    }

    return PK_CONSUMED;
  case PK_KEY_DOWN:
    count = picker_matches(p, c, m, CATALOG_MAX);
    if (count > 0) {
      p->sel = (p->sel + 1) % count;
    }

    return PK_CONSUMED;
  case 8:
  case 127:
    if (p->qlen > 0) {
      p->query[--p->qlen] = '\0';
      p->sel = 0;
    }

    return PK_CONSUMED;
  case 10:
  case 13:
    count = picker_matches(p, c, m, CATALOG_MAX);
    if (count == 0) {
      picker_close(p);

      return PK_CLOSED;
    }
    if (p->sel >= count) {
      p->sel = count - 1;
    }
    if (load_idx) {
      *load_idx = m[p->sel];
    }
    picker_close(p);

    return PK_LOAD;
  default:
    break;
  }

  if (key >= 32 && key <= 126 && p->qlen + 1 < PICKER_QUERY_MAX) {
    p->query[p->qlen++] = (char)key;
    p->query[p->qlen] = '\0';
    p->sel = 0;
  }

  return PK_CONSUMED;
}

// Append s to buf, cut or space-padded to exactly width columns (ASCII only)
static void put_cell(char *buf, size_t size, size_t *pos, const char *s,
                     int width) {
  int i = 0;
  for (; s[i] && i < width; i++) {
    if (*pos + 1 < size) {
      buf[(*pos)++] = s[i];
    }
  }
  for (; i < width; i++) {
    if (*pos + 1 < size) {
      buf[(*pos)++] = ' ';
    }
  }
}

static void put_raw(char *buf, size_t size, size_t *pos, const char *s) {
  for (; *s; s++) {
    if (*pos + 1 < size) {
      buf[(*pos)++] = *s;
    }
  }
}

static void put_row(char *buf, size_t size, size_t *pos, int row,
                    const char *style, const char *text) {
  char head[40];

  nl_snprintf(head, sizeof(head), "\033[%d;1H", row);

  put_raw(buf, size, pos, head);
  put_raw(buf, size, pos, style);
  put_cell(buf, size, pos, text, BOX_W);
  put_raw(buf, size, pos, "\033[0m");
}

int picker_render(const picker_t *p, const plugin_catalog_t *c, int ascii_h,
                  int color, char *buf, size_t size) {
  if (!p->active || size < 64) {
    return 0;
  }

  int m[CATALOG_MAX];
  int count = picker_matches(p, c, m, CATALOG_MAX);

  int vis = count < PICKER_MAX_ROWS ? count : PICKER_MAX_ROWS;
  int body = vis > 0 ? vis : 1; // "no match" takes a row
  int total = body + 1;         // + header
  if (ascii_h < total) {
    return 0;
  }

  int sel = p->sel;
  if (sel >= count) {
    sel = count > 0 ? count - 1 : 0;
  }

  // keep the selected row visible
  int first = 0;
  if (sel >= vis) {
    first = sel - vis + 1;
  }

  const char *hdr_style =
      color ? "\033[38;2;255;220;0m\033[48;2;0;40;80m" : "\033[7m";
  const char *sel_style =
      color ? "\033[38;2;255;255;255m\033[48;2;0;90;160m" : "\033[7m";
  const char *row_style =
      color ? "\033[38;2;200;200;200m\033[48;2;18;18;18m" : "\033[0m";

  size_t pos = 0;
  int top = ascii_h - total + 1;
  char line[160];

  nl_snprintf(line, sizeof(line),
              " plugins (%d/%d)  /%s_   enter load  esc close", count, c->count,
              p->query);
  put_row(buf, size, &pos, top, hdr_style, line);

  if (count == 0) {
    put_row(buf, size, &pos, top + 1, row_style, "   no plugin matches");
  }

  for (int i = 0; i < vis; i++) {
    const catalog_entry_t *e = &c->e[m[first + i]];
    int is_sel = (first + i == sel);

    if (e->state == PC_FAILED && e->err[0]) {
      nl_snprintf(line, sizeof(line), " %c %s  [failed: %s]",
                  is_sel ? '>' : ' ', e->name, e->err);
    } else {
      nl_snprintf(line, sizeof(line), " %c %s  [%s]", is_sel ? '>' : ' ',
                  e->name, catalog_state_name(e->state));
    }

    put_row(buf, size, &pos, top + 1 + i, is_sel ? sel_style : row_style, line);
  }

  buf[pos] = '\0';

  return (int)pos;
}

int toast_render(const char *msg, int ascii_h, int color, char *buf,
                 size_t size) {
  if (!msg || !msg[0] || ascii_h < 1 || size < 64) {
    return 0;
  }

  size_t pos = 0;
  char line[160];

  nl_snprintf(line, sizeof(line), " %s", msg);
  put_row(buf, size, &pos, ascii_h,
          color ? "\033[38;2;255;255;255m\033[48;2;60;60;60m" : "\033[7m",
          line);

  buf[pos] = '\0';

  return (int)pos;
}
