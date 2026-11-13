#ifndef NETVIEW_H
#define NETVIEW_H

/*
 * netview - watch picture from another terminal
 *
 * `--serve [host:]port` listens on TCP and sends every rendered frame (
 * picture only, as it goes to terminal) to whoever is connected. Default
 * address is loopback: reach it from another machine with
 *
 *     ssh -L 7000:localhost:7000 camera-host
 *     asciicam --watch localhost:7000          (or: nc localhost 7000)
 *
 * There is no authentication and no encryption, which is why nothing but
 * loopback is listened on unless asked for by address
 * Every frame repaints whole picture, so a viewer that is too slow simply
 * misses frames: camera is never held up
 * A frame is sent whole or not at all, apart from its tail, which is queued
 *
 * NOTE: Numeric IPv4 only (and "localhost"): no name lookup, no libc resolver
 * Supported on Linux, macOS, and Windows
 */

#include "nl_types.h"

#define NV_MAX_CLIENTS 8

typedef struct {
  int fd;
  uint8_t *pend; // unsent tail of a frame
  size_t plen;
  size_t ppos;
} nv_client_t;

typedef struct {
  int lfd;
  int port;
  int nclients;
  nv_client_t cl[NV_MAX_CLIENTS];
  uint64_t frames;  // broadcasts
  uint64_t dropped; // frame x viewer pairs skipped because it was behind
} nv_server_t;

// "7000", ":7000", "host:7000", "127.0.0.1:7000", "0.0.0.0:7000" or "*:7000"
// IP in host byte order; empty host = loopback
// Return 0 on success
int nv_parse_addr(const char *s, uint32_t *ip, int *port);

// Listen on ip:port (port 0 = any, see srv->port)
// Return 0 on success
int nv_listen(nv_server_t *srv, uint32_t ip, int port);

// Take waiting connections
// Number of new ones; viewers get a clear-screen
int nv_accept(nv_server_t *srv);

// Send frame to every viewer
// Number of viewers that got it (or its start)
int nv_broadcast(nv_server_t *srv, const uint8_t *buf, size_t n);

void nv_close(nv_server_t *srv);

// Connect to "host:port" (blocking)
// Socket, or -1
int nv_connect(const char *hostport);

// Plain socket helpers for room relay and call client (WARN: Linux only)
// Make fd non-blocking
// 0 on success
int nv_nonblock(int fd);

// Blocking (1) or not (0), for a descriptor do not own, such as tty
int nv_set_blocking(int fd, int blocking);

// Accept one waiting connection on a listening socket, non-blocking
// Return fd or -1
int nv_accept_fd(int lfd);

// Non-blocking send/recv: bytes moved, 0 from recv = peer closed, or -errno
// (-11 = would block)
long nv_send(int fd, const void *buf, size_t n);
long nv_recv(int fd, void *buf, size_t n);

// Wait up to timeout_ms until one of fds[0..n) can be read
// ready[i] is set to 1 for those that can
// Number ready, -1 on error
// Interrupted wait: 0
int nv_wait(const int *fds, int n, int timeout_ms, uint8_t *ready);

#endif
