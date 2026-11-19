/*
 * src/ramfs_blob.S - links a packed nl_ramfs archive (lib/nl_ramfs.h) into  binary
 * as read-only data
 *
 * Build with archive's path in NL_RAMFS_FILE, e.g.
 *   gcc -DNL_RAMFS_FILE='"build/ramfs.bin"' -c src/ramfs_blob.S -o ramfs_blob.o
 *
 * Nothing but those two symbols is defined; nl_ramfs_builtin() turns them into an
 * archive
 * File is read by assembler, so compiler's dependency output does not list it: a
 * build system has to make ramfs_blob.o depend on it itself
 *
 * NOTE: ELF only (x86-64 and AArch64 alike: no instructions, just data)
 */

#ifndef NL_RAMFS_FILE
#error "define NL_RAMFS_FILE to path of archive, as a string literal"
#endif

    .section .rodata.nl_ramfs, "a", @progbits
    .balign 16

    .globl nl_ramfs_blob_start
    .type  nl_ramfs_blob_start, @object
nl_ramfs_blob_start:
    .incbin NL_RAMFS_FILE
    .size  nl_ramfs_blob_start, . - nl_ramfs_blob_start

    .globl nl_ramfs_blob_end
    .type  nl_ramfs_blob_end, @object
nl_ramfs_blob_end:
    .size  nl_ramfs_blob_end, 0

    // Data only: keep linker from making stack executable
    .section .note.GNU-stack, "", @progbits

