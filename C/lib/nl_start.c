extern int main(int argc, char *argv[]);

#ifdef NL_SANITIZE_BUILD
/*
 * Under -fsanitize=address,undefined compiler inserts constructor and
 * destructor functions into .init_array / .fini_array via
 * __attribute__((constructor))
 */
typedef void (*_initfn_t)(void);
extern _initfn_t __init_array_start[] __attribute__((weak));
extern _initfn_t __init_array_end[] __attribute__((weak));
extern _initfn_t __fini_array_start[] __attribute__((weak));
extern _initfn_t __fini_array_end[] __attribute__((weak));

static __attribute__((used, noinline)) void _nl_start_c(int argc, char **argv) {
  // Run .init_array forward (ASan/UBSan initialise)
  for (_initfn_t *fn = __init_array_start; fn < __init_array_end; fn++) {
    // if (*fn) {
    (*fn)();
    // }
  }

  int rc = main(argc, argv);

  // Run .fini_array in reverse (ASan leak report / UBSan summary)
  for (_initfn_t *fn = __fini_array_end; fn > __fini_array_start;) {
    if (*--fn) {
      (*fn)();
    }
  }

  __asm__ volatile("mov %0, %%rdi\n"
                   "mov $231, %%rax\n" /* SYS_exit_group */
                   "syscall\n"
                   :
                   : "r"((long)rc)
                   : "memory");
  __builtin_unreachable();
}

__attribute__((naked)) void _start(void) {
  __asm__ volatile("xor   %%rbp, %%rbp  \n"
                   "pop   %%rdi          \n" /* argc */
                   "mov   %%rsp, %%rsi   \n" /* argv */
                   "and   $-16, %%rsp    \n" /* 16-byte align */
                   "call  _nl_start_c    \n" ::
                       : "memory");
}

#else /* normal (non-sanitize) build */

__attribute__((naked)) void _start(void) {
  __asm__ volatile(
      "xor   %%rbp, %%rbp  \n"
      "pop   %%rdi          \n" // rdi = argc
      "mov   %%rsp, %%rsi   \n" // rsi = argv
      "and   $-16, %%rsp    \n" // 16-byte align stack
      "call  main           \n"
      "mov   %%rax, %%rdi   \n" // exit code = return value of main()
      "mov   $231, %%rax    \n" // SYS_exit_group
      "syscall              \n" ::
          : "memory");
}

#endif /* NL_SANITIZE_BUILD */
