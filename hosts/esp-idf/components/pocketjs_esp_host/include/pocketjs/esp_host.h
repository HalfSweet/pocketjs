/* PocketJS ESP-IDF host: a QuickJS-ng guest owned by one FreeRTOS task,
 * ticked at a fixed rate through `globalThis.frame(...)`, with the network
 * modules (`globalThis.net` / `ws` / `httpd`) mounted over the portable core
 * (engine/net) and a network task driving lwIP sockets.
 *
 * Execution model: every guest
 * turn is one `frame()` call followed by the job drain, on the owner task
 * only. Before each frame the owner task runs `pnet_runtime_begin_tick()`
 * under the runtime lock; the network task services sockets under the same
 * lock and never touches QuickJS.
 */
#ifndef POCKETJS_ESP_HOST_H
#define POCKETJS_ESP_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pocketjs/net/runtime.h"
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pocketjs_esp_host pocketjs_esp_host_t;

typedef struct pocketjs_esp_host_config {
  /** Guest ticks per second (the realm's `__simHz`). Default 60. */
  uint32_t tick_hz;
  /** QuickJS memory limit in bytes (0 = 4 MiB). */
  size_t guest_memory_limit;
  /** QuickJS stack limit in bytes (0 = 3/4 of the guest task stack). */
  size_t guest_stack_limit;
  /** Allocate the QuickJS heap from PSRAM (recommended when present). */
  bool guest_in_psram;
  /** Owner task stack bytes (default 32 KiB) and priority/core. */
  uint32_t guest_task_stack;
  int guest_task_priority;
  int guest_task_core;
  /** Network task stack bytes (default 12 KiB) and priority/core. */
  uint32_t net_task_stack;
  int net_task_priority;
  int net_task_core;
  /** Immutable network policy JSON; NULL mounts no network module. */
  const char *network_policy_json;
  /** Enable TLS (https:/wss:) through the ESP-TLS provider with the IDF
   * certificate bundle. The wall clock must be trusted (SNTP/RTC) for
   * certificate validity; until then verifying connections fail closed with
   * tls_clock_untrusted. */
  bool network_tls;
  /** Which roles this host admits: `globalThis.net` is always mounted with
   * a policy; `ws` and `httpd` only when set (default true for both). A
   * product host mounts exactly the capabilities its target advertises. */
  bool mount_websocket_client;
  bool mount_http_server;
  /** Core limits; NULL = spec ceilings tightened by the host defaults. */
  const pnet_runtime_config *network_config;
  /** Sockets the driver may track (default 12). */
  int network_max_sockets;
  /** Called on the owner task after the namespaces are mounted and before
   * the bundle is evaluated (install host globals). */
  void (*before_eval)(JSContext *ctx, void *user);
  /** Called on the owner task after every frame + job drain (diagnostics). */
  void (*after_frame)(uint32_t frame, void *user);
  void *user;
} pocketjs_esp_host_config;

/** Fill in the defaults described above. */
void pocketjs_esp_host_config_defaults(pocketjs_esp_host_config *cfg);

/** Create the runtime and both tasks, evaluate `bundle` (an IIFE that
 * installs `globalThis.frame`), and start ticking. `bundle` must stay valid
 * for the host's lifetime (embedded flash text is fine). */
esp_err_t pocketjs_esp_host_start(const pocketjs_esp_host_config *cfg, const char *bundle, size_t bundle_len,
                                  pocketjs_esp_host_t **out_host);

/** Quiesce the network, run a bounded number of wind-down frames, then
 * release the guest and the runtime. */
void pocketjs_esp_host_stop(pocketjs_esp_host_t *host);

typedef struct pocketjs_esp_host_stats {
  uint32_t frames;
  uint32_t jobs;
  uint32_t frame_errors;
  size_t guest_heap_bytes;      /* QuickJS reported */
  size_t guest_heap_high_water;
  size_t net_heap_bytes;        /* core accounting */
  int net_sockets;
  uint32_t frame_max_us;
} pocketjs_esp_host_stats_t;

void pocketjs_esp_host_stats(pocketjs_esp_host_t *host, pocketjs_esp_host_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* POCKETJS_ESP_HOST_H */
