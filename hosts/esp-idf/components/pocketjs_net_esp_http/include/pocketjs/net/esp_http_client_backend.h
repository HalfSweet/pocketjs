// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POCKETJS_NET_ESP_HTTP_BACKEND_ID "pocketjs.net.esp-idf.http-client.v1"
#define POCKETJS_NET_ESP_HTTP_PROTOCOL_VERSION "http/1.1"

#define POCKETJS_NET_ESP_HTTP_MAX_URL_BYTES 2048U
#define POCKETJS_NET_ESP_HTTP_MAX_METHOD_BYTES 16U
#define POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADERS 16U
#define POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_NAME_BYTES 64U
#define POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_VALUE_BYTES 512U
#define POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_BYTES 4096U
#define POCKETJS_NET_ESP_HTTP_MAX_REQUEST_BODY_BYTES (16U * 1024U)
#define POCKETJS_NET_ESP_HTTP_TX_BUFFER_BYTES 4096U
#define POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_HEADERS 32U
#define POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_HEADER_BYTES (8U * 1024U)
#define POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_FIELD_BYTES 2048U
#define POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_BODY_BYTES (256U * 1024U)
#define POCKETJS_NET_ESP_HTTP_BODY_CHUNK_BYTES 2048U
#define POCKETJS_NET_ESP_HTTP_BUFFER_LEASES 8U
#define POCKETJS_NET_ESP_HTTP_DATA_EVENTS 16U
#define POCKETJS_NET_ESP_HTTP_DEFAULT_WORKER_STACK_BYTES (8U * 1024U)
#define POCKETJS_NET_ESP_HTTP_DEFAULT_IO_TIMEOUT_MS 5000U
#define POCKETJS_NET_ESP_HTTP_MAX_IO_TIMEOUT_MS 120000U

typedef struct pocketjs_net_esp_http_client pocketjs_net_esp_http_client_t;

typedef enum {
  POCKETJS_NET_ERROR_NONE = 0,
  POCKETJS_NET_ERROR_ABORTED,
  POCKETJS_NET_ERROR_TIMED_OUT,
  POCKETJS_NET_ERROR_BUSY,
  POCKETJS_NET_ERROR_RESOURCE_LIMIT,
  POCKETJS_NET_ERROR_UNSUPPORTED,
  POCKETJS_NET_ERROR_DNS_NOT_FOUND,
  POCKETJS_NET_ERROR_CONNECTION_REFUSED,
  POCKETJS_NET_ERROR_CONNECTION_RESET,
  POCKETJS_NET_ERROR_NETWORK_UNREACHABLE,
  POCKETJS_NET_ERROR_TLS_CERTIFICATE_INVALID,
  POCKETJS_NET_ERROR_TLS_HOSTNAME_MISMATCH,
  POCKETJS_NET_ERROR_TLS_HANDSHAKE_FAILED,
  POCKETJS_NET_ERROR_HTTP_PROTOCOL_ERROR,
  POCKETJS_NET_ERROR_SYSTEM,
} pocketjs_net_error_code_t;

typedef enum {
  POCKETJS_NET_ESP_HTTP_EVENT_RESPONSE_HEADER = 1,
  POCKETJS_NET_ESP_HTTP_EVENT_RESPONSE_HEADERS_COMPLETE,
  POCKETJS_NET_ESP_HTTP_EVENT_RESPONSE_BODY,
  POCKETJS_NET_ESP_HTTP_EVENT_COMPLETE,
  POCKETJS_NET_ESP_HTTP_EVENT_ERROR,
} pocketjs_net_esp_http_event_type_t;

typedef struct {
  const char *name;
  const char *value;
} pocketjs_net_http_header_t;

typedef struct {
  /**
   * Non-zero cancellation token chosen by the owner. Successfully accepted
   * requests must use values that strictly increase for this client lifetime.
   * A failed start does not consume its value. UINT32_MAX may be accepted once;
   * after that, no further request can be accepted by the same client.
   */
  uint32_t operation_id;
  const char *url;
  const char *method;
  const pocketjs_net_http_header_t *headers;
  size_t header_count;
  const void *body;
  size_t body_length;
  uint32_t io_timeout_ms;
  size_t max_response_body_bytes;
} pocketjs_net_esp_http_request_t;

/**
 * Notification hook called synchronously by the fixed worker after it queues
 * an event. It must return promptly, must not block or re-enter this client
 * through any API, and must not destroy the client. It should only signal the
 * product-owned scheduler that receive work is available.
 */
typedef void (*pocketjs_net_wake_fn)(void *context);
typedef bool (*pocketjs_net_wall_clock_trusted_fn)(void *context);

typedef struct {
  pocketjs_net_wake_fn wake;
  void *wake_context;
  bool allow_https;
  pocketjs_net_wall_clock_trusted_fn wall_clock_trusted;
  void *wall_clock_context;
  const char *worker_task_name;
  uint32_t worker_stack_bytes;
  /** Zero selects the backend default; explicit values must be lower than
   * configMAX_PRIORITIES. */
  UBaseType_t worker_priority;
  BaseType_t worker_core;
} pocketjs_net_esp_http_client_config_t;

