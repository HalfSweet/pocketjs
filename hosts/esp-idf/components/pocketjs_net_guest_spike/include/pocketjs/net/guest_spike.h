// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "pocketjs/esp_guest.h"
#include "pocketjs/net/esp_http_client_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POCKETJS_NET_GUEST_SPIKE_DEFAULT_ROUNDS 5U
#define POCKETJS_NET_GUEST_SPIKE_MAX_ROUNDS 100U
#define POCKETJS_NET_GUEST_SPIKE_DEFAULT_REQUEST_TIMEOUT_MS 5000U
#define POCKETJS_NET_GUEST_SPIKE_DEFAULT_TOTAL_TIMEOUT_MS 120000U
#define POCKETJS_NET_GUEST_SPIKE_MAX_TOTAL_TIMEOUT_MS 900000U
#define POCKETJS_NET_GUEST_SPIKE_DEFAULT_RESPONSE_BODY_BYTES 4096U
#define POCKETJS_NET_GUEST_SPIKE_MAX_RESPONSE_BODY_BYTES 4096U
#define POCKETJS_NET_GUEST_SPIKE_DEFAULT_GUEST_MEMORY_BYTES (512U * 1024U)
#define POCKETJS_NET_GUEST_SPIKE_DEFAULT_GUEST_STACK_BYTES (24U * 1024U)
#define POCKETJS_NET_GUEST_SPIKE_DEFAULT_EXECUTION_TIMEOUT_US 100000U
#define POCKETJS_NET_GUEST_SPIKE_DEFAULT_INTERRUPT_CHECKS 64U
#define POCKETJS_NET_GUEST_SPIKE_MAX_DEVICE_ID_BYTES 63U
#define POCKETJS_NET_GUEST_SPIKE_DETAIL_BYTES 192U

typedef struct {
  /** Plain HTTP origin such as "http://192.0.2.1:8088". */
  const char *base_url;
  /** Short diagnostic label included in the exact echo payload. */
  const char *device_id;
  /** Zero selects POCKETJS_NET_GUEST_SPIKE_DEFAULT_ROUNDS. */
  uint32_t rounds;
  /** Zero selects POCKETJS_NET_GUEST_SPIKE_DEFAULT_REQUEST_TIMEOUT_MS. */
  uint32_t request_timeout_ms;
  /** Zero selects POCKETJS_NET_GUEST_SPIKE_DEFAULT_TOTAL_TIMEOUT_MS. */
  uint32_t total_timeout_ms;
  /** Zero selects POCKETJS_NET_GUEST_SPIKE_DEFAULT_RESPONSE_BODY_BYTES. */
  size_t max_response_body_bytes;
  /** Zero selects POCKETJS_NET_GUEST_SPIKE_DEFAULT_GUEST_MEMORY_BYTES. */
  size_t guest_memory_limit_bytes;
  /** Zero selects POCKETJS_NET_GUEST_SPIKE_DEFAULT_GUEST_STACK_BYTES. */
  size_t guest_stack_limit_bytes;
  /** Require all QuickJS allocations to use external RAM. */
  bool allocate_guest_in_external_memory;
  /** Zero selects POCKETJS_NET_GUEST_SPIKE_DEFAULT_EXECUTION_TIMEOUT_US. */
  uint64_t guest_execution_timeout_us;
  /** Zero selects POCKETJS_NET_GUEST_SPIKE_DEFAULT_INTERRUPT_CHECKS. */
  uint32_t guest_max_interrupt_checks;
  /** Zero selects the backend default. */
  uint32_t worker_stack_bytes;
  /** Zero selects the backend default; otherwise below configMAX_PRIORITIES. */
  UBaseType_t worker_priority;
  BaseType_t worker_core;
} pocketjs_net_guest_spike_config_t;

typedef struct {
  bool passed;
  uint32_t requested_rounds;
  uint32_t completed_rounds;
  uint32_t operations_started;
  uint32_t operations_completed;
  uint64_t guest_jobs_executed;
  uint64_t elapsed_ms;
  uint64_t artifact_fnv1a64;
  uint32_t frame_count;
  size_t response_body_high_water_bytes;
  char detail[POCKETJS_NET_GUEST_SPIKE_DETAIL_BYTES];
  pocketjs_esp_guest_stats_t guest;
  pocketjs_net_esp_http_stats_t backend;
} pocketjs_net_guest_spike_result_t;

/**
 * Run a bounded plaintext GET /health and POST /echo probe from one PocketJS
 * QuickJS realm. The calling FreeRTOS task owns the realm and is occupied until
 * the probe completes. The HTTP worker only writes native events and sets a
 * wake bit; this owner task alone drains events, settles Promises and runs
 * jobs.
 *
 * The JavaScript bundle is mounted through a frozen private factory argument.
 * No globalThis.net or other persistent global binding is installed, and this
 * experimental runner does not advertise a public PocketJS network capability.
 * The product BSP must initialize and connect its network interface first.
 *
 * The runner temporarily owns one EventGroup and the calling task's QuickJS
 * execution. Native DNS/connect cancellation inherits the experimental ESP
 * HTTP backend's documented limitation, so teardown has no uniform time bound.
 */
esp_err_t
pocketjs_net_guest_spike_run(const pocketjs_net_guest_spike_config_t *config,
                             pocketjs_net_guest_spike_result_t *out_result);

#ifdef __cplusplus
}
#endif
