#include "platform.h"

#ifdef __LINUX_NOLIBC__

#include "nl_alloc.h"
#include "nl_dlfcn.h"
#include "nl_elf.h"
#include "nl_printf.h"
#include "nl_string.h"
#include "nl_syscall.h"

// Handle returned by nl_dlopen to callers; opaque to them
typedef struct {
  char *base;
  size_t load_size;
  long load_bias;

  Elf64_Sym *dynsym;
  char *dynstr;
  size_t sym_count; // validated: dynsym[0..sym_count) lies inside mapping
  size_t strsz;     // validated: dynstr[0..strsz) lies inside mapping
} nl_so_t;

// Limits: a plugin is a few KiB; anything near these is hostile or corrupt
#define NL_MAX_PHNUM 64
#define NL_MAX_VADDR (1UL << 40)
#define NL_MAX_SPAN (1UL << 28) // 256 MiB of address space per library

// Last-error buffer
static char _nl_dlerr[256];

static inline void _nl_set_err(const char *s) {
  size_t n = nl_strlen(s);
  if (n >= sizeof(_nl_dlerr)) {
    n = sizeof(_nl_dlerr) - 1;
  }

  nl_memcpy(_nl_dlerr, s, n);
  _nl_dlerr[n] = '\0';
}

const char *nl_dlerror(void) {
  if (!_nl_dlerr[0]) {
    return (const char *)0;
  }

  const char *e = _nl_dlerr;
  _nl_dlerr[0] = '\0';

  return e;
}

/* Symbol table available to loaded plugins
 *
 * External symbols that plugin .so files may reference through PLT
 *
 * Cast to void * is valid on POSIX (dlsym returns function pointers
 * in void *); GCC/Clang both handle it correctly on x86-64 */
static const struct {
  const char *name;
  void *addr;
} _nl_sym_tab[] = {
    {"malloc", (void *)nl_malloc},    {"calloc", (void *)nl_calloc},
    {"free", (void *)nl_free},        {"memcpy", (void *)nl_memcpy},
    {"memset", (void *)nl_memset},    {"strlen", (void *)nl_strlen},
    {"nl_malloc", (void *)nl_malloc}, {"nl_calloc", (void *)nl_calloc},
    {"nl_free", (void *)nl_free},     {"nl_memcpy", (void *)nl_memcpy},
    {"nl_memset", (void *)nl_memset}, {"nl_strlen", (void *)nl_strlen},
    {(const char *)0, (void *)0}};

// Internal helpers
// pread64 via raw syscall
static inline long _nl_pread(int fd, void *buf, size_t n, long off) {
  return __sc6(SYS_pread64, (long)fd, (long)buf, (long)n, off, 0, 0);
}

// Resolve an external symbol name against _nl_sym_tab
static void *_nl_resolve(const char *name) {
  for (int i = 0; _nl_sym_tab[i].name; i++) {
    if (nl_strcmp(name, _nl_sym_tab[i].name) == 0) {
      return _nl_sym_tab[i].addr;
    }
  }

  return (void *)0;
}

// Mapped image: every pointer derived from file goes through _nl_at()
typedef struct {
  char *base;
  size_t total;
  long bias;
  int nseg;
  struct {
    unsigned long start, end; // vaddr range of a PT_LOAD [start, end)
    int writable;
  } seg[NL_MAX_PHNUM];
} nl_map_t;

/* Pointer to [vaddr, vaddr+len) in loaded image, or NULL unless whole range
 * lies inside ONE loaded segment (writable if `write`). Pages between segments
 * are PROT_NONE, so "inside reservation" is not enough */
static void *_nl_at(const nl_map_t *m, unsigned long vaddr, unsigned long len,
                    int write) {
  for (int i = 0; i < m->nseg; i++) {
    if (vaddr >= m->seg[i].start && vaddr <= m->seg[i].end &&
        len <= m->seg[i].end - vaddr && (!write || m->seg[i].writable)) {
      return (void *)(m->bias + (long)vaddr);
    }
  }

  return (void *)0;
}

// NUL-terminated string at tab+off that ends inside table, or NULL
static const char *_nl_str(const char *tab, size_t strsz, unsigned long off) {
  if (off >= strsz) {
    return (const char *)0;
  }

  for (size_t i = (size_t)off; i < strsz; i++) {
    if (tab[i] == '\0') {
      return tab + off;
    }
  }

  return (const char *)0;
}

