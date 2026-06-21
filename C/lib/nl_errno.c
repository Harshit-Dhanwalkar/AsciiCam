#include "nl_errno.h"
#include "nl_syscall.h"

#include <stddef.h>

int errno = 0;

static void _ewrite(const char *s, size_t n) { __sc3(1, 2, (long)s, (long)n); }

static size_t _slen(const char *s) {
  const char *p = s;
  while (*p)
    p++;
  return (size_t)(p - s);
}

static const struct {
  int code;
  const char *msg;
} _errtab[] = {
    {1, "Operation not permitted"},  {2, "No such file or directory"},
    {9, "Bad file descriptor"},      {12, "Out of memory"},
    {13, "Permission denied"},       {16, "Device or resource busy"},
    {19, "No such device"},          {22, "Invalid argument"},
    {28, "No space left on device"}, {32, "Broken pipe"},
    {110, "Connection timed out"},   {0, (const char *)0}};

const char *nl_strerror(int e) {
  for (int i = 0; _errtab[i].msg; i++) {
    if (_errtab[i].code == e)
      return _errtab[i].msg;
  }
  return "Unknown error";
}

void nl_perror(const char *msg) {
  const char *estr = nl_strerror(errno);
  if (msg && msg[0]) {
    _ewrite(msg, _slen(msg));
    _ewrite(": ", 2);
  }
  _ewrite(estr, _slen(estr));
  _ewrite("\n", 1);
}
