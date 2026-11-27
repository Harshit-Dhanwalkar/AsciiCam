/* rv32_fast.c - Decode-cache + threaded-dispatch engine. See rv32_fast.h. */
#include "rv32_fast.h"

#if defined(__GNUC__) && !defined(RV32_NO_COMPUTED_GOTO)
#define RV32_CGOTO 1
#pragma GCC diagnostic ignored                                                 \
    "-Wpedantic" // Labels-as-values is a GNU extension
#else
#define RV32_CGOTO 0
#endif

// Decoded opcodes
// 0 is reserved for "not decoded yet"
enum {
  OP_NONE = 0,
  OP_NOP,
  OP_SLOW,
  OP_LUI,
  OP_AUIPC,
  OP_JAL,
  OP_JAL0,
  OP_JALR,
  OP_JALR0,
  OP_BEQ,
  OP_BNE,
  OP_BLT,
  OP_BGE,
  OP_BLTU,
  OP_BGEU,
  OP_LB,
  OP_LH,
  OP_LW,
  OP_LBU,
  OP_LHU,
  OP_SB,
  OP_SH,
  OP_SW,
  OP_ADDI,
  OP_SLTI,
  OP_SLTIU,
  OP_XORI,
  OP_ORI,
  OP_ANDI,
  OP_SLLI,
  OP_SRLI,
  OP_SRAI,
  OP_ADD,
  OP_SUB,
  OP_SLL,
  OP_SLT,
  OP_SLTU,
  OP_XOR,
  OP_SRL,
  OP_SRA,
  OP_OR,
  OP_AND,
  OP_MUL,
  OP_MULH,
  OP_MULHSU,
  OP_MULHU,
  OP_DIV,
  OP_DIVU,
  OP_REM,
  OP_REMU,
  OP_COUNT
};

static int32_t sext(uint32_t v, unsigned bits) {
  uint32_t m = 1u << (bits - 1);
  return (int32_t)((v ^ m) - m);
}

static uint32_t sra(uint32_t v, unsigned sh) {
  uint32_t r = v >> sh;
  if ((v & 0x80000000u) && sh) {
    r |= ~(0xFFFFFFFFu >> sh);
  }
  return r;
}

// Decode one instruction. Anything that could fault, trap or is rare gets
// OP_SLOW, which makes engine hand that exact instruction to rv32_step()
// Instructions whose only effect would be writing x0 become OP_NOP, so hot
// handlers never need to re-zero x0
static void decode(rv32_dinst_t *e, uint32_t inst) {
  unsigned rd = (inst >> 7) & 31u;
  unsigned f3 = (inst >> 12) & 7u;
  unsigned rs1 = (inst >> 15) & 31u;
  unsigned rs2 = (inst >> 20) & 31u;
  unsigned f7 = inst >> 25, op = OP_SLOW;
  int32_t imm = 0;

  switch (inst & 0x7Fu) {
  case 0x37:
    op = rd ? OP_LUI : OP_NOP;
    imm = (int32_t)(inst & 0xFFFFF000u);
    break;
  case 0x17:
    op = rd ? OP_AUIPC : OP_NOP;
    imm = (int32_t)(inst & 0xFFFFF000u);
    break;
  case 0x6F:
    imm = sext(((inst >> 31) << 20) | (((inst >> 12) & 0xFFu) << 12) |
                   (((inst >> 20) & 1u) << 11) | (((inst >> 21) & 0x3FFu) << 1),
               21);
    if (!(imm & 3)) {
      op = rd ? OP_JAL : OP_JAL0;
    }
    break;
  case 0x67:
    imm = sext(inst >> 20, 12);
    if (f3 == 0) {
      op = rd ? OP_JALR : OP_JALR0;
    }
    break;
  case 0x63:
    imm = sext(((inst >> 31) << 12) | (((inst >> 7) & 1u) << 11) |
                   (((inst >> 25) & 0x3Fu) << 5) | (((inst >> 8) & 0xFu) << 1),
               13);
    if (imm & 3) {
      break; // Misaligned target: slow path reports it
    }
    switch (f3) {
    case 0:
      op = OP_BEQ;
      break;
    case 1:
      op = OP_BNE;
      break;
    case 4:
      op = OP_BLT;
      break;
    case 5:
      op = OP_BGE;
      break;
    case 6:
      op = OP_BLTU;
      break;
    case 7:
      op = OP_BGEU;
      break;
    }
    break;
  case 0x03:
    imm = sext(inst >> 20, 12);
    if (rd)
      switch (f3) {
      case 0:
        op = OP_LB;
        break;
      case 1:
        op = OP_LH;
        break;
      case 2:
        op = OP_LW;
        break;
      case 4:
        op = OP_LBU;
        break;
      case 5:
        op = OP_LHU;
        break;
      }
    break;
  case 0x23:
    imm = sext(((inst >> 25) << 5) | ((inst >> 7) & 31u), 12);
    switch (f3) {
    case 0:
      op = OP_SB;
      break;
    case 1:
      op = OP_SH;
      break;
    case 2:
      op = OP_SW;
      break;
    }
    break;
  case 0x13:
    imm = sext(inst >> 20, 12);
    switch (f3) {
    case 0:
      op = OP_ADDI;
      break;
    case 2:
      op = OP_SLTI;
      break;
    case 3:
      op = OP_SLTIU;
      break;
    case 4:
      op = OP_XORI;
      break;
    case 6:
      op = OP_ORI;
      break;
    case 7:
      op = OP_ANDI;
      break;
    case 1:
      if (f7 == 0x00) {
        op = OP_SLLI;
        imm = (int32_t)rs2;
      }
      break;
    case 5:
      if (f7 == 0x00) {
        op = OP_SRLI;
        imm = (int32_t)rs2;
      } else if (f7 == 0x20) {
        op = OP_SRAI;
        imm = (int32_t)rs2;
      }
      break;
    }
    if (op != OP_SLOW && !rd)
      op = OP_NOP;
    break;
  case 0x33:
    if (f7 == 0x01) {
      static const uint8_t m[8] = {OP_MUL, OP_MULH, OP_MULHSU, OP_MULHU,
                                   OP_DIV, OP_DIVU, OP_REM,    OP_REMU};
      op = m[f3];
    } else if (f7 == 0x00) {
      static const uint8_t m[8] = {OP_ADD, OP_SLL, OP_SLT, OP_SLTU,
                                   OP_XOR, OP_SRL, OP_OR,  OP_AND};
      op = m[f3];
    } else if (f7 == 0x20) {
      if (f3 == 0) {
        op = OP_SUB;
      } else if (f3 == 5) {
        op = OP_SRA;
      }
    }
    if (op != OP_SLOW && !rd) {
      op = OP_NOP;
    }
    break;
  case 0x0F:
    op = OP_NOP;
    break;
  }
  e->op = (uint8_t)op;
  e->rd = (uint8_t)rd;
  e->rs1 = (uint8_t)rs1;
  e->rs2 = (uint8_t)rs2;
  e->imm = imm;
}

