/* PocketJS network core — BSD-socket NetDriver.
 *
 * One implementation of pocketjs/net/driver.h over the BSD socket API, used
 * by the desktop conformance harness (macOS/Linux) and by ESP-IDF, whose
 * lwIP exposes the same calls (socket/connect/select/getaddrinfo). The
 * driver owns a bounded socket table, a loopback UDP wake socket and a small
 * resolver queue; the host's network task drives it with:
 *
 *   for (;;) {
 *     lock();  pnet_posix_driver_dispatch(d, rt);  pnet_runtime_service(rt);
 *              timeout = pnet_runtime_next_deadline_ms(rt);               unlock();
 *     pnet_posix_driver_wait(d, timeout);   // blocking DNS + select, no lock held
 *   }
 *
 * `resolve()` never blocks the caller: lookups run inside wait() on the
 * network task and are handed to the runtime by dispatch(). Owner-thread
 * ops that need the network task to look at new work call
 * pnet_posix_driver_wake().
 */
#ifndef POCKETJS_NET_POSIX_DRIVER_H
#define POCKETJS_NET_POSIX_DRIVER_H

#include <stdint.h>

#include "pocketjs/net/driver.h"
#include "pocketjs/net/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pnet_posix_driver pnet_posix_driver;

/** Create a driver able to track `max_sockets` sockets. NULL on failure. */
pnet_posix_driver *pnet_posix_driver_create(int max_sockets);
void pnet_posix_driver_destroy(pnet_posix_driver *d);
const pnet_driver_ops *pnet_posix_driver_ops(void);

/** Wait for I/O, a wake or `timeout_ms` (negative = forever, 0 = poll).
 * Runs queued blocking DNS lookups first. Call WITHOUT the runtime lock. */
void pnet_posix_driver_wait(pnet_posix_driver *d, int timeout_ms);
/** Deliver completed resolver results to the runtime. Call WITH the lock. */
void pnet_posix_driver_dispatch(pnet_posix_driver *d, pnet_runtime *rt);
/** Interrupt a wait() from any thread. */
void pnet_posix_driver_wake(pnet_posix_driver *d);
/** Live sockets (for resource reports). */
int pnet_posix_driver_socket_count(pnet_posix_driver *d);

#ifdef __cplusplus
}
#endif

#endif /* POCKETJS_NET_POSIX_DRIVER_H */
