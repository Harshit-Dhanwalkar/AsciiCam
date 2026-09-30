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
  size_t sym_count;
} nl_so_t;

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
 * in void *); GCC/Clang both handle it correctly on x86-64
 */
static const struct {
  const char *name;
  void *addr;
} _nl_sym_tab[] = {{"malloc", (void *)nl_malloc}, {"calloc", (void *)nl_calloc},
                   {"free", (void *)nl_free},     {"memcpy", (void *)nl_memcpy},
                   {"memset", (void *)nl_memset}, {"strlen", (void *)nl_strlen},
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

/*
 * Apply a table of Elf64_Rela relocations
 *
 *   S = symbol value (0 for R_X86_64_RELATIVE which has no associated sym)
 *   A = r_addend
 *   B = load_bias  (how far the load base was shifted from p_vaddr==0)
 *
 * R_X86_64_RELATIVE  *target = B + A
 * R_X86_64_GLOB_DAT  *target = S
 * R_X86_64_JUMP_SLOT *target = S
 * R_X86_64_64        *target = S + A
 */
static void _nl_apply_rela(const Elf64_Rela *relas, size_t count,
                           const Elf64_Sym *dynsym, const char *dynstr,
                           long bias) {
  for (size_t i = 0; i < count; i++) {
    const Elf64_Rela *r = &relas[i];
    uint64_t *tgt = (uint64_t *)(bias + (long)r->r_offset);
    uint32_t sym_idx = ELF64_R_SYM(r->r_info);
    uint32_t type = ELF64_R_TYPE(r->r_info);

    uint64_t S = 0;
    if (sym_idx != 0) {
      const Elf64_Sym *sym = &dynsym[sym_idx];
      if (sym->st_shndx != SHN_UNDEF) {
        /* defined within this .so */
        S = (uint64_t)((long)sym->st_value + bias);
      } else {
        /* external - look up in symbol table */
        S = (uint64_t)_nl_resolve(dynstr + sym->st_name);
        /* Unknown external symbols are left as 0 */
      }
    }

    switch (type) {
    case R_X86_64_NONE:
      break;
    case R_X86_64_RELATIVE:
      *tgt = (uint64_t)(bias + r->r_addend);

      break;
    case R_X86_64_GLOB_DAT:
      *tgt = S;

      break;
    case R_X86_64_JUMP_SLOT:
      *tgt = S;

      break;
    case R_X86_64_64:
      *tgt = S + (uint64_t)r->r_addend;

      break;

    default:
      break; // silently skip unknown types
    }
  }
}

// public APIs
/* nl_dlopen */
/*
 * Load a shared object (.so) file into memory
 *
 * Algorithm
 * 1. pread ELF header; validate magic / class / type / machine
 * 2. pread all program headers
 * 3. Walk PT_LOAD to find [load_min, load_max) - total virtual span
 * 4. mmap(PROT_NONE) to reserve a contiguous address range of that size
 *    load_bias = reservation_base − load_min
 * 5. For each PT_LOAD segment:
 *    a. mmap(MAP_FIXED, fd, aligned file offset) file content
 *    b. mmap(MAP_FIXED|MAP_ANONYMOUS) for BSS pages beyond p_filesz
 * 6. Locate PT_DYNAMIC; walk DT_* entries to find SYMTAB / STRTAB /
 *    RELA / JMPREL and sysv .hash nchain for symbol count
 * 7. Apply RELA relocations from .rela.dyn and .rela.plt
 * 8. Return an nl_so_t handle
 */
void *nl_dlopen(const char *path, int flags) {
  (void)flags;
  _nl_dlerr[0] = '\0';

  // 1. open
  int fd = (int)__sc3(SYS_open, (long)path, (long)O_RDONLY, 0);
  if (fd < 0) {
    _nl_set_err("nl_dlopen: cannot open file");

    return (void *)0;
  }

  // 2. read ELF header
  Elf64_Ehdr ehdr;
  if (_nl_pread(fd, &ehdr, sizeof(ehdr), 0) != (long)sizeof(ehdr)) {
    _nl_set_err("nl_dlopen: short ELF header read");
    __sc1(SYS_close, fd);

    return (void *)0;
  }

  // 3. validate
  if (ehdr.e_ident[EI_MAG0] != 0x7f || ehdr.e_ident[EI_MAG1] != 'E' ||
      ehdr.e_ident[EI_MAG2] != 'L' || ehdr.e_ident[EI_MAG3] != 'F' ||
      ehdr.e_ident[EI_CLASS] != 2 || /* ELFCLASS64 */
      ehdr.e_type != ET_DYN || ehdr.e_machine != EM_X86_64) {
    _nl_set_err("nl_dlopen: not a valid x86-64 shared object");
    __sc1(SYS_close, fd);

    return (void *)0;
  }

  // 4. read program headers (cap at 64)
  int phnum = ehdr.e_phnum > 64 ? 64 : (int)ehdr.e_phnum;
  Elf64_Phdr phdrs[64];
  long phsz = (long)((size_t)phnum * sizeof(Elf64_Phdr));
  if (_nl_pread(fd, phdrs, (size_t)phsz, (long)ehdr.e_phoff) != phsz) {
    _nl_set_err("nl_dlopen: short program header read");
    __sc1(SYS_close, fd);

    return (void *)0;
  }

  // 5. compute load range
  unsigned long load_min = (unsigned long)-1UL, load_max = 0;
  for (int i = 0; i < phnum; i++) {
    if (phdrs[i].p_type != PT_LOAD || phdrs[i].p_memsz == 0) {
      continue;
    }

    unsigned long va = (unsigned long)phdrs[i].p_vaddr;
    unsigned long end = va + (unsigned long)phdrs[i].p_memsz;
    if (va < load_min) {
      load_min = va;
    }
    if (end > load_max) {
      load_max = end;
    }
  }

  if (load_max == 0) {
    _nl_set_err("nl_dlopen: no loadable segments");
    __sc1(SYS_close, fd);

    return (void *)0;
  }

  load_min = NL_ALIGN_DOWN(load_min, NL_PAGE_SIZE);
  load_max = NL_ALIGN_UP(load_max, NL_PAGE_SIZE);
  size_t total = load_max - load_min;

  // 6. reserve address space
  long mret = __sc6(SYS_mmap, 0, (long)total, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mret < 0) {
    _nl_set_err("nl_dlopen: address reservation mmap failed");
    __sc1(SYS_close, fd);

    return (void *)0;
  }

  char *base = (char *)mret;
  long bias = (long)base - (long)load_min;

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

    // file content: align offset and address to page boundary
    unsigned long off_align = NL_ALIGN_DOWN(p->p_offset, NL_PAGE_SIZE);
    unsigned long addr_align = bias + NL_ALIGN_DOWN(p->p_vaddr, NL_PAGE_SIZE);
    unsigned long map_sz =
        NL_ALIGN_UP((p->p_offset - off_align) + p->p_filesz, NL_PAGE_SIZE);

    if (p->p_filesz > 0) {
      long r = __sc6(SYS_mmap, (long)addr_align, (long)map_sz, prot,
                     MAP_PRIVATE | MAP_FIXED, (long)fd, (long)off_align);
      if (r < 0) {
        char tmp[128];
        nl_snprintf(tmp, sizeof(tmp),
                    "nl_dlopen: segment mmap failed (err %ld)", -r);
        _nl_set_err(tmp);
        __sc2(SYS_munmap, (long)base, (long)total);
        __sc1(SYS_close, fd);

        return (void *)0;
      }
    }

    // BSS: anonymous pages for p_memsz > p_filesz
    unsigned long bss_start =
        NL_ALIGN_UP(bias + p->p_vaddr + p->p_filesz, NL_PAGE_SIZE);
    unsigned long bss_end =
        NL_ALIGN_UP(bias + p->p_vaddr + p->p_memsz, NL_PAGE_SIZE);
    if (bss_end > bss_start) {
      // Partial page at p_filesz boundary is already zero-filled by kernel
      // (MAP_PRIVATE over file always zeroes bytes past EOF within same page).
      // Map full anonymous pages for remainder
      __sc6(SYS_mmap, (long)bss_start, (long)(bss_end - bss_start), prot,
            MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
    }
  }

  __sc1(SYS_close, fd); // fd no longer needed; MAP_PRIVATE copies stay

  // 8. locate PT_DYNAMIC
  Elf64_Dyn *dyn = (Elf64_Dyn *)0;
  for (int i = 0; i < phnum; i++) {
    if (phdrs[i].p_type == PT_DYNAMIC) {
      dyn = (Elf64_Dyn *)(bias + (long)phdrs[i].p_vaddr);

      break;
    }
  }

  if (!dyn) {
    _nl_set_err("nl_dlopen: no PT_DYNAMIC segment");
    __sc2(SYS_munmap, (long)base, (long)total);

    return (void *)0;
  }

  /* 9. parse dynamic section */
  Elf64_Sym *dynsym = (Elf64_Sym *)0;
  char *dynstr = (char *)0;
  const Elf64_Rela *rela = (const Elf64_Rela *)0;
  const Elf64_Rela *jmprel = (const Elf64_Rela *)0;
  size_t relasz = 0;
  size_t pltrelsz = 0;
  uint32_t *syshash = (uint32_t *)0; /* sysv .hash for sym count */

  for (Elf64_Dyn *d = dyn; d->d_tag != DT_NULL; d++) {
    switch (d->d_tag) {
    case DT_SYMTAB:
      dynsym = (Elf64_Sym *)(bias + (long)d->d_un.d_ptr);

      break;
    case DT_STRTAB:
      dynstr = (char *)(bias + (long)d->d_un.d_ptr);

      break;
    case DT_RELA:
      rela = (const Elf64_Rela *)(bias + (long)d->d_un.d_ptr);

      break;
    case DT_RELASZ:
      relasz = (size_t)d->d_un.d_val;

      break;
    case DT_JMPREL:
      jmprel = (const Elf64_Rela *)(bias + (long)d->d_un.d_ptr);

      break;
    case DT_PLTRELSZ:
      pltrelsz = (size_t)d->d_un.d_val;

      break;
    case DT_HASH:
      syshash = (uint32_t *)(bias + (long)d->d_un.d_ptr);

      break;

    default:
      break;
    }
  }

  if (!dynsym || !dynstr) {
    _nl_set_err("nl_dlopen: missing DT_SYMTAB or DT_STRTAB");
    __sc2(SYS_munmap, (long)base, (long)total);

    return (void *)0;
  }

  // symbol count from sysv .hash header: [0]=nbuckets [1]=nchain
  size_t sym_count = syshash ? (size_t)syshash[1] : 0;

  // 10. apply relocations
  if (rela && relasz > 0) {
    _nl_apply_rela(rela, relasz / sizeof(Elf64_Rela), dynsym, dynstr, bias);
  }
  if (jmprel && pltrelsz > 0) {
    _nl_apply_rela(jmprel, pltrelsz / sizeof(Elf64_Rela), dynsym, dynstr, bias);
  }

  // 11. build handle
  nl_so_t *so = (nl_so_t *)nl_malloc(sizeof(nl_so_t));
  if (!so) {
    _nl_set_err("nl_dlopen: out of memory for handle");
    __sc2(SYS_munmap, (long)base, (long)total);

    return (void *)0;
  }

  so->base = base;
  so->load_size = total;
  so->load_bias = bias;
  so->dynsym = dynsym;
  so->dynstr = dynstr;
  so->sym_count = sym_count;

  return (void *)so;
}

/* nl_dlsym */
void *nl_dlsym(void *handle, const char *name) {
  nl_so_t *so = (nl_so_t *)handle;
  if (!so || !so->dynsym || !so->dynstr || !name) {
    return (void *)0;
  }

  for (size_t i = 0; i < so->sym_count; i++) {
    const Elf64_Sym *sym = &so->dynsym[i];
    int bind = (int)ELF64_ST_BIND(sym->st_info);
    if ((bind == STB_GLOBAL || bind == STB_WEAK) &&
        sym->st_shndx != SHN_UNDEF &&
        nl_strcmp(so->dynstr + sym->st_name, name) == 0) {
      return (void *)(so->load_bias + (long)sym->st_value);
    }
  }

  {
    char tmp[160];
    nl_snprintf(tmp, sizeof(tmp), "nl_dlsym: '%s' not found", name);
    _nl_set_err(tmp);
  }

  return (void *)0;
}

/* nl_dlclose */
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
