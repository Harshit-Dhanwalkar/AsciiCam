#ifndef NL_SECCOMP_H
#define NL_SECCOMP_H

/*
 * nl_seccomp - syscall allow-list filter (seccomp-BPF) without libseccomp
 *
 * NOTE: Meant for code that must not be trusted with whole kernel interface,
 * e.g. a process that only runs a plugin on a frame in shared memory: it needs
 * read/write on a pipe, futex, exit, and nothing else
 *
 *   int allow[] = {SYS_read, SYS_write, SYS_futex, SYS_exit_group};
 *   nl_seccomp_lockdown(allow, 4, NL_SECCOMP_KILL);
 *
 * A syscall can also be allowed only with certain argument bits clear, e.g.
 * mmap without PROT_EXEC (nl_seccomp_rule_t, nl_seccomp_build_ex)
 * Only low 32 bits of argument are tested: right for int flags such as prot,
 * which is all kernel reads of them
 *
 * Program built (instruction numbers):
 *   0  A = arch
 *   1  if A != this build's arch -> kill
 *   2  A = syscall number
 *   3  if A >= 0x40000000 -> kill   (x32 ABI numbers alias real ones on
 *                                    x86-64; a filter on nr alone is bypassed)
 *   4  if A == allow[i] -> allow    (one instruction per allowed syscall)
 *   .  per rule, 5 instructions:    if A == rule.nr
 *                                     A = args[rule.arg]
 *                                     if A & rule.forbid -> deny, else allow
 *   .  deny action                  (anything not listed)
 *   .  kill
 *   .  allow
 *
 * Linux x86-64 and AArch64
 * A filter cannot be removed once installed, and is inherited by children
 * Installing needs no privileges (sets no_new_privs)
 */

#include "nl_syscall.h"

#if defined(__x86_64__)
#define NL_AUDIT_ARCH 0xC000003EU /* AUDIT_ARCH_X86_64 */
#ifndef SYS_prctl
#define SYS_prctl 157
#endif
#ifndef SYS_seccomp
#define SYS_seccomp 317
#endif
#elif defined(__aarch64__)
#define NL_AUDIT_ARCH 0xC00000B7U /* AUDIT_ARCH_AARCH64 */
#ifndef SYS_prctl
#define SYS_prctl 167
#endif
#ifndef SYS_seccomp
#define SYS_seccomp 277
#endif
#endif

// Actions: what happens to a syscall filter reaches a verdict on
#define NL_SECCOMP_KILL 0x80000000U /* SECCOMP_RET_KILL_PROCESS: SIGSYS */
#define NL_SECCOMP_ERRNO(e) (0x00050000U | ((unsigned)(e) & 0xffffU))
#define NL_SECCOMP_ALLOW 0x7fff0000U

// Budget that keeps every jump within 8-bit offsets: allowed numbers + 5 per
// rule <= NL_SECCOMP_MAX_ALLOW
#define NL_SECCOMP_MAX_ALLOW 240
#define NL_SECCOMP_MAX_INSNS (NL_SECCOMP_MAX_ALLOW + 7)

// Allow syscall nr only when (low 32 bits of args[arg]) & forbid == 0
typedef struct {
  int nr;
  unsigned arg;    /* 0..5 */
  unsigned forbid; /* nonzero */
} nl_seccomp_rule_t;

// Same layout as kernel's struct sock_filter / sock_fprog
typedef struct {
  unsigned short code;
  unsigned char jt, jf;
  unsigned int k;
} nl_sock_filter_t;

typedef struct {
  unsigned short len;
  const nl_sock_filter_t *filter;
} nl_sock_fprog_t;

#define NL_BPF_LD_W_ABS 0x20 /* BPF_LD  | BPF_W   | BPF_ABS */
#define NL_BPF_JEQ_K 0x15    /* BPF_JMP | BPF_JEQ | BPF_K */
#define NL_BPF_JGE_K 0x35    /* BPF_JMP | BPF_JGE | BPF_K */
#define NL_BPF_JSET_K 0x45   /* BPF_JMP | BPF_JSET | BPF_K */
#define NL_BPF_JA 0x05       /* BPF_JMP | BPF_JA */
#define NL_BPF_RET_K 0x06    /* BPF_RET | BPF_K */

static inline nl_sock_filter_t nl_bpf_(unsigned short code, unsigned char jt,
                                       unsigned char jf, unsigned int k) {
  nl_sock_filter_t f;
  f.code = code;
  f.jt = jt;
  f.jf = jf;
  f.k = k;

  return f;
}

