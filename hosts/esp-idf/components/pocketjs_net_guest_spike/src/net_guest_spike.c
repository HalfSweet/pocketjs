// SPDX-License-Identifier: MIT

#include "pocketjs/net/guest_spike.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "pocketjs/esp_guest.h"
#include "quickjs.h"

#define SPIKE_WAKE_BIT BIT0
#define SPIKE_MAX_EVENTS_PER_TURN 32U
#define SPIKE_MAX_JOBS_PER_TURN 64U
#define SPIKE_FRAME_INTERVAL_US 16667U
#define SPIKE_MINIMUM_FRAMES 2U
#define SPIKE_PRIVATE_ABI_VERSION 1U

static const char *TAG = "pocketjs_net_js";

static const char SPIKE_BUNDLE[] =
    "(binding => {\n"
    "  'use strict';\n"
    "  if (!Object.isFrozen(binding)) {\n"
    "    throw new Error('private binding is not frozen');\n"
    "  }\n"
    "  if ('net' in globalThis || '__pocketNetworkBindingV1' in globalThis) {\n"
    "    throw new Error('network binding leaked onto globalThis');\n"
    "  }\n"
    "  if (binding.abiVersion !== 1) {\n"
    "    throw new Error('private network ABI version mismatch');\n"
    "  }\n"
    "  const net = Object.freeze({\n"
    "    http: Object.freeze({ requestProbe: binding.requestProbe }),\n"
    "  });\n"
    "  const report = binding.report;\n"
    "  const deviceId = binding.deviceId;\n"
    "  const rounds = binding.rounds;\n"
    "  let randomState = 0x6d2b79f5;\n"
    "  let frameCount = 0;\n"
    "  globalThis.frame = () => {\n"
    "    frameCount += 1;\n"
    "    binding.observeFrame(frameCount);\n"
    "  };\n"
    "  const nextNonce = () => {\n"
    "    randomState ^= randomState << 13;\n"
    "    randomState ^= randomState >>> 17;\n"
    "    randomState ^= randomState << 5;\n"
    "    return ('00000000' + (randomState >>> 0).toString(16)).slice(-8);\n"
    "  };\n"
    "  return (async () => {\n"
    "    for (let round = 1; round <= rounds; round += 1) {\n"
    "      const health = await net.http.requestProbe('GET', '/health', '');\n"
    "      if (health.status !== 200 ||\n"
    "          !health.body.includes('\\\"status\\\":\\\"ok\\\"')) {\n"
    "        throw new Error(`health mismatch at round ${round}`);\n"
    "      }\n"
    "      const payload =\n"
    "        `pocketjs:${deviceId}:round:${round}:nonce:${nextNonce()}`;\n"
    "      const echo = await net.http.requestProbe('POST', '/echo', "
    "payload);\n"
    "      if (echo.status !== 200 || echo.body !== payload) {\n"
    "        throw new Error(`echo mismatch at round ${round}`);\n"
    "      }\n"
    "    }\n"
    "    report(true, `completed ${rounds} rounds`, rounds);\n"
    "  })().catch(error => {\n"
    "    const detail = error && error.stack ? error.stack : String(error);\n"
    "    report(false, detail, 0);\n"
    "  });\n"
    "})";

typedef struct {
  pocketjs_net_guest_spike_config_t config;
  pocketjs_net_guest_spike_result_t result;
  pocketjs_esp_guest_t *guest;
  JSContext *context;
  pocketjs_net_esp_http_client_t *client;
  EventGroupHandle_t wake_events;
  JSValue resolve;
  JSValue reject;
  bool promise_pending;
  bool report_received;
  bool native_failure;
  bool terminal_received;
  uint32_t active_operation_id;
  uint32_t next_operation_id;
  uint64_t last_event_sequence;
  int response_status;
  size_t response_body_length;
  size_t response_body_capacity;
  uint8_t response_body[POCKETJS_NET_GUEST_SPIKE_MAX_RESPONSE_BODY_BYTES + 1U];
  char base_url[POCKETJS_NET_ESP_HTTP_MAX_URL_BYTES + 1U];
  char device_id[POCKETJS_NET_GUEST_SPIKE_MAX_DEVICE_ID_BYTES + 1U];
} spike_runner_t;

