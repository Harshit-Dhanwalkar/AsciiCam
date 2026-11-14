/*
 * src/netview.c - TCP viewer, see include/netview.h
 */

#include "platform.h"

#include "nolibc.h"

#include "netview.h"

// Byte order for raw-syscall (Linux) code
#if defined(__LINUX_NOLIBC__) // raw-syscall
static uint16_t be16(int v) {
  return (uint16_t)(((unsigned)v & 255u) * 256u +
                    (((unsigned)v / 256u) & 255u));
}

static uint32_t be32(uint32_t v) {
  return (v % 256u) * 16777216u + ((v / 256u) % 256u) * 65536u +
         ((v / 65536u) % 256u) * 256u + (v / 16777216u) % 256u;
}
#endif

static int parse_port(const char *s, int *port) {
  if (!*s) {
    return -1;
  }

  long v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') {
      return -1;
    }
    v = v * 10 + (*s - '0');
    if (v > 65535) {
      return -1;
    }
  }

  *port = (int)v;

  return 0;
}

static int parse_ip(const char *s, size_t n, uint32_t *ip) {
  uint32_t v = 0;
  size_t i = 0;
  for (int part = 0; part < 4; part++) {
    unsigned num = 0;
    size_t digits = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      num = num * 10u + (unsigned)(s[i] - '0');
      digits++;
      i++;
      if (digits > 3 || num > 255u) {
        return -1;
      }
    }

    if (digits == 0) {
      return -1;
    }
    v = v * 256u + num;
    if (part < 3) {
      if (i >= n || s[i] != '.') {
        return -1;
      }
      i++;
    }
  }

  if (i != n) {
    return -1;
  }
  *ip = v;

  return 0;
}

static int is_localhost(const char *s) {
  static const char name[] = "localhost";
  for (size_t i = 0; i < 9; i++) {
    if (s[i] != name[i]) {
      return 0;
    }
  }

  return 1;
}

int nv_parse_addr(const char *s, uint32_t *ip, int *port) {
  if (!s || !ip || !port) {
    return -1;
  }

  const char *colon = NULL;
  for (const char *c = s; *c; c++) {
    if (*c == ':') {
      colon = c;
    }
  }

  if (!colon) {
    *ip = 0x7f000001u;

    return parse_port(s, port);
  }

  size_t hl = (size_t)(colon - s);
  if (hl == 0 || (hl == 9 && is_localhost(s))) {
    *ip = 0x7f000001u;
  } else if (hl == 1 && s[0] == '*') {
    *ip = 0;
  } else if (parse_ip(s, hl, ip) != 0) {
    return -1;
  }

  return parse_port(colon + 1, port);
}

//  Linux nolibc implementation
#if defined(__LINUX_NOLIBC__)

#define SYS_socket 41
#define SYS_connect 42
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_bind 49
#define SYS_listen 50
#define SYS_getsockname 51
#define SYS_setsockopt 54
#define SYS_accept4 288

#define AF_INET 2
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define IPPROTO_TCP 6
#define TCP_NODELAY 1
// Combined flags as single constants (tidy: no bitwise on signed ints)
#define NV_SOCK_LISTEN 02004001  // SOCK_STREAM, SOCK_NONBLOCK, SOCK_CLOEXEC
#define NV_SOCK_CLIENT 02000001  // SOCK_STREAM, SOCK_CLOEXEC
#define NV_ACCEPT_FLAGS 02004000 // SOCK_NONBLOCK, SOCK_CLOEXEC
#define NV_MSG_FLAGS 0x4040      // MSG_NOSIGNAL, MSG_DONTWAIT
#define NV_MSG_DONTWAIT 0x40
#define NV_EAGAIN 11
#define NV_EINTR 4

#define SYS_fcntl 72
#define NV_F_GETFL 3
#define NV_F_SETFL 4
#define NV_O_NONBLOCK 04000

struct nv_sockaddr_in {
  uint16_t family;
  uint16_t port; // network order
  uint32_t addr; // network order
  uint8_t zero[8];
};

#define HELLO "\033[0m\033[?25l\033[2J"

