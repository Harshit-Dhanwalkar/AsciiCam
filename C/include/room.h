#ifndef ROOM_H
#define ROOM_H

/*
 * room - relay of a video call (`asciicam --room [host:]port`)
 *
 * Takes pictures and chat from every client and hands them to others, see
 * callproto.h. It never decodes a picture: it reads only key/delta flag, so
 * that a client which missed a frame is not given deltas it cannot apply: it
 * gets frames of somebody only from next key on, and that somebody is asked for
 * one (NEED_KEY, at most once a second)
 * Slow clients lose frames, not room
 * No password and no encryption: listen on loopback and reach it through ssh -L
 * (or run it in docker image of this repository)
 */

#include "callproto.h"
#include "nl_types.h"

#define ROOM_MAX 8

typedef struct {
  int fd; // -1 = free slot
  msgbuf_t in;
  outq_t out;
  int joined;
  char name[CALL_NAME_MAX + 1];
  uint8_t synced[ROOM_MAX]; // has a key of sender i, deltas may follow
  long since_ms;            // connected at
  long need_key_ms;         // last NEED_KEY asked of this sender
  long window_ms;           // rate window start
  int frames_in_window;
  int chat_in_window;
} room_client_t;

typedef struct {
  int lfd;
  int port;
  room_client_t cl[ROOM_MAX];
  uint64_t frames_in, frames_out, frames_dropped, chats, kicked;
} room_t;

// Listen (port 0 = any, see room->port). 0 on success
int room_listen(room_t *r, uint32_t ip, int port);

// One round: wait up to timeout_ms for something to do, then accept, read,
// relay and flush
// now_ms is a monotonic clock
// Number of messages handled, Return -1 when room cannot go on
int room_step(room_t *r, int timeout_ms, long now_ms);

// Number of clients that have said hello
int room_members(const room_t *r);

void room_close(room_t *r);

#endif
