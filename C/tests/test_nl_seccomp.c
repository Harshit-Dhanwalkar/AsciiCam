/*
 * tests/test_nl_seccomp.c - lib/nl_seccomp.h
 *
 *   1. build(): argument checks
 *   2. generated program is run by a small BPF interpreter against every
 *      syscall number 0..1999, x32-style numbers and a foreign architecture:
 *      allowed numbers get ALLOW, everything else deny action and every jump
 *      lands inside program
 *   3. in forked children, real kernel filter: an allowed call works, a
 *      forbidden one is killed (SIGSYS) or fails with EPERM, as asked
 *
 * Step 3 runs on Linux x86-64 only and is skipped elsewhere
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/test_nl_seccomp.c lib/nl_mem.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c -Iinclude -Ilib -o /tmp/sc
 */

#include "../lib/nolibc.h"

#include "../lib/nl_seccomp.h"

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

#define PROG_MAX NL_SECCOMP_MAX_INSNS

// Run program on (arch, nr) the way the kernel does. Returns the action, or
// 0xdeadbeef for anything a verifier would reject (bad opcode, jump out of
// program, falling off the end)
static unsigned run_bpf(const nl_sock_filter_t *p, unsigned len, unsigned arch,
                        unsigned nr) {
  unsigned a = 0, pc = 0;
  for (unsigned steps = 0; steps <= len; steps++) {
    if (pc >= len) {
      return 0xdeadbeefU;
    }

    const nl_sock_filter_t *f = &p[pc];
    switch (f->code) {
    case NL_BPF_LD_W_ABS:
      if (f->k == 0) {
        a = nr;
      } else if (f->k == 4) {
        a = arch;
      } else {
        return 0xdeadbeefU;
      }

      pc++;

      break;
    case NL_BPF_JEQ_K:
      pc += 1 + (a == f->k ? f->jt : f->jf);

      break;
    case NL_BPF_JGE_K:
      pc += 1 + (a >= f->k ? f->jt : f->jf);

      break;
    case NL_BPF_RET_K:
      return f->k;
    default:
      return 0xdeadbeefU;
    }
  }

  return 0xdeadbeefU;
}

static int contains(const int *l, unsigned n, int v) {
  for (unsigned i = 0; i < n; i++) {
    if (l[i] == v) {
      return 1;
    }
  }

  return 0;
}

#ifdef NL_AUDIT_ARCH
static void check_semantics(const int *allow, unsigned n, unsigned deny,
                            const char *tag) {
  nl_sock_filter_t prog[PROG_MAX];
  unsigned len = nl_seccomp_build(prog, PROG_MAX, allow, n, deny);
  CHECK(len == n + 7, "%s: length %d", tag, (int)len);
  if (len == 0) {
    return;
  }

  int bad = 0;
  for (unsigned nr = 0; nr < 2000; nr++) {
    unsigned want = contains(allow, n, (int)nr) ? NL_SECCOMP_ALLOW : deny;
    unsigned got = run_bpf(prog, len, NL_AUDIT_ARCH, nr);
    if (got != want && bad++ < 5) {
      fprintf(stderr, "  [%s] nr %u: got %x, want %x\n", tag, nr, got, want);
    }
  }

  CHECK(bad == 0, "%s: %d wrong verdicts", tag, bad);

  // x32-style numbers, with and without an allowed number in the low bits
  for (unsigned i = 0; i < n; i++) {
    unsigned got =
        run_bpf(prog, len, NL_AUDIT_ARCH, 0x40000000U | (unsigned)allow[i]);
    CHECK(got == NL_SECCOMP_KILL, "%s: x32-style %d got %x", tag, allow[i],
          got);
  }

  CHECK(run_bpf(prog, len, NL_AUDIT_ARCH, 0x40000000U) == NL_SECCOMP_KILL,
        "%s: 0x40000000", tag);
  CHECK(run_bpf(prog, len, NL_AUDIT_ARCH, 0xffffffffU) == NL_SECCOMP_KILL,
        "%s: 0xffffffff", tag);

  // foreign architecture: numbers mean something else there, so even an
  // "allowed" one must be refused (and never with a polite errno)
  for (unsigned i = 0; i < n; i++) {
    CHECK(run_bpf(prog, len, NL_AUDIT_ARCH ^ 0x1000U, (unsigned)allow[i]) ==
              NL_SECCOMP_KILL,
          "%s: foreign arch, nr %d", tag, allow[i]);
  }

  CHECK(run_bpf(prog, len, 0, 0) == NL_SECCOMP_KILL, "%s: arch 0", tag);
}

static void semantics(void) {
  int none[1] = {0};
  check_semantics(none, 0, NL_SECCOMP_KILL, "deny all");
  check_semantics(none, 0, NL_SECCOMP_ERRNO(1), "deny all errno");

  int few[] = {0, 1, 60, 231, 202, 999, 1999};
  check_semantics(few, 7, NL_SECCOMP_KILL, "few kill");
  check_semantics(few, 7, NL_SECCOMP_ERRNO(13), "few errno");

  // the largest allowed list: jumps at their 8-bit limit
  int many[NL_SECCOMP_MAX_ALLOW];
  for (int i = 0; i < NL_SECCOMP_MAX_ALLOW; i++) {
    many[i] = i * 7 % 1999;
  }

  check_semantics(many, NL_SECCOMP_MAX_ALLOW, NL_SECCOMP_KILL, "max list");
}
#endif