static long send_some(int fd, const uint8_t *p, size_t n) {
  for (;;) {
    long r = __sc6(SYS_sendto, fd, (long)p, (long)n, NV_MSG_FLAGS, 0, 0);
    if (r == -NV_EINTR) {
      continue;
    }

    return r;
  }
}

static void drop_client(nv_server_t *srv, int i) {
  close(srv->cl[i].fd);
  nl_free(srv->cl[i].pend);
  srv->cl[i] = srv->cl[srv->nclients - 1];
  srv->nclients--;
  srv->cl[srv->nclients] = (nv_client_t){.fd = -1};
}

int nv_listen(nv_server_t *srv, uint32_t ip, int port) {
  nl_memset(srv, 0, sizeof(*srv));
  for (int i = 0; i < NV_MAX_CLIENTS; i++) {
    srv->cl[i].fd = -1;
  }

  srv->lfd = -1;
  long fd = __sc3(SYS_socket, AF_INET, NV_SOCK_LISTEN, 0);
  if (fd < 0) {
    return -1;
  }

  int one = 1;
  __sc6(SYS_setsockopt, fd, SOL_SOCKET, SO_REUSEADDR, (long)&one, sizeof(one),
        0);
  struct nv_sockaddr_in a = {AF_INET, be16(port), be32(ip), {0}};
  if (__sc3(SYS_bind, fd, (long)&a, sizeof(a)) < 0 ||
      __sc3(SYS_listen, fd, 16, 0) < 0) {
    close((int)fd);

    return -1;
  }

  struct nv_sockaddr_in got = a;
  unsigned len = sizeof(got);
  if (__sc3(SYS_getsockname, fd, (long)&got, (long)&len) == 0) {
    srv->port =
        (int)(((unsigned)got.port % 256u) * 256u + (unsigned)got.port / 256u);
  } else {
    srv->port = port;
  }

  srv->lfd = (int)fd;

  return 0;
}

int nv_accept(nv_server_t *srv) {
  int fresh = 0;
  while (srv->lfd >= 0) {
    long fd = __sc4(SYS_accept4, srv->lfd, 0, 0, NV_ACCEPT_FLAGS);
    if (fd == -NV_EINTR) {
      continue;
    }

    if (fd < 0) {
      break; // none waiting (or an error that will show again)
    }

    if (srv->nclients >= NV_MAX_CLIENTS) {
      close((int)fd);

      continue;
    }

    int one = 1;
    __sc6(SYS_setsockopt, fd, IPPROTO_TCP, TCP_NODELAY, (long)&one, sizeof(one),
          0);
    srv->cl[srv->nclients] = (nv_client_t){.fd = (int)fd, .need_key = 1};
    srv->nclients++;
    (void)send_some((int)fd, (const uint8_t *)HELLO, sizeof(HELLO) - 1);
    fresh++;
  }

  return fresh;
}

int nv_broadcast2(nv_server_t *srv, const uint8_t *delta, size_t dn,
                  const uint8_t *key, size_t kn) {
  int served = 0;
  srv->frames++;
  for (int i = 0; i < srv->nclients;) {
    nv_client_t *c = &srv->cl[i];
    int dead = 0;

    if (c->pend) { // finish previous frame first
      long r = send_some(c->fd, c->pend + c->ppos, c->plen - c->ppos);
      if (r >= 0) {
        c->ppos += (size_t)r;
      } else if (r != -NV_EAGAIN) {
        dead = 1;
      }

      if (!dead && c->ppos >= c->plen) {
        nl_free(c->pend);
        c->pend = NULL;
        c->plen = c->ppos = 0;
      }

      if (!dead && c->pend) {
        srv->dropped++;
        c->need_key = 1; // it misses this one
        i++;

        continue;
      }
    }

    if (!dead) {
      int use_key = c->need_key && key;
      const uint8_t *buf = use_key ? key : delta;
      size_t n = use_key ? kn : dn;
      long r = send_some(c->fd, buf, n);
      if (r == -NV_EAGAIN) {
        srv->dropped++;
        c->need_key = 1;
      } else if (r < 0) {
        dead = 1;
      } else {
        served++;
        if (use_key) {
          c->need_key = 0;
        }

        if ((size_t)r < n) {
          c->pend = nl_malloc(n - (size_t)r);
          if (c->pend) {
            nl_memcpy(c->pend, buf + r, n - (size_t)r);
            c->plen = n - (size_t)r;
            c->ppos = 0;
          } else {
            dead = 1; // cannot finish frame: stream would be torn
          }
        }
      }
    }

    if (dead) {
      drop_client(srv, i);

      continue;
    }

    i++;
  }

  return served;
}