static uint64_t fnv1a64(const char *data, size_t length) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t index = 0; index < length; ++index) {
    hash ^= (uint8_t)data[index];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static size_t bounded_length(const char *value, size_t maximum) {
  if (value == NULL) {
    return maximum + 1U;
  }
  size_t length = strnlen(value, maximum + 1U);
  return length;
}

static void copy_detail(spike_runner_t *runner, const char *detail) {
  if (detail == NULL) {
    runner->result.detail[0] = '\0';
    return;
  }
  size_t length = strnlen(detail, sizeof(runner->result.detail) - 1U);
  memcpy(runner->result.detail, detail, length);
  runner->result.detail[length] = '\0';
}

static esp_err_t
normalize_config(const pocketjs_net_guest_spike_config_t *input,
                 spike_runner_t *runner) {
  if (input == NULL || input->base_url == NULL || input->device_id == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const size_t raw_base_length =
      bounded_length(input->base_url, POCKETJS_NET_ESP_HTTP_MAX_URL_BYTES);
  if (raw_base_length < sizeof("http://x") - 1U ||
      raw_base_length > POCKETJS_NET_ESP_HTTP_MAX_URL_BYTES ||
      memcmp(input->base_url, "http://", sizeof("http://") - 1U) != 0 ||
      strchr(input->base_url + sizeof("http://") - 1U, '?') != NULL ||
      strchr(input->base_url + sizeof("http://") - 1U, '#') != NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  size_t base_length = raw_base_length;
  while (base_length > sizeof("http://") - 1U &&
         input->base_url[base_length - 1U] == '/') {
    --base_length;
  }
  if (base_length <= sizeof("http://") - 1U ||
      base_length + sizeof("/health") > sizeof(runner->base_url)) {
    return ESP_ERR_INVALID_ARG;
  }
  memcpy(runner->base_url, input->base_url, base_length);
  runner->base_url[base_length] = '\0';

  const size_t device_length = bounded_length(
      input->device_id, POCKETJS_NET_GUEST_SPIKE_MAX_DEVICE_ID_BYTES);
  if (device_length == 0 ||
      device_length > POCKETJS_NET_GUEST_SPIKE_MAX_DEVICE_ID_BYTES) {
    return ESP_ERR_INVALID_ARG;
  }
  for (size_t index = 0; index < device_length; ++index) {
    const unsigned char byte = (unsigned char)input->device_id[index];
    if (byte < 0x20U || byte == 0x7fU) {
      return ESP_ERR_INVALID_ARG;
    }
  }
  memcpy(runner->device_id, input->device_id, device_length + 1U);

  runner->config = *input;
  runner->config.base_url = runner->base_url;
  runner->config.device_id = runner->device_id;
  runner->config.rounds = input->rounds == 0
                              ? POCKETJS_NET_GUEST_SPIKE_DEFAULT_ROUNDS
                              : input->rounds;
  runner->config.request_timeout_ms =
      input->request_timeout_ms == 0
          ? POCKETJS_NET_GUEST_SPIKE_DEFAULT_REQUEST_TIMEOUT_MS
          : input->request_timeout_ms;
  runner->config.total_timeout_ms =
      input->total_timeout_ms == 0
          ? POCKETJS_NET_GUEST_SPIKE_DEFAULT_TOTAL_TIMEOUT_MS
          : input->total_timeout_ms;
  runner->config.max_response_body_bytes =
      input->max_response_body_bytes == 0
          ? POCKETJS_NET_GUEST_SPIKE_DEFAULT_RESPONSE_BODY_BYTES
          : input->max_response_body_bytes;
  runner->config.guest_memory_limit_bytes =
      input->guest_memory_limit_bytes == 0
          ? POCKETJS_NET_GUEST_SPIKE_DEFAULT_GUEST_MEMORY_BYTES
          : input->guest_memory_limit_bytes;
  runner->config.guest_stack_limit_bytes =
      input->guest_stack_limit_bytes == 0
          ? POCKETJS_NET_GUEST_SPIKE_DEFAULT_GUEST_STACK_BYTES
          : input->guest_stack_limit_bytes;
  runner->config.guest_execution_timeout_us =
      input->guest_execution_timeout_us == 0
          ? POCKETJS_NET_GUEST_SPIKE_DEFAULT_EXECUTION_TIMEOUT_US
          : input->guest_execution_timeout_us;
  runner->config.guest_max_interrupt_checks =
      input->guest_max_interrupt_checks == 0
          ? POCKETJS_NET_GUEST_SPIKE_DEFAULT_INTERRUPT_CHECKS
          : input->guest_max_interrupt_checks;

  if (runner->config.rounds > POCKETJS_NET_GUEST_SPIKE_MAX_ROUNDS ||
      runner->config.request_timeout_ms >
          POCKETJS_NET_ESP_HTTP_MAX_IO_TIMEOUT_MS ||
      runner->config.total_timeout_ms >
          POCKETJS_NET_GUEST_SPIKE_MAX_TOTAL_TIMEOUT_MS ||
      runner->config.max_response_body_bytes == 0 ||
      runner->config.max_response_body_bytes >
          POCKETJS_NET_GUEST_SPIKE_MAX_RESPONSE_BODY_BYTES ||
      runner->config.worker_priority >= (UBaseType_t)configMAX_PRIORITIES) {
    return ESP_ERR_INVALID_ARG;
  }

  runner->response_body_capacity = runner->config.max_response_body_bytes;
  runner->result.requested_rounds = runner->config.rounds;
  runner->result.artifact_fnv1a64 =
      fnv1a64(SPIKE_BUNDLE, sizeof(SPIKE_BUNDLE) - 1U);
  runner->next_operation_id = 1U;
  runner->resolve = JS_UNDEFINED;
  runner->reject = JS_UNDEFINED;
  return ESP_OK;
}

static void wake_owner(void *context) {
  spike_runner_t *runner = context;
  if (runner != NULL && runner->wake_events != NULL) {
    (void)xEventGroupSetBits(runner->wake_events, SPIKE_WAKE_BIT);
  }
}

static JSValue new_error(JSContext *context, const char *message,
                         const char *code) {
  JSValue error = JS_NewError(context);
  if (JS_IsException(error)) {
    return error;
  }
  if (JS_SetPropertyStr(context, error, "message",
                        JS_NewString(context, message)) < 0 ||
      JS_SetPropertyStr(context, error, "code", JS_NewString(context, code)) <
          0) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  return error;
}

static bool call_settler(spike_runner_t *runner, JSValueConst function,
                         JSValueConst value, const char *phase) {
  JSValue call_result =
      JS_Call(runner->context, function, JS_UNDEFINED, 1, &value);
  if (JS_IsException(call_result)) {
    pocketjs_esp_guest_log_exception(runner->guest, phase);
    runner->native_failure = true;
    JS_FreeValue(runner->context, call_result);
    return false;
  }
  JS_FreeValue(runner->context, call_result);
  return true;
}

static void clear_promise(spike_runner_t *runner) {
  JS_FreeValue(runner->context, runner->resolve);
  JS_FreeValue(runner->context, runner->reject);
  runner->resolve = JS_UNDEFINED;
  runner->reject = JS_UNDEFINED;
  runner->promise_pending = false;
  runner->active_operation_id = 0;
}

static bool reject_pending(spike_runner_t *runner, const char *message,
                           const char *code) {
  if (!runner->promise_pending) {
    return false;
  }
  JSValue error = new_error(runner->context, message, code);
  if (JS_IsException(error)) {
    pocketjs_esp_guest_log_exception(runner->guest, "create_rejection");
    runner->native_failure = true;
    clear_promise(runner);
    return false;
  }
  const bool settled =
      call_settler(runner, runner->reject, error, "reject_request");
  JS_FreeValue(runner->context, error);
  clear_promise(runner);
  return settled;
}

static JSValue js_request(JSContext *context, JSValueConst this_value,
                          int argument_count, JSValueConst *arguments) {
  (void)this_value;
  spike_runner_t *runner = JS_GetContextOpaque(context);
  if (runner == NULL || runner->native_failure || runner->report_received) {
    return JS_ThrowInternalError(context, "network spike runner is not active");
  }
  if (runner->promise_pending) {
    return JS_ThrowInternalError(context, "only one request may be in flight");
  }
  if (argument_count != 3) {
    return JS_ThrowTypeError(context, "request requires method, path and body");
  }

  size_t method_length = 0;
  size_t path_length = 0;
  size_t body_length = 0;
  const char *method = JS_ToCStringLen(context, &method_length, arguments[0]);
  const char *path = JS_ToCStringLen(context, &path_length, arguments[1]);
  const char *body = JS_ToCStringLen(context, &body_length, arguments[2]);
  if (method == NULL || path == NULL || body == NULL) {
    if (method != NULL) {
      JS_FreeCString(context, method);
    }
    if (path != NULL) {
      JS_FreeCString(context, path);
    }
    if (body != NULL) {
      JS_FreeCString(context, body);
    }
    return JS_EXCEPTION;
  }

  const bool health_request =
      method_length == 3 && memcmp(method, "GET", 3) == 0 && path_length == 7 &&
      memcmp(path, "/health", 7) == 0 && body_length == 0;
  const bool echo_request =
      method_length == 4 && memcmp(method, "POST", 4) == 0 &&
      path_length == 5 && memcmp(path, "/echo", 5) == 0 && body_length > 0 &&
      body_length <= POCKETJS_NET_ESP_HTTP_MAX_REQUEST_BODY_BYTES;
  if (!health_request && !echo_request) {
    JS_FreeCString(context, method);
    JS_FreeCString(context, path);
    JS_FreeCString(context, body);
    return JS_ThrowTypeError(context, "request is outside the smoke allowlist");
  }

  char url[POCKETJS_NET_ESP_HTTP_MAX_URL_BYTES + 1U];
  const size_t base_length = strlen(runner->base_url);
  if (path_length > sizeof(url) - base_length - 1U) {
    JS_FreeCString(context, method);
    JS_FreeCString(context, path);
    JS_FreeCString(context, body);
    return JS_ThrowRangeError(context, "request URL exceeds the fixed limit");
  }
  memcpy(url, runner->base_url, base_length);
  memcpy(url + base_length, path, path_length);
  url[base_length + path_length] = '\0';

  JSValue settling_functions[2] = {JS_UNDEFINED, JS_UNDEFINED};
  JSValue promise = JS_NewPromiseCapability(context, settling_functions);
  if (JS_IsException(promise)) {
    JS_FreeCString(context, method);
    JS_FreeCString(context, path);
    JS_FreeCString(context, body);
    return promise;
  }

  runner->resolve = settling_functions[0];
  runner->reject = settling_functions[1];
  runner->promise_pending = true;
  runner->active_operation_id = runner->next_operation_id;
  runner->terminal_received = false;
  runner->response_status = 0;
  runner->response_body_length = 0;

  const pocketjs_net_http_header_t echo_headers[] = {
      {.name = "Content-Type", .value = "application/octet-stream"},
  };
  const pocketjs_net_esp_http_request_t request = {
      .operation_id = runner->active_operation_id,
      .url = url,
      .method = method,
      .headers = echo_request ? echo_headers : NULL,
      .header_count = echo_request ? 1U : 0U,
      .body = echo_request ? body : NULL,
      .body_length = echo_request ? body_length : 0U,
      .io_timeout_ms = runner->config.request_timeout_ms,
      .max_response_body_bytes = runner->response_body_capacity,
  };
  const esp_err_t started =
      pocketjs_net_esp_http_client_start(runner->client, &request);

  JS_FreeCString(context, method);
  JS_FreeCString(context, path);
  JS_FreeCString(context, body);

  if (started != ESP_OK) {
    char message[96];
    (void)snprintf(message, sizeof(message),
                   "native request admission failed: %s",
                   esp_err_to_name(started));
    (void)reject_pending(runner, message, "system_error");
    return promise;
  }

  ++runner->result.operations_started;
  ++runner->next_operation_id;
  return promise;
}

static JSValue js_report(JSContext *context, JSValueConst this_value,
                         int argument_count, JSValueConst *arguments) {
  (void)this_value;
  spike_runner_t *runner = JS_GetContextOpaque(context);
  if (runner == NULL || argument_count != 3) {
    return JS_ThrowTypeError(context,
                             "report requires pass, detail and rounds");
  }
  if (runner->report_received) {
    return JS_ThrowInternalError(context, "result was already reported");
  }

  const int passed = JS_ToBool(context, arguments[0]);
  const char *detail = JS_ToCString(context, arguments[1]);
  uint32_t completed_rounds = 0;
  if (passed < 0 || detail == NULL ||
      JS_ToUint32(context, &completed_rounds, arguments[2]) < 0) {
    if (detail != NULL) {
      JS_FreeCString(context, detail);
    }
    return JS_EXCEPTION;
  }

  runner->report_received = true;
  runner->result.passed = passed != 0;
  runner->result.completed_rounds = completed_rounds;
  copy_detail(runner, detail);
  JS_FreeCString(context, detail);
  return JS_UNDEFINED;
}

static JSValue js_observe_frame(JSContext *context, JSValueConst this_value,
                                int argument_count, JSValueConst *arguments) {
  (void)this_value;
  spike_runner_t *runner = JS_GetContextOpaque(context);
  uint32_t frame_count = 0;
  if (runner == NULL || argument_count != 1 ||
      JS_ToUint32(context, &frame_count, arguments[0]) < 0) {
    return JS_EXCEPTION;
  }
  if (frame_count != runner->result.frame_count + 1U) {
    return JS_ThrowInternalError(context,
                                 "frame counter lost monotonic ordering");
  }
  runner->result.frame_count = frame_count;
  return JS_UNDEFINED;
}

static bool set_binding_property(JSContext *context, JSValue binding,
                                 const char *name, JSValue value) {
  if (JS_IsException(value)) {
    return false;
  }
  return JS_SetPropertyStr(context, binding, name, value) >= 0;
}

static esp_err_t mount_bundle(spike_runner_t *runner) {
  JSValue binding = JS_NewObject(runner->context);
  if (JS_IsException(binding)) {
    pocketjs_esp_guest_log_exception(runner->guest, "create_private_binding");
    return ESP_ERR_NO_MEM;
  }

  const bool complete =
      set_binding_property(
          runner->context, binding, "abiVersion",
          JS_NewUint32(runner->context, SPIKE_PRIVATE_ABI_VERSION)) &&
      set_binding_property(
          runner->context, binding, "requestProbe",
          JS_NewCFunction(runner->context, js_request, "requestProbe", 3)) &&
      set_binding_property(
          runner->context, binding, "report",
          JS_NewCFunction(runner->context, js_report, "report", 3)) &&
      set_binding_property(runner->context, binding, "observeFrame",
                           JS_NewCFunction(runner->context, js_observe_frame,
                                           "observeFrame", 1)) &&
      set_binding_property(runner->context, binding, "deviceId",
                           JS_NewString(runner->context, runner->device_id)) &&
      set_binding_property(
          runner->context, binding, "rounds",
          JS_NewUint32(runner->context, runner->config.rounds));
  if (!complete) {
    pocketjs_esp_guest_log_exception(runner->guest, "populate_private_binding");
    JS_FreeValue(runner->context, binding);
    return ESP_FAIL;
  }

  JSValue factory_result = JS_UNDEFINED;
  const esp_err_t mounted = pocketjs_esp_guest_mount_factory(
      runner->guest, "pocketjs-net-guest-spike.js", SPIKE_BUNDLE,
      sizeof(SPIKE_BUNDLE) - 1U, binding, &factory_result);
  JS_FreeValue(runner->context, binding);
  if (mounted != ESP_OK) {
    return mounted;
  }
  if (!JS_IsPromise(factory_result)) {
    JS_FreeValue(runner->context, factory_result);
    copy_detail(runner, "Guest factory did not return a Promise");
    return ESP_ERR_INVALID_RESPONSE;
  }
  JS_FreeValue(runner->context, factory_result);
  return ESP_OK;
}

static esp_err_t release_payload(spike_runner_t *runner,
                                 pocketjs_net_esp_http_event_t *event) {
  if (event->lease_id == 0) {
    return ESP_OK;
  }
  return pocketjs_net_esp_http_client_release_event(runner->client, event);
}

static esp_err_t consume_event(spike_runner_t *runner,
                               pocketjs_net_esp_http_event_t *event) {
  const bool terminal = event->type == POCKETJS_NET_ESP_HTTP_EVENT_COMPLETE ||
                        event->type == POCKETJS_NET_ESP_HTTP_EVENT_ERROR;
  if (terminal) {
    runner->terminal_received = true;
    if (event->lease_id != 0 || event->lease_generation != 0 ||
        event->payload != NULL || event->payload_length != 0) {
      (void)release_payload(runner, event);
      copy_detail(runner, "terminal HTTP event carried a payload lease");
      return ESP_ERR_INVALID_STATE;
    }
  }
  if (event->sequence <= runner->last_event_sequence) {
    (void)release_payload(runner, event);
    copy_detail(runner, "HTTP backend event sequence was not monotonic");
    return ESP_ERR_INVALID_STATE;
  }
  runner->last_event_sequence = event->sequence;

  if (!runner->promise_pending ||
      event->operation_id != runner->active_operation_id) {
    (void)release_payload(runner, event);
    copy_detail(runner,
                "HTTP event operation id did not match the Guest Promise");
    return ESP_ERR_INVALID_STATE;
  }

  switch (event->type) {
  case POCKETJS_NET_ESP_HTTP_EVENT_RESPONSE_HEADER:
    if (event->lease_id == 0 || event->lease_generation == 0 ||
        event->payload == NULL) {
      copy_detail(runner, "response header did not carry a payload lease");
      return ESP_ERR_INVALID_STATE;
    }
    return release_payload(runner, event);

  case POCKETJS_NET_ESP_HTTP_EVENT_RESPONSE_HEADERS_COMPLETE:
    if (event->lease_id != 0 || event->lease_generation != 0 ||
        event->payload != NULL || event->payload_length != 0) {
      (void)release_payload(runner, event);
      copy_detail(runner, "headers-complete event carried a payload lease");
      return ESP_ERR_INVALID_STATE;
    }
    if (runner->response_status != 0) {
      copy_detail(runner, "duplicate response headers-complete event");
      return ESP_ERR_INVALID_STATE;
    }
    runner->response_status = event->detail.response.status;
    return ESP_OK;

  case POCKETJS_NET_ESP_HTTP_EVENT_RESPONSE_BODY:
    if (event->lease_id == 0 || event->lease_generation == 0 ||
        event->payload == NULL ||
        runner->response_body_length > runner->response_body_capacity ||
        event->payload_length >
            runner->response_body_capacity - runner->response_body_length) {
      (void)release_payload(runner, event);
      copy_detail(runner, "response exceeded the Guest Binding body limit");
      return ESP_ERR_INVALID_SIZE;
    }
    memcpy(runner->response_body + runner->response_body_length, event->payload,
           event->payload_length);
    runner->response_body_length += event->payload_length;
    if (runner->response_body_length >
        runner->result.response_body_high_water_bytes) {
      runner->result.response_body_high_water_bytes =
          runner->response_body_length;
    }
    return release_payload(runner, event);

  case POCKETJS_NET_ESP_HTTP_EVENT_COMPLETE: {
    if (runner->response_status == 0) {
      copy_detail(runner, "request completed without response metadata");
      return ESP_ERR_INVALID_RESPONSE;
    }
    runner->response_body[runner->response_body_length] = '\0';
    JSValue response = JS_NewObject(runner->context);
    if (JS_IsException(response) ||
        JS_SetPropertyStr(
            runner->context, response, "status",
            JS_NewInt32(runner->context, runner->response_status)) < 0 ||
        JS_SetPropertyStr(runner->context, response, "body",
                          JS_NewStringLen(runner->context,
                                          (const char *)runner->response_body,
                                          runner->response_body_length)) < 0) {
      if (!JS_IsException(response)) {
        JS_FreeValue(runner->context, response);
      }
      pocketjs_esp_guest_log_exception(runner->guest, "create_response");
      return ESP_ERR_NO_MEM;
    }

    ++runner->result.operations_completed;
    const bool settled =
        call_settler(runner, runner->resolve, response, "resolve_request");
    JS_FreeValue(runner->context, response);
    clear_promise(runner);
    return settled ? ESP_OK : ESP_FAIL;
  }

  case POCKETJS_NET_ESP_HTTP_EVENT_ERROR: {
    char message[144];
    const char *code = pocketjs_net_error_code_name(event->detail.error.code);
    (void)snprintf(message, sizeof(message),
                   "native HTTP operation failed: %s (cause=%s)", code,
                   esp_err_to_name(event->detail.error.cause_code));
    (void)reject_pending(runner, message, code);
    return runner->native_failure ? ESP_FAIL : ESP_OK;
  }

  default:
    (void)release_payload(runner, event);
    copy_detail(runner, "unknown HTTP backend event type");
    return ESP_ERR_INVALID_RESPONSE;
  }
}

static esp_err_t execute_guest_checkpoint(spike_runner_t *runner) {
  size_t jobs = 0;
  bool jobs_pending = false;
  const esp_err_t executed = pocketjs_esp_guest_execute_jobs(
      runner->guest, SPIKE_MAX_JOBS_PER_TURN, &jobs, &jobs_pending);
  runner->result.guest_jobs_executed += jobs;
  if (executed != ESP_OK) {
    return executed;
  }
  if (jobs_pending) {
    copy_detail(runner, "Guest exceeded the bounded jobs-per-turn budget");
    return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

static esp_err_t service_turn(spike_runner_t *runner) {
  size_t events = 0;
  while (events < SPIKE_MAX_EVENTS_PER_TURN) {
    pocketjs_net_esp_http_event_t event;
    const esp_err_t received =
        pocketjs_net_esp_http_client_receive(runner->client, &event, 0);
    if (received == ESP_ERR_TIMEOUT) {
      break;
    }
    if (received != ESP_OK) {
      return received;
    }
    ++events;
    const esp_err_t consumed = consume_event(runner, &event);
    if (consumed != ESP_OK) {
      return consumed;
    }
  }
  return execute_guest_checkpoint(runner);
}

static uint64_t elapsed_ms_since(int64_t started_us) {
  const int64_t elapsed_us = esp_timer_get_time() - started_us;
  return elapsed_us <= 0 ? 0U : (uint64_t)elapsed_us / 1000U;
}

static TickType_t scheduler_wait_ticks(const spike_runner_t *runner,
                                       int64_t started_us,
                                       int64_t next_frame_us) {
  const int64_t now_us = esp_timer_get_time();
  const int64_t total_deadline_us =
      started_us + (int64_t)runner->config.total_timeout_ms * 1000;
  if (now_us >= total_deadline_us) {
    return 0;
  }
  if (next_frame_us <= now_us) {
    return 1;
  }
  int64_t deadline_us = total_deadline_us;
  if (next_frame_us < deadline_us) {
    deadline_us = next_frame_us;
  }
  const uint64_t remaining_ms = (uint64_t)(deadline_us - now_us + 999) / 1000U;
  TickType_t ticks = pdMS_TO_TICKS((uint32_t)remaining_ms);
  return ticks == 0 ? 1 : ticks;
}

static void drain_cancelled_operation(spike_runner_t *runner) {
  if (!runner->promise_pending || runner->client == NULL) {
    return;
  }

  const uint32_t operation_id = runner->active_operation_id;
  const esp_err_t cancelled =
      pocketjs_net_esp_http_client_cancel(runner->client, operation_id);
  if (cancelled != ESP_OK && cancelled != ESP_ERR_NOT_FOUND) {
    ESP_LOGW(TAG, "cancel failed: %s", esp_err_to_name(cancelled));
  }

  while (!runner->terminal_received) {
    pocketjs_net_esp_http_event_t event;
    const esp_err_t received = pocketjs_net_esp_http_client_receive(
        runner->client, &event, portMAX_DELAY);
    if (received != ESP_OK) {
      break;
    }
    if (event.lease_id != 0) {
      const esp_err_t released = release_payload(runner, &event);
      if (released != ESP_OK) {
        ESP_LOGW(TAG, "cleanup lease release failed: %s",
                 esp_err_to_name(released));
      }
    }
    if (event.type == POCKETJS_NET_ESP_HTTP_EVENT_COMPLETE ||
        event.type == POCKETJS_NET_ESP_HTTP_EVENT_ERROR) {
      runner->terminal_received = true;
    }
  }
}

static void cleanup_runner(spike_runner_t *runner) {
  if (runner->client != NULL) {
    drain_cancelled_operation(runner);
    pocketjs_net_esp_http_client_get_stats(runner->client,
                                           &runner->result.backend);
    pocketjs_net_esp_http_client_destroy(runner->client);
    runner->client = NULL;
  }
  if (runner->context != NULL) {
    (void)pocketjs_esp_guest_get_stats(runner->guest, &runner->result.guest);
    if (runner->promise_pending) {
      clear_promise(runner);
    }
    JS_SetContextOpaque(runner->context, NULL);
    runner->context = NULL;
  }
  if (runner->guest != NULL) {
    pocketjs_esp_guest_destroy(runner->guest);
    runner->guest = NULL;
  }
  if (runner->wake_events != NULL) {
    vEventGroupDelete(runner->wake_events);
    runner->wake_events = NULL;
  }
}

esp_err_t
pocketjs_net_guest_spike_run(const pocketjs_net_guest_spike_config_t *config,
                             pocketjs_net_guest_spike_result_t *out_result) {
  if (out_result == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  memset(out_result, 0, sizeof(*out_result));

  spike_runner_t *runner = calloc(1, sizeof(*runner));
  if (runner == NULL) {
    return ESP_ERR_NO_MEM;
  }

  esp_err_t result = normalize_config(config, runner);
  if (result != ESP_OK) {
    free(runner);
    return result;
  }

  const int64_t started_us = esp_timer_get_time();
  runner->wake_events = xEventGroupCreate();
  if (runner->wake_events == NULL) {
    result = ESP_ERR_NO_MEM;
    goto finish;
  }

  const pocketjs_esp_guest_config_t guest_config = {
      .memory_limit_bytes = runner->config.guest_memory_limit_bytes,
      .stack_limit_bytes = runner->config.guest_stack_limit_bytes,
      .allocate_in_external_memory =
          runner->config.allocate_guest_in_external_memory,
      .execution_timeout_us = runner->config.guest_execution_timeout_us,
      .max_interrupt_checks = runner->config.guest_max_interrupt_checks,
  };
  result = pocketjs_esp_guest_create(&guest_config, &runner->guest);
  if (result != ESP_OK) {
    goto finish;
  }
  runner->context = pocketjs_esp_guest_context(runner->guest);
  if (runner->context == NULL) {
    result = ESP_ERR_INVALID_STATE;
    goto finish;
  }
  JS_SetContextOpaque(runner->context, runner);

  const pocketjs_net_esp_http_client_config_t client_config = {
      .wake = wake_owner,
      .wake_context = runner,
      .allow_https = false,
      .worker_task_name = "pocketjs-net-js-http",
      .worker_stack_bytes = runner->config.worker_stack_bytes,
      .worker_priority = runner->config.worker_priority,
      .worker_core = runner->config.worker_core,
  };
  result = pocketjs_net_esp_http_client_create(&client_config, &runner->client);
  if (result != ESP_OK) {
    goto finish;
  }

  result = mount_bundle(runner);
  if (result != ESP_OK) {
    goto finish;
  }

  int64_t next_frame_us = esp_timer_get_time();
  while ((!runner->report_received ||
          runner->result.frame_count < SPIKE_MINIMUM_FRAMES) &&
         !runner->native_failure) {
    if (elapsed_ms_since(started_us) >= runner->config.total_timeout_ms) {
      copy_detail(runner, "Guest network smoke exceeded its total timeout");
      result = ESP_ERR_TIMEOUT;
      goto finish;
    }

    const int64_t now_us = esp_timer_get_time();
    if (now_us >= next_frame_us) {
      result = pocketjs_esp_guest_call_frame(runner->guest, 0, NULL);
      if (result != ESP_OK) {
        goto finish;
      }
      result = execute_guest_checkpoint(runner);
      if (result != ESP_OK) {
        goto finish;
      }
      next_frame_us += SPIKE_FRAME_INTERVAL_US;
    }

    result = service_turn(runner);
    if (result != ESP_OK) {
      goto finish;
    }
    if ((runner->report_received &&
         runner->result.frame_count >= SPIKE_MINIMUM_FRAMES) ||
        runner->native_failure) {
      break;
    }
    const TickType_t ticks =
        scheduler_wait_ticks(runner, started_us, next_frame_us);
    if (ticks == 0) {
      copy_detail(runner, "Guest network smoke exceeded its total timeout");
      result = ESP_ERR_TIMEOUT;
      goto finish;
    }
    (void)xEventGroupWaitBits(runner->wake_events, SPIKE_WAKE_BIT, pdTRUE,
                              pdFALSE, ticks);
  }

  if (runner->native_failure || !runner->report_received) {
    result = ESP_FAIL;
  } else if (!runner->result.passed ||
             runner->result.frame_count < SPIKE_MINIMUM_FRAMES ||
             runner->result.completed_rounds != runner->config.rounds ||
             runner->result.operations_started != runner->config.rounds * 2U ||
             runner->result.operations_completed !=
                 runner->config.rounds * 2U) {
    if (runner->result.passed) {
      runner->result.passed = false;
      copy_detail(runner, "Guest report did not match native operation counts");
    }
    result = ESP_FAIL;
  } else {
    result = ESP_OK;
  }

finish:
  runner->result.elapsed_ms = elapsed_ms_since(started_us);
  if (result != ESP_OK) {
    runner->result.passed = false;
    if (runner->result.detail[0] == '\0') {
      copy_detail(runner, esp_err_to_name(result));
    }
  }
  cleanup_runner(runner);
  *out_result = runner->result;

  ESP_LOGI(TAG,
           "POCKETJS_NET_GUEST_SPIKE pass=%d rounds=%" PRIu32 "/%" PRIu32
           " operations=%" PRIu32 "/%" PRIu32 " frames=%" PRIu32
           " jobs=%" PRIu64 " elapsed_ms=%" PRIu64
           " artifact_fnv1a64=%016" PRIx64 " detail=%s",
           out_result->passed, out_result->completed_rounds,
           out_result->requested_rounds, out_result->operations_completed,
           out_result->operations_started, out_result->frame_count,
           out_result->guest_jobs_executed, out_result->elapsed_ms,
           out_result->artifact_fnv1a64, out_result->detail);

  free(runner);
  return result;
}
