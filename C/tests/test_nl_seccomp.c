/*
 * tests/test_nl_seccomp.c - lib/nl_seccomp.h
 *
 *   1. build(): argument checks, including argument rules
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

// Run program on (arch, nr, args) way kernel does. Returns action, or
// 0xdeadbeef for anything a verifier would reject (bad opcode, jump out of
// program, falling off end)
// Only low word of an argument may be read: that is all builder promises to
// test
static unsigned run_bpf_args(const nl_sock_filter_t *p, unsigned len,
                             unsigned arch, unsigned nr,
                             const unsigned long long *args) {
  unsigned a = 0;
  unsigned pc = 0;
  for (unsigned steps = 0; steps <= len; steps++) {
    if (pc >= len) {
      return 0xdeadbeefU;
    }

    const nl_sock_filter_t *f = &p[pc];
    switch (f->code) {
    case NL_BPF_LD_W_ABS: {
      if (f->k == 0) {
        a = nr;
      } else if (f->k == 4) {
        a = arch;
      } else if (f->k >= 16 && f->k < 64 && (f->k & 7) == 0) {
        a = (unsigned)args[(f->k - 16) / 8];
      } else {
        return 0xdeadbeefU;
      }

      pc++;

      break;
    }
    case NL_BPF_JEQ_K: {
      pc += 1 + (a == f->k ? f->jt : f->jf);

      break;
    }
    case NL_BPF_JSET_K: {
      pc += 1 + ((a & f->k) ? f->jt : f->jf);

      break;
    }
    case NL_BPF_JA: {
      pc += 1 + f->k;

      break;
    }
    case NL_BPF_JGE_K: {
      pc += 1 + (a >= f->k ? f->jt : f->jf);

      break;
    }
    case NL_BPF_RET_K: {
      return f->k;
    }
    default: {
      return 0xdeadbeefU;
    }
    }
  }

  return 0xdeadbeefU;
}

static const unsigned long long no_args[6] = {0, 0, 0, 0, 0, 0};

static unsigned run_bpf(const nl_sock_filter_t *p, unsigned len, unsigned arch,
                        unsigned nr) {
  return run_bpf_args(p, len, arch, nr, no_args);
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

  // x32-style numbers, with and without an allowed number in low bits
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

  // Foreign architecture: numbers mean something else there, so even an
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

  // Largest allowed list: jumps at their 8-bit limit
  int many[NL_SECCOMP_MAX_ALLOW];
  for (int i = 0; i < NL_SECCOMP_MAX_ALLOW; i++) {
    many[i] = i * 7 % 1999;
  }

  check_semantics(many, NL_SECCOMP_MAX_ALLOW, NL_SECCOMP_KILL, "max list");
}
#endif

#ifdef NL_AUDIT_ARCH
// Allowed numbers, argument rules, and what each must do for every number and
// for argument values with and without forbidden bits
static void check_rules(const int *allow, unsigned n,
                        const nl_seccomp_rule_t *r, unsigned nr, unsigned deny,
                        const char *tag) {
  nl_sock_filter_t prog[PROG_MAX];
  unsigned len = nl_seccomp_build_ex(prog, PROG_MAX, allow, n, r, nr, deny);
  CHECK(len == n + 5 * nr + 7, "%s: length %d", tag, (int)len);
  if (len == 0) {
    return;
  }

  static const unsigned long long vals[] = {0,
                                            1,
                                            2,
                                            3,
                                            4,
                                            5,
                                            6,
                                            7,
                                            8,
                                            0x80000000ULL,
                                            0xffffffffULL,
                                            0x100000000ULL,
                                            0x100000004ULL,
                                            0xffffffff00000000ULL,
                                            ~0ULL};
  int bad = 0;
  for (unsigned num = 0; num < 600; num++) {
    for (unsigned vi = 0; vi < sizeof(vals) / sizeof(*vals); vi++) {
      unsigned long long args[6];
      for (int k = 0; k < 6; k++) {
        args[k] = vals[(vi + (unsigned)k * 3) % (sizeof(vals) / sizeof(*vals))];
      }

      unsigned want = deny;
      if (contains(allow, n, (int)num)) {
        want = NL_SECCOMP_ALLOW;
      } else {
        for (unsigned i = 0; i < nr; i++) {
          if (r[i].nr == (int)num) {
            want = ((unsigned)args[r[i].arg] & r[i].forbid) ? deny
                                                            : NL_SECCOMP_ALLOW;
            break;
          }
        }
      }

      unsigned got = run_bpf_args(prog, len, NL_AUDIT_ARCH, num, args);
      if (got != want && bad++ < 5) {
        fprintf(stderr, "  [%s] nr %u args0=%llx: got %x want %x\n", tag, num,
                args[0], got, want);
      }
    }
  }

  CHECK(bad == 0, "%s: %d wrong verdicts", tag, bad);

  // x32-style numbers and a foreign architecture are refused whatever rule
  for (unsigned i = 0; i < nr; i++) {
    CHECK(run_bpf_args(prog, len, NL_AUDIT_ARCH,
                       0x40000000U | (unsigned)r[i].nr,
                       no_args) == NL_SECCOMP_KILL,
          "%s: x32 form of rule %d", tag, (int)i);
    CHECK(run_bpf_args(prog, len, NL_AUDIT_ARCH ^ 0x1000U, (unsigned)r[i].nr,
                       no_args) == NL_SECCOMP_KILL,
          "%s: foreign arch, rule %d", tag, (int)i);
  }
}

static void rules(void) {
  int base[] = {0, 1, 231};
  nl_seccomp_rule_t one[] = {{9, 2, 4}};
  check_rules(base, 3, one, 1, NL_SECCOMP_KILL, "mmap no exec, kill");
  check_rules(base, 3, one, 1, NL_SECCOMP_ERRNO(1), "mmap no exec, errno");
  check_rules((const int *)0, 0, one, 1, NL_SECCOMP_KILL, "rule only");

  nl_seccomp_rule_t several[] = {{9, 2, 4},
                                 {10, 2, 4},
                                 {100, 5, 0x80000000U},
                                 {101, 0, 0xffffffffU},
                                 {102, 3, 1}};
  check_rules(base, 3, several, 5, NL_SECCOMP_ERRNO(13), "several rules");

  // A number in both lists: plain entry comes first and wins
  nl_seccomp_rule_t dup[] = {{1, 0, 0xffffffffU}};
  check_rules(base, 3, dup, 1, NL_SECCOMP_KILL, "plain wins over rule");

  // Budget: 48 rules is 240, limit
  nl_seccomp_rule_t lots[NL_SECCOMP_MAX_ALLOW / 5];
  for (int i = 0; i < NL_SECCOMP_MAX_ALLOW / 5; i++) {
    lots[i].nr = 3 + i * 11;
    lots[i].arg = (unsigned)(i % 6);
    lots[i].forbid = 1u << (i % 31);
  }

  check_rules((const int *)0, 0, lots, NL_SECCOMP_MAX_ALLOW / 5,
              NL_SECCOMP_KILL, "maximum rules");
}
#endif

static void rule_args(void) {
  nl_sock_filter_t prog[PROG_MAX];
  int ok[] = {1, 2};
  nl_seccomp_rule_t good[] = {{9, 2, 4}};
  nl_seccomp_rule_t bad_arg[] = {{9, 6, 4}};
  nl_seccomp_rule_t no_bits[] = {{9, 2, 0}};
  nl_seccomp_rule_t neg[] = {{-1, 2, 4}};
  nl_seccomp_rule_t lots[NL_SECCOMP_MAX_ALLOW / 5 + 1];
  for (unsigned i = 0; i < sizeof(lots) / sizeof(*lots); i++) {
    lots[i].nr = (int)i;
    lots[i].arg = 0;
    lots[i].forbid = 1;
  }

#ifdef NL_AUDIT_ARCH
  CHECK(nl_seccomp_build_ex(prog, PROG_MAX, ok, 2, good, 1, NL_SECCOMP_KILL) ==
            14,
        "rule ok");
  CHECK(nl_seccomp_build_ex(prog, 13, ok, 2, good, 1, NL_SECCOMP_KILL) == 0,
        "cap too small for rule");
  CHECK(nl_seccomp_build_ex(prog, PROG_MAX, ok, 2, bad_arg, 1,
                            NL_SECCOMP_KILL) == 0,
        "arg 6 accepted");
  CHECK(nl_seccomp_build_ex(prog, PROG_MAX, ok, 2, no_bits, 1,
                            NL_SECCOMP_KILL) == 0,
        "empty forbid mask accepted");
  CHECK(nl_seccomp_build_ex(prog, PROG_MAX, ok, 2, neg, 1, NL_SECCOMP_KILL) ==
            0,
        "negative rule number accepted");
  CHECK(nl_seccomp_build_ex(prog, PROG_MAX, ok, 2, (const nl_seccomp_rule_t *)0,
                            1, NL_SECCOMP_KILL) == 0,
        "NULL rules with nr > 0");
  CHECK(nl_seccomp_build_ex(prog, PROG_MAX + 40, (const int *)0, 0, lots,
                            NL_SECCOMP_MAX_ALLOW / 5 + 1, NL_SECCOMP_KILL) == 0,
        "rule budget exceeded");
  CHECK(nl_seccomp_build_ex(prog, PROG_MAX + 40, ok, 2, lots,
                            NL_SECCOMP_MAX_ALLOW / 5, NL_SECCOMP_KILL) == 0,
        "list + rules budget exceeded");
#else
  (void)prog, (void)ok, (void)good, (void)bad_arg, (void)no_bits, (void)neg;
  (void)lots;
#endif
}

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

// Run fn in a child
// Returns exit code, or 1000 + signal number
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

// Allowed calls work; getpid is not allowed -> SIGSYS
static void child_kill(void) {
  if (nl_seccomp_lockdown(base_allow, 3, NL_SECCOMP_KILL) < 0) {
    __sc1(SYS_exit_group, 91);
  }

  long w = __sc3(NR_WRITE, 2, (long)"", 0);
  if (w != 0) {
    __sc1(SYS_exit_group, 92);
  }

  __sc1(NR_GETPID, 0);
  __sc1(SYS_exit_group, 93); // Not reached: killed above
}

// getpid is refused with EPERM, process lives on
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

// no_new_privs is on afterwards, and nothing can take filter off again
static void child_sticky(void) {
  if (nl_seccomp_lockdown(base_allow, 3, NL_SECCOMP_ERRNO(1)) < 0) {
    __sc1(SYS_exit_group, 91);
  }

  if (__sc6(NR_PRCTL, 39 /* PR_GET_NO_NEW_PRIVS */, 0, 0, 0, 0, 0) != 1) {
    __sc1(SYS_exit_group, 92);
  }

  // A second filter may only narrow things: allow everything, still denied
  int all[] = {NR_GETPID, NR_WRITE, 231, NR_PRCTL, 317 /* seccomp */};
  if (nl_seccomp_lockdown(all, 5, NL_SECCOMP_KILL) >= 0) {
    // seccomp() itself was not allowed by first filter, so this must fail
    __sc1(SYS_exit_group, 93);
  }

  __sc1(SYS_exit_group, 8);
}