int nv_broadcast(nv_server_t *srv, const uint8_t *buf, size_t n) {
  return nv_broadcast2(srv, buf, n, NULL, 0);
}

int nv_wants_key(const nv_server_t *srv) {
  int n = 0;
  for (int i = 0; i < srv->nclients; i++) {
    n += srv->cl[i].need_key ? 1 : 0;
  }

  return n;
}

void nv_want_key_all(nv_server_t *srv) {
  for (int i = 0; i < srv->nclients; i++) {
    srv->cl[i].need_key = 1;
  }
}

void nv_close(nv_server_t *srv) {
  for (int i = 0; i < srv->nclients; i++) {
    close(srv->cl[i].fd);
    nl_free(srv->cl[i].pend);
  }

  if (srv->lfd >= 0) {
    close(srv->lfd);
  }

  nl_memset(srv, 0, sizeof(*srv));
  srv->lfd = -1;
}

int nv_connect(const char *hostport) {
  uint32_t ip;
  int port;
  if (nv_parse_addr(hostport, &ip, &port) != 0 || port == 0) {
    return -1;
  }

  long fd = __sc3(SYS_socket, AF_INET, NV_SOCK_CLIENT, 0);
  if (fd < 0) {
    return -1;
  }

  struct nv_sockaddr_in a = {
      AF_INET, be16(port), be32(ip == 0 ? 0x7f000001u : ip), {0}};
  long r;
  do {
    r = __sc3(SYS_connect, fd, (long)&a, sizeof(a));
  } while (r == -NV_EINTR);

  if (r < 0) {
    close((int)fd);

    return -1;
  }

  int one = 1;
  __sc6(SYS_setsockopt, fd, IPPROTO_TCP, TCP_NODELAY, (long)&one, sizeof(one),
        0);

  return (int)fd;
}

int nv_nonblock(int fd) {
  long fl = __sc3(SYS_fcntl, fd, NV_F_GETFL, 0);
  if (fl < 0) {
    return -1;
  }

  unsigned have = (unsigned)fl;
  if (have / NV_O_NONBLOCK % 2u == 1u) {
    return 0;
  }

  return __sc3(SYS_fcntl, fd, NV_F_SETFL, (long)(have + NV_O_NONBLOCK)) < 0 ? -1
                                                                            : 0;
}

// Blocking or not for fd, whatever it was. 0 on success
int nv_set_blocking(int fd, int blocking) {
  long fl = __sc3(SYS_fcntl, fd, NV_F_GETFL, 0);
  if (fl < 0) {
    return -1;
  }

  unsigned have = (unsigned)fl;
  int is_nonblock = have / NV_O_NONBLOCK % 2u == 1u;
  if (is_nonblock == !blocking) {
    return 0;
  }

  unsigned want = is_nonblock ? have - NV_O_NONBLOCK : have + NV_O_NONBLOCK;

  return __sc3(SYS_fcntl, fd, NV_F_SETFL, (long)want) < 0 ? -1 : 0;
}

int nv_accept_fd(int lfd) {
  for (;;) {
    long fd = __sc4(SYS_accept4, lfd, 0, 0, NV_ACCEPT_FLAGS);
    if (fd == -NV_EINTR) {
      continue;
    }

    if (fd < 0) {
      return -1;
    }

    int one = 1;
    __sc6(SYS_setsockopt, fd, IPPROTO_TCP, TCP_NODELAY, (long)&one, sizeof(one),
          0);

    return (int)fd;
  }
}

int nv_broadcast2(nv_server_t *srv, const uint8_t *delta, size_t dn,
                  const uint8_t *key, size_t kn) {
  (void)srv;
  (void)delta;
  (void)dn;
  (void)key;
  (void)kn;

  return 0;
}