static void *_nl_fail(const char *msg, int fd, const nl_map_t *m) {
  _nl_set_err(msg);
  if (fd >= 0) {
    __sc1(SYS_close, fd);
  }

  if (m && m->base) {
    __sc2(SYS_munmap, (long)m->base, (long)m->total);
  }

  return (void *)0;
}

/* Apply a table of Elf64_Rela relocations. Everything file controls is checked
 * first: table is inside image, each target is 8 writable bytes inside a
 * writable segment, each symbol index and name is inside tables
 * Returns 0, or -1 with reason in error buffer
 *
 *   S = symbol value (0 for R_X86_64_RELATIVE which has no associated sym)
 *   A = r_addend
 *   B = load_bias  (how far load base was shifted from p_vaddr==0)
 *
 * R_X86_64_RELATIVE  *target = B + A
 * R_X86_64_GLOB_DAT  *target = S
 * R_X86_64_JUMP_SLOT *target = S
 * R_X86_64_64        *target = S + A */
static int _nl_apply_rela(const nl_map_t *m, const Elf64_Rela *relas,
                          size_t count, const Elf64_Sym *dynsym,
                          size_t sym_count, const char *dynstr, size_t strsz) {
  for (size_t i = 0; i < count; i++) {
    const Elf64_Rela *r = &relas[i];
    uint64_t *tgt = (uint64_t *)_nl_at(m, r->r_offset, sizeof(uint64_t), 1);
    uint32_t sym_idx = ELF64_R_SYM(r->r_info);
    uint32_t type = ELF64_R_TYPE(r->r_info);
    if (!tgt) {
      _nl_set_err("nl_dlopen: relocation target outside writable segments");

      return -1;
    }

    uint64_t S = 0;
    if (type != R_X86_64_NONE && type != R_X86_64_RELATIVE && sym_idx != 0) {
      if (sym_idx >= sym_count) {
        _nl_set_err("nl_dlopen: relocation symbol index out of range");

        return -1;
      }

      const Elf64_Sym *sym = &dynsym[sym_idx];
      if (sym->st_shndx != SHN_UNDEF) {
        S = (uint64_t)((long)sym->st_value + m->bias); // defined in this .so
      } else {
        const char *name = _nl_str(dynstr, strsz, sym->st_name);
        if (!name) {
          _nl_set_err("nl_dlopen: symbol name outside string table");

          return -1;
        }

        S = (uint64_t)_nl_resolve(name);
        if (!S && ELF64_ST_BIND(sym->st_info) != STB_WEAK) {
          char tmp[160];
          nl_snprintf(tmp, sizeof(tmp), "nl_dlopen: unresolved symbol '%s'",
                      name);
          _nl_set_err(tmp);

          return -1;
        }
      }
    }

    switch (type) {
    case R_X86_64_NONE:
      break;
    case R_X86_64_RELATIVE:
      *tgt = (uint64_t)(m->bias + r->r_addend);

      break;
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT:
      *tgt = S;

      break;
    case R_X86_64_64:
      *tgt = S + (uint64_t)r->r_addend;

      break;
    default:
      _nl_set_err("nl_dlopen: unsupported relocation type");

      return -1;
    }
  }

  return 0;
}

// public APIs
/* nl_dlopen */
/*
 * Load a shared object (.so) file into memory
 *
 * File is untrusted: every offset, size and address in it is checked
 * before use, and a failure at any point unmaps whatever was mapped
 *
 * 1. pread ELF header; validate magic / class / type / machine / sizes
 * 2. pread all program headers (inside file)
 * 3. Validate PT_LOADs and compute [load_min, load_max
 * 4. mmap(PROT_NONE) to reserve a contiguous address range of that size
 *    load_bias = reservation_base − load_min
 * 5. For each PT_LOAD segment:
 *    a. mmap(MAP_FIXED, fd, aligned file offset) file content
 *    b. zero the tail of the last file page, mmap anonymous pages for the rest
 * 6. Locate PT_DYNAMIC; walk DT_* entries (bounded) to find SYMTAB / STRTAB /
 *    RELA / JMPREL and sysv .hash nchain for symbol count
 * 7. Apply RELA relocations from .rela.dyn and .rela.plt
 * 8. Return an nl_so_t handle */
