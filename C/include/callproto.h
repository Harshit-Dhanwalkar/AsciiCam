#ifndef CALLPROTO_H
#define CALLPROTO_H

/*
 * callproto - messages between a call client and room relay
 *
 * A message is 4 header bytes (type, then payload length as 24 bit big endian)
 * and payload
 * Client to relay:
 *   HELLO  version (1 byte), name         first message, nothing else before
 *   FRAME  cellcodec frame                own picture
 *   CHAT   text
 * Relay to client:
 *   WELCOME id (1 byte)                   answer to HELLO
 *   JOIN   id, name                       somebody is in room (also sent
 *                                         for everybody already there)
 *   LEAVE  id
 *   PFRAME id, cellcodec frame            picture of somebody else
 *   PCHAT  id, text                       chat line, own lines come back too
 *   NEED_KEY                              send a key frame next
 *   FULL                                  room is full, connection closes
 *
 * A reader (msgbuf) takes bytes as they come and hands out whole messages; a
 * writer (outq) queues whole messages and sends what socket takes
 * Neither ever blocks
 */

#include "nl_types.h"

#define CALL_VERSION 1
#define CALL_NAME_MAX 16
#define CALL_CHAT_MAX 200
#define CALL_MAX_PAYLOAD 40000
#define CALL_HDR 4

enum {
  CALL_HELLO = 1,
  CALL_FRAME = 2,
  CALL_CHAT = 3,
  CALL_WELCOME = 16,
  CALL_JOIN = 17,
  CALL_LEAVE = 18,
  CALL_PFRAME = 19,
  CALL_PCHAT = 20,
  CALL_NEED_KEY = 21,
  CALL_FULL = 22
};

// Reader
typedef struct {
  uint8_t *buf; // CALL_HDR + CALL_MAX_PAYLOAD
  size_t n;
} msgbuf_t;

int mb_init(msgbuf_t *mb);
void mb_free(msgbuf_t *mb);

// Read what socket has. Bytes read, 0 = peer closed, -1 = error, -2 = nothing
// there now, -3 = buffer is full of an unfinished oversized message
long mb_fill(msgbuf_t *mb, int fd);

// Feed bytes by hand (tests). 0, or -1 when they do not fit
int mb_feed(msgbuf_t *mb, const void *data, size_t n);

// First whole message: 1 and type/payload/len set (payload valid until mb_pop),
// 0 when more is needed, -1 on a header that can never be valid
int mb_next(const msgbuf_t *mb, int *type, const uint8_t **payload,
            size_t *len);
void mb_pop(msgbuf_t *mb);

// Writer
#define CALL_OUTQ 65536

typedef struct {
  uint8_t *buf;
  size_t n;       // bytes queued
  size_t pos;     // bytes already sent
  uint64_t total; // bytes sent over lifetime
} outq_t;

int oq_init(outq_t *q);
void oq_free(outq_t *q);

// Queue one message (payload may be NULL when len is 0; two parts are joined:
// head of hn bytes, then body of bn)
// Return 0, or -1 when it does not fit
int oq_put(outq_t *q, int type, const void *head, size_t hn, const void *body,
           size_t bn);

// Bytes waiting
size_t oq_pending(const outq_t *q);

// Send what socket takes
// Return 0 ok, -1 connection is dead
int oq_flush(outq_t *q, int fd);

// Printable ASCII only, at most max bytes (dst needs max + 1), blanks trimmed;
// control and other bytes become '?'
// Length of the result
size_t call_clean_text(char *dst, const uint8_t *src, size_t n, size_t max);

#endif
