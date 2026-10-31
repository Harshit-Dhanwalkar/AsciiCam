/*
 * tests/fuzz_elf.c - hostile-input tests for nl_dlopen()/nl_dlsym()
 *
 * nl_dlopen parses a file from disk and then writes to addresses computed from
 * it (relocations), so a damaged or malicious .so must produce an error, never
 * a crash and never a write outside mapping it reserved
 *
 * Two parts:
 *   1. Crafted files: one field of a real plugin is made hostile, loader
 *      must refuse it (or, where harmless, load it) without touching a canary
 *      mapping placed right after reservation
 *   2. Mutation fuzzing: random damage to header, program headers, dynamic
 *      section, symbol/relocation tables or file length. Every case runs in a
 *      forked child (alarm = hang), so a crash or hang is reported with its
 *      seed instead of killing test
 *
 * gcc -D__LINUX_NOLIBC__ -DPLATFORM_LINUX -nostdinc -isystem $(gcc
 * -print-file-name=include) -ffreestanding -fno-builtin -fno-stack-protector \
 *     -nostdlib -static tests/fuzz_elf.c lib/nl_dlfcn.c lib/nl_printf.c \
 *     lib/nl_errno.c lib/nl_alloc.c lib/nl_start.c -Iinclude -Ilib -o /tmp/fe
 * run from C/ after `make plugins`:
 *   ./fe [plugin.so=build/edge_detect.so] [iterations=4000] [seed=1] [tmpfile]
 */
#include "../lib/nl_elf.h"
#include "../lib/nolibc.h"

#define MAX_FILE (256 * 1024)
#define CANARY_PAGES 64
#define CANARY_BYTE 0x5A

static unsigned char orig[MAX_FILE];
static unsigned char buf[MAX_FILE];
static long orig_len;
static const char *tmp_path = "build/fuzz_elf.tmp";

static unsigned long long rng_state;
static unsigned long long rnd(void) {
  rng_state ^= rng_state >> 12;
  rng_state ^= rng_state << 25;
  rng_state ^= rng_state >> 27;

  return rng_state * 0x2545F4914F6CDD1DULL;
}
static unsigned rnd_n(unsigned n) { return (unsigned)(rnd() % n); }

static long to_long(const char *s) {
  long v = 0;
  while (*s >= '0' && *s <= '9') {
    v = v * 10 + (*s++ - '0');
  }

  return v;
}

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

static long read_file(const char *path, unsigned char *dst, long cap) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }

  long n = 0;
  for (;;) {
    long r = read(fd, dst + n, (size_t)(cap - n));
    if (r <= 0) {
      break;
    }

    n += r;
  }

  close(fd);

  return n;
}

static int write_file(const char *path, const unsigned char *src, long n) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return -1;
  }

  long off = 0;
  while (off < n) {
    long w = write(fd, src + off, (size_t)(n - off));
    if (w <= 0) {
      close(fd);

      return -1;
    }

    off += w;
  }

  close(fd);

  return 0;
}

// Field access into the working copy
#define EH ((Elf64_Ehdr *)buf)
#define PH(i) ((Elf64_Phdr *)(buf + EH->e_phoff) + (i))

static Elf64_Phdr *find_ph(unsigned type, int nth) {
  for (int i = 0; i < EH->e_phnum; i++) {
    if (PH(i)->p_type == type && nth-- == 0) {
      return PH(i);
    }
  }

  return (Elf64_Phdr *)0;
}

// File offset of a virtual address (valid for the PT_LOAD that holds it)
static long vaddr_to_off(unsigned long va) {
  for (int i = 0; i < EH->e_phnum; i++) {
    Elf64_Phdr *p = PH(i);
    if (p->p_type == PT_LOAD && va >= p->p_vaddr &&
        va < p->p_vaddr + p->p_filesz) {
      return (long)(va - p->p_vaddr + p->p_offset);
    }
  }

  return -1;
}