typedef struct {
  size_t max_inflight_requests;
  size_t max_url_bytes;
  size_t max_request_headers;
  size_t max_request_header_bytes;
  size_t max_request_body_bytes;
  size_t native_tx_buffer_bytes;
  size_t max_response_headers;
  size_t max_response_header_bytes;
  size_t max_response_body_bytes;
  size_t body_chunk_bytes;
  size_t buffer_leases;
  size_t queued_data_events;
} pocketjs_net_esp_http_limits_t;

typedef struct {
  const char *id;
  const char *protocol_version;
  const char *implementation_version;
  bool native_backend;
  bool response_streaming;
  bool automatic_redirects_disabled;
  bool automatic_auth_retries_disabled;
  bool internal_tls_compiled;
  bool duplicate_response_headers;
  bool manual_connection_close;
  bool bounded_response_parser;
  bool arbitrary_method;
  bool wire_status_text;
  bool cancel_during_connect;
  bool response_trailers;
  pocketjs_net_esp_http_limits_t hard_limits;
} pocketjs_net_esp_http_backend_descriptor_t;

typedef struct {
  pocketjs_net_esp_http_event_type_t type;
  uint32_t operation_id;
  uint64_t sequence;
  const uint8_t *payload;
  size_t payload_length;
  uint32_t lease_id;
  uint32_t lease_generation;
  union {
    struct {
      size_t name_length;
      size_t value_length;
    } header;
    struct {
      int status;
      int64_t content_length;
      bool chunked;
    } response;
    struct {
      pocketjs_net_error_code_t code;
      esp_err_t cause_code;
      int tls_code;
      bool temporary;
    } error;
  } detail;
} pocketjs_net_esp_http_event_t;

typedef struct {
  uint64_t submitted_requests;
  uint64_t completed_requests;
  uint64_t failed_requests;
  uint64_t response_body_bytes;
  size_t buffer_leases_high_water;
  size_t worker_stack_low_water_bytes;
} pocketjs_net_esp_http_stats_t;

/**
 * This descriptor identifies an experimental native Backend candidate. It does
 * not advertise a PocketJS public capability; the product Host must still pass
 * policy, scheduler, private-ABI, protocol and hardware admission.
 */
const pocketjs_net_esp_http_backend_descriptor_t *
pocketjs_net_esp_http_backend_descriptor(void);

/**
 * Create one bounded HTTP client worker. The component never initializes a
 * network interface and never calls JavaScript. The product BSP owns both.
 */
esp_err_t pocketjs_net_esp_http_client_create(
    const pocketjs_net_esp_http_client_config_t *config,
    pocketjs_net_esp_http_client_t **out_client);

/**
 * Snapshot the request synchronously and queue it for the native worker. At
 * most one request is accepted until its terminal event has been received.
 * Admission and the non-blocking COMMAND_START enqueue share the owner-state
 * mutex. ESP_OK consumes operation_id; every failure leaves it available for a
 * retry. Each consumed ID must be strictly greater than every previously
 * consumed ID for this client. Consuming UINT32_MAX permanently exhausts the
 * client's ID space, so the owner must destroy and create a new client before
 * starting another request.
 */
esp_err_t pocketjs_net_esp_http_client_start(
    pocketjs_net_esp_http_client_t *client,
    const pocketjs_net_esp_http_request_t *request);

/**
 * Request cooperative cancellation. This call briefly takes the bounded owner
 * state mutex so operation-id validation and flag publication are linearized
 * with terminal receipt and the next start. It never touches the ESP HTTP
 * Client handle or performs I/O. An in-progress DNS, connect, TLS, read or
 * write call cannot be interrupted; in particular, synchronous DNS may not
 * honor io_timeout_ms. operation_id is the token of a request whose start
 * returned ESP_OK. The token can cancel only that request while it is queued or
 * running; it is invalid for cancellation after the worker claims the terminal
 * event. Strictly increasing IDs prevent an old token from naming a future
 * request. ESP_OK means cancellation won the terminal claim and the exactly-
 * once terminal event will report ABORTED. ESP_ERR_NOT_FOUND means the token is
 * not the cancellable active request or the terminal claim already won.
 */
esp_err_t
pocketjs_net_esp_http_client_cancel(pocketjs_net_esp_http_client_t *client,
                                    uint32_t operation_id);

/**
 * Receive the globally oldest visible data or terminal event. BODY and HEADER
 * payloads remain owned by the component until release_event succeeds.
 */
esp_err_t
pocketjs_net_esp_http_client_receive(pocketjs_net_esp_http_client_t *client,
                                     pocketjs_net_esp_http_event_t *out_event,
                                     TickType_t wait_ticks);

/** Release a payload lease exactly once. Payload-free events need no release.
 */
esp_err_t pocketjs_net_esp_http_client_release_event(
    pocketjs_net_esp_http_client_t *client,
    pocketjs_net_esp_http_event_t *event);

void pocketjs_net_esp_http_client_get_stats(
    pocketjs_net_esp_http_client_t *client,
    pocketjs_net_esp_http_stats_t *out_stats);

/**
 * Publish cancellation, join the worker and release every native allocation.
 * The caller must have stopped concurrent API calls. There is no uniform upper
 * bound for destruction: native calls cannot be interrupted and synchronous
 * DNS may outlive io_timeout_ms.
 */
void pocketjs_net_esp_http_client_destroy(
    pocketjs_net_esp_http_client_t *client);

const char *pocketjs_net_error_code_name(pocketjs_net_error_code_t code);

#ifdef __cplusplus
}
#endif