static void build_args(void) {
  nl_sock_filter_t prog[PROG_MAX];
  int ok[] = {1, 2, 3};
  int neg[] = {1, -1, 3};
  int big[NL_SECCOMP_MAX_ALLOW + 1] = {0};

#ifdef NL_AUDIT_ARCH
  CHECK(nl_seccomp_build(prog, PROG_MAX, ok, 3, NL_SECCOMP_KILL) == 10, "ok");
  CHECK(nl_seccomp_build(prog, 9, ok, 3, NL_SECCOMP_KILL) == 0,
        "cap too small");
  CHECK(nl_seccomp_build(prog, 10, ok, 3, NL_SECCOMP_KILL) == 10, "cap exact");
  CHECK(nl_seccomp_build(prog, PROG_MAX, neg, 3, NL_SECCOMP_KILL) == 0,
        "negative number");
  CHECK(nl_seccomp_build(prog, PROG_MAX + 8, big, NL_SECCOMP_MAX_ALLOW + 1,
                         NL_SECCOMP_KILL) == 0,
        "list over limit");
  CHECK(nl_seccomp_build(prog, PROG_MAX, (const int *)0, 3, NL_SECCOMP_KILL) ==
            0,
        "NULL list with n > 0");
  CHECK(nl_seccomp_install((const nl_sock_filter_t *)0, 5) < 0, "NULL program");
  CHECK(nl_seccomp_install(prog, 0) < 0, "empty program");
#else
  CHECK(nl_seccomp_build(prog, PROG_MAX, ok, 3, NL_SECCOMP_KILL) == 0,
        "unsupported arch must refuse");
  (void)neg, (void)big;
#endif
}

#if defined(__x86_64__)
#define NR_WRITE 1
#define NR_GETPID 39
#define NR_OPENAT 257
#define NR_PRCTL 157

// Run fn in a child. Returns exit code, or 1000 + signal number
static int in_child(void (*fn)(void)) {
  long pid = __sc6(SYS_fork, 0, 0, 0, 0, 0, 0);
  if (pid == 0) {
    __sc1(37 /* alarm */, 5);
    fn();
    __sc1(SYS_exit_group, 90);
  }

  int status = 0;
  __sc4(SYS_wait4, pid, (long)&status, 0, 0);
  if ((status & 0x7f) != 0) {
    return 1000 + (status & 0x7f);
  }

  return (status >> 8) & 0xff;
}

static const int base_allow[] = {NR_WRITE, 231 /* exit_group */, NR_PRCTL};

// allowed calls work; getpid is not allowed -> SIGSYS
static void child_kill(void) {
  if (nl_seccomp_lockdown(base_allow, 3, NL_SECCOMP_KILL) < 0) {
    __sc1(SYS_exit_group, 91);
  }

  long w = __sc3(NR_WRITE, 2, (long)"", 0);
  if (w != 0) {
    __sc1(SYS_exit_group, 92);
  }

  __sc1(NR_GETPID, 0);
  __sc1(SYS_exit_group, 93); // not reached: killed above
}

// getpid is refused with EPERM, the process lives on
static void child_errno(void) {
  if (nl_seccomp_lockdown(base_allow, 3, NL_SECCOMP_ERRNO(1)) < 0) {
    __sc1(SYS_exit_group, 91);
  }

  long r = __sc1(NR_GETPID, 0);
  if (r != -1) {
    __sc1(SYS_exit_group, 92);
  }

  long o = __sc6(NR_OPENAT, -100, (long)"/dev/null", 0, 0, 0, 0);
  if (o != -1) {
    __sc1(SYS_exit_group, 93);
  }

  __sc1(SYS_exit_group, 7);
}

// no_new_privs is on afterwards, and nothing can take the filter off again
static void child_sticky(void) {
  if (nl_seccomp_lockdown(base_allow, 3, NL_SECCOMP_ERRNO(1)) < 0) {
    __sc1(SYS_exit_group, 91);
  }

  if (__sc6(NR_PRCTL, 39 /* PR_GET_NO_NEW_PRIVS */, 0, 0, 0, 0, 0) != 1) {
    __sc1(SYS_exit_group, 92);
  }

  // a second filter may only narrow things: allow everything, still denied
  int all[] = {NR_GETPID, NR_WRITE, 231, NR_PRCTL, 317 /* seccomp */};
  if (nl_seccomp_lockdown(all, 5, NL_SECCOMP_KILL) >= 0) {
    // seccomp() itself was not allowed by the first filter, so this must fail
    __sc1(SYS_exit_group, 93);
  }

  __sc1(SYS_exit_group, 8);
}

static void kernel_tests(void) {
  int r = in_child(child_kill);
  CHECK(r == 1031, "kill: expected SIGSYS (1031), got %d", r);
  r = in_child(child_errno);
  CHECK(r == 7, "errno: got %d", r);
  r = in_child(child_sticky);
  CHECK(r == 8, "sticky: got %d", r);
}
#else
static void kernel_tests(void) {
  fprintf(stderr, "kernel tests skipped: not x86-64\n");
}
#endif

int main(void) {
  build_args();
#ifdef NL_AUDIT_ARCH
  semantics();
#endif
  kernel_tests();

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