void *nl_dlopen(const char *path, int flags) {
  (void)flags;
  _nl_dlerr[0] = '\0';

  // 1. open, size
  int fd = (int)__sc3(SYS_open, (long)path, (long)O_RDONLY, 0);
  if (fd < 0) {
    return _nl_fail("nl_dlopen: cannot open file", -1, (nl_map_t *)0);
  }

  long fsz_l = __sc3(SYS_lseek, (long)fd, 0, 2 /* SEEK_END */);
  if (fsz_l < 0) {
    return _nl_fail("nl_dlopen: cannot determine file size", fd, (nl_map_t *)0);
  }

  unsigned long fsize = (unsigned long)fsz_l;

  // 2. read ELF header
  Elf64_Ehdr ehdr = {0}; // filled by a raw pread syscall
  if (_nl_pread(fd, &ehdr, sizeof(ehdr), 0) != (long)sizeof(ehdr)) {
    return _nl_fail("nl_dlopen: short ELF header read", fd, (nl_map_t *)0);
  }

  // 3. validate
  if (ehdr.e_ident[EI_MAG0] != 0x7f || ehdr.e_ident[EI_MAG1] != 'E' ||
      ehdr.e_ident[EI_MAG2] != 'L' || ehdr.e_ident[EI_MAG3] != 'F' ||
      ehdr.e_ident[EI_CLASS] != 2 || // ELFCLASS64
      ehdr.e_ident[EI_DATA] != 1 ||  // little endian
      ehdr.e_type != ET_DYN || ehdr.e_machine != EM_X86_64) {

    return _nl_fail("nl_dlopen: not a valid x86-64 shared object", fd,
                    (nl_map_t *)0);
  }

  if (ehdr.e_phentsize != sizeof(Elf64_Phdr) || ehdr.e_phnum == 0 ||
      ehdr.e_phnum > NL_MAX_PHNUM) {
    return _nl_fail("nl_dlopen: bad program header table size", fd,
                    (nl_map_t *)0);
  }

  // 4. read program headers
  int phnum = (int)ehdr.e_phnum;
  Elf64_Phdr phdrs[NL_MAX_PHNUM];
  unsigned long phsz = (unsigned long)phnum * sizeof(Elf64_Phdr);
  if (ehdr.e_phoff > fsize || phsz > fsize - ehdr.e_phoff ||
      _nl_pread(fd, phdrs, (size_t)phsz, (long)ehdr.e_phoff) != (long)phsz) {
    return _nl_fail("nl_dlopen: program headers outside file", fd,
                    (nl_map_t *)0);
  }

  // 5. validate PT_LOADs, compute load range
  unsigned long load_min = (unsigned long)-1UL, load_max = 0;
  for (int i = 0; i < phnum; i++) {
    const Elf64_Phdr *p = &phdrs[i];
    if (p->p_type != PT_LOAD || p->p_memsz == 0) {
      continue;
    }

    if (p->p_vaddr > NL_MAX_VADDR || p->p_memsz > NL_MAX_SPAN ||
        p->p_filesz > p->p_memsz || p->p_offset > fsize ||
        p->p_filesz > fsize - p->p_offset ||
        (p->p_vaddr & (NL_PAGE_SIZE - 1)) !=
            (p->p_offset & (NL_PAGE_SIZE - 1))) {
      return _nl_fail("nl_dlopen: malformed PT_LOAD segment", fd,
                      (nl_map_t *)0);
    }

    if ((p->p_flags & PF_W) && (p->p_flags & PF_X)) {
      return _nl_fail("nl_dlopen: writable+executable segment refused", fd,
                      (nl_map_t *)0);
    }

    if (p->p_memsz > p->p_filesz && !(p->p_flags & PF_W)) {
      return _nl_fail("nl_dlopen: read-only segment with zero-fill", fd,
                      (nl_map_t *)0);
    }

    unsigned long va = (unsigned long)p->p_vaddr;
    unsigned long end = va + (unsigned long)p->p_memsz; // no wrap: bounded

    // Segments are mapped page by page with MAP_FIXED: two sharing a page
    // would replace each other's mapping (and its permissions), so the
    // writable-segment checks later would describe memory that is not there
    for (int j = 0; j < i; j++) {
      const Elf64_Phdr *q = &phdrs[j];
      if (q->p_type != PT_LOAD || q->p_memsz == 0) {
        continue;
      }

      if (NL_ALIGN_DOWN(va, NL_PAGE_SIZE) <
              NL_ALIGN_UP(q->p_vaddr + q->p_memsz, NL_PAGE_SIZE) &&
          NL_ALIGN_DOWN(q->p_vaddr, NL_PAGE_SIZE) <
              NL_ALIGN_UP(end, NL_PAGE_SIZE)) {
        return _nl_fail("nl_dlopen: PT_LOAD segments share a page", fd,
                        (nl_map_t *)0);
      }
    }

    if (va < load_min) {
      load_min = va;
    }
    if (end > load_max) {
      load_max = end;
    }
  }

  if (load_max == 0) {
    return _nl_fail("nl_dlopen: no loadable segments", fd, (nl_map_t *)0);
  }

  load_min = NL_ALIGN_DOWN(load_min, NL_PAGE_SIZE);
  load_max = NL_ALIGN_UP(load_max, NL_PAGE_SIZE);
  if (load_max - load_min > NL_MAX_SPAN) {
    return _nl_fail("nl_dlopen: image span too large", fd, (nl_map_t *)0);
  }

  nl_map_t m = {0};
  m.total = load_max - load_min;

  // 6. reserve address space
  long mret = __sc6(SYS_mmap, 0, (long)m.total, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mret < 0 && mret > -4096) {
    return _nl_fail("nl_dlopen: address reservation mmap failed", fd,
                    (nl_map_t *)0);
  }

  m.base = (char *)mret;
  m.bias = (long)m.base - (long)load_min;

  // 7. map each PT_LOAD segment
  for (int i = 0; i < phnum; i++) {
    Elf64_Phdr *p = &phdrs[i];
    if (p->p_type != PT_LOAD || p->p_memsz == 0) {
      continue;
    }

    int prot = ((p->p_flags & PF_R) ? PROT_READ : 0) |
               ((p->p_flags & PF_W) ? PROT_WRITE : 0) |
               ((p->p_flags & PF_X) ? PROT_EXEC : 0);
    if (!prot) {
      prot = PROT_READ; // sanity
    }

    // File content: align offset and address to page boundary
    // Everything below stays inside [base, base+total): checked, not assumed
    unsigned long off_align = NL_ALIGN_DOWN(p->p_offset, NL_PAGE_SIZE);
    unsigned long va_align = NL_ALIGN_DOWN(p->p_vaddr, NL_PAGE_SIZE);
    unsigned long addr_align = (unsigned long)m.bias + va_align;
    unsigned long map_sz =
        NL_ALIGN_UP((p->p_offset - off_align) + p->p_filesz, NL_PAGE_SIZE);
    unsigned long bss_end_va =
        NL_ALIGN_UP(p->p_vaddr + p->p_memsz, NL_PAGE_SIZE);
    if (va_align < load_min || bss_end_va > load_max ||
        va_align + map_sz > load_max) {
      return _nl_fail("nl_dlopen: segment outside reservation", fd, &m);
    }

    if (p->p_filesz > 0) {
      long r = __sc6(SYS_mmap, (long)addr_align, (long)map_sz, prot,
                     MAP_PRIVATE | MAP_FIXED, (long)fd, (long)off_align);
      if (r != (long)addr_align) {
        char tmp[128];
        nl_snprintf(tmp, sizeof(tmp),
                    "nl_dlopen: segment mmap failed (err %ld)", -r);

        return _nl_fail(tmp, fd, &m);
      }
    }

    // Zero-fill (.bss): the rest of the last file page still holds file
    // bytes (the next section, not zeros), so clear it by hand, then give
    // the pages beyond it anonymous zero memory
    if (p->p_memsz > p->p_filesz) {
      unsigned long file_end = (unsigned long)m.bias + p->p_vaddr + p->p_filesz;
      unsigned long page_end = NL_ALIGN_UP(file_end, NL_PAGE_SIZE);
      unsigned long mem_end = (unsigned long)m.bias + p->p_vaddr + p->p_memsz;
      unsigned long zero_to = mem_end < page_end ? mem_end : page_end;
      if (p->p_filesz > 0 && zero_to > file_end) {
        nl_memset((void *)file_end, 0, zero_to - file_end);
      }

      // With no file bytes the very first page needs mapping too
      unsigned long bss_start = p->p_filesz > 0 ? page_end : addr_align;
      unsigned long bss_end = (unsigned long)m.bias + bss_end_va;
      if (bss_end > bss_start) {
        long r = __sc6(SYS_mmap, (long)bss_start, (long)(bss_end - bss_start),
                       prot, MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
        if (r != (long)bss_start) {
          return _nl_fail("nl_dlopen: bss mmap failed", fd, &m);
        }
      }
    }

    m.seg[m.nseg].start = p->p_vaddr;
    m.seg[m.nseg].end = p->p_vaddr + p->p_memsz;
    m.seg[m.nseg].writable = (p->p_flags & PF_W) ? 1 : 0;
    m.nseg++;
  }

  __sc1(SYS_close, fd); // fd no longer needed; MAP_PRIVATE copies stay
  fd = -1;

  // 8. locate PT_DYNAMIC (must lie in a loaded segment; walk is bounded)
  const Elf64_Dyn *dyn = (const Elf64_Dyn *)0;
  size_t ndyn = 0;
  for (int i = 0; i < phnum; i++) {
    if (phdrs[i].p_type == PT_DYNAMIC) {
      dyn =
          (const Elf64_Dyn *)_nl_at(&m, phdrs[i].p_vaddr, phdrs[i].p_memsz, 0);
      ndyn = (size_t)(phdrs[i].p_memsz / sizeof(Elf64_Dyn));

      break;
    }
  }

  if (!dyn || ndyn == 0) {
    return _nl_fail("nl_dlopen: no usable PT_DYNAMIC segment", fd, &m);
  }

  // 9. parse dynamic section
  unsigned long symtab = 0;
  unsigned long strtab = 0;
  unsigned long rela = 0;
  unsigned long jmprel = 0;
  unsigned long hash = 0;
  unsigned long relasz = 0;
  unsigned long pltrelsz = 0;
  unsigned long strsz = 0;
  int have_symtab = 0;
  int have_strtab = 0;
  int have_hash = 0;
  int terminated = 0;

  for (size_t i = 0; i < ndyn; i++) {
    const Elf64_Dyn *d = &dyn[i];
    if (d->d_tag == DT_NULL) {
      terminated = 1;

      break;
    }

    switch (d->d_tag) {
    case DT_SYMTAB:
      symtab = d->d_un.d_ptr;
      have_symtab = 1;

      break;
    case DT_STRTAB:
      strtab = d->d_un.d_ptr;
      have_strtab = 1;

      break;
    case DT_STRSZ:
      strsz = d->d_un.d_val;

      break;
    case DT_RELA:
      rela = d->d_un.d_ptr;

      break;
    case DT_RELASZ:
      relasz = d->d_un.d_val;

      break;
    case DT_JMPREL:
      jmprel = d->d_un.d_ptr;

      break;
    case DT_PLTRELSZ:
      pltrelsz = d->d_un.d_val;

      break;
    case DT_HASH:
      hash = d->d_un.d_ptr;
      have_hash = 1;

      break;
    case DT_SYMENT:
      if (d->d_un.d_val != sizeof(Elf64_Sym)) {
        return _nl_fail("nl_dlopen: unexpected DT_SYMENT", fd, &m);
      }

      break;
    case DT_RELAENT:
      if (d->d_un.d_val != sizeof(Elf64_Rela)) {
        return _nl_fail("nl_dlopen: unexpected DT_RELAENT", fd, &m);
      }

      break;
    case DT_PLTREL:
      if (d->d_un.d_val != DT_RELA) {
        return _nl_fail("nl_dlopen: only RELA relocations supported", fd, &m);
      }

      break;
    default:
      break;
    }
  }

  if (!terminated) {
    return _nl_fail("nl_dlopen: unterminated dynamic section", fd, &m);
  }

  if (!have_symtab || !have_strtab || strsz == 0) {
    return _nl_fail("nl_dlopen: missing DT_SYMTAB, DT_STRTAB or DT_STRSZ", fd,
                    &m);
  }

  if (!have_hash) {
    return _nl_fail("nl_dlopen: missing DT_HASH (link with --hash-style=both)",
                    fd, &m);
  }

  // sysv .hash header: [0]=nbuckets [1]=nchain (= symbol count); the whole
  // hash table and the symbol table must lie inside the image
  const uint32_t *syshash = (const uint32_t *)_nl_at(&m, hash, 8, 0);
  if (!syshash) {
    return _nl_fail("nl_dlopen: DT_HASH outside image", fd, &m);
  }

  unsigned long nbucket = syshash[0], nchain = syshash[1];
  if (!_nl_at(&m, hash, 8 + 4 * (nbucket + nchain), 0) || nchain == 0 ||
      nchain > NL_MAX_SPAN / sizeof(Elf64_Sym)) {
    return _nl_fail("nl_dlopen: DT_HASH table malformed", fd, &m);
  }

  size_t sym_count = (size_t)nchain;
  Elf64_Sym *dynsym =
      (Elf64_Sym *)_nl_at(&m, symtab, sym_count * sizeof(Elf64_Sym), 0);
  char *dynstr = (char *)_nl_at(&m, strtab, strsz, 0);
  if (!dynsym || !dynstr) {
    return _nl_fail("nl_dlopen: symbol or string table outside image", fd, &m);
  }

  if (relasz % sizeof(Elf64_Rela) || pltrelsz % sizeof(Elf64_Rela)) {
    return _nl_fail("nl_dlopen: relocation table size not a multiple of 24", fd,
                    &m);
  }

  // 10. apply relocations
  if (relasz > 0) {
    const Elf64_Rela *tab = (const Elf64_Rela *)_nl_at(&m, rela, relasz, 0);
    if (!tab) {
      return _nl_fail("nl_dlopen: DT_RELA outside image", fd, &m);
    }

    if (_nl_apply_rela(&m, tab, relasz / sizeof(Elf64_Rela), dynsym, sym_count,
                       dynstr, strsz) < 0) {
      return _nl_fail(_nl_dlerr, fd, &m);
    }
  }

  if (pltrelsz > 0) {
    const Elf64_Rela *tab = (const Elf64_Rela *)_nl_at(&m, jmprel, pltrelsz, 0);
    if (!tab) {
      return _nl_fail("nl_dlopen: DT_JMPREL outside image", fd, &m);
    }

    if (_nl_apply_rela(&m, tab, pltrelsz / sizeof(Elf64_Rela), dynsym,
                       sym_count, dynstr, strsz) < 0) {
      return _nl_fail(_nl_dlerr, fd, &m);
    }
  }

  // 11. build handle
  nl_so_t *so = (nl_so_t *)nl_malloc(sizeof(nl_so_t));
  if (!so) {
    return _nl_fail("nl_dlopen: out of memory for handle", fd, &m);
  }

  so->base = m.base;
  so->load_size = m.total;
  so->load_bias = m.bias;
  so->dynsym = dynsym;
  so->dynstr = dynstr;
  so->sym_count = sym_count;
  so->strsz = (size_t)strsz;

  return (void *)so;
}

// nl_dlsym
void *nl_dlsym(void *handle, const char *name) {
  nl_so_t *so = (nl_so_t *)handle;
  if (!so || !so->dynsym || !so->dynstr || !name) {
    return (void *)0;
  }

  for (size_t i = 0; i < so->sym_count; i++) {
    const Elf64_Sym *sym = &so->dynsym[i];
    int bind = (int)ELF64_ST_BIND(sym->st_info);
    if ((bind == STB_GLOBAL || bind == STB_WEAK) &&
        sym->st_shndx != SHN_UNDEF) {
      const char *sn = _nl_str(so->dynstr, so->strsz, sym->st_name);
      if (sn && nl_strcmp(sn, name) == 0) {
        return (void *)(so->load_bias + (long)sym->st_value);
      }
    }
  }

  {
    char tmp[160];
    nl_snprintf(tmp, sizeof(tmp), "nl_dlsym: '%s' not found", name);
    _nl_set_err(tmp);
  }

  return (void *)0;
}

// nl_dlclose
int nl_dlclose(void *handle) {
  nl_so_t *so = (nl_so_t *)handle;
  if (!so) {
    return -1;
  }

  __sc2(SYS_munmap, (long)so->base, (long)so->load_size);
  nl_free(so);

  return 0;
}

#endif /* __LINUX_NOLIBC__ */
