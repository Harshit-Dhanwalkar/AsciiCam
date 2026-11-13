/*
 * src/callproto.c - message framing, see include/callproto.h
 */

#include "platform.h"

#include "nolibc.h"

#include "callproto.h"
#include "netview.h"

#define MB_CAP (CALL_HDR + CALL_MAX_PAYLOAD)

// Move bytes towards start of same buffer (dst < src)
static void shift_down(void *dst, const void *src, size_t n) {
  uint8_t *d = dst;
  const uint8_t *sp = src;
  for (size_t i = 0; i < n; i++) {
    d[i] = sp[i];
  }
}

int mb_init(msgbuf_t *mb) {
  mb->buf = nl_malloc(MB_CAP);
  mb->n = 0;

  return mb->buf ? 0 : -1;
}

void mb_free(msgbuf_t *mb) {
  nl_free(mb->buf);
  mb->buf = NULL;
  mb->n = 0;
}

long mb_fill(msgbuf_t *mb, int fd) {
  if (mb->n >= MB_CAP) {
    return -3;
  }

  long r = nv_recv(fd, mb->buf + mb->n, MB_CAP - mb->n);
  if (r > 0) {
    mb->n += (size_t)r;

    return r;
  }

  if (r == 0) {
    return 0;
  }

  return r == -11 ? -2 : -1;
}

int mb_feed(msgbuf_t *mb, const void *data, size_t n) {
  if (n > MB_CAP - mb->n) {
    return -1;
  }

  nl_memcpy(mb->buf + mb->n, data, n);
  mb->n += n;

  return 0;
}

int mb_next(const msgbuf_t *mb, int *type, const uint8_t **payload,
            size_t *len) {
  if (mb->n < CALL_HDR) {
    return 0;
  }

  size_t l = (size_t)mb->buf[1] * 65536u + (size_t)mb->buf[2] * 256u +
             (size_t)mb->buf[3];
  if (l > CALL_MAX_PAYLOAD || mb->buf[0] == 0) {
    return -1;
  }

  if (mb->n < CALL_HDR + l) {
    return 0;
  }

  *type = mb->buf[0];
  *payload = mb->buf + CALL_HDR;
  *len = l;

  return 1;
}

void mb_pop(msgbuf_t *mb) {
  if (mb->n < CALL_HDR) {
    return;
  }

  size_t l = (size_t)mb->buf[1] * 65536u + (size_t)mb->buf[2] * 256u +
             (size_t)mb->buf[3];
  size_t total = CALL_HDR + l;
  if (total >= mb->n) {
    mb->n = 0;

    return;
  }

  shift_down(mb->buf, mb->buf + total, mb->n - total);
  mb->n -= total;
}

int oq_init(outq_t *q) {
  q->buf = nl_malloc(CALL_OUTQ);
  q->n = 0;
  q->pos = 0;

  return q->buf ? 0 : -1;
}

void oq_free(outq_t *q) {
  nl_free(q->buf);
  q->buf = NULL;
  q->n = q->pos = 0;
}

size_t oq_pending(const outq_t *q) { return q->n - q->pos; }

int oq_put(outq_t *q, int type, const void *head, size_t hn, const void *body,
           size_t bn) {
  size_t len = hn + bn;
  if (!q->buf || len > CALL_MAX_PAYLOAD || type < 1 || type > 255) {
    return -1;
  }

  if (q->pos > 0 && q->pos == q->n) {
    q->n = q->pos = 0;
  }

  if (CALL_OUTQ - q->n < CALL_HDR + len) {
    if (q->pos == 0) {
      return -1;
    }

    shift_down(q->buf, q->buf + q->pos, q->n - q->pos);
    q->n -= q->pos;
    q->pos = 0;
    if (CALL_OUTQ - q->n < CALL_HDR + len) {
      return -1;
    }
  }

  uint8_t *p = q->buf + q->n;
  p[0] = (uint8_t)type;
  p[1] = (uint8_t)(len / 65536u);
  p[2] = (uint8_t)(len / 256u);
  p[3] = (uint8_t)len;
  if (hn) {
    nl_memcpy(p + CALL_HDR, head, hn);
  }

  if (bn) {
    nl_memcpy(p + CALL_HDR + hn, body, bn);
  }

  q->n += CALL_HDR + len;

  return 0;
}

int oq_flush(outq_t *q, int fd) {
  while (q->pos < q->n) {
    long r = nv_send(fd, q->buf + q->pos, q->n - q->pos);
    if (r > 0) {
      q->pos += (size_t)r;

      continue;
    }

    if (r == -11) {
      break;
    }

    return -1;
  }

  if (q->pos == q->n) {
    q->n = q->pos = 0;
  }

  return 0;
}

size_t call_clean_text(char *dst, const uint8_t *src, size_t n, size_t max) {
  size_t k = 0;
  for (size_t i = 0; i < n && k < max; i++) {
    uint8_t c = src[i];
    dst[k++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
  }

  // trim blanks at both ends
  size_t a = 0;
  while (a < k && dst[a] == ' ') {
    a++;
  }

  while (k > a && dst[k - 1] == ' ') {
    k--;
  }

  if (a > 0) {
    shift_down(dst, dst + a, k - a);
    k -= a;
  }

  dst[k] = '\0';

  return k;
}
