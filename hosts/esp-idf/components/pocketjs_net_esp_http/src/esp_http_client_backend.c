// SPDX-License-Identifier: MIT

#include "pocketjs/net/esp_http_client_backend.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_idf_version.h"
#include "esp_tls_errors.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/x509.h"
#include "sdkconfig.h"

#if CONFIG_ESP_HTTP_CLIENT_EVENT_POST_TIMEOUT != 0
#error "PocketJS net requires CONFIG_ESP_HTTP_CLIENT_EVENT_POST_TIMEOUT=0"
#endif
#if defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_BASIC_AUTH) &&                       \
    CONFIG_ESP_HTTP_CLIENT_ENABLE_BASIC_AUTH
#error "PocketJS net requires ESP HTTP Client Basic authentication disabled"
#endif
#if defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_DIGEST_AUTH) &&                      \
    CONFIG_ESP_HTTP_CLIENT_ENABLE_DIGEST_AUTH
#error "PocketJS net requires ESP HTTP Client Digest authentication disabled"
#endif
#if defined(CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS) &&                   \
    CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS
#error                                                                         \
    "PocketJS net copies response headers and requires saved headers disabled"
#endif

#if defined(CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS) &&                            \
    CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS &&                                     \
    defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) &&                              \
    CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#define POCKETJS_NET_ESP_HTTP_INTERNAL_TLS 1
#else
#define POCKETJS_NET_ESP_HTTP_INTERNAL_TLS 0
#endif

#define POCKETJS_NET_ESP_HTTP_EVENT_QUEUE_LENGTH                               \
  (POCKETJS_NET_ESP_HTTP_DATA_EVENTS + 1U)
#define POCKETJS_NET_ESP_HTTP_COMMAND_QUEUE_LENGTH 2U
#define POCKETJS_NET_ESP_HTTP_MIN_WORKER_STACK_BYTES (6U * 1024U)
#define POCKETJS_NET_ESP_HTTP_MAX_WORKER_STACK_BYTES (32U * 1024U)
#define POCKETJS_NET_ESP_HTTP_BACKPRESSURE_POLL_MS 10U
#define POCKETJS_NET_ESP_HTTP_OWNER_LOCK_TIMEOUT_MS 100U
#define POCKETJS_NET_STRINGIFY_INNER(value) #value
#define POCKETJS_NET_STRINGIFY(value) POCKETJS_NET_STRINGIFY_INNER(value)
#define POCKETJS_NET_ESP_HTTP_IMPLEMENTATION_VERSION                                    \
  "esp-idf-v" POCKETJS_NET_STRINGIFY(ESP_IDF_VERSION_MAJOR) "." POCKETJS_NET_STRINGIFY( \
      ESP_IDF_VERSION_MINOR) "." POCKETJS_NET_STRINGIFY(ESP_IDF_VERSION_PATCH) "-experimental"

/* ESP HTTP Client formats the request line and initial Host field in its TX
 * buffer. URL bytes cover the partitioned authority plus request-target. */
_Static_assert(POCKETJS_NET_ESP_HTTP_TX_BUFFER_BYTES >=
                   POCKETJS_NET_ESP_HTTP_MAX_URL_BYTES +
                       POCKETJS_NET_ESP_HTTP_MAX_METHOD_BYTES +
                       sizeof("  HTTP/1.1\r\nHost: \r\n") - 1U,
               "native TX buffer must hold the maximum request line and Host");
_Static_assert(POCKETJS_NET_ESP_HTTP_TX_BUFFER_BYTES >=
                   POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_NAME_BYTES +
                       POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_VALUE_BYTES +
                       sizeof(": \r\n") - 1U,
               "native TX buffer must hold the maximum request header");

typedef enum {
  CLIENT_STATE_IDLE = 0,
  CLIENT_STATE_QUEUED,
  CLIENT_STATE_RUNNING,
  CLIENT_STATE_TERMINAL_QUEUED,
} client_state_t;

typedef enum {
  COMMAND_START = 1,
  COMMAND_SHUTDOWN,
} command_t;

typedef struct {
  char name[POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_NAME_BYTES + 1U];
  char value[POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_VALUE_BYTES + 1U];
} request_header_snapshot_t;

typedef struct {
  uint32_t operation_id;
  char url[POCKETJS_NET_ESP_HTTP_MAX_URL_BYTES + 1U];
  char method[POCKETJS_NET_ESP_HTTP_MAX_METHOD_BYTES + 1U];
  esp_http_client_method_t method_code;
  request_header_snapshot_t headers[POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADERS];
  size_t header_count;
  uint8_t body[POCKETJS_NET_ESP_HTTP_MAX_REQUEST_BODY_BYTES];
  size_t body_length;
  uint32_t io_timeout_ms;
  size_t max_response_body_bytes;
  bool https;
} request_snapshot_t;

typedef struct {
  uint32_t generation;
  bool in_use;
} lease_state_t;

typedef struct {
  pocketjs_net_esp_http_client_t *client;
  uint32_t operation_id;
  size_t response_header_count;
  size_t response_header_bytes;
  size_t response_body_bytes;
  bool native_headers_complete;
  bool wire_transfer_encoding_seen;
  bool wire_chunked;
  pocketjs_net_error_code_t failure;
  esp_err_t cause;
} execution_context_t;

struct pocketjs_net_esp_http_client {
  QueueHandle_t commands;
  QueueHandle_t events;
  QueueHandle_t free_leases;
  SemaphoreHandle_t mutex;
  SemaphoreHandle_t stopped;
  TaskHandle_t worker;

  client_state_t state;
  request_snapshot_t request;
  uint32_t last_operation_id;
  atomic_bool cancel_requested;
  atomic_bool closing;
  atomic_uint_fast32_t active_operation_id;

  pocketjs_net_wake_fn wake;
  void *wake_context;
  bool allow_https;
  pocketjs_net_wall_clock_trusted_fn wall_clock_trusted;
  void *wall_clock_context;

  uint64_t next_sequence;
  uint8_t *lease_storage;
  lease_state_t leases[POCKETJS_NET_ESP_HTTP_BUFFER_LEASES];
  size_t leases_in_use;
  pocketjs_net_esp_http_stats_t stats;
};