void rv32_fast_init(rv32_fast_t *f, rv32_t *cpu, rv32_dinst_t *cache,
                    uint32_t cache_words) {
  uint32_t max = cpu->ram_size >> 2;
  f->cpu = cpu;
  f->cache = cache;
  f->words = cache_words < max ? cache_words : max;
  rv32_fast_flush(f);
}

void rv32_fast_flush(rv32_fast_t *f) {
  uint32_t i;
  for (i = 0; i < f->words; i++) {
    f->cache[i].op = OP_NONE;
  }
}

void rv32_fast_invalidate(rv32_fast_t *f, uint32_t addr, uint32_t len) {
  uint32_t off = addr - f->cpu->ram_base, first, last;
  if (!len || off >= f->cpu->ram_size) {
    return;
  }
  first = off >> 2;
  last = (off + len - 1) >> 2;
  if (last < first || last >= f->words) { // Wrapped or past cache
    last = f->words ? f->words - 1 : 0;
  }
  for (; first <= last && first < f->words; first++) {
    f->cache[first].op = OP_NONE;
  }
}

enum rv32_status rv32_fast_run(rv32_fast_t *f, uint64_t max_steps) {
  rv32_t *c = f->cpu;
  uint32_t *x = c->x;
  uint8_t *ram = c->ram;
  rv32_dinst_t *cache = f->cache;
  const uint32_t base = c->ram_base, ramsz = c->ram_size, cw = f->words;
  uint32_t pc = c->pc;
  uint64_t left = max_steps ? max_steps : ~(uint64_t)0;
  uint64_t n = 0; // Instructions retired by fast path
  enum rv32_status st = RV32_OK;
  rv32_dinst_t *e;
  uint32_t a, b, o, w;

#if RV32_CGOTO
  static const void *const tab[OP_COUNT] = {
      [OP_NONE] = &&L_SLOW, // Never dispatched: decode runs first
      [OP_NOP] = &&L_NOP,     [OP_SLOW] = &&L_SLOW,     [OP_LUI] = &&L_LUI,
      [OP_AUIPC] = &&L_AUIPC, [OP_JAL] = &&L_JAL,       [OP_JAL0] = &&L_JAL0,
      [OP_JALR] = &&L_JALR,   [OP_JALR0] = &&L_JALR0,   [OP_BEQ] = &&L_BEQ,
      [OP_BNE] = &&L_BNE,     [OP_BLT] = &&L_BLT,       [OP_BGE] = &&L_BGE,
      [OP_BLTU] = &&L_BLTU,   [OP_BGEU] = &&L_BGEU,     [OP_LB] = &&L_LB,
      [OP_LH] = &&L_LH,       [OP_LW] = &&L_LW,         [OP_LBU] = &&L_LBU,
      [OP_LHU] = &&L_LHU,     [OP_SB] = &&L_SB,         [OP_SH] = &&L_SH,
      [OP_SW] = &&L_SW,       [OP_ADDI] = &&L_ADDI,     [OP_SLTI] = &&L_SLTI,
      [OP_SLTIU] = &&L_SLTIU, [OP_XORI] = &&L_XORI,     [OP_ORI] = &&L_ORI,
      [OP_ANDI] = &&L_ANDI,   [OP_SLLI] = &&L_SLLI,     [OP_SRLI] = &&L_SRLI,
      [OP_SRAI] = &&L_SRAI,   [OP_ADD] = &&L_ADD,       [OP_SUB] = &&L_SUB,
      [OP_SLL] = &&L_SLL,     [OP_SLT] = &&L_SLT,       [OP_SLTU] = &&L_SLTU,
      [OP_XOR] = &&L_XOR,     [OP_SRL] = &&L_SRL,       [OP_SRA] = &&L_SRA,
      [OP_OR] = &&L_OR,       [OP_AND] = &&L_AND,       [OP_MUL] = &&L_MUL,
      [OP_MULH] = &&L_MULH,   [OP_MULHSU] = &&L_MULHSU, [OP_MULHU] = &&L_MULHU,
      [OP_DIV] = &&L_DIV,     [OP_DIVU] = &&L_DIVU,     [OP_REM] = &&L_REM,
      [OP_REMU] = &&L_REMU,
  };
#define H(name) L_##name:
#define DISPATCH() goto *tab[e->op]
#else
#define H(name) case OP_##name:
#define DISPATCH() goto sw_dispatch
#endif

// Fetch decoded entry for pc, decoding it first if needed
#define SETUP()                                                                \
  do {                                                                         \
    if (!left) {                                                               \
      goto done;                                                               \
    }                                                                          \
    left--;                                                                    \
    n++;                                                                       \
    o = pc - base;                                                             \
    if ((pc & 3u) || (o >> 2) >= cw) {                                         \
      goto slow;                                                               \
    }                                                                          \
    e = &cache[o >> 2];                                                        \
    if (!e->op) {                                                              \
      const uint8_t *q = ram + o;                                              \
      decode(e, (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 |  \
                    (uint32_t)q[3] << 24);                                     \
    }                                                                          \
  } while (0)
#define NEXT()                                                                 \
  do {                                                                         \
    SETUP();                                                                   \
    DISPATCH();                                                                \
  } while (0)
#define ADV()                                                                  \
  do {                                                                         \
    pc += 4;                                                                   \
    NEXT();                                                                    \
  } while (0)

#define RA (x[e->rs1])
#define RB (x[e->rs2])
#define IMM ((uint32_t)e->imm)
#define SGN(v) ((v) ^ 0x80000000u)

#define ALU_I(name, expr)                                                      \
  H(name) {                                                                    \
    a = RA;                                                                    \
    b = IMM;                                                                   \
    x[e->rd] = (expr);                                                         \
    ADV();                                                                     \
  }
#define ALU_R(name, expr)                                                      \
  H(name) {                                                                    \
    a = RA;                                                                    \
    b = RB;                                                                    \
    x[e->rd] = (expr);                                                         \
    ADV();                                                                     \
  }
#define BRANCH(name, cond)                                                     \
  H(name) {                                                                    \
    a = RA;                                                                    \
    b = RB;                                                                    \
    if (cond) {                                                                \
      pc += IMM;                                                               \
    } else {                                                                   \
      pc += 4;                                                                 \
    }                                                                          \
    NEXT();                                                                    \
  }

#define LOAD(name, sz, expr)                                                   \
  H(name) {                                                                    \
    a = RA + IMM;                                                              \
    o = a - base;                                                              \
    if (o >= ramsz || ramsz - o < (sz)) {                                      \
      goto slow;                                                               \
    }                                                                          \
    {                                                                          \
      const uint8_t *q = ram + o;                                              \
      (void)q;                                                                 \
      x[e->rd] = (expr);                                                       \
    }                                                                          \
    ADV();                                                                     \
  }
#define STORE(name, sz, body)                                                  \
  H(name) {                                                                    \
    a = RA + IMM;                                                              \
    o = a - base;                                                              \
    b = RB;                                                                    \
    if (o >= ramsz || ramsz - o < (sz)) {                                      \
      goto slow;                                                               \
    }                                                                          \
    {                                                                          \
      uint8_t *q = ram + o;                                                    \
      body                                                                     \
    }                                                                          \
    w = o >> 2;                                                                \
    if (w < cw) {                                                              \
      cache[w].op = OP_NONE;                                                   \
    }                                                                          \
    w = (o + (sz) - 1) >> 2;                                                   \
    if (w < cw) {                                                              \
      cache[w].op = OP_NONE;                                                   \
    }                                                                          \
    ADV();                                                                     \
  }

  NEXT();

#if !RV32_CGOTO
sw_dispatch:
  switch (e->op) {
#endif

    H(NOP) { ADV(); }
    H(LUI) {
      x[e->rd] = IMM;
      ADV();
    }
    H(AUIPC) {
      x[e->rd] = pc + IMM;
      ADV();
    }

    H(JAL) {
      x[e->rd] = pc + 4;
      pc += IMM;
      NEXT();
    }
    H(JAL0) {
      pc += IMM;
      NEXT();
    }
    H(JALR) {
      a = (RA + IMM) & ~1u;
      if (a & 3u) {
        goto slow;
      }
      x[e->rd] = pc + 4; // After reading rs1: rd may equal rs1
      pc = a;
      NEXT();
    }
    H(JALR0) {
      a = (RA + IMM) & ~1u;
      if (a & 3u) {
        goto slow;
      }
      pc = a;
      NEXT();
    }

    BRANCH(BEQ, a == b)
    BRANCH(BNE, a != b)
    BRANCH(BLT, SGN(a) < SGN(b))
    BRANCH(BGE, SGN(a) >= SGN(b))
    BRANCH(BLTU, a < b)
    BRANCH(BGEU, a >= b)

    LOAD(LB, 1, (uint32_t)sext(q[0], 8))
    LOAD(LBU, 1, q[0])
    LOAD(LH, 2, (uint32_t)sext((uint32_t)q[0] | (uint32_t)q[1] << 8, 16))
    LOAD(LHU, 2, (uint32_t)q[0] | (uint32_t)q[1] << 8)
    LOAD(LW, 4,
         (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 |
             (uint32_t)q[3] << 24)

    STORE(SB, 1, q[0] = (uint8_t)b;)
    STORE(SH, 2, q[0] = (uint8_t)b; q[1] = (uint8_t)(b >> 8);)
    STORE(SW, 4, q[0] = (uint8_t)b; q[1] = (uint8_t)(b >> 8);
          q[2] = (uint8_t)(b >> 16); q[3] = (uint8_t)(b >> 24);)

    ALU_I(ADDI, a + b)
    ALU_I(SLTI, SGN(a) < SGN(b))
    ALU_I(SLTIU, a < b)
    ALU_I(XORI, a ^ b)
    ALU_I(ORI, a | b)
    ALU_I(ANDI, a & b)
    ALU_I(SLLI, a << b)
    ALU_I(SRLI, a >> b)
    ALU_I(SRAI, sra(a, b))

    ALU_R(ADD, a + b)
    ALU_R(SUB, a - b)
    ALU_R(SLL, a << (b & 31u))
    ALU_R(SLT, SGN(a) < SGN(b))
    ALU_R(SLTU, a < b)
    ALU_R(XOR, a ^ b)
    ALU_R(SRL, a >> (b & 31u))
    ALU_R(SRA, sra(a, b & 31u))
    ALU_R(OR, a | b)
    ALU_R(AND, a & b)

    ALU_R(MUL, a * b)
    ALU_R(MULH, (uint32_t)(((uint64_t)(int64_t)(int32_t)a *
                            (uint64_t)(int64_t)(int32_t)b) >>
                           32))
    ALU_R(MULHSU,
          (uint32_t)(((uint64_t)(int64_t)(int32_t)a * (uint64_t)b) >> 32))
    ALU_R(MULHU, (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32))
    ALU_R(DIV, b == 0 ? 0xFFFFFFFFu
               : (a == 0x80000000u && b == 0xFFFFFFFFu)
                   ? a
                   : (uint32_t)((int32_t)a / (int32_t)b))
    ALU_R(DIVU, b ? a / b : 0xFFFFFFFFu)
    ALU_R(REM, b == 0 ? a
               : (a == 0x80000000u && b == 0xFFFFFFFFu)
                   ? 0u
                   : (uint32_t)((int32_t)a % (int32_t)b))
    ALU_R(REMU, b ? a % b : a)

    H(SLOW) {
    slow:
      // Run exactly this instruction on reference interpreter
      n--;
      c->pc = pc;
      st = rv32_step(c);
      if (st != RV32_OK) {
        goto out;
      }
      pc = c->pc;
      NEXT();
    }

#if !RV32_CGOTO
  default:
    goto slow;
  }
#endif

done:
  c->pc = pc;
out:
  c->instret += n;
  return st;
}
