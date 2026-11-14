#ifndef CALL_H
#define CALL_H

/*
 * call - client side of a video call (`asciicam --call [host:]port`)
 *
 * Holds what room told us: who is there, their last picture, chat
 * Our own pictures go out as cellcodec frames, a key at first, when asked, and
 * every CALL_KEY_EVERY frames, deltas against last frame queued otherwise
 * Nothing here blocks or touches terminal or camera: call_ui.c does that, tests
 * drive this directly
 */

#include "callproto.h"
#include "cellcodec.h"
#include "room.h"

#define CALL_KEY_EVERY 40
#define CALL_LOG_LINES 64
#define CALL_LOG_W 240

typedef struct {
  int present;
  char name[CALL_NAME_MAX + 1];
  cc_grid_t grid;
  int have_picture; // a key was received
} call_peer_t;

typedef struct {
  int fd; // -1 = not connected
  msgbuf_t in;
  outq_t out;
  int id; // -1 until WELCOME
  int closed;
  int full; // room said it is full
  char name[CALL_NAME_MAX + 1];
  call_peer_t peer[ROOM_MAX];
  cc_grid_t local; // our last picture
  cc_grid_t sent;  // what receivers hold of us (last frame queued)
  int have_sent;
  int need_key;
  int since_key;
  char log[CALL_LOG_LINES][CALL_LOG_W];
  int log_count;                 // lines ever written
  int dirty;                     // screen should be redrawn
  int sig;                       // layout of the last compose
  cc_grid_t shown[ROOM_MAX + 1]; // screen holds of each tile (0: us)
  uint64_t frames_sent;
  uint64_t frames_skipped;
  uint64_t frames_received;
  uint64_t bytes_in;     // bytes read from room
  int rate_in, rate_out; // bytes per second, set by caller
  int video_off;         // send a "camera off" card, not picture
} call_t;

// Picture shown in place of sender's camera when video is off: cols x rows
// cells, centred text
// Return bytes written, 0 when it does not fit
size_t call_off_card(char *out, size_t cap, int cols, int rows);

// Connect to "host:port" and say hello
// Return 0 on success
int call_open(call_t *c, const char *hostport, const char *name);

// Same on an already connected socket (tests)
int call_open_fd(call_t *c, int fd, const char *name);

// Read what came, handle it, send what is queued
// Return 0 ok, -1 when connection is gone (c->closed is set, see c->full)
int call_pump(call_t *c);

// Our picture: an escape frame as renderer makes it (position from 1,1)
// 0 sent, 1 skipped (link too slow: nothing lost, next one follows), -1 cannot
// be sent (not renderable, or too many cells for a message)
int call_send_picture(call_t *c, const char *ansi, size_t n);

// Chat line
// Return 0 queued, -1 empty or queue full
int call_say(call_t *c, const char *text);

// Chat line n back from newest (0 = newest); NULL when there is none
const char *call_log_line(const call_t *c, int back);

int call_members(const call_t *c); // including us

void call_close(call_t *c);

// Screen for a terminal of term_w x term_h: title, everybody's picture, last
// chat lines and line being typed (typing may be NULL when idle)
// Clears screen first when layout changed since last call
// Bytes written, 0 when cap is too small
size_t call_compose(call_t *c, const char *typing, int term_w, int term_h,
                    char *out, size_t cap);

// Room call_compose may need
size_t call_max_compose(const call_t *c, int term_w, int term_h);

#endif
