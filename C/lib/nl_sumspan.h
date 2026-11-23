/*
 * lib/nl_sumspan.h - byte-summing kernels for area-average downscale, in
 * x86-64 assembly with pure-C twins
 *
 *   nl_sumspan_u8   sums of many byte spans of one row (grayscale)
 *   nl_addrow_u8    adds one row of bytes into 32-bit column sums (colour)
 *
 *   void nl_sumspan_u8(const uint8_t *row, const int32_t *xs,
 *                      const int32_t *xe, int n, uint64_t *acc);
 *
 * For i in [0, n): acc[i] += row[xs[i]] + ... + row[xe[i] - 1]
 * Spans are half-open; a span with xe <= xs adds nothing
 * Caller keeps every span inside row: nothing outside [xs, xe) is ever read
 *
 * This is what area-average downscale (grayscale_to_ascii) spends its time
 * on: every source pixel is summed into cell it falls in
 * Assembly adds 16 pixels per PSADBW (SSE2, part of x86-64 baseline: no CPU
 * check) instead of one byte per iteration. acc is 64-bit so a whole 4096 x
 * 4096 picture in one cell cannot overflow
 *
 *   void nl_addrow_u8(uint32_t *acc, const uint8_t *src, size_t n);
 *
 * acc[i] += src[i] for i in [0, n). Colour frames are R G B interleaved, so
 * picture is added up one source row after another into per-column sums
 * (one cell is 8 rows high, say); only then are columns summed per cell, which
 * is 8 times less scalar work. SSE2 widens 16 bytes per step. 32 bits hold
 * 255 * 16.7 million rows: always enough. acc is not aligned, n is any
 *
 * Use
 *   - One translation unit per program defines NL_SUMSPAN_IMPL before
 *     including this header: that emits assembly (once)
 *     Any other unit includes it without NL_SUMSPAN_IMPL and gets declaration
 *     only
 *   - nl_sumspan_u8_c() is always available (also with NL_SUMSPAN_NO_ASM), it
 *     is the reference tests compare assembly with
 *
 * Assembly is used on x86-64 Linux (System V ABI, ELF) only
 * Everywhere else (macOS has other symbol rules, Windows another ABI, ARM64
 * another ISA) nl_sumspan_u8 is the C loop, so results are identical on every
 * platform
 */

#ifndef NL_SUMSPAN_H
#define NL_SUMSPAN_H

#include "nl_types.h"

#if defined(__x86_64__) && defined(__linux__) && !defined(NL_SUMSPAN_NO_ASM)
#define NL_SUMSPAN_ASM 1
#else
#define NL_SUMSPAN_ASM 0
#endif

// Pure-C twin: same contract, same result
static inline void nl_sumspan_u8_c(const uint8_t *row, const int32_t *xs,
                                   const int32_t *xe, int n, uint64_t *acc) {
  for (int i = 0; i < n; i++) {
    uint64_t s = 0;
    for (int x = xs[i]; x < xe[i]; x++) {
      s += row[x];
    }

    acc[i] += s;
  }
}

static inline void nl_addrow_u8_c(uint32_t *acc, const uint8_t *src, size_t n) {
  for (size_t i = 0; i < n; i++) {
    acc[i] += src[i];
  }
}

#if NL_SUMSPAN_ASM

void nl_addrow_u8(uint32_t *acc, const uint8_t *src, size_t n);

void nl_sumspan_u8(const uint8_t *row, const int32_t *xs, const int32_t *xe,
                   int n, uint64_t *acc);

#ifdef NL_SUMSPAN_IMPL

/* System V AMD64: rdi = row, rsi = xs, rdx = xe, ecx = n, r8 = acc
 * rax = cell index, r9 = read pointer, r10 = bytes left, r11 = cell sum,
 * xmm0 = zero, xmm1 = two 64-bit partial sums, rbx (saved) = byte temp */
