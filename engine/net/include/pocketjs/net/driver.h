/* PocketJS network core — NetDriver interface (plain transport substrate).
 *
 * The driver is the host's non-blocking socket layer: resolver, byte-stream
 * connect/read/write/shutdown/close, listener accept, local/remote address
 * metadata and reactor interest. It never sees HTTP, WebSocket or TLS
 * (docs/pocketjs-network-architecture.md §13). lwIP, BSD sockets, Winsock and
 * console SDK sockets all fit this shape.
 *
 * Threading: the core calls the driver only from inside `pnet_runtime_service`
 * and the owner-thread ops, all serialized by the host. The host's network
 * task waits on the sockets it created (select/poll/epoll/lwIP select) with a
 * timeout of `pnet_runtime_next_deadline_ms`, then calls service().
 */
#ifndef POCKETJS_NET_DRIVER_H
#define POCKETJS_NET_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Host socket identity; -1 is invalid. */
typedef int pnet_sock;
#define PNET_SOCK_INVALID (-1)

/** Binary IP address; `family` is 4 or 6, `addr` holds 4 or 16 bytes. */
typedef struct pnet_addr {
  uint8_t family;
  uint8_t addr[16];
  uint16_t port;
} pnet_addr;

/** Portable driver error codes (negative). The driver maps errno/lwIP/SDK
 * codes onto these before returning; the raw code may travel in `cause`. */
enum {
  PNET_IO_OK = 0,
  PNET_IO_AGAIN = -1,     /* would block; try again after the reactor wakes */
  PNET_IO_EOF = -2,       /* orderly end of stream (read only) */
  PNET_IO_CLOSED = -3,    /* connection reset / broken pipe */
  PNET_IO_REFUSED = -4,   /* connect refused / unreachable */
  PNET_IO_TIMEOUT = -5,   /* driver-level timeout (e.g. TCP connect) */
  PNET_IO_ADDRINUSE = -6, /* bind: address in use */
  PNET_IO_NOMEM = -7,     /* out of sockets/buffers */
  PNET_IO_ERROR = -8,     /* anything else */
};

/** Reactor interest flags for `interest()`. */
enum {
  PNET_INTEREST_READ = 1u << 0,
  PNET_INTEREST_WRITE = 1u << 1,
};

typedef struct pnet_driver_ops {
  /** Start resolving `host` (ASCII, no port). Completion arrives through
   * `pnet_runtime_resolve_done(rt, req_id, ...)`, which the driver may call
   * synchronously from inside this function or later from the network task.
   * Return < 0 (a PNET_IO_* code) if the request cannot start. */
  int (*resolve)(void *ctx, uint32_t req_id, const char *host);
  /** The core no longer needs `req_id`; a later completion is ignored. */
  void (*resolve_cancel)(void *ctx, uint32_t req_id);

  /** Begin a non-blocking TCP connect. Returns the socket, or
   * PNET_SOCK_INVALID with *err set. */
  pnet_sock (*connect)(void *ctx, const pnet_addr *addr, int *err);
  /** 0 = still connecting, 1 = connected, < 0 = failed (PNET_IO_*). */
  int (*connect_status)(void *ctx, pnet_sock s);

  /** Read up to `len` bytes: > 0 bytes, PNET_IO_EOF, PNET_IO_AGAIN, or an
   * error code. */
  int (*read)(void *ctx, pnet_sock s, uint8_t *buf, size_t len);
  /** Write up to `len` bytes: >= 0 bytes written (0 = nothing accepted),
   * PNET_IO_AGAIN, or an error code. */
  int (*write)(void *ctx, pnet_sock s, const uint8_t *buf, size_t len);
  void (*shutdown_write)(void *ctx, pnet_sock s);
  void (*close)(void *ctx, pnet_sock s);
  /** Register what the core is waiting for on `s` (bitmask of
   * PNET_INTEREST_*; 0 clears). Level-triggered semantics are assumed. */
  void (*interest)(void *ctx, pnet_sock s, unsigned flags);

  /** Bind + listen. On success returns the listener socket and fills
   * `bound` with the actual local address (port resolved for ephemeral). */
  pnet_sock (*listen)(void *ctx, const pnet_addr *addr, int backlog, pnet_addr *bound, int *err);
  /** Accept one connection: the new socket (non-blocking) with `peer`
   * filled, or PNET_SOCK_INVALID with *err = PNET_IO_AGAIN / error. */
  pnet_sock (*accept)(void *ctx, pnet_sock listener, pnet_addr *peer, int *err);

  /** Local address of a connected/bound socket; 0 on success. */
  int (*local_addr)(void *ctx, pnet_sock s, pnet_addr *out);
} pnet_driver_ops;

#ifdef __cplusplus
}
#endif

#endif /* POCKETJS_NET_DRIVER_H */
