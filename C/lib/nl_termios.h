#ifndef NL_TERMIOS_H
#define NL_TERMIOS_H

/*
 * nl_termios.h - Linux x86-64 kernel termios ABI (what TCGETS/TCSETS read and
 * write)
 *
 * Constants are asm-generic values used by x86-64 and arm64 Linux
 */

#include "nl_types.h"

#ifdef __LINUX_NOLIBC__

#define NCCS 19

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;

struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[NCCS];
};

/* c_lflag */
#define ISIG 0000001
#define ICANON 0000002
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHONL 0000100
#define IEXTEN 0100000

/* c_iflag */
#define BRKINT 0000002
#define ICRNL 0000400
#define INPCK 0000020
#define ISTRIP 0000040
#define IXON 0002000

/* c_oflag */
#define OPOST 0000001

/* c_cflag */
#define CS8 0000060

/* c_cc indices */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6

#endif /* __LINUX_NOLIBC__ */

#endif /* NL_TERMIOS_H */