// Fill out[0..cap) with a program that allows exactly syscalls in allow[0..n)
// and those of rules[0..nr) when their argument test passes and gives every
// other one `deny` (NL_SECCOMP_KILL or NL_SECCOMP_ERRNO(e))
// Returns instruction count, or 0 if n + 5 * nr is over NL_SECCOMP_MAX_ALLOW, a
// number is negative, a rule has arg > 5 or forbid == 0, cap is too small, or
// this architecture is not supported. n == nr == 0 is valid: all denied
static inline unsigned nl_seccomp_build_ex(nl_sock_filter_t *out, unsigned cap,
                                           const int *allow, unsigned n,
                                           const nl_seccomp_rule_t *rules,
                                           unsigned nr, unsigned deny) {
#ifdef NL_AUDIT_ARCH
  if (n > NL_SECCOMP_MAX_ALLOW || nr > NL_SECCOMP_MAX_ALLOW ||
      n + 5 * nr > NL_SECCOMP_MAX_ALLOW || (n && !allow) || (nr && !rules)) {
    return 0;
  }

  unsigned len = n + 5 * nr + 7;
  if (cap < len) {
    return 0;
  }

  for (unsigned i = 0; i < n; i++) {
    if (allow[i] < 0) {
      return 0;
    }
  }

  for (unsigned i = 0; i < nr; i++) {
    if (rules[i].nr < 0 || rules[i].arg > 5 || rules[i].forbid == 0) {
      return 0;
    }
  }

  unsigned base_r = 4 + n;
  unsigned deny_at = base_r + 5 * nr;
  unsigned kill_at = deny_at + 1;
  unsigned allow_at = deny_at + 2;
  out[0] = nl_bpf_(NL_BPF_LD_W_ABS, 0, 0, 4); // seccomp_data.arch
  out[1] =
      nl_bpf_(NL_BPF_JEQ_K, 0, (unsigned char)(kill_at - 2), NL_AUDIT_ARCH);
  out[2] = nl_bpf_(NL_BPF_LD_W_ABS, 0, 0, 0); // seccomp_data.nr
  out[3] = nl_bpf_(NL_BPF_JGE_K, (unsigned char)(kill_at - 4), 0, 0x40000000U);
  for (unsigned i = 0; i < n; i++) {
    out[4 + i] = nl_bpf_(NL_BPF_JEQ_K, (unsigned char)(allow_at - (4 + i) - 1),
                         0, (unsigned)allow[i]);
  }

  // A still holds syscall number here and stays so on every path that continues
  // to next rule (only matching path loads an argument)
  for (unsigned i = 0; i < nr; i++) {
    unsigned at = base_r + 5 * i;
    out[at] = nl_bpf_(NL_BPF_JEQ_K, 0, 4, (unsigned)rules[i].nr); /* skip 4 */
    out[at + 1] = nl_bpf_(NL_BPF_LD_W_ABS, 0, 0, 16 + 8 * rules[i].arg);
    out[at + 2] = nl_bpf_(NL_BPF_JSET_K, 0, 1, rules[i].forbid);
    out[at + 3] = nl_bpf_(NL_BPF_JA, 0, 0, deny_at - (at + 3) - 1);
    out[at + 4] = nl_bpf_(NL_BPF_JA, 0, 0, allow_at - (at + 4) - 1);
  }

  out[deny_at] = nl_bpf_(NL_BPF_RET_K, 0, 0, deny);
  out[kill_at] = nl_bpf_(NL_BPF_RET_K, 0, 0, NL_SECCOMP_KILL);
  out[allow_at] = nl_bpf_(NL_BPF_RET_K, 0, 0, NL_SECCOMP_ALLOW);

  return len;
#else
  (void)out, (void)cap, (void)allow, (void)n, (void)rules, (void)nr;
  (void)deny;

  return 0;
#endif
}

// Plain list, no argument rules
static inline unsigned nl_seccomp_build(nl_sock_filter_t *out, unsigned cap,
                                        const int *allow, unsigned n,
                                        unsigned deny) {
  return nl_seccomp_build_ex(out, cap, allow, n, (const nl_seccomp_rule_t *)0,
                             0, deny);
}

// Install a program for this thread (and everything it forks) 0, or -errno from
// kernel
static inline int nl_seccomp_install(const nl_sock_filter_t *prog, unsigned n) {
#ifdef NL_AUDIT_ARCH
  if (!prog || n == 0 || n > 4096) {
    return -22; /* EINVAL */
  }

  long r = __sc6(SYS_prctl, 38 /* PR_SET_NO_NEW_PRIVS */, 1, 0, 0, 0, 0);
  if (r < 0) {
    return (int)r;
  }

  nl_sock_fprog_t fp;
  fp.len = (unsigned short)n;
  fp.filter = prog;
  r = __sc3(SYS_seccomp, 1 /* SECCOMP_SET_MODE_FILTER */, 0, (long)&fp);

  return r < 0 ? (int)r : 0;
#else
  (void)prog, (void)n;

  return -38; /* ENOSYS */
#endif
}

// Build and install in one step. 0, or negative on failure
static inline int nl_seccomp_lockdown_ex(const int *allow, unsigned n,
                                         const nl_seccomp_rule_t *rules,
                                         unsigned nr, unsigned deny) {
  nl_sock_filter_t prog[NL_SECCOMP_MAX_INSNS];
  unsigned len = nl_seccomp_build_ex(prog, NL_SECCOMP_MAX_INSNS, allow, n,
                                     rules, nr, deny);
  if (len == 0) {
    return -22;
  }

  return nl_seccomp_install(prog, len);
}

static inline int nl_seccomp_lockdown(const int *allow, unsigned n,
                                      unsigned deny) {
  return nl_seccomp_lockdown_ex(allow, n, (const nl_seccomp_rule_t *)0, 0,
                                deny);
}

#endif /* NL_SECCOMP_H */