static const pocketjs_net_esp_http_backend_descriptor_t s_descriptor = {
    .id = POCKETJS_NET_ESP_HTTP_BACKEND_ID,
    .protocol_version = POCKETJS_NET_ESP_HTTP_PROTOCOL_VERSION,
    .implementation_version = POCKETJS_NET_ESP_HTTP_IMPLEMENTATION_VERSION,
    .native_backend = true,
    .response_streaming = true,
    .automatic_redirects_disabled = true,
    .automatic_auth_retries_disabled = true,
    .internal_tls_compiled = POCKETJS_NET_ESP_HTTP_INTERNAL_TLS,
    .duplicate_response_headers = true,
    .manual_connection_close = true,
    .bounded_response_parser = false,
    .arbitrary_method = false,
    .wire_status_text = false,
    .cancel_during_connect = false,
    .response_trailers = false,
    .hard_limits =
        {
            .max_inflight_requests = 1U,
            .max_url_bytes = POCKETJS_NET_ESP_HTTP_MAX_URL_BYTES,
            .max_request_headers = POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADERS,
            .max_request_header_bytes =
                POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_BYTES,
            .max_request_body_bytes =
                POCKETJS_NET_ESP_HTTP_MAX_REQUEST_BODY_BYTES,
            .native_tx_buffer_bytes = POCKETJS_NET_ESP_HTTP_TX_BUFFER_BYTES,
            .max_response_headers = POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_HEADERS,
            .max_response_header_bytes =
                POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_HEADER_BYTES,
            .max_response_body_bytes =
                POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_BODY_BYTES,
            .body_chunk_bytes = POCKETJS_NET_ESP_HTTP_BODY_CHUNK_BYTES,
            .buffer_leases = POCKETJS_NET_ESP_HTTP_BUFFER_LEASES,
            .queued_data_events = POCKETJS_NET_ESP_HTTP_DATA_EVENTS,
        },
};

static TickType_t backpressure_poll_ticks(void) {
  TickType_t ticks = pdMS_TO_TICKS(POCKETJS_NET_ESP_HTTP_BACKPRESSURE_POLL_MS);
  return ticks == 0 ? 1 : ticks;
}

static TickType_t owner_lock_timeout_ticks(void) {
  TickType_t ticks = pdMS_TO_TICKS(POCKETJS_NET_ESP_HTTP_OWNER_LOCK_TIMEOUT_MS);
  return ticks == 0 ? 1 : ticks;
}

static size_t bounded_string_length(const char *value, size_t maximum) {
  if (value == NULL) {
    return maximum + 1U;
  }

  size_t length = 0;
  while (length <= maximum && value[length] != '\0') {
    ++length;
  }
  return length;
}