#define NR_MMAP 9
#define MAP_PRIV_ANON 0x22

static const nl_seccomp_rule_t mmap_rule[] = {{NR_MMAP, 2, 4 /* PROT_EXEC */}};

static long try_mmap(long prot) {
  return __sc6(NR_MMAP, 0, 4096, prot, MAP_PRIV_ANON, -1, 0);
}

// mmap without PROT_EXEC works, anything executable is refused with EPERM
static void child_mmap_errno(void) {
  if (nl_seccomp_lockdown_ex(base_allow, 3, mmap_rule, 1, NL_SECCOMP_ERRNO(1)) <
      0) {
    __sc1(SYS_exit_group, 91);
  }

  long rw = try_mmap(3);
  if (rw < 4096 && rw > -4096) { // An error number, not an address
    __sc1(SYS_exit_group, 92);
  }

  if (try_mmap(0) < 4096 && try_mmap(0) > -4096) { // PROT_NONE is fine too
    __sc1(SYS_exit_group, 93);
  }

  if (try_mmap(7) != -1 || try_mmap(5) != -1 || try_mmap(4) != -1) {
    __sc1(SYS_exit_group, 94);
  }

  // Upper half of argument is not what kernel reads: still refused
  if (try_mmap(4L | (1L << 40)) != -1) {
    __sc1(SYS_exit_group, 95);
  }

  __sc1(SYS_exit_group, 9);
}

// With kill as default, an executable mapping ends process
static void child_mmap_kill(void) {
  if (nl_seccomp_lockdown_ex(base_allow, 3, mmap_rule, 1, NL_SECCOMP_KILL) <
      0) {
    __sc1(SYS_exit_group, 91);
  }

  if (try_mmap(3) >= 0 && try_mmap(3) < 4096) {
    __sc1(SYS_exit_group, 92);
  }

  try_mmap(7);
  __sc1(SYS_exit_group, 93); // Not reached
}

static void kernel_tests(void) {
  int r = in_child(child_mmap_errno);
  CHECK(r == 9, "mmap rule (errno): got %d", r);
  r = in_child(child_mmap_kill);
  CHECK(r == 1031, "mmap rule (kill): expected SIGSYS (1031), got %d", r);
  r = in_child(child_kill);
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
  rule_args();
#ifdef NL_AUDIT_ARCH
  semantics();
  rules();
#endif
  kernel_tests();

  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