long nv_send(int fd, const void *buf, size_t n) {
  return send_some(fd, (const uint8_t *)buf, n);
}

long nv_recv(int fd, void *buf, size_t n) {
  for (;;) {
    long r = __sc6(SYS_recvfrom, fd, (long)buf, (long)n, NV_MSG_DONTWAIT, 0, 0);
    if (r == -NV_EINTR) {
      continue;
    }

    return r;
  }
}

int nv_wait(const int *fds, int n, int timeout_ms, uint8_t *ready) {
  nl_fd_set rs;
  nl_memset(&rs, 0, sizeof(rs));
  int maxfd = -1;
  for (int i = 0; i < n; i++) {
    ready[i] = 0;
    if (fds[i] >= 0 && fds[i] < 1024) {
      unsigned u = (unsigned)fds[i];
      rs.fds_bits[u / 64u] |= 1UL << (u % 64u);
      maxfd = fds[i] > maxfd ? fds[i] : maxfd;
    }
  }

  struct nl_timeval tv = {timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000L};
  int r = nl_select(maxfd + 1, &rs, NULL, NULL, &tv);
  if (r < 0) {
    return errno == NV_EINTR ? 0 : -1;
  }

  int got = 0;
  for (int i = 0; i < n; i++) {
    if (fds[i] >= 0 && fds[i] < 1024) {
      unsigned u = (unsigned)fds[i];
      if ((rs.fds_bits[u / 64u] >> (u % 64u)) % 2UL != 0) {
        ready[i] = 1;
        got++;
      }
    }
  }

  return got;
}

// macOS / WINDOWS PROPER IMPLEMENTATION
#else

#ifdef PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#define close closesocket
#define NV_EAGAIN WSAEWOULDBLOCK
#define NV_EINTR WSAEINTR
#define MSG_NOSIGNAL 0
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h> // TCP_NODELAY
#include <sys/socket.h>
#include <unistd.h>
#define NV_EAGAIN EAGAIN
#define NV_EINTR EINTR
#ifdef PLATFORM_MACOS
#define SO_NOSIGPIPE 0x1022
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

#define HELLO "\033[0m\033[?25l\033[2J"

int nv_listen(nv_server_t *srv, uint32_t ip, int port) {
  nl_memset(srv, 0, sizeof(*srv));
  for (int i = 0; i < NV_MAX_CLIENTS; i++)
    srv->cl[i].fd = -1;
  srv->lfd = -1;

#ifdef PLATFORM_WINDOWS
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    return -1;
  }
#endif

  srv->lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (srv->lfd < 0) {
    return -1;
  }

#ifdef PLATFORM_MACOS
  int nosigpipe = 1;
  setsockopt(srv->lfd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

  int one = 1;
  setsockopt(srv->lfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one,
             sizeof(one));

#ifdef PLATFORM_WINDOWS
  u_long mode = 1;
  ioctlsocket(srv->lfd, FIONBIO, &mode);
#else
  int flags = fcntl(srv->lfd, F_GETFL, 0);
  fcntl(srv->lfd, F_SETFL, flags | O_NONBLOCK);
#endif

  struct sockaddr_in a;
  nl_memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(ip == 0 ? 0x7f000001u : ip);

  if (bind(srv->lfd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
      listen(srv->lfd, 16) < 0) {
    close(srv->lfd);
    srv->lfd = -1;

    return -1;
  }

  struct sockaddr_in got;
  socklen_t len = sizeof(got);
  if (getsockname(srv->lfd, (struct sockaddr *)&got, &len) == 0) {
    srv->port = ntohs(got.sin_port);
  } else {
    srv->port = port;
  }
  return 0;
}

int nv_accept(nv_server_t *srv) {
  int fresh = 0;
  while (srv->lfd >= 0) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int fd = accept(srv->lfd, (struct sockaddr *)&client_addr, &client_len);
    if (fd < 0) {
#ifdef PLATFORM_WINDOWS
      if (WSAGetLastError() == WSAEWOULDBLOCK ||
          WSAGetLastError() == WSAEINTR) {
        break;
      }
#else
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        break;
      }
#endif
      break;
    }
    if (srv->nclients >= NV_MAX_CLIENTS) {
      close(fd);
      continue;
    }

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#ifdef PLATFORM_MACOS
    int nosigpipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

#ifdef PLATFORM_WINDOWS
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

    srv->cl[srv->nclients] = (nv_client_t){.fd = fd};
    srv->nclients++;
    send(fd, HELLO, sizeof(HELLO) - 1, MSG_NOSIGNAL);
    fresh++;
  }
  return fresh;
}

