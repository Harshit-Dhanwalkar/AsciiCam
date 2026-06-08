#include "nl_getopt.h"
#include "nl_syscall.h"
#include <stddef.h>

int nl_optind = 1;
int nl_opterr = 1;
char *nl_optarg = (char *)0;

static int _optpos = 0;

static void _ewrite(const char *s, size_t n) { __sc3(1, 2, (long)s, (long)n); }

int nl_getopt(int argc, char *const argv[], const char *opts) {
  nl_optarg = (char *)0;

  if (nl_optind < argc &&
      (argv[nl_optind][0] != '-' || argv[nl_optind][1] == '\0'))
    return -1;

  if (nl_optind >= argc)
    return -1;

  const char *arg = argv[nl_optind];
  if (arg[0] == '-' && arg[1] == '-') {
    nl_optind++;
    return -1;
  }

  if (_optpos == 0)
    _optpos = 1;
  char c = arg[_optpos++];

  const char *o = opts;
  while (*o) {
    if (*o == c)
      break;
    o += (*o != ':') ? 1 : 1;
  }
  if (!*o) {
    if (nl_opterr) {
      char msg[] = "Unknown option: -?\n";
      msg[17] = c;
      _ewrite(msg, 19);
    }
    if (arg[_optpos] == '\0') {
      nl_optind++;
      _optpos = 0;
    }
    return '?';
  }

  if (o[1] == ':') {
    if (arg[_optpos] != '\0') {
      nl_optarg = (char *)&arg[_optpos];
    } else {
      nl_optind++;
      if (nl_optind >= argc) {
        if (nl_opterr)
          _ewrite("Missing argument\n", 17);
        _optpos = 0;
        return '?';
      }
      nl_optarg = argv[nl_optind];
    }
    nl_optind++;
    _optpos = 0;
  } else {
    if (arg[_optpos] == '\0') {
      nl_optind++;
      _optpos = 0;
    }
  }

  return (unsigned char)c;
}
