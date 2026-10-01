/*
 * tests/test_nl_thread.c - stress tests for futex/clone threading layer
 * (nl_mutex_t, nl_cond_t, nl_thread_t)
 *
 * NOTE: Linux nolibc only
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) \
 *     -ffreestanding -fno-builtin -fno-stack-protector -nostdlib -static \
 *     test_nl_thread.c ../lib/nl_printf.c ../lib/nl_errno.c ../lib/nl_alloc.c \
 *     ../lib/nl_start.c -I../include -I../lib -o test_nl_thread
 */
#include "../lib/nolibc.h"
#include "nl_thread.h"

static int fails, total;
#define CHECK(c, ...)                                                          \
  do {                                                                         \
    total++;                                                                   \
    if (!(c)) {                                                                \
      fails++;                                                                 \
      fprintf(stderr, "FAIL line %d: ", __LINE__);                             \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)

// 1. mutex: N threads x M increments must sum exactly
#define NT 8
#define ITERS 20000
static nl_mutex_t g_m = NL_MUTEX_INITIALIZER;
static long g_counter;

static void *incr_thread(void *arg) {
  (void)arg;
  for (int i = 0; i < ITERS; i++) {
    nl_mutex_lock(&g_m);
    long v = g_counter;

    __asm__ volatile("" ::: "memory"); // widen the race window
    g_counter = v + 1;

    nl_mutex_unlock(&g_m);
  }

  return (void *)0;
}

static void test_mutex(void) {
  nl_thread_t th[NT];
  g_counter = 0;
  for (int i = 0; i < NT; i++) {
    CHECK(nl_thread_create(&th[i], incr_thread, (void *)0) == 0, "create %d",
          i);
  }

  for (int i = 0; i < NT; i++) {
    nl_thread_join(&th[i]);
  }

  CHECK(g_counter == (long)NT * ITERS, "counter=%ld expected %ld", g_counter,
        (long)NT * ITERS);
}

// 2. thread receives its argument and runs on its own stack
struct argbox {
  int in;
  int out;
};

static void *arg_thread(void *p) {
  struct argbox *a = p;
  volatile char big[64 * 1024]; // prove the stack is real and large enough/
  for (unsigned i = 0; i < sizeof(big); i += 4096) {
    big[i] = (char)i;
  }

  a->out = a->in * 3 + 1 + (big[0] - big[0]);

  return (void *)0;
}

static void test_args_and_stack(void) {
  struct argbox a = {14, 0};
  nl_thread_t t;

  CHECK(nl_thread_create(&t, arg_thread, &a) == 0, "create");

  nl_thread_join(&t);

  CHECK(a.out == 43, "arg not delivered, out=%d", a.out);
}

// 3. many create/join cycles (leaks / bad munmap show up as ENOMEM)
static void *nop_thread(void *arg) {
  (*(int *)arg)++;

  return (void *)0;
}

static void test_cycles(void) {
  int ran = 0, created = 0;
  for (int i = 0; i < 2000; i++) {
    nl_thread_t t;
    int hits = 0;
    if (nl_thread_create(&t, nop_thread, &hits) != 0) {
      break;
    }

    created++;
    nl_thread_join(&t);
    ran += hits;
  }

  CHECK(created == 2000, "only created %d/2000 threads", created);
  CHECK(ran == created, "ran %d of %d", ran, created);
}

// 4. bounded producer/consumer through a cond var: no lost items
#define RING 4
#define ITEMS 50000

static struct {
  nl_mutex_t m;
  nl_cond_t not_full, not_empty;
  int buf[RING];
  int head, tail, count;
  long sum_in, sum_out;
  int consumed;
} q = {NL_MUTEX_INITIALIZER,
       NL_COND_INITIALIZER,
       NL_COND_INITIALIZER,
       {0},
       0,
       0,
       0,
       0,
       0,
       0};

static void *producer(void *arg) {
  (void)arg;

  for (int i = 1; i <= ITEMS; i++) {
    nl_mutex_lock(&q.m);

    while (q.count == RING) {
      nl_cond_wait(&q.not_full, &q.m);
    }

    q.buf[q.tail] = i;
    q.tail = (q.tail + 1) % RING;
    q.count++;
    q.sum_in += i;

    nl_cond_signal(&q.not_empty);
    nl_mutex_unlock(&q.m);
  }

  return (void *)0;
}

static void *consumer(void *arg) {
  (void)arg;

  for (int i = 0; i < ITEMS; i++) {
    nl_mutex_lock(&q.m);

    while (q.count == 0) {
      nl_cond_wait(&q.not_empty, &q.m);
    }

    int v = q.buf[q.head];
    q.head = (q.head + 1) % RING;
    q.count--;
    q.sum_out += v;
    q.consumed++;

    nl_cond_signal(&q.not_full);
    nl_mutex_unlock(&q.m);
  }

  return (void *)0;
}

static void test_prodcons(void) {
  nl_thread_t p, c;

  CHECK(nl_thread_create(&c, consumer, (void *)0) == 0, "create consumer");
  CHECK(nl_thread_create(&p, producer, (void *)0) == 0, "create producer");

  nl_thread_join(&p);
  nl_thread_join(&c);
  CHECK(q.consumed == ITEMS, "consumed %d of %d", q.consumed, ITEMS);
  CHECK(q.sum_in == q.sum_out, "sum mismatch in=%ld out=%ld", q.sum_in,
        q.sum_out);
}

// 5. broadcast wakes every waiter
static nl_mutex_t bm = NL_MUTEX_INITIALIZER;
static nl_cond_t bc = NL_COND_INITIALIZER;

static int bgo;
static int bwoke;
static int bwaiting;

static void *waiter(void *arg) {
  (void)arg;

  nl_mutex_lock(&bm);
  bwaiting++;
  while (!bgo) {
    nl_cond_wait(&bc, &bm);
  }

  bwoke++;
  nl_mutex_unlock(&bm);

  return (void *)0;
}

static void test_broadcast(void) {
  nl_thread_t th[6];
  for (int i = 0; i < 6; i++) {
    nl_thread_create(&th[i], waiter, (void *)0);
  }

  for (;;) { // wait until all six are parked
    nl_mutex_lock(&bm);
    int n = bwaiting;

    nl_mutex_unlock(&bm);
    if (n == 6) {
      break;
    }

    nl_usleep(1000);
  }

  nl_usleep(20000);
  nl_mutex_lock(&bm);

  bgo = 1;
  nl_cond_broadcast(&bc);
  nl_mutex_unlock(&bm);
  for (int i = 0; i < 6; i++) {
    nl_thread_join(&th[i]);
  }

  CHECK(bwoke == 6, "broadcast woke %d of 6", bwoke);
}

// 6. stack has a PROT_NONE guard page at its low end
static volatile int g_park;

static void *park_thread(void *arg) {
  (void)arg;

  while (!__atomic_load_n(&g_park, __ATOMIC_ACQUIRE)) {
    nl_usleep(500);
  }

  return (void *)0;
}

static void test_guard_page(void) {
  nl_thread_t t;
  g_park = 0;

  CHECK(nl_thread_create(&t, park_thread, (void *)0) == 0, "create");

  int fd = nl_open("/dev/zero", O_RDONLY, 0);
  CHECK(fd >= 0, "open /dev/zero");
  if (fd >= 0) {
    long guard = nl_read(fd, t.stack_base, 1);
    long body = nl_read(fd, (char *)t.stack_base + NL_THREAD_GUARD_SIZE, 1);

    CHECK(guard < 0, "guard page is writable (read returned %ld)", guard);
    CHECK(body == 1, "stack body not writable (read returned %ld)", body);

    nl_close(fd);
  }

  __atomic_store_n(&g_park, 1, __ATOMIC_RELEASE);

  nl_thread_join(&t);
}

int main(void) {
  test_mutex();
  test_args_and_stack();
  test_cycles();
  test_prodcons();
  test_broadcast();
  test_guard_page();

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