static bool ascii_equal_ignore_case(const char *left, size_t left_length,
                                    const char *right) {
  size_t right_length = strlen(right);
  if (left_length != right_length) {
    return false;
  }

  for (size_t index = 0; index < left_length; ++index) {
    unsigned char a = (unsigned char)left[index];
    unsigned char b = (unsigned char)right[index];
    if (a >= 'A' && a <= 'Z') {
      a = (unsigned char)(a + ('a' - 'A'));
    }
    if (b >= 'A' && b <= 'Z') {
      b = (unsigned char)(b + ('a' - 'A'));
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

static bool ascii_equal_ignore_case_trim_spaces(const char *left,
                                                size_t left_length,
                                                const char *right) {
  /* ESP-IDF v6.0.2's transfer-coding matcher accepts SP after "chunked" but
   * not HTAB. Match that narrower behavior so the wire flag never claims that
   * undecoded chunk framing is a decoded body. */
  while (left_length > 0 && *left == ' ') {
    ++left;
    --left_length;
  }
  while (left_length > 0 && left[left_length - 1U] == ' ') {
    --left_length;
  }
  return ascii_equal_ignore_case(left, left_length, right);
}

static bool is_http_token_character(unsigned char value) {
  if ((value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
      (value >= 'a' && value <= 'z')) {
    return true;
  }

  switch (value) {
  case '!':
  case '#':
  case '$':
  case '%':
  case '&':
  case '\'':
  case '*':
  case '+':
  case '-':
  case '.':
  case '^':
  case '_':
  case '`':
  case '|':
  case '~':
    return true;
  default:
    return false;
  }
}

static bool is_http_token(const char *value, size_t length) {
  if (value == NULL || length == 0) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    if (!is_http_token_character((unsigned char)value[index])) {
      return false;
    }
  }
  return true;
}

static bool has_header_value_control(const char *value, size_t length) {
  for (size_t index = 0; index < length; ++index) {
    unsigned char byte = (unsigned char)value[index];
    if (byte == '\r' || byte == '\n' || byte == 0x7f ||
        (byte < 0x20 && byte != '\t')) {
      return true;
    }
  }
  return false;
}

static bool is_backend_owned_request_header(const char *name,
                                            size_t name_length) {
  static const char *const names[] = {
      "Connection", "Content-Length",    "Host",
      "Trailer",    "Transfer-Encoding", "Upgrade",
  };
  for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
    if (ascii_equal_ignore_case(name, name_length, names[index])) {
      return true;
    }
  }
  return false;
}

static bool map_method(const char *method, size_t length,
                       esp_http_client_method_t *out_method) {
  struct method_mapping {
    const char *name;
    esp_http_client_method_t code;
  };
  static const struct method_mapping mappings[] = {
      {"GET", HTTP_METHOD_GET},         {"HEAD", HTTP_METHOD_HEAD},
      {"POST", HTTP_METHOD_POST},       {"PUT", HTTP_METHOD_PUT},
      {"PATCH", HTTP_METHOD_PATCH},     {"DELETE", HTTP_METHOD_DELETE},
      {"OPTIONS", HTTP_METHOD_OPTIONS},
  };

  for (size_t index = 0; index < sizeof(mappings) / sizeof(mappings[0]);
       ++index) {
    if (strlen(mappings[index].name) == length &&
        memcmp(method, mappings[index].name, length) == 0) {
      *out_method = mappings[index].code;
      return true;
    }
  }
  return false;
}

static esp_err_t validate_url(const char *url, size_t length, bool *out_https) {
  static const char http_prefix[] = "http://";
  static const char https_prefix[] = "https://";
  size_t authority_start = 0;

  if (length > sizeof(https_prefix) - 1U &&
      memcmp(url, https_prefix, sizeof(https_prefix) - 1U) == 0) {
    *out_https = true;
    authority_start = sizeof(https_prefix) - 1U;
  } else if (length > sizeof(http_prefix) - 1U &&
             memcmp(url, http_prefix, sizeof(http_prefix) - 1U) == 0) {
    *out_https = false;
    authority_start = sizeof(http_prefix) - 1U;
  } else {
    return ESP_ERR_NOT_SUPPORTED;
  }

  size_t authority_end = length;
  for (size_t index = authority_start; index < length; ++index) {
    unsigned char byte = (unsigned char)url[index];
    if (byte <= 0x20 || byte == 0x7f || byte == '#') {
      return ESP_ERR_INVALID_ARG;
    }
    if (authority_end == length && (byte == '/' || byte == '?')) {
      authority_end = index;
    }
  }
  if (authority_end == authority_start) {
    return ESP_ERR_INVALID_ARG;
  }
  for (size_t index = authority_start; index < authority_end; ++index) {
    if (url[index] == '@') {
      return ESP_ERR_NOT_SUPPORTED;
    }
  }
  return ESP_OK;
}

static void wake_owner(pocketjs_net_esp_http_client_t *client) {
  if (client->wake != NULL) {
    client->wake(client->wake_context);
  }
}

static bool
cancellation_requested(const pocketjs_net_esp_http_client_t *client) {
  return atomic_load_explicit(&client->cancel_requested,
                              memory_order_acquire) ||
         atomic_load_explicit(&client->closing, memory_order_acquire);
}

static void set_execution_failure(execution_context_t *context,
                                  pocketjs_net_error_code_t failure,
                                  esp_err_t cause) {
  if (context->failure == POCKETJS_NET_ERROR_NONE) {
    context->failure = failure;
    context->cause = cause;
  }
}

static esp_err_t acquire_lease(pocketjs_net_esp_http_client_t *client,
                               uint32_t *out_id, uint32_t *out_generation,
                               uint8_t **out_buffer) {
  uint8_t index = 0;
  for (;;) {
    if (cancellation_requested(client)) {
      return ESP_ERR_INVALID_STATE;
    }
    if (xQueueReceive(client->free_leases, &index, backpressure_poll_ticks()) ==
        pdTRUE) {
      break;
    }
  }

  if (xSemaphoreTake(client->mutex, portMAX_DELAY) != pdTRUE) {
    (void)xQueueSend(client->free_leases, &index, 0);
    return ESP_FAIL;
  }
  if (index >= POCKETJS_NET_ESP_HTTP_BUFFER_LEASES ||
      client->leases[index].in_use) {
    xSemaphoreGive(client->mutex);
    (void)xQueueSend(client->free_leases, &index, 0);
    return ESP_ERR_INVALID_STATE;
  }

  uint32_t generation = client->leases[index].generation + 1U;
  if (generation == 0) {
    generation = 1U;
  }
  client->leases[index].generation = generation;
  client->leases[index].in_use = true;
  ++client->leases_in_use;
  if (client->leases_in_use > client->stats.buffer_leases_high_water) {
    client->stats.buffer_leases_high_water = client->leases_in_use;
  }
  xSemaphoreGive(client->mutex);

  *out_id = (uint32_t)index + 1U;
  *out_generation = generation;
  *out_buffer = client->lease_storage +
                ((size_t)index * POCKETJS_NET_ESP_HTTP_BODY_CHUNK_BYTES);
  return ESP_OK;
}

static esp_err_t release_lease(pocketjs_net_esp_http_client_t *client,
                               uint32_t lease_id, uint32_t generation,
                               const uint8_t *payload) {
  if (lease_id == 0 || lease_id > POCKETJS_NET_ESP_HTTP_BUFFER_LEASES) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t index = (uint8_t)(lease_id - 1U);
  const uint8_t *expected =
      client->lease_storage +
      ((size_t)index * POCKETJS_NET_ESP_HTTP_BODY_CHUNK_BYTES);

  if (xSemaphoreTake(client->mutex, portMAX_DELAY) != pdTRUE) {
    return ESP_FAIL;
  }
  if (!client->leases[index].in_use ||
      client->leases[index].generation != generation || payload != expected) {
    xSemaphoreGive(client->mutex);
    return ESP_ERR_INVALID_STATE;
  }
  client->leases[index].in_use = false;
  --client->leases_in_use;
  xSemaphoreGive(client->mutex);

  if (xQueueSend(client->free_leases, &index, 0) != pdTRUE) {
    if (xSemaphoreTake(client->mutex, portMAX_DELAY) == pdTRUE) {
      client->leases[index].in_use = true;
      ++client->leases_in_use;
      xSemaphoreGive(client->mutex);
    }
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

static esp_err_t enqueue_data_event(pocketjs_net_esp_http_client_t *client,
                                    pocketjs_net_esp_http_event_t *event) {
  for (;;) {
    if (cancellation_requested(client)) {
      return ESP_ERR_INVALID_STATE;
    }

    /* Only this worker produces events. Limiting non-terminal entries to
     * DATA_EVENTS permanently reserves the last queue slot for settlement. */
    if (uxQueueMessagesWaiting(client->events) <
        POCKETJS_NET_ESP_HTTP_DATA_EVENTS) {
      event->sequence = ++client->next_sequence;
      if (xQueueSend(client->events, event, 0) == pdTRUE) {
        wake_owner(client);
        return ESP_OK;
      }
    }
    vTaskDelay(backpressure_poll_ticks());
  }
}

static esp_err_t enqueue_header(execution_context_t *context, const char *name,
                                size_t name_length, const char *value,
                                size_t value_length) {
  pocketjs_net_esp_http_client_t *client = context->client;
  uint32_t lease_id = 0;
  uint32_t generation = 0;
  uint8_t *buffer = NULL;
  esp_err_t result = acquire_lease(client, &lease_id, &generation, &buffer);
  if (result != ESP_OK) {
    return result;
  }

  memcpy(buffer, name, name_length);
  memcpy(buffer + name_length, value, value_length);
  pocketjs_net_esp_http_event_t event = {
      .type = POCKETJS_NET_ESP_HTTP_EVENT_RESPONSE_HEADER,
      .operation_id = context->operation_id,
      .payload = buffer,
      .payload_length = name_length + value_length,
      .lease_id = lease_id,
      .lease_generation = generation,
      .detail.header =
          {
              .name_length = name_length,
              .value_length = value_length,
          },
  };
  result = enqueue_data_event(client, &event);
  if (result != ESP_OK) {
    (void)release_lease(client, lease_id, generation, buffer);
  }
  return result;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event) {
  execution_context_t *context = event == NULL ? NULL : event->user_data;
  if (context == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (event->event_id == HTTP_EVENT_ON_HEADERS_COMPLETE) {
    context->native_headers_complete = true;
    return ESP_OK;
  }
  if (event->event_id != HTTP_EVENT_ON_HEADER) {
    return ESP_OK;
  }
  if (context->failure != POCKETJS_NET_ERROR_NONE) {
    return ESP_FAIL;
  }
  if (cancellation_requested(context->client)) {
    set_execution_failure(context, POCKETJS_NET_ERROR_ABORTED,
                          ESP_ERR_INVALID_STATE);
    return ESP_FAIL;
  }
  /* Reject trailers that ESP HTTP Client exposes through the header callback
   * instead of misordering them after body events. Its parser does not promise
   * to expose every final trailer, so this is not complete trailer validation.
   */
  if (context->native_headers_complete) {
    set_execution_failure(context, POCKETJS_NET_ERROR_UNSUPPORTED,
                          ESP_ERR_NOT_SUPPORTED);
    return ESP_FAIL;
  }

  size_t name_length = bounded_string_length(
      event->header_key, POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_FIELD_BYTES);
  size_t value_length = bounded_string_length(
      event->header_value, POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_FIELD_BYTES);
  if (name_length > POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_FIELD_BYTES ||
      value_length > POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_FIELD_BYTES ||
      name_length + value_length > POCKETJS_NET_ESP_HTTP_BODY_CHUNK_BYTES ||
      !is_http_token(event->header_key, name_length) ||
      has_header_value_control(event->header_value, value_length)) {
    set_execution_failure(context, POCKETJS_NET_ERROR_HTTP_PROTOCOL_ERROR,
                          ESP_ERR_INVALID_RESPONSE);
    return ESP_FAIL;
  }

  size_t framed_bytes = name_length + value_length + 4U;
  if (context->response_header_count >=
          POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_HEADERS ||
      framed_bytes > POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_HEADER_BYTES -
                         context->response_header_bytes) {
    set_execution_failure(context, POCKETJS_NET_ERROR_RESOURCE_LIMIT,
                          ESP_ERR_NO_MEM);
    return ESP_FAIL;
  }

  if (ascii_equal_ignore_case(event->header_key, name_length,
                              "Transfer-Encoding")) {
    if (context->wire_transfer_encoding_seen) {
      set_execution_failure(context, POCKETJS_NET_ERROR_HTTP_PROTOCOL_ERROR,
                            ESP_ERR_INVALID_RESPONSE);
      return ESP_FAIL;
    }
    context->wire_transfer_encoding_seen = true;
    if (!ascii_equal_ignore_case_trim_spaces(event->header_value, value_length,
                                             "chunked")) {
      set_execution_failure(context, POCKETJS_NET_ERROR_UNSUPPORTED,
                            ESP_ERR_NOT_SUPPORTED);
      return ESP_FAIL;
    }
    context->wire_chunked = true;
  }

  ++context->response_header_count;
  context->response_header_bytes += framed_bytes;
  esp_err_t result = enqueue_header(context, event->header_key, name_length,
                                    event->header_value, value_length);
  if (result != ESP_OK) {
    set_execution_failure(context,
                          cancellation_requested(context->client)
                              ? POCKETJS_NET_ERROR_ABORTED
                              : POCKETJS_NET_ERROR_SYSTEM,
                          result);
    return ESP_FAIL;
  }
  return ESP_OK;
}

static esp_err_t enqueue_headers_complete(execution_context_t *context,
                                          int status, int64_t content_length,
                                          bool chunked) {
  pocketjs_net_esp_http_event_t event = {
      .type = POCKETJS_NET_ESP_HTTP_EVENT_RESPONSE_HEADERS_COMPLETE,
      .operation_id = context->operation_id,
      .detail.response =
          {
              .status = status,
              .content_length = content_length,
              .chunked = chunked,
          },
  };
  return enqueue_data_event(context->client, &event);
}

static esp_err_t enqueue_body_lease(execution_context_t *context,
                                    uint32_t lease_id, uint32_t generation,
                                    uint8_t *buffer, size_t length) {
  pocketjs_net_esp_http_event_t event = {
      .type = POCKETJS_NET_ESP_HTTP_EVENT_RESPONSE_BODY,
      .operation_id = context->operation_id,
      .payload = buffer,
      .payload_length = length,
      .lease_id = lease_id,
      .lease_generation = generation,
  };
  return enqueue_data_event(context->client, &event);
}

static pocketjs_net_error_code_t map_native_error(esp_err_t cause,
                                                  int socket_errno,
                                                  int tls_code, int tls_flags,
                                                  bool https) {
  if (cause == ESP_ERR_NO_MEM) {
    return POCKETJS_NET_ERROR_RESOURCE_LIMIT;
  }
  if (cause == ESP_ERR_HTTP_EAGAIN || cause == ESP_ERR_HTTP_READ_TIMEOUT ||
      cause == ESP_ERR_HTTP_CONNECTING ||
      cause == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT ||
      cause == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT ||
      socket_errno == ETIMEDOUT || socket_errno == EAGAIN ||
      socket_errno == EWOULDBLOCK) {
    return POCKETJS_NET_ERROR_TIMED_OUT;
  }
  if (cause == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
    return POCKETJS_NET_ERROR_DNS_NOT_FOUND;
  }
  if (socket_errno == ECONNREFUSED) {
    return POCKETJS_NET_ERROR_CONNECTION_REFUSED;
  }
  if (socket_errno == ECONNRESET || cause == ESP_ERR_HTTP_CONNECTION_CLOSED) {
    return POCKETJS_NET_ERROR_CONNECTION_RESET;
  }
  if (socket_errno == ENETUNREACH || socket_errno == EHOSTUNREACH) {
    return POCKETJS_NET_ERROR_NETWORK_UNREACHABLE;
  }
#ifdef MBEDTLS_X509_BADCERT_CN_MISMATCH
  if (https && (tls_flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0) {
    return POCKETJS_NET_ERROR_TLS_HOSTNAME_MISMATCH;
  }
#endif
  if (https && tls_flags != 0) {
    return POCKETJS_NET_ERROR_TLS_CERTIFICATE_INVALID;
  }
  if (https && (tls_code != 0 || (cause >= ESP_ERR_ESP_TLS_BASE &&
                                  cause < ESP_ERR_ESP_TLS_BASE + 0x100))) {
    return POCKETJS_NET_ERROR_TLS_HANDSHAKE_FAILED;
  }
  if (cause == ESP_ERR_HTTP_INCOMPLETE_DATA ||
      cause == ESP_ERR_HTTP_FETCH_HEADER || cause == ESP_ERR_INVALID_RESPONSE) {
    return POCKETJS_NET_ERROR_HTTP_PROTOCOL_ERROR;
  }
  return POCKETJS_NET_ERROR_SYSTEM;
}

static bool error_is_temporary(pocketjs_net_error_code_t code) {
  return code == POCKETJS_NET_ERROR_TIMED_OUT ||
         code == POCKETJS_NET_ERROR_CONNECTION_RESET ||
         code == POCKETJS_NET_ERROR_NETWORK_UNREACHABLE;
}

static void snapshot_native_error(esp_http_client_handle_t handle,
                                  esp_err_t *cause, int *socket_errno,
                                  int *tls_code, int *tls_flags) {
  *socket_errno = esp_http_client_get_errno(handle);
  esp_err_t tls_error =
      esp_http_client_get_and_clear_last_tls_error(handle, tls_code, tls_flags);
  if (tls_error != ESP_OK) {
    *cause = tls_error;
  }
}

static pocketjs_net_error_code_t
execute_request(pocketjs_net_esp_http_client_t *client, esp_err_t *out_cause,
                int *out_tls_code) {
  request_snapshot_t *request = &client->request;
  execution_context_t context = {
      .client = client,
      .operation_id = request->operation_id,
      .failure = POCKETJS_NET_ERROR_NONE,
      .cause = ESP_OK,
  };
  esp_http_client_handle_t handle = NULL;
  esp_err_t operation_result = ESP_OK;
  int socket_errno = 0;
  int tls_code = 0;
  int tls_flags = 0;

  if (cancellation_requested(client)) {
    *out_cause = ESP_ERR_INVALID_STATE;
    *out_tls_code = 0;
    return POCKETJS_NET_ERROR_ABORTED;
  }

  esp_http_client_config_t config = {
      .url = request->url,
      .auth_type = HTTP_AUTH_TYPE_NONE,
      .method = request->method_code,
      .timeout_ms = (int)request->io_timeout_ms,
      .disable_auto_redirect = true,
      .max_redirection_count = 1,
      .max_authorization_retries = -1,
      .event_handler = http_event_handler,
      .buffer_size = (int)POCKETJS_NET_ESP_HTTP_BODY_CHUNK_BYTES,
      .buffer_size_tx = (int)POCKETJS_NET_ESP_HTTP_TX_BUFFER_BYTES,
      .user_data = &context,
      .skip_cert_common_name_check = false,
      .keep_alive_enable = false,
      .addr_type = HTTP_ADDR_TYPE_INET,
  };
#if POCKETJS_NET_ESP_HTTP_INTERNAL_TLS
  if (request->https) {
    config.crt_bundle_attach = esp_crt_bundle_attach;
  }
#endif

  /* All handle-bearing calls in this function execute on the fixed worker. */
  handle = esp_http_client_init(&config);
  if (handle == NULL) {
    operation_result = ESP_ERR_NO_MEM;
    goto finish;
  }

  operation_result = esp_http_client_delete_header(handle, "User-Agent");
  if (operation_result != ESP_OK) {
    goto finish;
  }
  operation_result = esp_http_client_set_method(handle, request->method_code);
  if (operation_result != ESP_OK) {
    goto finish;
  }
  for (size_t index = 0; index < request->header_count; ++index) {
    operation_result = esp_http_client_set_header(
        handle, request->headers[index].name, request->headers[index].value);
    if (operation_result != ESP_OK) {
      goto finish;
    }
  }
  operation_result = esp_http_client_set_header(handle, "Connection", "close");
  if (operation_result != ESP_OK) {
    goto finish;
  }

  operation_result = esp_http_client_open(handle, (int)request->body_length);
  if (operation_result != ESP_OK) {
    goto finish;
  }
  if (cancellation_requested(client)) {
    context.failure = POCKETJS_NET_ERROR_ABORTED;
    context.cause = ESP_ERR_INVALID_STATE;
    goto finish;
  }

  size_t written = 0;
  while (written < request->body_length) {
    if (cancellation_requested(client)) {
      context.failure = POCKETJS_NET_ERROR_ABORTED;
      context.cause = ESP_ERR_INVALID_STATE;
      goto finish;
    }
    int result =
        esp_http_client_write(handle, (const char *)request->body + written,
                              (int)(request->body_length - written));
    if (result <= 0) {
      operation_result = ESP_ERR_HTTP_WRITE_DATA;
      goto finish;
    }
    written += (size_t)result;
  }

  if (cancellation_requested(client)) {
    context.failure = POCKETJS_NET_ERROR_ABORTED;
    context.cause = ESP_ERR_INVALID_STATE;
    goto finish;
  }

  int64_t fetched_length = esp_http_client_fetch_headers(handle);
  if (fetched_length < 0) {
    operation_result = fetched_length == -(int64_t)ESP_ERR_HTTP_EAGAIN
                           ? ESP_ERR_HTTP_EAGAIN
                           : ESP_ERR_HTTP_FETCH_HEADER;
    goto finish;
  }
  if (context.failure != POCKETJS_NET_ERROR_NONE) {
    goto finish;
  }

  int status = esp_http_client_get_status_code(handle);
  int64_t content_length = esp_http_client_get_content_length(handle);
  if (status < 100 || status > 599) {
    context.failure = POCKETJS_NET_ERROR_HTTP_PROTOCOL_ERROR;
    context.cause = ESP_ERR_INVALID_RESPONSE;
    goto finish;
  }
  if (content_length < -1) {
    context.failure = POCKETJS_NET_ERROR_HTTP_PROTOCOL_ERROR;
    context.cause = ESP_ERR_INVALID_RESPONSE;
    goto finish;
  }
  if (status < 200) {
    context.failure = POCKETJS_NET_ERROR_UNSUPPORTED;
    context.cause = ESP_ERR_NOT_SUPPORTED;
    goto finish;
  }
  if (content_length > 0 &&
      (uint64_t)content_length > (uint64_t)request->max_response_body_bytes) {
    context.failure = POCKETJS_NET_ERROR_RESOURCE_LIMIT;
    context.cause = ESP_ERR_NO_MEM;
    goto finish;
  }

  operation_result = enqueue_headers_complete(&context, status, content_length,
                                              context.wire_chunked);
  if (operation_result != ESP_OK) {
    context.failure = cancellation_requested(client)
                          ? POCKETJS_NET_ERROR_ABORTED
                          : POCKETJS_NET_ERROR_SYSTEM;
    context.cause = operation_result;
    goto finish;
  }

  bool response_has_no_body = request->method_code == HTTP_METHOD_HEAD ||
                              status == 204 || status == 304 ||
                              (status >= 100 && status < 200);
  while (!response_has_no_body) {
    if (cancellation_requested(client)) {
      context.failure = POCKETJS_NET_ERROR_ABORTED;
      context.cause = ESP_ERR_INVALID_STATE;
      goto finish;
    }

    uint32_t lease_id = 0;
    uint32_t generation = 0;
    uint8_t *buffer = NULL;
    operation_result = acquire_lease(client, &lease_id, &generation, &buffer);
    if (operation_result != ESP_OK) {
      context.failure = cancellation_requested(client)
                            ? POCKETJS_NET_ERROR_ABORTED
                            : POCKETJS_NET_ERROR_SYSTEM;
      context.cause = operation_result;
      goto finish;
    }
    if (cancellation_requested(client)) {
      (void)release_lease(client, lease_id, generation, buffer);
      context.failure = POCKETJS_NET_ERROR_ABORTED;
      context.cause = ESP_ERR_INVALID_STATE;
      goto finish;
    }

    int read_result = esp_http_client_read(
        handle, (char *)buffer, (int)POCKETJS_NET_ESP_HTTP_BODY_CHUNK_BYTES);
    if (context.failure != POCKETJS_NET_ERROR_NONE) {
      (void)release_lease(client, lease_id, generation, buffer);
      goto finish;
    }
    if (read_result <= 0) {
      (void)release_lease(client, lease_id, generation, buffer);
      if (read_result < 0) {
        operation_result = read_result == -(int)ESP_ERR_HTTP_EAGAIN
                               ? ESP_ERR_HTTP_EAGAIN
                               : ESP_FAIL;
      }
      break;
    }

    size_t read_length = (size_t)read_result;
    if (read_length >
        request->max_response_body_bytes - context.response_body_bytes) {
      (void)release_lease(client, lease_id, generation, buffer);
      context.failure = POCKETJS_NET_ERROR_RESOURCE_LIMIT;
      context.cause = ESP_ERR_NO_MEM;
      goto finish;
    }
    operation_result =
        enqueue_body_lease(&context, lease_id, generation, buffer, read_length);
    if (operation_result != ESP_OK) {
      (void)release_lease(client, lease_id, generation, buffer);
      context.failure = cancellation_requested(client)
                            ? POCKETJS_NET_ERROR_ABORTED
                            : POCKETJS_NET_ERROR_SYSTEM;
      context.cause = operation_result;
      goto finish;
    }
    context.response_body_bytes += read_length;
  }

  if (!response_has_no_body && context.failure == POCKETJS_NET_ERROR_NONE &&
      operation_result != ESP_ERR_HTTP_EAGAIN && content_length >= 0 &&
      context.response_body_bytes != (uint64_t)content_length) {
    context.failure = POCKETJS_NET_ERROR_HTTP_PROTOCOL_ERROR;
    context.cause = ESP_ERR_HTTP_INCOMPLETE_DATA;
    goto finish;
  }
  if (operation_result != ESP_OK) {
    goto finish;
  }
  if (!response_has_no_body &&
      !esp_http_client_is_complete_data_received(handle)) {
    context.failure = POCKETJS_NET_ERROR_HTTP_PROTOCOL_ERROR;
    context.cause = ESP_ERR_HTTP_INCOMPLETE_DATA;
    goto finish;
  }

finish:
  if (handle != NULL) {
    if (operation_result != ESP_OK ||
        context.failure != POCKETJS_NET_ERROR_NONE ||
        cancellation_requested(client)) {
      snapshot_native_error(handle, &operation_result, &socket_errno, &tls_code,
                            &tls_flags);
    }
    (void)esp_http_client_close(handle);
    (void)esp_http_client_cleanup(handle);
  }

  if (xSemaphoreTake(client->mutex, portMAX_DELAY) == pdTRUE) {
    client->stats.response_body_bytes += context.response_body_bytes;
    xSemaphoreGive(client->mutex);
  }

  *out_tls_code = tls_code;
  if (cancellation_requested(client) ||
      context.failure == POCKETJS_NET_ERROR_ABORTED) {
    *out_cause =
        context.cause == ESP_OK ? ESP_ERR_INVALID_STATE : context.cause;
    return POCKETJS_NET_ERROR_ABORTED;
  }
  if (context.failure != POCKETJS_NET_ERROR_NONE) {
    *out_cause = context.cause;
    return context.failure;
  }
  if (operation_result != ESP_OK) {
    *out_cause = operation_result;
    return map_native_error(operation_result, socket_errno, tls_code, tls_flags,
                            request->https);
  }

  *out_cause = ESP_OK;
  return POCKETJS_NET_ERROR_NONE;
}

static void enqueue_terminal_event(pocketjs_net_esp_http_client_t *client,
                                   uint32_t operation_id,
                                   pocketjs_net_error_code_t result,
                                   esp_err_t cause, int tls_code) {
  /* Terminal claim shares the owner-state mutex with cancel, terminal receive,
   * and start. A successful cancel that wins this lock therefore changes the
   * settlement to ABORTED; once this claim wins, later cancel returns
   * NOT_FOUND.
   */
  if (xSemaphoreTake(client->mutex, portMAX_DELAY) == pdTRUE) {
    if (cancellation_requested(client)) {
      result = POCKETJS_NET_ERROR_ABORTED;
      cause = ESP_ERR_INVALID_STATE;
      tls_code = 0;
    }
    client->state = CLIENT_STATE_TERMINAL_QUEUED;
    if (result == POCKETJS_NET_ERROR_NONE) {
      ++client->stats.completed_requests;
    } else {
      ++client->stats.failed_requests;
    }
    client->stats.worker_stack_low_water_bytes =
        (size_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    xSemaphoreGive(client->mutex);
  }

  pocketjs_net_esp_http_event_t event = {
      .type = result == POCKETJS_NET_ERROR_NONE
                  ? POCKETJS_NET_ESP_HTTP_EVENT_COMPLETE
                  : POCKETJS_NET_ESP_HTTP_EVENT_ERROR,
      .operation_id = operation_id,
  };
  if (result != POCKETJS_NET_ERROR_NONE) {
    event.detail.error.code = result;
    event.detail.error.cause_code = cause;
    event.detail.error.tls_code = tls_code;
    event.detail.error.temporary = error_is_temporary(result);
  }

  event.sequence = ++client->next_sequence;
  /* DATA_EVENTS is an enforced producer ceiling, so this send owns the
   * pre-reserved terminal slot and cannot be blocked by response data. */
  (void)xQueueSend(client->events, &event, portMAX_DELAY);
  wake_owner(client);
}

static void http_worker(void *argument) {
  pocketjs_net_esp_http_client_t *client = argument;
  for (;;) {
    command_t command = 0;
    if (xQueueReceive(client->commands, &command, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    if (command == COMMAND_SHUTDOWN) {
      break;
    }
    if (command != COMMAND_START) {
      continue;
    }

    uint32_t operation_id = client->request.operation_id;
    if (xSemaphoreTake(client->mutex, portMAX_DELAY) == pdTRUE) {
      client->state = CLIENT_STATE_RUNNING;
      xSemaphoreGive(client->mutex);
    }
    esp_err_t cause = ESP_OK;
    int tls_code = 0;
    pocketjs_net_error_code_t result =
        execute_request(client, &cause, &tls_code);
    enqueue_terminal_event(client, operation_id, result, cause, tls_code);
  }

  xSemaphoreGive(client->stopped);
  vTaskDelete(NULL);
}

const pocketjs_net_esp_http_backend_descriptor_t *
pocketjs_net_esp_http_backend_descriptor(void) {
  return &s_descriptor;
}

esp_err_t pocketjs_net_esp_http_client_create(
    const pocketjs_net_esp_http_client_config_t *config,
    pocketjs_net_esp_http_client_t **out_client) {
  if (out_client == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_client = NULL;

  uint32_t stack_bytes = config == NULL || config->worker_stack_bytes == 0
                             ? POCKETJS_NET_ESP_HTTP_DEFAULT_WORKER_STACK_BYTES
                             : config->worker_stack_bytes;
  UBaseType_t priority = config == NULL || config->worker_priority == 0
                             ? tskIDLE_PRIORITY + 4U
                             : config->worker_priority;
  BaseType_t core = config == NULL ? tskNO_AFFINITY : config->worker_core;
  if (stack_bytes < POCKETJS_NET_ESP_HTTP_MIN_WORKER_STACK_BYTES ||
      stack_bytes > POCKETJS_NET_ESP_HTTP_MAX_WORKER_STACK_BYTES ||
      priority >= (UBaseType_t)configMAX_PRIORITIES ||
      (core != tskNO_AFFINITY && (core < 0 || core >= portNUM_PROCESSORS))) {
    return ESP_ERR_INVALID_ARG;
  }

  pocketjs_net_esp_http_client_t *client = heap_caps_calloc(
      1, sizeof(*client), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (client == NULL) {
    return ESP_ERR_NO_MEM;
  }
  client->lease_storage =
      heap_caps_calloc(POCKETJS_NET_ESP_HTTP_BUFFER_LEASES,
                       POCKETJS_NET_ESP_HTTP_BODY_CHUNK_BYTES,
                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  client->commands = xQueueCreate(POCKETJS_NET_ESP_HTTP_COMMAND_QUEUE_LENGTH,
                                  sizeof(command_t));
  client->events = xQueueCreate(POCKETJS_NET_ESP_HTTP_EVENT_QUEUE_LENGTH,
                                sizeof(pocketjs_net_esp_http_event_t));
  client->free_leases =
      xQueueCreate(POCKETJS_NET_ESP_HTTP_BUFFER_LEASES, sizeof(uint8_t));
  client->mutex = xSemaphoreCreateMutex();
  client->stopped = xSemaphoreCreateBinary();
  if (client->lease_storage == NULL || client->commands == NULL ||
      client->events == NULL || client->free_leases == NULL ||
      client->mutex == NULL || client->stopped == NULL) {
    goto allocation_failed;
  }

  for (uint8_t index = 0; index < POCKETJS_NET_ESP_HTTP_BUFFER_LEASES;
       ++index) {
    if (xQueueSend(client->free_leases, &index, 0) != pdTRUE) {
      goto allocation_failed;
    }
  }
  client->state = CLIENT_STATE_IDLE;
  client->wake = config == NULL ? NULL : config->wake;
  client->wake_context = config == NULL ? NULL : config->wake_context;
  client->allow_https = config != NULL && config->allow_https;
  client->wall_clock_trusted =
      config == NULL ? NULL : config->wall_clock_trusted;
  client->wall_clock_context =
      config == NULL ? NULL : config->wall_clock_context;
  atomic_init(&client->cancel_requested, false);
  atomic_init(&client->closing, false);
  atomic_init(&client->active_operation_id, 0);

  const char *task_name = config != NULL && config->worker_task_name != NULL
                              ? config->worker_task_name
                              : "pocketjs-net-http";
  BaseType_t created =
      core == tskNO_AFFINITY
          ? xTaskCreate(http_worker, task_name, stack_bytes, client, priority,
                        &client->worker)
          : xTaskCreatePinnedToCore(http_worker, task_name, stack_bytes, client,
                                    priority, &client->worker, core);
  if (created != pdPASS) {
    goto allocation_failed;
  }

  *out_client = client;
  return ESP_OK;

allocation_failed:
  if (client->stopped != NULL) {
    vSemaphoreDelete(client->stopped);
  }
  if (client->mutex != NULL) {
    vSemaphoreDelete(client->mutex);
  }
  if (client->free_leases != NULL) {
    vQueueDelete(client->free_leases);
  }
  if (client->events != NULL) {
    vQueueDelete(client->events);
  }
  if (client->commands != NULL) {
    vQueueDelete(client->commands);
  }
  heap_caps_free(client->lease_storage);
  heap_caps_free(client);
  return ESP_ERR_NO_MEM;
}

esp_err_t pocketjs_net_esp_http_client_start(
    pocketjs_net_esp_http_client_t *client,
    const pocketjs_net_esp_http_request_t *request) {
  if (client == NULL || request == NULL || request->operation_id == 0 ||
      request->url == NULL || request->method == NULL ||
      (request->header_count > 0 && request->headers == NULL) ||
      (request->body_length > 0 && request->body == NULL)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (atomic_load_explicit(&client->closing, memory_order_acquire)) {
    return ESP_ERR_INVALID_STATE;
  }

  size_t url_length =
      bounded_string_length(request->url, POCKETJS_NET_ESP_HTTP_MAX_URL_BYTES);
  size_t method_length = bounded_string_length(
      request->method, POCKETJS_NET_ESP_HTTP_MAX_METHOD_BYTES);
  if (url_length == 0 || url_length > POCKETJS_NET_ESP_HTTP_MAX_URL_BYTES ||
      method_length == 0 ||
      method_length > POCKETJS_NET_ESP_HTTP_MAX_METHOD_BYTES ||
      request->header_count > POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADERS ||
      request->body_length > POCKETJS_NET_ESP_HTTP_MAX_REQUEST_BODY_BYTES ||
      request->max_response_body_bytes >
          POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_BODY_BYTES ||
      request->io_timeout_ms > POCKETJS_NET_ESP_HTTP_MAX_IO_TIMEOUT_MS) {
    return ESP_ERR_INVALID_SIZE;
  }

  esp_http_client_method_t method_code = HTTP_METHOD_GET;
  if (!is_http_token(request->method, method_length) ||
      !map_method(request->method, method_length, &method_code)) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  if ((method_code == HTTP_METHOD_GET || method_code == HTTP_METHOD_HEAD) &&
      request->body_length > 0) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  bool https = false;
  esp_err_t result = validate_url(request->url, url_length, &https);
  if (result != ESP_OK) {
    return result;
  }
  if (https && (!POCKETJS_NET_ESP_HTTP_INTERNAL_TLS || !client->allow_https ||
                client->wall_clock_trusted == NULL ||
                !client->wall_clock_trusted(client->wall_clock_context))) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  size_t request_header_bytes = 0;
  size_t header_name_lengths[POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADERS] = {0};
  size_t header_value_lengths[POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADERS] = {0};
  for (size_t index = 0; index < request->header_count; ++index) {
    const pocketjs_net_http_header_t *header = &request->headers[index];
    size_t name_length = bounded_string_length(
        header->name, POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_NAME_BYTES);
    size_t value_length = bounded_string_length(
        header->value, POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_VALUE_BYTES);
    if (name_length == 0 ||
        name_length > POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_NAME_BYTES ||
        value_length > POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_VALUE_BYTES ||
        !is_http_token(header->name, name_length) ||
        has_header_value_control(header->value, value_length) ||
        is_backend_owned_request_header(header->name, name_length)) {
      return ESP_ERR_INVALID_ARG;
    }
    size_t framed_bytes = name_length + value_length + 4U;
    if (framed_bytes >
        POCKETJS_NET_ESP_HTTP_MAX_REQUEST_HEADER_BYTES - request_header_bytes) {
      return ESP_ERR_INVALID_SIZE;
    }
    request_header_bytes += framed_bytes;
    header_name_lengths[index] = name_length;
    header_value_lengths[index] = value_length;
  }

  /* Taking with zero ticks makes start a bounded snapshot operation. The
   * caller can retry if the owner races a short state/lease bookkeeping step.
   */
  if (xSemaphoreTake(client->mutex, 0) != pdTRUE) {
    return ESP_ERR_INVALID_STATE;
  }
  if (client->state != CLIENT_STATE_IDLE ||
      atomic_load_explicit(&client->closing, memory_order_acquire)) {
    xSemaphoreGive(client->mutex);
    return ESP_ERR_INVALID_STATE;
  }
  if (request->operation_id <= client->last_operation_id) {
    xSemaphoreGive(client->mutex);
    return ESP_ERR_INVALID_ARG;
  }

  request_snapshot_t *snapshot = &client->request;
  snapshot->operation_id = request->operation_id;
  memcpy(snapshot->url, request->url, url_length + 1U);
  memcpy(snapshot->method, request->method, method_length + 1U);
  snapshot->method_code = method_code;
  snapshot->header_count = request->header_count;
  for (size_t index = 0; index < request->header_count; ++index) {
    memcpy(snapshot->headers[index].name, request->headers[index].name,
           header_name_lengths[index] + 1U);
    memcpy(snapshot->headers[index].value, request->headers[index].value,
           header_value_lengths[index] + 1U);
  }
  if (request->body_length > 0) {
    memcpy(snapshot->body, request->body, request->body_length);
  }
  snapshot->body_length = request->body_length;
  snapshot->io_timeout_ms = request->io_timeout_ms == 0
                                ? POCKETJS_NET_ESP_HTTP_DEFAULT_IO_TIMEOUT_MS
                                : request->io_timeout_ms;
  snapshot->max_response_body_bytes =
      request->max_response_body_bytes == 0
          ? POCKETJS_NET_ESP_HTTP_MAX_RESPONSE_BODY_BYTES
          : request->max_response_body_bytes;
  snapshot->https = https;

  command_t command = COMMAND_START;
  if (xQueueSend(client->commands, &command, 0) != pdTRUE) {
    xSemaphoreGive(client->mutex);
    return ESP_ERR_NO_MEM;
  }

  client->state = CLIENT_STATE_QUEUED;
  client->last_operation_id = request->operation_id;
  atomic_store_explicit(&client->active_operation_id, request->operation_id,
                        memory_order_release);
  atomic_store_explicit(&client->cancel_requested, false, memory_order_release);
  ++client->stats.submitted_requests;
  xSemaphoreGive(client->mutex);
  return ESP_OK;
}

esp_err_t
pocketjs_net_esp_http_client_cancel(pocketjs_net_esp_http_client_t *client,
                                    uint32_t operation_id) {
  if (client == NULL || operation_id == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  /* Linearize operation identity and cancellation publication with terminal
   * receipt and the next start. This prevents a delayed cancel for an old
   * operation from setting the flag after a new operation has started. */
  if (xSemaphoreTake(client->mutex, owner_lock_timeout_ticks()) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  bool cancellable = client->state == CLIENT_STATE_QUEUED ||
                     client->state == CLIENT_STATE_RUNNING;
  if (!cancellable ||
      atomic_load_explicit(&client->active_operation_id,
                           memory_order_acquire) != operation_id) {
    xSemaphoreGive(client->mutex);
    return ESP_ERR_NOT_FOUND;
  }
  atomic_store_explicit(&client->cancel_requested, true, memory_order_release);
  xSemaphoreGive(client->mutex);
  return ESP_OK;
}

esp_err_t
pocketjs_net_esp_http_client_receive(pocketjs_net_esp_http_client_t *client,
                                     pocketjs_net_esp_http_event_t *out_event,
                                     TickType_t wait_ticks) {
  if (client == NULL || out_event == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(out_event, 0, sizeof(*out_event));
  if (xQueueReceive(client->events, out_event, wait_ticks) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }

  if (out_event->type == POCKETJS_NET_ESP_HTTP_EVENT_COMPLETE ||
      out_event->type == POCKETJS_NET_ESP_HTTP_EVENT_ERROR) {
    if (xSemaphoreTake(client->mutex, portMAX_DELAY) == pdTRUE) {
      if (client->state == CLIENT_STATE_TERMINAL_QUEUED &&
          atomic_load_explicit(&client->active_operation_id,
                               memory_order_acquire) ==
              out_event->operation_id) {
        client->state = CLIENT_STATE_IDLE;
        atomic_store_explicit(&client->active_operation_id, 0,
                              memory_order_release);
        atomic_store_explicit(&client->cancel_requested, false,
                              memory_order_release);
      }
      xSemaphoreGive(client->mutex);
    }
  }
  return ESP_OK;
}

esp_err_t pocketjs_net_esp_http_client_release_event(
    pocketjs_net_esp_http_client_t *client,
    pocketjs_net_esp_http_event_t *event) {
  if (client == NULL || event == NULL || event->lease_id == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t result = release_lease(client, event->lease_id,
                                   event->lease_generation, event->payload);
  if (result == ESP_OK) {
    event->payload = NULL;
    event->payload_length = 0;
    event->lease_id = 0;
    event->lease_generation = 0;
  }
  return result;
}

void pocketjs_net_esp_http_client_get_stats(
    pocketjs_net_esp_http_client_t *client,
    pocketjs_net_esp_http_stats_t *out_stats) {
  if (client == NULL || out_stats == NULL) {
    return;
  }
  if (xSemaphoreTake(client->mutex, portMAX_DELAY) == pdTRUE) {
    *out_stats = client->stats;
    xSemaphoreGive(client->mutex);
  }
}

void pocketjs_net_esp_http_client_destroy(
    pocketjs_net_esp_http_client_t *client) {
  if (client == NULL) {
    return;
  }

  atomic_store_explicit(&client->closing, true, memory_order_release);
  atomic_store_explicit(&client->cancel_requested, true, memory_order_release);
  command_t command = COMMAND_SHUTDOWN;
  (void)xQueueSend(client->commands, &command, portMAX_DELAY);
  (void)xSemaphoreTake(client->stopped, portMAX_DELAY);

  vSemaphoreDelete(client->stopped);
  vSemaphoreDelete(client->mutex);
  vQueueDelete(client->free_leases);
  vQueueDelete(client->events);
  vQueueDelete(client->commands);
  heap_caps_free(client->lease_storage);
  heap_caps_free(client);
}

const char *pocketjs_net_error_code_name(pocketjs_net_error_code_t code) {
  switch (code) {
  case POCKETJS_NET_ERROR_NONE:
    return "none";
  case POCKETJS_NET_ERROR_ABORTED:
    return "aborted";
  case POCKETJS_NET_ERROR_TIMED_OUT:
    return "timed_out";
  case POCKETJS_NET_ERROR_BUSY:
    return "busy";
  case POCKETJS_NET_ERROR_RESOURCE_LIMIT:
    return "resource_limit";
  case POCKETJS_NET_ERROR_UNSUPPORTED:
    return "unsupported";
  case POCKETJS_NET_ERROR_DNS_NOT_FOUND:
    return "dns_not_found";
  case POCKETJS_NET_ERROR_CONNECTION_REFUSED:
    return "connection_refused";
  case POCKETJS_NET_ERROR_CONNECTION_RESET:
    return "connection_reset";
  case POCKETJS_NET_ERROR_NETWORK_UNREACHABLE:
    return "network_unreachable";
  case POCKETJS_NET_ERROR_TLS_CERTIFICATE_INVALID:
    return "tls_certificate_invalid";
  case POCKETJS_NET_ERROR_TLS_HOSTNAME_MISMATCH:
    return "tls_hostname_mismatch";
  case POCKETJS_NET_ERROR_TLS_HANDSHAKE_FAILED:
    return "tls_handshake_failed";
  case POCKETJS_NET_ERROR_HTTP_PROTOCOL_ERROR:
    return "http_protocol_error";
  case POCKETJS_NET_ERROR_SYSTEM:
    return "system";
  default:
    return "unknown";
  }
}
