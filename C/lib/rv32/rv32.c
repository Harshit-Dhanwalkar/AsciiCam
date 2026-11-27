// rv32.c - RV32IM interpreter core
#include "rv32.h"

// Small helpers
static int32_t sext(uint32_t v, unsigned bits) {
  uint32_t m = 1u << (bits - 1);
  return (int32_t)((v ^ m) - m);
}

// Portable arithmetic shift right
// (WARN: does not rely on signed >> behaviour)
static uint32_t sra(uint32_t v, unsigned sh) {
  uint32_t r = v >> sh;
  if ((v & 0x80000000u) && sh) {
    r |= ~(0xFFFFFFFFu >> sh);
  }
  return r;
}

// Memory
static int mem_load(rv32_t *c, uint32_t addr, unsigned size, uint32_t *out) {
  uint32_t off = addr - c->ram_base;
  if (off < c->ram_size && c->ram_size - off >= size) {
    const uint8_t *p = c->ram + off;
    uint32_t v = p[0];
    if (size > 1) {
      v |= (uint32_t)p[1] << 8;
    }
    if (size > 2) {
      v |= (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    }
    *out = v;
    return 0;
  }

  if (c->mmio_read && c->mmio_read(c->user, addr, size, out) == 0) {
    return 0;
  }

  c->fault_addr = addr;
  return -1;
}

static int mem_store(rv32_t *c, uint32_t addr, unsigned size, uint32_t val) {
  uint32_t off = addr - c->ram_base;
  if (off < c->ram_size && c->ram_size - off >= size) {
    uint8_t *p = c->ram + off;
    p[0] = (uint8_t)val;
    if (size > 1) {
      p[1] = (uint8_t)(val >> 8);
    }
    if (size > 2) {
      p[2] = (uint8_t)(val >> 16);
      p[3] = (uint8_t)(val >> 24);
    }

    return 0;
  }

  if (c->mmio_write && c->mmio_write(c->user, addr, size, val) == 0) {
    return 0;
  }

  c->fault_addr = addr;
  return -1;
}

// Public API
void rv32_init(rv32_t *c, uint8_t *ram, uint32_t ram_base, uint32_t ram_size,
               uint32_t entry_pc, uint32_t stack_top) {
  int i;
  for (i = 0; i < 32; i++) {
    c->x[i] = 0;
  }
  c->pc = entry_pc;
  c->x[2] = stack_top;
  c->ram = ram;
  c->ram_base = ram_base;
  c->ram_size = ram_size;
  c->mmio_read = 0;
  c->mmio_write = 0;
  c->user = 0;
  c->instret = 0;
  c->fault_addr = 0;
}

enum rv32_status rv32_step(rv32_t *c) {
  uint32_t pc = c->pc, inst, next = pc + 4;
  uint32_t a, b, v, target;
  unsigned rd, f3, rs1, rs2, f7;
  int32_t imm;

  if (pc & 3u) {
    c->fault_addr = pc;
    return RV32_FETCH_MISALIGNED;
  }
  if (mem_load(c, pc, 4, &inst)) {
    return RV32_ACCESS_FAULT;
  }

  rd = (inst >> 7) & 31u;
  f3 = (inst >> 12) & 7u;
  rs1 = (inst >> 15) & 31u;
  rs2 = (inst >> 20) & 31u;
  f7 = inst >> 25;
  a = c->x[rs1];
  b = c->x[rs2];

  switch (inst & 0x7Fu) {
  case 0x37: { /* LUI */
    c->x[rd] = inst & 0xFFFFF000u;
    break;
  }
  case 0x17: { /* AUIPC */
    c->x[rd] = pc + (inst & 0xFFFFF000u);
    break;
  }
  case 0x6F: { /* JAL */
    imm = sext(((inst >> 31) << 20) | (((inst >> 12) & 0xFFu) << 12) |
                   (((inst >> 20) & 1u) << 11) | (((inst >> 21) & 0x3FFu) << 1),
               21);
    target = pc + (uint32_t)imm;
    if (target & 3u) {
      c->fault_addr = target;
      return RV32_FETCH_MISALIGNED;
    }
    c->x[rd] = next;
    next = target;
    break;
  }
  case 0x67: { /* JALR */
    if (f3) {
      return RV32_ILLEGAL;
    }
    imm = sext(inst >> 20, 12);
    target = (a + (uint32_t)imm) & ~1u;
    if (target & 3u) {
      c->fault_addr = target;
      return RV32_FETCH_MISALIGNED;
    }
    c->x[rd] = next; /* after computing target: rd may == rs1 */
    next = target;
    break;
  }
  case 0x63: { /* branches */
    imm = sext(((inst >> 31) << 12) | (((inst >> 7) & 1u) << 11) |
                   (((inst >> 25) & 0x3Fu) << 5) | (((inst >> 8) & 0xFu) << 1),
               13);
    switch (f3) {
    case 0:
      v = (a == b);
      break; /* BEQ  */
    case 1:
      v = (a != b);
      break; /* BNE  */
    case 4:
      v = ((a ^ 0x80000000u) < (b ^ 0x80000000u));
      break; /* BLT  */
    case 5:
      v = ((a ^ 0x80000000u) >= (b ^ 0x80000000u));
      break; /* BGE  */
    case 6:
      v = (a < b);
      break; /* BLTU */
    case 7:
      v = (a >= b);
      break; /* BGEU */
    default:
      return RV32_ILLEGAL;
    }
    if (v) {
      target = pc + (uint32_t)imm;
      if (target & 3u) {
        c->fault_addr = target;
        return RV32_FETCH_MISALIGNED;
      }
      next = target;
    }
    break;
  }
  case 0x03: { /* loads */
    imm = sext(inst >> 20, 12);
    a += (uint32_t)imm;
    switch (f3) {
    case 0:
      if (mem_load(c, a, 1, &v))
        return RV32_ACCESS_FAULT;
      v = (uint32_t)sext(v, 8);
      break;
    case 1:
      if (mem_load(c, a, 2, &v))
        return RV32_ACCESS_FAULT;
      v = (uint32_t)sext(v, 16);
      break;
    case 2:
      if (mem_load(c, a, 4, &v))
        return RV32_ACCESS_FAULT;
      break;
    case 4:
      if (mem_load(c, a, 1, &v))
        return RV32_ACCESS_FAULT;
      break;
    case 5:
      if (mem_load(c, a, 2, &v))
        return RV32_ACCESS_FAULT;
      break;
    default:
      return RV32_ILLEGAL;
    }
    c->x[rd] = v;
    break;
  }
  case 0x23: { /* stores */
    imm = sext(((inst >> 25) << 5) | ((inst >> 7) & 31u), 12);
    a += (uint32_t)imm;
    if (f3 > 2) {
      return RV32_ILLEGAL;
    }
    if (mem_store(c, a, 1u << f3, b)) {
      return RV32_ACCESS_FAULT;
    }
    break;
  }
  case 0x13: { /* OP-IMM */
    imm = sext(inst >> 20, 12);
    b = (uint32_t)imm;
    switch (f3) {
    case 0:
      v = a + b;
      break; /* ADDI  */
    case 2:
      v = ((a ^ 0x80000000u) < (b ^ 0x80000000u));
      break; /* SLTI  */
    case 3:
      v = (a < b);
      break; /* SLTIU */
    case 4:
      v = a ^ b;
      break; /* XORI  */
    case 6:
      v = a | b;
      break; /* ORI   */
    case 7:
      v = a & b;
      break; /* ANDI  */
    case 1:  /* SLLI  */
      if (f7)
        return RV32_ILLEGAL;
      v = a << rs2;
      break;
    case 5: /* SRLI/SRAI */
      if (f7 == 0x00)
        v = a >> rs2;
      else if (f7 == 0x20)
        v = sra(a, rs2);
      else
        return RV32_ILLEGAL;
      break;
    default:
      return RV32_ILLEGAL;
    }
    c->x[rd] = v;
    break;
  }
  case 0x33: { /* OP and M extension */
    if (f7 == 0x01) {
      switch (f3) {
      case 0:
        v = a * b;
        break;  /* MUL    */
      case 1: { /* MULH   */
        uint64_t p =
            (uint64_t)(int64_t)(int32_t)a * (uint64_t)(int64_t)(int32_t)b;
        v = (uint32_t)(p >> 32);
        break;
      }
      case 2: { /* MULHSU */
        uint64_t p = (uint64_t)(int64_t)(int32_t)a * (uint64_t)b;
        v = (uint32_t)(p >> 32);
        break;
      }
      case 3:
        v = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
        break; /* MULHU  */
      case 4:  /* DIV    */
        if (b == 0)
          v = 0xFFFFFFFFu;
        else if (a == 0x80000000u && b == 0xFFFFFFFFu)
          v = a;
        else {
          int32_t sa = (int32_t)a, sb = (int32_t)b;
          v = (uint32_t)(sa / sb);
        }
        break;
      case 5:
        v = b ? a / b : 0xFFFFFFFFu;
        break; /* DIVU   */
      case 6:  /* REM    */
        if (b == 0)
          v = a;
        else if (a == 0x80000000u && b == 0xFFFFFFFFu)
          v = 0;
        else {
          int32_t sa = (int32_t)a, sb = (int32_t)b;
          v = (uint32_t)(sa % sb);
        }
        break;
      default:
        v = b ? a % b : a;
        break; /* REMU   */
      }
    } else if (f7 == 0x00 || f7 == 0x20) {
      unsigned alt = (f7 == 0x20);
      switch (f3) {
      case 0:
        v = alt ? a - b : a + b;
        break; /* SUB/ADD */
      case 1:
        if (alt)
          return RV32_ILLEGAL;
        v = a << (b & 31u);
        break; /* SLL    */
      case 2:
        if (alt)
          return RV32_ILLEGAL; /* SLT    */
        v = ((a ^ 0x80000000u) < (b ^ 0x80000000u));
        break;
      case 3:
        if (alt)
          return RV32_ILLEGAL;
        v = (a < b);
        break; /* SLTU   */
      case 4:
        if (alt)
          return RV32_ILLEGAL;
        v = a ^ b;
        break; /* XOR    */
      case 5:
        v = alt ? sra(a, b & 31u) : a >> (b & 31u);
        break; /* SRL/SRA */
      case 6:
        if (alt)
          return RV32_ILLEGAL;
        v = a | b;
        break; /* OR     */
      default:
        if (alt)
          return RV32_ILLEGAL;
        v = a & b;
        break; /* AND    */
      }
    } else {
      return RV32_ILLEGAL;
    }
    c->x[rd] = v;
    break;
  }
  case 0x0F: { /* FENCE / FENCE.I: no-ops on a single in-order core */
    break;
  }
  case 0x73: { /* SYSTEM: only ECALL / EBREAK */
    if (f3 == 0 && rd == 0 && rs1 == 0 && (inst >> 20) <= 1u) {
      c->pc = next;
      c->x[0] = 0;
      c->instret++;
      return (inst >> 20) ? RV32_EBREAK : RV32_ECALL;
    }
    return RV32_ILLEGAL;
  }
  default:
    return RV32_ILLEGAL;
  }

  c->x[0] = 0;
  c->pc = next;
  c->instret++;
  return RV32_OK;
}

enum rv32_status rv32_run(rv32_t *c, uint64_t max_steps) {
  uint64_t n = 0;
  for (;;) {
    enum rv32_status s;
    if (max_steps && n >= max_steps) {
      return RV32_OK;
    }
    s = rv32_step(c);
    if (s != RV32_OK) {
      return s;
    }
    n++;
  }
}