static Elf64_Dyn *find_dyn(long tag) {
  Elf64_Phdr *d = find_ph(PT_DYNAMIC, 0);
  if (!d) {
    return (Elf64_Dyn *)0;
  }

  for (Elf64_Dyn *e = (Elf64_Dyn *)(buf + d->p_offset); e->d_tag != DT_NULL;
       e++) {
    if (e->d_tag == tag) {
      return e;
    }
  }

  return (Elf64_Dyn *)0;
}

// Pointer into the working copy for a DT_* address tag
static void *dyn_table(long tag) {
  Elf64_Dyn *e = find_dyn(tag);
  if (!e) {
    return (void *)0;
  }

  long off = vaddr_to_off(e->d_un.d_ptr);

  return off < 0 ? (void *)0 : buf + off;
}

/*
 * Run one load attempt in a child process. The child maps a canary region,
 * loads the file (the kernel places the reservation right below the canary, so
 * a MAP_FIXED write past the reservation lands on it), then checks the canary.
 * Returns: 0 = refused cleanly, 1 = loaded, -1 = crashed/hung/clobbered
 */
static int try_load(const char *tag) {
  if (write_file(tmp_path, buf, orig_len) < 0) {
    fprintf(stderr, "cannot write %s\n", tmp_path);

    return -1;
  }

  long pid = __sc6(SYS_fork, 0, 0, 0, 0, 0, 0);
  if (pid == 0) {
    __sc1(37 /* alarm */, 5);
    unsigned char *canary = (unsigned char *)__sc6(
        SYS_mmap, 0, CANARY_PAGES * 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (long i = 0; i < CANARY_PAGES * 4096; i++) {
      canary[i] = CANARY_BYTE;
    }

    void *h = dlopen(tmp_path, 0);
    int rc = h ? 1 : 0;
    if (h) {
      dlsym(h, "plugin_get");
      dlclose(h);
    }

    for (long i = 0; i < CANARY_PAGES * 4096; i++) {
      if (canary[i] != CANARY_BYTE) {
        __sc1(SYS_exit_group, 99); // loader wrote outside its reservation
      }
    }

    __sc1(SYS_exit_group, rc);
  }

  int status = 0;
  __sc4(SYS_wait4, pid, (long)&status, 0, 0);
  if ((status & 0x7f) != 0) {
    fprintf(stderr, "  [%s] killed by signal %d\n", tag, status & 0x7f);

    return -1;
  }

  int code = (status >> 8) & 0xff;
  if (code == 99) {
    fprintf(stderr, "  [%s] wrote outside its reservation\n", tag);

    return -1;
  }

  return code;
}

static void reset(void) { nl_memcpy(buf, orig, (size_t)orig_len); }

// Hostile-field case: apply, run, restore
#define MUST_REFUSE(tag, stmt)                                                 \
  do {                                                                         \
    reset();                                                                   \
    stmt;                                                                      \
    int r = try_load(tag);                                                     \
    CHECK(r == 0, "%s: expected clean refusal, got %d", tag, r);               \
  } while (0)

static void crafted(void) {
  reset();
  CHECK(try_load("pristine") == 1, "unmodified plugin must still load");

  // ELF header
  MUST_REFUSE("ei_class32", buf[EI_CLASS] = 1);
  MUST_REFUSE("e_type_exec", EH->e_type = 2);
  MUST_REFUSE("e_machine", EH->e_machine = 183);
  MUST_REFUSE("phnum_zero", EH->e_phnum = 0);
  MUST_REFUSE("phnum_huge", EH->e_phnum = 0xffff);
  MUST_REFUSE("phentsize_bad", EH->e_phentsize = 8);
  MUST_REFUSE("phoff_past_eof", EH->e_phoff = (unsigned long)orig_len + 4096);
  MUST_REFUSE("phoff_negative", EH->e_phoff = (unsigned long)-64);
  MUST_REFUSE("phoff_wrap", EH->e_phoff = 0x8000000000000000UL);

  // Program headers
  MUST_REFUSE("filesz_gt_memsz",
              find_ph(PT_LOAD, 0)->p_filesz =
                  find_ph(PT_LOAD, 0)->p_memsz + (CANARY_PAGES / 2) * 4096);
  MUST_REFUSE("memsz_huge", find_ph(PT_LOAD, 0)->p_memsz = 1UL << 62);
  MUST_REFUSE("memsz_wrap", find_ph(PT_LOAD, 1)->p_memsz = (unsigned long)-1);
  MUST_REFUSE("vaddr_wrap",
              find_ph(PT_LOAD, 1)->p_vaddr = (unsigned long)-4096);
  MUST_REFUSE("offset_past_eof",
              find_ph(PT_LOAD, 1)->p_offset = (unsigned long)orig_len + 8192);
  MUST_REFUSE("offset_wrap", find_ph(PT_LOAD, 1)->p_offset = (unsigned long)-1);
  MUST_REFUSE("filesz_past_eof",
              find_ph(PT_LOAD, 0)->p_filesz = (unsigned long)orig_len * 4);
  // A second PT_LOAD over the data page (this was a real crasher found by the
  // fuzzer: GNU_RELRO's type byte became PT_LOAD with no flags)
  MUST_REFUSE("overlapping_load",
              find_ph(0x6474e552 /* PT_GNU_RELRO */, 0)->p_type = PT_LOAD);
  // Data segment with no file bytes at all: its pages (which hold .dynamic)
  // are pure zero-fill; the loader must map them, then find no usable dynamic
  // section instead of faulting (second real crasher from the fuzzer)
  MUST_REFUSE("data_segment_no_file_bytes", find_ph(PT_LOAD, 3)->p_filesz = 0);
  MUST_REFUSE("misaligned_offset", find_ph(PT_LOAD, 1)->p_offset += 8);
  MUST_REFUSE("dynamic_out_of_range",
              find_ph(PT_DYNAMIC, 0)->p_vaddr = 1UL << 40);
  MUST_REFUSE("dynamic_size_zero", find_ph(PT_DYNAMIC, 0)->p_memsz = 0);

  // Dynamic section
  MUST_REFUSE("symtab_wild", find_dyn(DT_SYMTAB)->d_un.d_ptr = 1UL << 45);
  MUST_REFUSE("strtab_wild",
              find_dyn(DT_STRTAB)->d_un.d_ptr = (unsigned long)-8);
  MUST_REFUSE("rela_wild", find_dyn(DT_RELA)->d_un.d_ptr = 1UL << 44);
  MUST_REFUSE("relasz_huge", find_dyn(DT_RELASZ)->d_un.d_val = 1UL << 40);
  MUST_REFUSE("hash_wild", find_dyn(DT_HASH)->d_un.d_ptr = 1UL << 46);
  MUST_REFUSE("strsz_huge", find_dyn(DT_STRSZ)->d_un.d_val = 1UL << 40);
  MUST_REFUSE("syment_bad", find_dyn(DT_SYMENT)->d_un.d_val = 8);
  MUST_REFUSE("relaent_bad", find_dyn(DT_RELAENT)->d_un.d_val = 8);

  // Relocations: write targets and symbol references
  Elf64_Rela *rel = (Elf64_Rela *)dyn_table(DT_RELA);
  if (rel && find_dyn(DT_RELASZ)->d_un.d_val >= sizeof(Elf64_Rela)) {
    MUST_REFUSE("rela_target_wild",
                ((Elf64_Rela *)dyn_table(DT_RELA))->r_offset = 1UL << 44);
    MUST_REFUSE("rela_target_negative",
                ((Elf64_Rela *)dyn_table(DT_RELA))->r_offset =
                    (unsigned long)-8);
    MUST_REFUSE("rela_target_readonly",
                ((Elf64_Rela *)dyn_table(DT_RELA))->r_offset =
                    find_ph(PT_LOAD, 0)->p_vaddr + 16);
    MUST_REFUSE("rela_target_straddles_end",
                ((Elf64_Rela *)dyn_table(DT_RELA))->r_offset =
                    find_ph(PT_LOAD, 1)->p_vaddr +
                    find_ph(PT_LOAD, 1)->p_memsz - 4);
    MUST_REFUSE("rela_sym_out_of_range",
                ((Elf64_Rela *)dyn_table(DT_RELA))->r_info =
                    ((unsigned long)0x7fffffff << 32) | R_X86_64_GLOB_DAT);
    MUST_REFUSE("rela_type_unknown",
                ((Elf64_Rela *)dyn_table(DT_RELA))->r_info = 0x7e);
  }

  Elf64_Rela *plt = (Elf64_Rela *)dyn_table(DT_JMPREL);
  if (plt) {
    MUST_REFUSE("jmprel_target_wild",
                ((Elf64_Rela *)dyn_table(DT_JMPREL))->r_offset = 1UL << 44);
  }

  // Symbols: a name that points outside the string table, an unterminated
  // string, an unresolvable external
  Elf64_Sym *sy = (Elf64_Sym *)dyn_table(DT_SYMTAB);
  CHECK(sy != 0, "plugin has a dynamic symbol table");
  if (sy) {
    // Find an external reference to attack
    int ext = -1;
    Elf64_Dyn *hash = find_dyn(DT_HASH);
    unsigned nsym = hash ? ((uint32_t *)dyn_table(DT_HASH))[1] : 0;
    for (unsigned i = 1; i < nsym; i++) {
      if (sy[i].st_shndx == SHN_UNDEF &&
          ELF64_ST_BIND(sy[i].st_info) == STB_GLOBAL) {
        ext = (int)i;

        break;
      }
    }

    if (ext > 0) {
      MUST_REFUSE("symname_wild",
                  ((Elf64_Sym *)dyn_table(DT_SYMTAB))[ext].st_name =
                      0x7ffffff0);
      MUST_REFUSE(
          "symname_unresolved",
          ((char *)dyn_table(
              DT_STRTAB))[((Elf64_Sym *)dyn_table(DT_SYMTAB))[ext].st_name] =
              'Z');
    }
  }

  // sysv hash with an absurd chain count: loading must not walk past the
  // symbol table when the symbol table is later searched
  reset();
  uint32_t *h = (uint32_t *)dyn_table(DT_HASH);
  if (h) {
    h[1] = 0x7fffffff;
    int r = try_load("nchain_huge");
    CHECK(r == 0, "nchain_huge: expected clean refusal, got %d", r);
  }

  // Truncated files: refused while any loaded segment is cut off; a cut that
  // only removes the section header table (not needed to load) is fine
  reset();
  unsigned long needed = 0;
  for (int i = 0; i < EH->e_phnum; i++) {
    if (PH(i)->p_type == PT_LOAD &&
        PH(i)->p_offset + PH(i)->p_filesz > needed) {
      needed = PH(i)->p_offset + PH(i)->p_filesz;
    }
  }

  for (long cut = 0; cut < orig_len; cut += (cut < 256 ? 7 : 997)) {
    reset();
    long keep = orig_len;
    orig_len = cut;
    int r = try_load("truncated");
    orig_len = keep;
    int want = (unsigned long)cut >= needed ? 1 : 0;
    CHECK(r == want,
          "truncated at %ld (segments need %lu): expected %d, got %d", cut,
          needed, want, r);
    if (r != want) {
      break;
    }
  }

  reset();
}

static void mutate_once(int mode) {
  reset();
  unsigned n = 1 + rnd_n(6);
  Elf64_Phdr *dyn_ph = find_ph(PT_DYNAMIC, 0);
  long lo = 0, hi = orig_len;

  switch (mode) {
  case 0: // ELF header
    hi = (long)sizeof(Elf64_Ehdr);

    break;
  case 1: // program header table
    lo = (long)EH->e_phoff;
    hi = lo + (long)EH->e_phnum * (long)sizeof(Elf64_Phdr);

    break;
  case 2: // dynamic section
    if (dyn_ph) {
      lo = (long)dyn_ph->p_offset;
      hi = lo + (long)dyn_ph->p_filesz;
    }

    break;
  case 3: { // symbol table and relocations
    void *t = dyn_table(rnd_n(2) ? DT_SYMTAB : DT_RELA);
    if (t) {
      lo = (unsigned char *)t - buf;
      hi = lo + 256;
    }

    break;
  }
  case 4: // string table and hash
    if (dyn_table(rnd_n(2) ? DT_STRTAB : DT_HASH)) {
      lo = (unsigned char *)dyn_table(DT_STRTAB) - buf;
      hi = lo + 128;
    }

    break;
  default: // anywhere
    break;
  }

  if (hi > orig_len) {
    hi = orig_len;
  }

  if (lo >= hi) {
    lo = 0;
    hi = orig_len;
  }

  static const unsigned long interesting[] = {0,
                                              1,
                                              8,
                                              0x1000,
                                              0x7fffffff,
                                              0xffffffffUL,
                                              1UL << 40,
                                              1UL << 62,
                                              1UL << 63,
                                              (unsigned long)-1,
                                              (unsigned long)-4096};

  for (unsigned i = 0; i < n; i++) {
    long at = lo + (long)rnd_n((unsigned)(hi - lo));
    if (mode == 6 && at + 8 <= orig_len) { // whole words: boundary values
      unsigned long v =
          interesting[rnd_n(sizeof(interesting) / sizeof(interesting[0]))];
      nl_memcpy(buf + (at & ~7L), &v, 8);
    } else {
      buf[at] = (unsigned char)(rnd() & 0xff);
    }
  }
}

static void fuzz(long iterations) {
  long crashes = 0, loaded = 0, refused = 0;
  for (long it = 0; it < iterations; it++) {
    int mode = (int)(it % 7);
    mutate_once(mode);
    int r = try_load("fuzz");
    if (r < 0) {
      crashes++;
      char name[128];
      nl_snprintf(name, sizeof(name), "fuzz_elf_crash_%ld.so", it);
      write_file(name, buf, orig_len);
      fprintf(stderr, "  iteration %ld (mode %d) saved to %s\n", it, mode,
              name);
      if (crashes >= 5) {
        break;
      }
    } else if (r == 1) {
      loaded++;
    } else {
      refused++;
    }
  }

  fprintf(stderr, "fuzz: %ld refused, %ld loaded, %ld crashed/clobbered\n",
          refused, loaded, crashes);
  CHECK(crashes == 0, "%ld mutated files crashed or clobbered memory", crashes);
}

int main(int argc, char **argv) {
  const char *so = argc > 1 ? argv[1] : "build/edge_detect.so";
  long iters = argc > 2 ? to_long(argv[2]) : 4000;
  unsigned long long seed = argc > 3 ? (unsigned long long)to_long(argv[3]) : 1;
  if (argc > 4) {
    tmp_path = argv[4];
  }

  rng_state = seed * 0x9E3779B97F4A7C15ULL + 1;
  orig_len = read_file(so, orig, MAX_FILE);
  if (orig_len <= (long)sizeof(Elf64_Ehdr)) {
    fprintf(stderr, "error: cannot read '%s' - run `make plugins` first\n", so);

    return 1;
  }

  reset();
  crafted();
  reset();
  fuzz(iters);

  unlink(tmp_path);
  fprintf(stderr, "%d checks, %d failed\n", total, fails);

  return fails ? 1 : 0;
}
