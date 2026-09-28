#ifndef NL_ELF_H
#define NL_ELF_H

/*
 * nl_elf.h - ELF64 types and constants
 *
 * ELF header, program header, dynamic section, symbol table, and RELA
 * relocations for x86-64
 */

#include <stdint.h>

/* Scalar typedefs */
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t Elf64_Sxword;

/* ELF header (64 bytes) */
#define EI_NIDENT 16

typedef struct {
  unsigned char e_ident[EI_NIDENT];
  Elf64_Half e_type;
  Elf64_Half e_machine;
  Elf64_Word e_version;
  Elf64_Addr e_entry;
  Elf64_Off e_phoff; /* program header table offset */
  Elf64_Off e_shoff;
  Elf64_Word e_flags;
  Elf64_Half e_ehsize;
  Elf64_Half e_phentsize;
  Elf64_Half e_phnum; /* number of program headers   */
  Elf64_Half e_shentsize;
  Elf64_Half e_shnum;
  Elf64_Half e_shstrndx;
} Elf64_Ehdr;

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4 /* 2 = ELFCLASS64    */
#define EI_DATA 5  /* 1 = ELFDATA2LSB   */

#define ET_DYN 3 /* shared object     */
#define EM_X86_64 62

/* Program header (56 bytes) */
typedef struct {
  Elf64_Word p_type;
  Elf64_Word p_flags;
  Elf64_Off p_offset; /* offset in file                        */
  Elf64_Addr p_vaddr; /* virtual address in memory             */
  Elf64_Addr p_paddr;
  Elf64_Xword p_filesz; /* bytes in file image                  */
  Elf64_Xword p_memsz;  /* bytes in memory image (>= p_filesz)  */
  Elf64_Xword p_align;
} Elf64_Phdr;

#define PT_LOAD 1
#define PT_DYNAMIC 2

#define PF_X 1
#define PF_W 2
#define PF_R 4

/* Dynamic section entry (16 bytes) */
typedef struct {
  Elf64_Sxword d_tag;
  union {
    Elf64_Xword d_val;
    Elf64_Addr d_ptr;
  } d_un;
} Elf64_Dyn;

#define DT_NULL 0
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_SYMENT 11
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_GNU_HASH 0x6ffffef5UL

/* Symbol table entry (24 bytes) */
typedef struct {
  Elf64_Word st_name;
  unsigned char st_info;
  unsigned char st_other;
  Elf64_Half st_shndx;
  Elf64_Addr st_value;
  Elf64_Xword st_size;
} Elf64_Sym;

#define ELF64_ST_BIND(i) ((unsigned char)(i) >> 4)
#define ELF64_ST_TYPE(i) ((unsigned char)(i) & 0xf)

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define SHN_UNDEF 0

/* RELA relocation entry (24 bytes) */
typedef struct {
  Elf64_Addr r_offset;
  Elf64_Xword r_info;
  Elf64_Sxword r_addend;
} Elf64_Rela;

#define ELF64_R_SYM(i) ((uint32_t)((uint64_t)(i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)((uint64_t)(i) & 0xffffffffULL))

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

/* Utility macros */
#define NL_PAGE_SIZE 4096UL
#define NL_ALIGN_DOWN(v, a) ((unsigned long)(v) & ~((unsigned long)(a) - 1))
#define NL_ALIGN_UP(v, a)                                                      \
  (((unsigned long)(v) + (unsigned long)(a) - 1) & ~((unsigned long)(a) - 1))

#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_FIXED
#define MAP_FIXED 0x10
#endif

#endif /* NL_ELF_H */
