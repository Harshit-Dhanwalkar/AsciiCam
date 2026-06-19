extern int main(int argc, char *argv[]);

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