int nv_broadcast(nv_server_t *srv, const uint8_t *buf, size_t n) {
  int served = 0;
  srv->frames++;
  for (int i = 0; i < srv->nclients;) {
    nv_client_t *c = &srv->cl[i];
    int dead = 0;
    if (c->pend) {
      long r = send(c->fd, (const char *)c->pend + c->ppos, c->plen - c->ppos,
                    MSG_NOSIGNAL);
      if (r >= 0) {
        c->ppos += (size_t)r;
      } else {
#ifdef PLATFORM_WINDOWS
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
          dead = 1;
        }
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          dead = 1;
        }
#endif
      }
      if (!dead && c->ppos >= c->plen) {
        nl_free(c->pend);
        c->pend = NULL;
        c->plen = c->ppos = 0;
      }
      if (!dead && c->pend) {
        srv->dropped++;
        i++;
        continue;
      }
    }
    if (!dead) {
      long r = send(c->fd, (const char *)buf, n, MSG_NOSIGNAL);
      if (r < 0) {
#ifdef PLATFORM_WINDOWS
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
          srv->dropped++;
        } else {
          dead = 1;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          srv->dropped++;
        } else {
          dead = 1;
        }
#endif
      } else {
        served++;
        if ((size_t)r < n) {
          c->pend = nl_malloc(n - (size_t)r);
          if (c->pend) {
            nl_memcpy(c->pend, buf + r, n - (size_t)r);
            c->plen = n - (size_t)r;
            c->ppos = 0;
          } else {
            dead = 1;
          }
        }
      }
    }
    if (dead) {
      close(c->fd);
      nl_free(c->pend);
      srv->cl[i] = srv->cl[srv->nclients - 1];
      srv->nclients--;
      srv->cl[srv->nclients] = (nv_client_t){.fd = -1};
      continue;
    }
    i++;
  }

  return served;
}

void nv_close(nv_server_t *srv) {
  for (int i = 0; i < srv->nclients; i++) {
    close(srv->cl[i].fd);
    nl_free(srv->cl[i].pend);
  }

  if (srv->lfd >= 0) {
    close(srv->lfd);
  }

  nl_memset(srv, 0, sizeof(*srv));
  srv->lfd = -1;
#ifdef PLATFORM_WINDOWS
  WSACleanup();
#endif
}

int nv_connect(const char *hostport) {
  uint32_t ip;
  int port;
  if (nv_parse_addr(hostport, &ip, &port) != 0 || port == 0) {
    return -1;
  }

  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    return -1;
  }

#ifdef PLATFORM_MACOS
  int nosigpipe = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

  struct sockaddr_in a;
  nl_memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(ip == 0 ? 0x7f000001u : ip);

  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    close(fd);
    return -1;
  }

  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

#ifdef PLATFORM_WINDOWS
  u_long mode = 1;
  ioctlsocket(fd, FIONBIO, &mode);
#else
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

  return fd;
}

int nv_nonblock(int fd) {
  (void)fd;

  return -1;
}

int nv_set_blocking(int fd, int blocking) {
  (void)fd;
  (void)blocking;

  return -1;
}

int nv_accept_fd(int lfd) {
  (void)lfd;

  return -1;
}

long nv_send(int fd, const void *buf, size_t n) {
  (void)fd;
  (void)buf;
  (void)n;

  return -1;
}

long nv_recv(int fd, void *buf, size_t n) {
  (void)fd;
  (void)buf;
  (void)n;

  return -1;
}

int nv_wait(const int *fds, int n, int timeout_ms, uint8_t *ready) {
  (void)fds;
  (void)n;
  (void)timeout_ms;
  (void)ready;

  return -1;
}

#endif // end of !LINUX_NOLIBC