__asm__(".pushsection .text,\"ax\",@progbits\n"
        ".globl nl_sumspan_u8\n"
        ".type nl_sumspan_u8, @function\n"
        ".p2align 4\n"
        "nl_sumspan_u8:\n"
        "    movl    %ecx, %ecx\n" // n -> 64 bit, upper half is junk
        "    testq   %rcx, %rcx\n"
        "    jz      .Lnl_ss_ret\n"
        "    pushq   %rbx\n"
        "    pxor    %xmm0, %xmm0\n"
        "    xorl    %eax, %eax\n"
        ".Lnl_ss_cell:\n"
        "    movslq  (%rsi,%rax,4), %r9\n"  // xs[i]
        "    movslq  (%rdx,%rax,4), %r10\n" // xe[i]
        "    subq    %r9, %r10\n"           // Length
        "    addq    %rdi, %r9\n"           // Pointer to first byte
        "    xorl    %r11d, %r11d\n"
        "    pxor    %xmm1, %xmm1\n"
        "    testq   %r10, %r10\n"
        "    jle     .Lnl_ss_store\n" // Empty or backwards: add 0
        ".Lnl_ss_v16:\n"
        "    cmpq    $16, %r10\n"
        "    jb      .Lnl_ss_v8\n"
        "    movdqu  (%r9), %xmm2\n"
        "    psadbw  %xmm0, %xmm2\n" // |b - 0| summed per 8 bytes
        "    paddq   %xmm2, %xmm1\n"
        "    addq    $16, %r9\n"
        "    subq    $16, %r10\n"
        "    jmp     .Lnl_ss_v16\n"
        ".Lnl_ss_v8:\n"
        "    cmpq    $8, %r10\n"
        "    jb      .Lnl_ss_red\n"
        "    movq    (%r9), %xmm2\n" // 8 bytes, upper half zeroed
        "    psadbw  %xmm0, %xmm2\n"
        "    paddq   %xmm2, %xmm1\n"
        "    addq    $8, %r9\n"
        "    subq    $8, %r10\n"
        ".Lnl_ss_red:\n"
        "    movhlps %xmm1, %xmm2\n" // Fold two lanes
        "    paddq   %xmm2, %xmm1\n"
        "    movq    %xmm1, %r11\n"
        "    testq   %r10, %r10\n"
        "    jz      .Lnl_ss_store\n"
        ".Lnl_ss_byte:\n" // 1..7 bytes left
        "    movzbl  (%r9), %ebx\n"
        "    addq    %rbx, %r11\n"
        "    incq    %r9\n"
        "    decq    %r10\n"
        "    jnz     .Lnl_ss_byte\n"
        ".Lnl_ss_store:\n"
        "    addq    %r11, (%r8,%rax,8)\n"
        "    incq    %rax\n"
        "    cmpq    %rcx, %rax\n"
        "    jb      .Lnl_ss_cell\n"
        "    popq    %rbx\n"
        ".Lnl_ss_ret:\n"
        "    ret\n"
        ".size nl_sumspan_u8, .-nl_sumspan_u8\n"
        ".popsection\n");

/* System V AMD64: rdi = acc (u32), rsi = src (u8), rdx = n
 * Bytes are widened 8 -> 16 -> 32 bit against zero (xmm0), 16 per step */
__asm__(".pushsection .text,\"ax\",@progbits\n"
        ".globl nl_addrow_u8\n"
        ".type nl_addrow_u8, @function\n"
        ".p2align 4\n"
        "nl_addrow_u8:\n"
        "    pxor      %xmm0, %xmm0\n"
        ".Lnl_ar_v16:\n"
        "    cmpq      $16, %rdx\n"
        "    jb        .Lnl_ar_tail\n"
        "    movdqu    (%rsi), %xmm1\n"
        "    movdqa    %xmm1, %xmm2\n"
        "    punpcklbw %xmm0, %xmm1\n" // Bytes 0..7 as u16
        "    punpckhbw %xmm0, %xmm2\n" // Bytes 8..15 as u16
        "    movdqa    %xmm1, %xmm3\n"
        "    punpcklwd %xmm0, %xmm1\n" // Bytes 0..3 as u32
        "    punpckhwd %xmm0, %xmm3\n" // Bytes 4..7
        "    movdqa    %xmm2, %xmm4\n"
        "    punpcklwd %xmm0, %xmm2\n" // Bytes 8..11
        "    punpckhwd %xmm0, %xmm4\n" // Bytes 12..15
        "    movdqu    (%rdi), %xmm5\n"
        "    paddd     %xmm1, %xmm5\n"
        "    movdqu    %xmm5, (%rdi)\n"
        "    movdqu    16(%rdi), %xmm5\n"
        "    paddd     %xmm3, %xmm5\n"
        "    movdqu    %xmm5, 16(%rdi)\n"
        "    movdqu    32(%rdi), %xmm5\n"
        "    paddd     %xmm2, %xmm5\n"
        "    movdqu    %xmm5, 32(%rdi)\n"
        "    movdqu    48(%rdi), %xmm5\n"
        "    paddd     %xmm4, %xmm5\n"
        "    movdqu    %xmm5, 48(%rdi)\n"
        "    addq      $16, %rsi\n"
        "    addq      $64, %rdi\n"
        "    subq      $16, %rdx\n"
        "    jmp       .Lnl_ar_v16\n"
        ".Lnl_ar_tail:\n"
        "    testq     %rdx, %rdx\n"
        "    jz        .Lnl_ar_ret\n"
        ".Lnl_ar_byte:\n" // 1..15 bytes left
        "    movzbl    (%rsi), %eax\n"
        "    addl      %eax, (%rdi)\n"
        "    incq      %rsi\n"
        "    addq      $4, %rdi\n"
        "    decq      %rdx\n"
        "    jnz       .Lnl_ar_byte\n"
        ".Lnl_ar_ret:\n"
        "    ret\n"
        ".size nl_addrow_u8, .-nl_addrow_u8\n"
        ".popsection\n");

#endif // NL_SUMSPAN_IMPL

#else // !NL_SUMSPAN_ASM

static inline void nl_addrow_u8(uint32_t *acc, const uint8_t *src, size_t n) {
  nl_addrow_u8_c(acc, src, n);
}

static inline void nl_sumspan_u8(const uint8_t *row, const int32_t *xs,
                                 const int32_t *xe, int n, uint64_t *acc) {
  nl_sumspan_u8_c(row, xs, xe, n, acc);
}

#endif

#endif // NL_SUMSPAN_H
