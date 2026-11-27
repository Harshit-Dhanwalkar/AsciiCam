/*
 * rv32_enc.h - tiny RV32IM instruction encoders and macros
 * Header-only, hosted or freestanding
 * Used by tests and benchmark and meant to be reused by assembler later
 */

#ifndef RV32_ENC_H
#define RV32_ENC_H

#include <stdint.h>

static inline uint32_t enc_r(unsigned op, unsigned f3, unsigned f7, unsigned rd,
                             unsigned rs1, unsigned rs2) {
  return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static inline uint32_t enc_i(unsigned op, unsigned f3, unsigned rd,
                             unsigned rs1, int imm) {
  return ((uint32_t)imm << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static inline uint32_t enc_s(unsigned f3, unsigned rs1, unsigned rs2, int imm) {
  uint32_t i = (uint32_t)imm;
  return ((i >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
         ((i & 31u) << 7) | 0x23;
}
static inline uint32_t enc_b(unsigned f3, unsigned rs1, unsigned rs2, int imm) {
  uint32_t i = (uint32_t)imm;
  return (((i >> 12) & 1u) << 31) | (((i >> 5) & 0x3Fu) << 25) | (rs2 << 20) |
         (rs1 << 15) | (f3 << 12) | (((i >> 1) & 0xFu) << 8) |
         (((i >> 11) & 1u) << 7) | 0x63;
}
static inline uint32_t enc_u(unsigned op, unsigned rd, uint32_t imm20) {
  return (imm20 << 12) | (rd << 7) | op;
}
static inline uint32_t enc_j(unsigned rd, int imm) {
  uint32_t i = (uint32_t)imm;
  return (((i >> 20) & 1u) << 31) | (((i >> 1) & 0x3FFu) << 21) |
         (((i >> 11) & 1u) << 20) | (((i >> 12) & 0xFFu) << 12) | (rd << 7) |
         0x6F;
}

#define ADDI(rd, rs, i) enc_i(0x13, 0, rd, rs, i)
#define ADD(rd, a, b) enc_r(0x33, 0, 0x00, rd, a, b)
#define SUB(rd, a, b) enc_r(0x33, 0, 0x20, rd, a, b)
#define SLL(rd, a, b) enc_r(0x33, 1, 0x00, rd, a, b)
#define SLT(rd, a, b) enc_r(0x33, 2, 0x00, rd, a, b)
#define SLTU(rd, a, b) enc_r(0x33, 3, 0x00, rd, a, b)
#define SRL(rd, a, b) enc_r(0x33, 5, 0x00, rd, a, b)
#define SRA(rd, a, b) enc_r(0x33, 5, 0x20, rd, a, b)
#define MUL(rd, a, b) enc_r(0x33, 0, 1, rd, a, b)
#define MULH(rd, a, b) enc_r(0x33, 1, 1, rd, a, b)
#define MULHSU(rd, a, b) enc_r(0x33, 2, 1, rd, a, b)
#define MULHU(rd, a, b) enc_r(0x33, 3, 1, rd, a, b)
#define DIV(rd, a, b) enc_r(0x33, 4, 1, rd, a, b)
#define DIVU(rd, a, b) enc_r(0x33, 5, 1, rd, a, b)
#define REM(rd, a, b) enc_r(0x33, 6, 1, rd, a, b)
#define REMU(rd, a, b) enc_r(0x33, 7, 1, rd, a, b)
#define LUI(rd, i) enc_u(0x37, rd, i)
#define AUIPC(rd, i) enc_u(0x17, rd, i)
#define LB(rd, rs, i) enc_i(0x03, 0, rd, rs, i)
#define LH(rd, rs, i) enc_i(0x03, 1, rd, rs, i)
#define LW(rd, rs, i) enc_i(0x03, 2, rd, rs, i)
#define LBU(rd, rs, i) enc_i(0x03, 4, rd, rs, i)
#define LHU(rd, rs, i) enc_i(0x03, 5, rd, rs, i)
#define SB(rs2, rs1, i) enc_s(0, rs1, rs2, i)
#define SH(rs2, rs1, i) enc_s(1, rs1, rs2, i)
#define SW(rs2, rs1, i) enc_s(2, rs1, rs2, i)
#define BEQ(a, b, o) enc_b(0, a, b, o)
#define BNE(a, b, o) enc_b(1, a, b, o)
#define BLT(a, b, o) enc_b(4, a, b, o)
#define BGE(a, b, o) enc_b(5, a, b, o)
#define BLTU(a, b, o) enc_b(6, a, b, o)
#define BGEU(a, b, o) enc_b(7, a, b, o)
#define JAL(rd, o) enc_j(rd, o)
#define JALR(rd, rs, i) enc_i(0x67, 0, rd, rs, i)
#define ECALL 0x00000073u
#define EBREAK 0x00100073u

/* Load a 32-bit constant into rd (LUI + ADDI with carry fix-up)
 * Writes 2 words to p, returns 2 */
static inline unsigned rv32_li(uint32_t *p, unsigned rd, uint32_t v) {
  uint32_t lo = v & 0xFFFu, hi = v >> 12;
  if (lo & 0x800u) {
    hi = (hi + 1) & 0xFFFFFu;
  }
  p[0] = LUI(rd, hi);
  p[1] = ADDI(rd, rd, (int)(lo & 0x800u ? lo - 0x1000u : lo));
  return 2;
}

#endif /* RV32_ENC_H */
