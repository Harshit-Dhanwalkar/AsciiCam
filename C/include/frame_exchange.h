#ifndef FRAME_EXCHANGE_H
#define FRAME_EXCHANGE_H

/*
 * frame_exchange.h - latest-wins triple buffer between one producer (capture
 * thread) and one consumer (render loop)
 *
 * Three slots, and at any time each is in at most one role:
 *   writing  slot producer is filling (nobody else touches it)
 *   ready    newest finished frame consumer has not taken yet
 *   reading  slot consumer holds until its next acquire
 *
 * NOTE: Because there are three slots and only two of those roles can be held
 * by "other side" while producer picks one, fx_write_begin() always
 * finds a free slot: producer never waits for consumer, and consumer never
 * sees a half-written frame. If consumer is slower, an unread ready frame
 * is simply replaced (counted in `dropped`)
 *
 * Pure state machine, no I/O: only slot indices change hands under lock;
 * pixel data is copied outside it
 */

#include "nl_thread.h"

#define FX_SLOTS 3

typedef struct {
  nl_mutex_t lock;
  int ready;   // newest published slot, not yet taken; -1 if none
  int reading; // slot consumer holds; -1 if none
  int writing; // slot producer fills; -1 if none
  unsigned long published;
  unsigned long dropped; // published frames replaced before they were taken
} frame_exchange_t;

static inline void fx_init(frame_exchange_t *fx) {
  nl_mutex_init(&fx->lock);
  fx->ready = -1;
  fx->reading = -1;
  fx->writing = -1;
  fx->published = 0;
  fx->dropped = 0;
}

// Producer: pick a slot to fill. Never blocks, never returns a slot
// consumer holds or may take
static inline int fx_write_begin(frame_exchange_t *fx) {
  nl_mutex_lock(&fx->lock);

  int idx = 0;
  while (idx == fx->ready || idx == fx->reading) {
    idx++;
  }

  fx->writing = idx;
  nl_mutex_unlock(&fx->lock);

  return idx;
}

// Producer: frame in slot from fx_write_begin() is complete
static inline void fx_write_publish(frame_exchange_t *fx) {
  nl_mutex_lock(&fx->lock);

  if (fx->ready >= 0) {
    fx->dropped++;
  }

  fx->ready = fx->writing;
  fx->writing = -1;
  fx->published++;

  nl_mutex_unlock(&fx->lock);
}

// Producer: slot from fx_write_begin() holds nothing usable (bad frame)
static inline void fx_write_abort(frame_exchange_t *fx) {
  nl_mutex_lock(&fx->lock);
  fx->writing = -1;
  nl_mutex_unlock(&fx->lock);
}

// Consumer: take newest unseen frame. Returns its slot, which stays valid
// until next successful call, or -1 if nothing new was published
static inline int fx_read_acquire(frame_exchange_t *fx) {
  nl_mutex_lock(&fx->lock);

  int idx = fx->ready;
  if (idx >= 0) {
    fx->reading = idx;
    fx->ready = -1;
  }

  nl_mutex_unlock(&fx->lock);

  return idx;
}

#endif
