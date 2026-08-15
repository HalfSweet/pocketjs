// SPDX-License-Identifier: MIT

#include "pocketjs/net/http_client_core.h"

#include "pocketjs/net/http1_wire.h"

#include <limits.h>
#include <string.h>

#define CORE_MAGIC UINT64_C(0x504a534854545031)
#define CORE_CLOSE_TIMEOUT_US UINT64_C(1000000)

typedef enum {
  CORE_IDLE = 0,
  CORE_RESOLVING,
  CORE_CONNECTING,
  CORE_WRITING_HEAD,
  CORE_WRITING_BODY,
  CORE_WAITING_REQUEST_BODY,
  CORE_WRITING_REQUEST_BODY_CHUNK,
  CORE_WRITING_REQUEST_BODY_END,
  CORE_READING,
  CORE_CLOSING,
  CORE_WAITING_TERMINAL_RETIRE,
} core_state_t;

typedef enum {
  TRANSPORT_OPERATION_NONE = 0,
  TRANSPORT_OPERATION_RESOLVE,
  TRANSPORT_OPERATION_CONNECT,
  TRANSPORT_OPERATION_WRITE,
  TRANSPORT_OPERATION_READ,
  TRANSPORT_OPERATION_CLOSE,
} transport_operation_kind_t;

typedef enum {
  EVENT_EMPTY = 0,
  EVENT_PENDING,
  EVENT_DELIVERING,
} event_state_t;

struct pocketjs_net_http_client_core {
  uint64_t magic;
  pocketjs_net_http_client_core_config_t config;
  core_state_t state;
  event_state_t event_state;
  pocketjs_net_http_client_event_t event;
  uint64_t last_operation_token;
  uint64_t last_transport_token;
  uint64_t event_sequence;
  uint64_t body_lease_generation;
  uint64_t last_request_body_generation;
  uint64_t last_request_body_pull_generation;
  uint64_t lifecycle_generation;
  uint64_t now_us;
  uint64_t connect_deadline_us;
  uint64_t headers_deadline_us;
  uint64_t idle_deadline_us;
  uint64_t total_deadline_us;
  uint64_t close_deadline_us;
  pocketjs_net_http_client_operation_token_t operation_token;

  bool permission_callback_active;
  bool shutdown_requested;
  bool transport_cancel_requested;
  bool close_cancel_requested;
  uint32_t poison_flags;
  int32_t first_poison_cause_code;

  pocketjs_net_http_client_scheme_t scheme;
  char hostname[POCKETJS_NET_HTTP_CLIENT_CORE_MAX_HOST_BYTES + 1U];
  char host_field[POCKETJS_NET_HTTP_CLIENT_CORE_MAX_HOST_BYTES + 7U];
  uint8_t target[POCKETJS_NET_HTTP_CLIENT_CORE_MAX_TARGET_BYTES];
  size_t target_length;
  uint16_t port;
  bool numeric_host;
  uint32_t numeric_ipv4_be;
  uint32_t selected_ipv4_be;

  uint8_t method[POCKETJS_NET_HTTP_CLIENT_CORE_MAX_METHOD_BYTES];
  size_t method_length;
  uint8_t request_header_storage
      [POCKETJS_NET_HTTP_CLIENT_CORE_MAX_REQUEST_HEADER_BYTES];
  size_t request_header_storage_used;
  pocketjs_net_http1_header_t
      request_headers[POCKETJS_NET_HTTP_CLIENT_CORE_MAX_REQUEST_HEADERS + 2U];
  size_t request_header_count;
  uint8_t request_body[POCKETJS_NET_HTTP_CLIENT_CORE_MAX_REQUEST_BODY_BYTES];
  size_t request_body_length;
  pocketjs_net_http_client_request_body_kind_t request_body_kind;
  bool request_body_length_known;
  uint64_t request_body_expected_length;
  uint64_t request_body_submitted_length;
  uint64_t request_body_generation;
  uint64_t request_body_pull_generation;
  size_t request_body_pull_maximum;
  size_t request_body_pending_payload_length;
  bool request_body_pull_active;
  bool request_body_pull_event_retired;
  pocketjs_net_http1_request_encoder_t encoder;
  bool encoder_done;
  uint8_t write_bytes[POCKETJS_NET_HTTP_CLIENT_CORE_WRITE_BYTES];
  size_t write_length;

  pocketjs_net_http1_response_parser_t parser;
  bool final_headers_seen;
  bool headers_delivered;
  bool parser_complete;
  bool force_no_body;
  pocketjs_net_http_client_error_t callback_error;
  unsigned response_status;
  uint8_t response_header_storage
      [POCKETJS_NET_HTTP_CLIENT_CORE_MAX_RESPONSE_HEADER_BYTES];
  size_t response_header_storage_used;
  size_t response_header_field_bytes;
  pocketjs_net_http_client_header_t
      response_headers[POCKETJS_NET_HTTP_CLIENT_CORE_MAX_RESPONSE_HEADERS];
  size_t response_header_count;
  pocketjs_net_http_client_slice_t response_status_text;

  bool transport_active;
  transport_operation_kind_t transport_operation_kind;
  uint64_t transport_operation_token;
  pocketjs_net_http_client_transport_connection_t connection;
  bool connection_valid;
  pocketjs_net_http_client_transport_read_lease_t transport_read_lease;
  bool transport_read_lease_valid;
  const uint8_t *transport_read_bytes;
  size_t transport_read_maximum;
  size_t transport_read_length;
  size_t transport_read_offset;
  bool transport_read_eof_pending;
  pocketjs_net_http_client_transport_read_lease_t orphan_read_lease;
  bool orphan_read_lease_valid;
  uint64_t completion_retire_token;
  bool completion_retire_pending;

  size_t body_credit;
  uint8_t body_bytes[POCKETJS_NET_HTTP_CLIENT_CORE_BODY_LEASE_BYTES];
  size_t body_byte_count;
  bool body_lease_active;
  bool body_lease_released;
  pocketjs_net_http_client_body_lease_t body_lease;

  bool terminal_selected;
  bool terminal_success;
  pocketjs_net_http_client_error_t terminal_error;
  int32_t terminal_cause;
};

_Static_assert(sizeof(struct pocketjs_net_http_client_core) <=
                   POCKETJS_NET_HTTP_CLIENT_CORE_INSTANCE_BYTES,
               "HTTP Client Core storage constant is too small");
_Static_assert(POCKETJS_NET_HTTP_CLIENT_CORE_REQUEST_BODY_CHUNK_BYTES + 32U <=
                   POCKETJS_NET_HTTP_CLIENT_CORE_WRITE_BYTES,
               "request chunk plus chunked framing must fit write storage");

static const pocketjs_net_http_client_core_descriptor_t descriptor = {
    .id = POCKETJS_NET_HTTP_CLIENT_CORE_ID,
    .experimental = true,
    .advertises_public_capability = false,
    .plaintext_http = true,
    .https_fail_closed_before_io = true,
    .owner_pumped = true,
    .one_operation = true,
    .fixed_core_storage = true,
    .headers_first = true,
    .explicit_body_credit = true,
    .explicit_body_lease = true,
    .redirects_followed = false,
    .hidden_retry = false,
    .hidden_auth = false,
    .hidden_cookie_store = false,
    .proxy = false,
    .content_decoding = false,
    .cleanup_faults_separate_from_terminal = true,
    .poison_is_machine_readable = true,
    .explicit_shutdown_lifecycle = true,
    .fixed_request_body = true,
    .streaming_request_body = true,
    .chunked_request_body = true,
    .known_length_streaming_request_body = true,
    .streaming_request_body_buffered_in_full = false,
    .instance_bytes = POCKETJS_NET_HTTP_CLIENT_CORE_INSTANCE_BYTES,
    .max_request_body_bytes =
        POCKETJS_NET_HTTP_CLIENT_CORE_MAX_REQUEST_BODY_BYTES,
    .max_fixed_request_body_bytes =
        POCKETJS_NET_HTTP_CLIENT_CORE_MAX_REQUEST_BODY_BYTES,
    .max_request_body_chunk_bytes =
        POCKETJS_NET_HTTP_CLIENT_CORE_REQUEST_BODY_CHUNK_BYTES,
    .body_lease_bytes = POCKETJS_NET_HTTP_CLIENT_CORE_BODY_LEASE_BYTES,
};

const pocketjs_net_http_client_core_descriptor_t *
pocketjs_net_http_client_core_descriptor(void) {
  return &descriptor;
}

static bool core_is_live(const pocketjs_net_http_client_core_t *core) {
  return core != NULL && core->magic == CORE_MAGIC;
}

static bool core_public_entry_allowed(
    const pocketjs_net_http_client_core_t *core) {
  return core_is_live(core) && !core->permission_callback_active;
}

static void poison_core(pocketjs_net_http_client_core_t *core,
                        pocketjs_net_http_client_poison_flag_t flag,
                        int32_t cause_code) {
  if (core->poison_flags == 0U) {
    core->first_poison_cause_code = cause_code;
  }
  core->poison_flags |= (uint32_t)flag;
}

static size_t owned_transport_read_lease_count(
    const pocketjs_net_http_client_core_t *core) {
  return (core->transport_read_lease_valid ? 1U : 0U) +
         (core->orphan_read_lease_valid ? 1U : 0U);
}

static bool core_is_quiescent_internal(
    const pocketjs_net_http_client_core_t *core) {
  return core->state == CORE_IDLE && core->event_state == EVENT_EMPTY &&
         !core->transport_active && !core->connection_valid &&
         !core->transport_read_lease_valid && !core->orphan_read_lease_valid &&
         !core->completion_retire_pending && !core->body_lease_active &&
         !core->request_body_pull_active &&
         !core->permission_callback_active;
}

static bool ascii_is_alpha(uint8_t byte) {
  return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

static bool ascii_is_digit(uint8_t byte) { return byte >= '0' && byte <= '9'; }

static uint8_t ascii_lower(uint8_t byte) {
  return byte >= 'A' && byte <= 'Z' ? (uint8_t)(byte + ('a' - 'A')) : byte;
}

static bool ascii_equal_case(const uint8_t *left, size_t left_length,
                             const char *right) {
  size_t right_length = strlen(right);
  if (left_length != right_length) {
    return false;
  }
  for (size_t index = 0; index < left_length; ++index) {
    if (ascii_lower(left[index]) != ascii_lower((uint8_t)right[index])) {
      return false;
    }
  }
  return true;
}

static bool is_tchar(uint8_t byte) {
  return ascii_is_alpha(byte) || ascii_is_digit(byte) || byte == '!' ||
         byte == '#' || byte == '$' || byte == '%' || byte == '&' ||
         byte == '\'' || byte == '*' || byte == '+' || byte == '-' ||
         byte == '.' || byte == '^' || byte == '_' || byte == '`' ||
         byte == '|' || byte == '~';
}

static bool valid_method(const uint8_t *method, size_t length) {
  if (method == NULL || length == 0U ||
      length > POCKETJS_NET_HTTP_CLIENT_CORE_MAX_METHOD_BYTES) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    if (!is_tchar(method[index])) {
      return false;
    }
  }
  return !ascii_equal_case(method, length, "CONNECT") &&
         !ascii_equal_case(method, length, "TRACE") &&
         !ascii_equal_case(method, length, "TRACK");
}

static bool valid_config(const pocketjs_net_http_client_core_config_t *config) {
  if (config == NULL || config->transport_ops == NULL ||
      config->allow_endpoint == NULL || config->connect_timeout_us == 0U ||
      config->headers_timeout_us == 0U || config->idle_timeout_us == 0U ||
      config->total_timeout_us == 0U ||
      config->response_header_bytes_limit >
          POCKETJS_NET_HTTP_CLIENT_CORE_MAX_RESPONSE_HEADER_BYTES) {
    return false;
  }
  const pocketjs_net_http_client_transport_ops_t *ops = config->transport_ops;
  return ops->start_resolve != NULL && ops->start_connect != NULL &&
         ops->start_read != NULL && ops->start_write != NULL &&
         ops->start_close != NULL && ops->cancel != NULL && ops->pump != NULL &&
         ops->take_completion != NULL && ops->retire_completion != NULL &&
         ops->read_lease_view != NULL && ops->release_read_lease != NULL;
}

static uint64_t deadline_after(uint64_t now_us, uint64_t duration_us) {
  return duration_us > UINT64_MAX - now_us ? UINT64_MAX : now_us + duration_us;
}

static uint64_t earlier_deadline(uint64_t left, uint64_t right) {
  return left < right ? left : right;
}

static bool next_transport_token(pocketjs_net_http_client_core_t *core,
                                 uint64_t *out_token) {
  if (core->last_transport_token == UINT64_MAX) {
    return false;
  }
  ++core->last_transport_token;
  *out_token = core->last_transport_token;
  return true;
}

static bool next_event_sequence(pocketjs_net_http_client_core_t *core,
                                uint64_t *out_sequence) {
  if (core->event_sequence == UINT64_MAX) {
    return false;
  }
  ++core->event_sequence;
  *out_sequence = core->event_sequence;
  return true;
}

static bool parse_ipv4(const uint8_t *data, size_t length,
                       uint32_t *out_ipv4_be) {
  uint8_t octets[4];
  size_t index = 0U;
  for (size_t part = 0U; part < 4U; ++part) {
    size_t start = index;
    unsigned value = 0U;
    while (index < length && ascii_is_digit(data[index])) {
      unsigned digit = (unsigned)(data[index] - '0');
      if (value > (255U - digit) / 10U) {
        return false;
      }
      value = value * 10U + digit;
      ++index;
    }
    if (index == start || (index - start > 1U && data[start] == '0')) {
      return false;
    }
    octets[part] = (uint8_t)value;
    if (part == 3U) {
      if (index != length) {
        return false;
      }
    } else if (index == length || data[index++] != '.') {
      return false;
    }
  }
  memcpy(out_ipv4_be, octets, sizeof(octets));
  return true;
}

static bool looks_numeric_host(const uint8_t *data, size_t length) {
  if (length == 0U) {
    return false;
  }
  for (size_t index = 0U; index < length; ++index) {
    if (!ascii_is_digit(data[index]) && data[index] != '.') {
      return false;
    }
  }
  return true;
}

static bool canonicalize_dns_name(const uint8_t *data, size_t length,
                                  char *output) {
  if (length == 0U || length > POCKETJS_NET_HTTP_CLIENT_CORE_MAX_HOST_BYTES ||
      data[length - 1U] == '.') {
    return false;
  }
  size_t label_start = 0U;
  for (size_t index = 0U; index <= length; ++index) {
    if (index != length && data[index] != '.') {
      uint8_t byte = data[index];
      if (!ascii_is_alpha(byte) && !ascii_is_digit(byte) && byte != '-') {
        return false;
      }
      output[index] = (char)ascii_lower(byte);
      continue;
    }
    size_t label_length = index - label_start;
    if (label_length == 0U || label_length > 63U ||
        output[label_start] == '-' || output[index - 1U] == '-') {
      return false;
    }
    if (index != length) {
      output[index] = '.';
      label_start = index + 1U;
    }
  }
  output[length] = '\0';
  return true;
}

static bool parse_port(const uint8_t *data, size_t length, uint16_t *out_port) {
  if (length == 0U || (length > 1U && data[0] == '0')) {
    return false;
  }
  unsigned value = 0U;
  for (size_t index = 0U; index < length; ++index) {
    if (!ascii_is_digit(data[index])) {
      return false;
    }
    unsigned digit = (unsigned)(data[index] - '0');
    if (value > (65535U - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
  }
  if (value == 0U) {
    return false;
  }
  *out_port = (uint16_t)value;
  return true;
}

static size_t write_port(uint16_t port, char output[5]) {
  char reversed[5];
  size_t length = 0U;
  do {
    reversed[length++] = (char)('0' + port % 10U);
    port = (uint16_t)(port / 10U);
  } while (port != 0U);
  for (size_t index = 0U; index < length; ++index) {
    output[index] = reversed[length - index - 1U];
  }
  return length;
}

static bool parse_url(pocketjs_net_http_client_core_t *core,
                      pocketjs_net_http_client_slice_t url) {
  if (url.data == NULL || url.length == 0U ||
      url.length > POCKETJS_NET_HTTP_CLIENT_CORE_MAX_URL_BYTES) {
    return false;
  }
  size_t scheme_bytes = 0U;
  uint16_t default_port = 0U;
  if (url.length >= 7U && ascii_equal_case(url.data, 7U, "http://")) {
    core->scheme = POCKETJS_NET_HTTP_CLIENT_SCHEME_HTTP;
    scheme_bytes = 7U;
    default_port = 80U;
  } else if (url.length >= 8U && ascii_equal_case(url.data, 8U, "https://")) {
    core->scheme = POCKETJS_NET_HTTP_CLIENT_SCHEME_HTTPS;
    scheme_bytes = 8U;
    default_port = 443U;
  } else {
    return false;
  }

  for (size_t index = scheme_bytes; index < url.length; ++index) {
    if (url.data[index] == '#') {
      return false;
    }
  }

  size_t authority_end = scheme_bytes;
  while (authority_end < url.length && url.data[authority_end] != '/' &&
         url.data[authority_end] != '?') {
    if (url.data[authority_end] == '@' || url.data[authority_end] == '[' ||
        url.data[authority_end] == ']') {
      return false;
    }
    ++authority_end;
  }
  if (authority_end == scheme_bytes) {
    return false;
  }

  size_t colon = authority_end;
  for (size_t index = scheme_bytes; index < authority_end; ++index) {
    if (url.data[index] == ':') {
      if (colon != authority_end) {
        return false;
      }
      colon = index;
    }
  }
  size_t hostname_length = colon - scheme_bytes;
  if (hostname_length == 0U ||
      hostname_length > POCKETJS_NET_HTTP_CLIENT_CORE_MAX_HOST_BYTES) {
    return false;
  }
  core->port = default_port;
  if (colon != authority_end &&
      !parse_port(url.data + colon + 1U, authority_end - colon - 1U,
                  &core->port)) {
    return false;
  }

  core->numeric_host = parse_ipv4(url.data + scheme_bytes, hostname_length,
                                  &core->numeric_ipv4_be);
  if (!core->numeric_host) {
    if (looks_numeric_host(url.data + scheme_bytes, hostname_length) ||
        !canonicalize_dns_name(url.data + scheme_bytes, hostname_length,
                               core->hostname)) {
      return false;
    }
  } else {
    memcpy(core->hostname, url.data + scheme_bytes, hostname_length);
    core->hostname[hostname_length] = '\0';
  }

  size_t target_length = 0U;
  if (authority_end == url.length) {
    core->target[target_length++] = '/';
  } else if (url.data[authority_end] == '?') {
    if (url.length - authority_end + 1U > sizeof(core->target)) {
      return false;
    }
    core->target[target_length++] = '/';
    memcpy(core->target + target_length, url.data + authority_end,
           url.length - authority_end);
    target_length += url.length - authority_end;
  } else {
    if (url.length - authority_end > sizeof(core->target)) {
      return false;
    }
    memcpy(core->target, url.data + authority_end, url.length - authority_end);
    target_length = url.length - authority_end;
  }
  core->target_length = target_length;

  memcpy(core->host_field, core->hostname, hostname_length);
  size_t host_length = hostname_length;
  if (core->port != default_port) {
    core->host_field[host_length++] = ':';
    host_length += write_port(core->port, core->host_field + host_length);
  }
  core->host_field[host_length] = '\0';
  return true;
}

static bool forbidden_request_header(const uint8_t *name, size_t length) {
  static const char *const forbidden[] = {
      "host",
      "content-length",
      "transfer-encoding",
      "trailer",
      "connection",
      "keep-alive",
      "proxy-connection",
      "proxy-authorization",
      "proxy-authenticate",
      "te",
      "upgrade",
      "accept-encoding",
  };
  for (size_t index = 0U; index < sizeof(forbidden) / sizeof(forbidden[0]);
       ++index) {
    if (ascii_equal_case(name, length, forbidden[index])) {
      return true;
    }
  }
  static const char proxy_prefix[] = "proxy-";
  if (length >= sizeof(proxy_prefix) - 1U) {
    bool matches = true;
    for (size_t index = 0U; index < sizeof(proxy_prefix) - 1U; ++index) {
      if (ascii_lower(name[index]) != (uint8_t)proxy_prefix[index]) {
        matches = false;
        break;
      }
    }
    if (matches) {
      return true;
    }
  }
  return false;
}

static pocketjs_net_http_client_start_result_t
snapshot_request(pocketjs_net_http_client_core_t *core,
                 const pocketjs_net_http_client_request_t *request) {
  if (request == NULL || request->operation_token == 0U ||
      (request->header_count != 0U && request->headers == NULL) ||
      request->body_kind > POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_STREAMING ||
      (request->body.length != 0U && request->body.data == NULL)) {
    return POCKETJS_NET_HTTP_CLIENT_START_INVALID_ARGUMENT;
  }
  switch (request->body_kind) {
  case POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_NONE:
    if (request->body.length != 0U ||
        request->streaming_content_length_known ||
        request->streaming_content_length != 0U) {
      return POCKETJS_NET_HTTP_CLIENT_START_INVALID_ARGUMENT;
    }
    break;
  case POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_FIXED:
    if (request->streaming_content_length_known ||
        request->streaming_content_length != 0U) {
      return POCKETJS_NET_HTTP_CLIENT_START_INVALID_ARGUMENT;
    }
    if (request->body.length >
        POCKETJS_NET_HTTP_CLIENT_CORE_MAX_REQUEST_BODY_BYTES) {
      return POCKETJS_NET_HTTP_CLIENT_START_LIMIT_EXCEEDED;
    }
    break;
  case POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_STREAMING:
    if (request->body.length != 0U ||
        (!request->streaming_content_length_known &&
         request->streaming_content_length != 0U)) {
      return POCKETJS_NET_HTTP_CLIENT_START_INVALID_ARGUMENT;
    }
    if (core->last_request_body_generation == UINT64_MAX ||
        core->last_request_body_pull_generation == UINT64_MAX) {
      return POCKETJS_NET_HTTP_CLIENT_START_TOKEN_EXHAUSTED;
    }
    break;
  default:
    return POCKETJS_NET_HTTP_CLIENT_START_INVALID_ARGUMENT;
  }
  if (core->last_operation_token == UINT64_MAX) {
    return POCKETJS_NET_HTTP_CLIENT_START_TOKEN_EXHAUSTED;
  }
  if (request->operation_token <= core->last_operation_token) {
    return POCKETJS_NET_HTTP_CLIENT_START_INVALID_ARGUMENT;
  }
  if (!parse_url(core, request->url)) {
    return POCKETJS_NET_HTTP_CLIENT_START_INVALID_URL;
  }
  if (core->scheme == POCKETJS_NET_HTTP_CLIENT_SCHEME_HTTPS) {
    return POCKETJS_NET_HTTP_CLIENT_START_UNSUPPORTED_TLS;
  }
  if (!valid_method(request->method.data, request->method.length)) {
    return POCKETJS_NET_HTTP_CLIENT_START_FORBIDDEN_REQUEST;
  }
  if (request->header_count >
          POCKETJS_NET_HTTP_CLIENT_CORE_MAX_REQUEST_HEADERS) {
    return POCKETJS_NET_HTTP_CLIENT_START_LIMIT_EXCEEDED;
  }
  if (request->body_kind != POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_NONE &&
      (ascii_equal_case(request->method.data, request->method.length, "GET") ||
       ascii_equal_case(request->method.data, request->method.length,
                        "HEAD"))) {
    return POCKETJS_NET_HTTP_CLIENT_START_FORBIDDEN_REQUEST;
  }

  memcpy(core->method, request->method.data, request->method.length);
  core->method_length = request->method.length;
  core->request_header_storage_used = 0U;
  core->request_header_count = 0U;
  for (size_t index = 0U; index < request->header_count; ++index) {
    pocketjs_net_http_client_header_t header = request->headers[index];
    if (header.name.data == NULL || header.name.length == 0U ||
        (header.value.length != 0U && header.value.data == NULL)) {
      return POCKETJS_NET_HTTP_CLIENT_START_INVALID_ARGUMENT;
    }
    if (forbidden_request_header(header.name.data, header.name.length)) {
      return POCKETJS_NET_HTTP_CLIENT_START_FORBIDDEN_REQUEST;
    }
    if (header.name.length > sizeof(core->request_header_storage) -
                                 core->request_header_storage_used ||
        header.value.length > sizeof(core->request_header_storage) -
                                  core->request_header_storage_used -
                                  header.name.length) {
      return POCKETJS_NET_HTTP_CLIENT_START_LIMIT_EXCEEDED;
    }
    uint8_t *name =
        core->request_header_storage + core->request_header_storage_used;
    memcpy(name, header.name.data, header.name.length);
    core->request_header_storage_used += header.name.length;
    uint8_t *value =
        core->request_header_storage + core->request_header_storage_used;
    if (header.value.length != 0U) {
      memcpy(value, header.value.data, header.value.length);
    }
    core->request_header_storage_used += header.value.length;
    core->request_headers[core->request_header_count++] =
        (pocketjs_net_http1_header_t){
            .name = {.data = name, .length = header.name.length},
            .value = {.data = value, .length = header.value.length},
        };
  }
  static const uint8_t connection_name[] = "Connection";
  static const uint8_t connection_value[] = "close";
  static const uint8_t encoding_name[] = "Accept-Encoding";
  static const uint8_t encoding_value[] = "identity";
  core->request_headers[core->request_header_count++] =
      (pocketjs_net_http1_header_t){
          .name = {.data = connection_name,
                   .length = sizeof(connection_name) - 1U},
          .value = {.data = connection_value,
                    .length = sizeof(connection_value) - 1U},
      };
  core->request_headers[core->request_header_count++] =
      (pocketjs_net_http1_header_t){
          .name = {.data = encoding_name, .length = sizeof(encoding_name) - 1U},
          .value = {.data = encoding_value,
                    .length = sizeof(encoding_value) - 1U},
      };
  if (request->body_kind == POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_FIXED &&
      request->body.length != 0U) {
    memcpy(core->request_body, request->body.data, request->body.length);
  }
  core->request_body_kind = request->body_kind;
  core->request_body_length =
      request->body_kind == POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_FIXED
          ? request->body.length
          : 0U;
  core->request_body_length_known =
      request->body_kind == POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_STREAMING &&
      request->streaming_content_length_known;
  core->request_body_expected_length =
      core->request_body_length_known ? request->streaming_content_length : 0U;
  core->request_body_submitted_length = 0U;
  core->request_body_pending_payload_length = 0U;
  core->request_body_pull_active = false;
  core->request_body_pull_event_retired = false;

  pocketjs_net_http1_request_body_kind_t wire_body_kind =
      POCKETJS_NET_HTTP1_REQUEST_BODY_NONE;
  uint64_t wire_content_length = 0U;
  if (core->request_body_kind ==
      POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_FIXED) {
    wire_body_kind = POCKETJS_NET_HTTP1_REQUEST_BODY_FIXED;
    wire_content_length = core->request_body_length;
  } else if (core->request_body_kind ==
             POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_STREAMING) {
    wire_body_kind = core->request_body_length_known
                         ? POCKETJS_NET_HTTP1_REQUEST_BODY_FIXED
                         : POCKETJS_NET_HTTP1_REQUEST_BODY_CHUNKED;
    wire_content_length = core->request_body_expected_length;
  }

  pocketjs_net_http1_request_t wire_request = {
      .method = {.data = core->method, .length = core->method_length},
      .target = {.data = core->target, .length = core->target_length},
      .host = {.data = (const uint8_t *)core->host_field,
               .length = strlen(core->host_field)},
      .headers = core->request_headers,
      .header_count = core->request_header_count,
      .body_kind = wire_body_kind,
      .content_length = wire_content_length,
  };
  pocketjs_net_http1_wire_error_t wire_error =
      pocketjs_net_http1_request_encoder_init(
          &core->encoder, &wire_request, &pocketjs_net_http1_default_limits);
  if (wire_error == POCKETJS_NET_HTTP1_WIRE_ERROR_NONE) {
    return POCKETJS_NET_HTTP_CLIENT_START_OK;
  }
  return pocketjs_net_http1_wire_error_is_limit(wire_error)
             ? POCKETJS_NET_HTTP_CLIENT_START_LIMIT_EXCEEDED
             : POCKETJS_NET_HTTP_CLIENT_START_FORBIDDEN_REQUEST;
}

static bool publish_event(pocketjs_net_http_client_core_t *core,
                          pocketjs_net_http_client_event_type_t type) {
  uint64_t sequence = 0U;
  bool terminal = type == POCKETJS_NET_HTTP_CLIENT_EVENT_COMPLETE ||
                  type == POCKETJS_NET_HTTP_CLIENT_EVENT_ERROR;
  if (core->event_state != EVENT_EMPTY ||
      (!terminal && core->event_sequence >= UINT64_MAX - 1U) ||
      !next_event_sequence(core, &sequence)) {
    return false;
  }
  memset(&core->event, 0, sizeof(core->event));
  core->event.type = type;
  core->event.sequence = sequence;
  core->event.operation_token = core->operation_token;
  core->event_state = EVENT_PENDING;
  return true;
}

static bool publish_headers(pocketjs_net_http_client_core_t *core) {
  if (!publish_event(core, POCKETJS_NET_HTTP_CLIENT_EVENT_RESPONSE_HEADERS)) {
    return false;
  }
  core->event.detail.response.status_code = core->response_status;
  core->event.detail.response.status_text = core->response_status_text;
  core->event.detail.response.headers = core->response_headers;
  core->event.detail.response.header_count = core->response_header_count;
  return true;
}

static bool publish_terminal(pocketjs_net_http_client_core_t *core) {
  pocketjs_net_http_client_event_type_t type =
      core->terminal_success ? POCKETJS_NET_HTTP_CLIENT_EVENT_COMPLETE
                             : POCKETJS_NET_HTTP_CLIENT_EVENT_ERROR;
  if (!publish_event(core, type)) {
    return false;
  }
  if (!core->terminal_success) {
    core->event.detail.error.code = core->terminal_error;
    core->event.detail.error.cause_code = core->terminal_cause;
  }
  core->state = CORE_WAITING_TERMINAL_RETIRE;
  return true;
}

static bool response_on_status(void *context, unsigned http_minor,
                               unsigned status_code, const uint8_t *status_text,
                               size_t status_text_length, bool informational) {
  (void)http_minor;
  pocketjs_net_http_client_core_t *core = context;
  if (informational) {
    return true;
  }
  core->callback_error = 0;
  core->force_no_body = false;
  core->response_header_storage_used = 0U;
  core->response_header_field_bytes = 0U;
  core->response_header_count = 0U;
  core->response_status = status_code;
  if (status_text_length > sizeof(core->response_header_storage)) {
    core->callback_error = POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT;
    return false;
  }
  uint8_t *copy = core->response_header_storage;
  if (status_text_length != 0U) {
    memcpy(copy, status_text, status_text_length);
  }
  core->response_header_storage_used = status_text_length;
  core->response_status_text = (pocketjs_net_http_client_slice_t){
      .data = copy,
      .length = status_text_length,
  };
  return true;
}

static bool response_on_header(void *context, const uint8_t *name,
                               size_t name_length, const uint8_t *value,
                               size_t value_length, bool informational) {
  pocketjs_net_http_client_core_t *core = context;
  if (informational) {
    return true;
  }
  if (ascii_equal_case(name, name_length, "Content-Encoding") &&
      !ascii_equal_case(value, value_length, "identity")) {
    core->callback_error = POCKETJS_NET_HTTP_CLIENT_ERROR_PROTOCOL;
    return false;
  }
  const size_t header_limit =
      core->config.response_header_bytes_limit == 0U
          ? POCKETJS_NET_HTTP_CLIENT_CORE_MAX_RESPONSE_HEADER_BYTES
          : core->config.response_header_bytes_limit;
  if (value_length > SIZE_MAX - 4U ||
      name_length > SIZE_MAX - value_length - 4U) {
    core->callback_error = POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT;
    return false;
  }
  const size_t field_bytes = name_length + value_length + 4U;
  if (field_bytes > header_limit ||
      core->response_header_field_bytes > header_limit - field_bytes) {
    core->callback_error = POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT;
    return false;
  }
  if (core->response_header_count >=
          POCKETJS_NET_HTTP_CLIENT_CORE_MAX_RESPONSE_HEADERS ||
      name_length > sizeof(core->response_header_storage) -
                        core->response_header_storage_used ||
      value_length > sizeof(core->response_header_storage) -
                         core->response_header_storage_used - name_length) {
    core->callback_error = POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT;
    return false;
  }
  uint8_t *name_copy =
      core->response_header_storage + core->response_header_storage_used;
  memcpy(name_copy, name, name_length);
  core->response_header_storage_used += name_length;
  uint8_t *value_copy =
      core->response_header_storage + core->response_header_storage_used;
  if (value_length != 0U) {
    memcpy(value_copy, value, value_length);
  }
  core->response_header_storage_used += value_length;
  core->response_header_field_bytes += field_bytes;
  core->response_headers[core->response_header_count++] =
      (pocketjs_net_http_client_header_t){
          .name = {.data = name_copy, .length = name_length},
          .value = {.data = value_copy, .length = value_length},
      };
  return true;
}

static bool
response_on_headers_complete(void *context, unsigned status_code,
                             pocketjs_net_http1_response_body_kind_t body_kind,
                             uint64_t content_length, bool informational) {
  (void)content_length;
  pocketjs_net_http_client_core_t *core = context;
  if (informational) {
    return true;
  }
  if (status_code == 205U) {
    if (body_kind == POCKETJS_NET_HTTP1_RESPONSE_BODY_CHUNKED ||
        (body_kind == POCKETJS_NET_HTTP1_RESPONSE_BODY_FIXED &&
         content_length != 0U)) {
      core->callback_error = POCKETJS_NET_HTTP_CLIENT_ERROR_PROTOCOL;
      return false;
    }
    core->force_no_body = true;
  }
  core->final_headers_seen = true;
  if (!publish_headers(core)) {
    core->callback_error = POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT;
    return false;
  }
  return true;
}

static bool response_on_body(void *context, const uint8_t *body,
                             size_t body_length) {
  pocketjs_net_http_client_core_t *core = context;
  if (!core->headers_delivered || body_length > core->body_credit ||
      body_length > sizeof(core->body_bytes) - core->body_byte_count) {
    core->callback_error = POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT;
    return false;
  }
  if (body_length != 0U) {
    memcpy(core->body_bytes + core->body_byte_count, body, body_length);
  }
  core->body_byte_count += body_length;
  core->body_credit -= body_length;
  return true;
}

static void response_on_complete(void *context) {
  pocketjs_net_http_client_core_t *core = context;
  core->parser_complete = true;
}

static pocketjs_net_http_client_error_t
map_transport_error(pocketjs_net_http_client_transport_error_t error) {
  switch (error) {
  case POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR_ABORTED:
    return POCKETJS_NET_HTTP_CLIENT_ERROR_ABORTED;
  case POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR_TIMED_OUT:
    return POCKETJS_NET_HTTP_CLIENT_ERROR_TIMED_OUT;
  case POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR_DNS:
    return POCKETJS_NET_HTTP_CLIENT_ERROR_DNS;
  case POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR_CONNECT:
    return POCKETJS_NET_HTTP_CLIENT_ERROR_CONNECT;
  case POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR_IO:
    return POCKETJS_NET_HTTP_CLIENT_ERROR_IO;
  case POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR_RESOURCE_LIMIT:
    return POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT;
  case POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR_TLS:
  case POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR_INVALID:
  case POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR_NONE:
  default:
    return POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT;
  }
}

static void select_failure(pocketjs_net_http_client_core_t *core,
                           pocketjs_net_http_client_error_t error,
                           int32_t cause) {
  if (core->terminal_selected) {
    return;
  }
  core->terminal_selected = true;
  core->terminal_success = false;
  core->terminal_error = error;
  core->terminal_cause = cause;
}

static void select_success(pocketjs_net_http_client_core_t *core) {
  if (core->terminal_selected) {
    return;
  }
  core->terminal_selected = true;
  core->terminal_success = true;
}

static void revoke_request_body_credit(
    pocketjs_net_http_client_core_t *core) {
  core->request_body_pull_active = false;
  core->request_body_pull_event_retired = false;
  core->request_body_pull_maximum = 0U;
  if (core->event_state != EVENT_EMPTY &&
      core->event.type == POCKETJS_NET_HTTP_CLIENT_EVENT_REQUEST_BODY_PULL) {
    core->event_state = EVENT_EMPTY;
    memset(&core->event, 0, sizeof(core->event));
  }
}

static bool publish_request_body_pull(
    pocketjs_net_http_client_core_t *core) {
  if (core->state != CORE_WAITING_REQUEST_BODY || core->terminal_selected ||
      core->event_state != EVENT_EMPTY || core->request_body_pull_active) {
    return false;
  }
  size_t maximum = POCKETJS_NET_HTTP_CLIENT_CORE_REQUEST_BODY_CHUNK_BYTES;
  if (core->request_body_length_known) {
    if (core->request_body_submitted_length >=
        core->request_body_expected_length) {
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_REQUEST_BODY,
                     POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_CAUSE_LENGTH_UNDERFLOW);
      return false;
    }
    uint64_t remaining = core->request_body_expected_length -
                         core->request_body_submitted_length;
    if (remaining < maximum) {
      maximum = (size_t)remaining;
    }
  }
  if (maximum == 0U ||
      core->last_request_body_pull_generation == UINT64_MAX) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT, 0);
    return false;
  }
  ++core->last_request_body_pull_generation;
  core->request_body_pull_generation =
      core->last_request_body_pull_generation;
  core->request_body_pull_maximum = maximum;
  core->request_body_pull_active = true;
  core->request_body_pull_event_retired = false;
  if (!publish_event(core,
                     POCKETJS_NET_HTTP_CLIENT_EVENT_REQUEST_BODY_PULL)) {
    revoke_request_body_credit(core);
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT, 0);
    return false;
  }
  core->event.detail.request_body_pull.body_generation =
      core->request_body_generation;
  core->event.detail.request_body_pull.pull_generation =
      core->request_body_pull_generation;
  core->event.detail.request_body_pull.maximum_bytes = maximum;
  return true;
}

static bool invoke_permission(
    pocketjs_net_http_client_core_t *core,
    const pocketjs_net_http_client_endpoint_t *endpoint,
    core_state_t expected_state, bool *out_allowed) {
  uint64_t generation = core->lifecycle_generation;
  pocketjs_net_http_client_operation_token_t operation_token =
      core->operation_token;
  core->permission_callback_active = true;
  bool allowed = core->config.allow_endpoint(core->config.permission_context,
                                              endpoint);
  core->permission_callback_active = false;
  if (!core_is_live(core) || core->lifecycle_generation != generation ||
      core->state != expected_state ||
      core->operation_token != operation_token || core->transport_active ||
      core->terminal_selected) {
    if (core_is_live(core)) {
      poison_core(core,
                  POCKETJS_NET_HTTP_CLIENT_POISON_PERMISSION_REENTRANCY, 0);
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
    }
    *out_allowed = false;
    return false;
  }
  *out_allowed = allowed;
  return true;
}

static bool read_lease_equal(
    pocketjs_net_http_client_transport_read_lease_t left,
    pocketjs_net_http_client_transport_read_lease_t right) {
  return left.slot == right.slot && left.generation == right.generation;
}

static bool release_transport_read_lease(
    pocketjs_net_http_client_core_t *core) {
  if (!core->transport_read_lease_valid) {
    return true;
  }
  pocketjs_net_http_client_transport_result_t result =
      core->config.transport_ops->release_read_lease(
          core->config.transport_context, core->transport_read_lease);
  if (result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_READ_LEASE_RELEASE,
                (int32_t)result);
    return false;
  }
  core->transport_read_lease_valid = false;
  core->transport_read_bytes = NULL;
  core->transport_read_length = 0U;
  core->transport_read_offset = 0U;
  core->transport_read_eof_pending = false;
  return true;
}

static bool release_orphan_read_lease(
    pocketjs_net_http_client_core_t *core) {
  if (!core->orphan_read_lease_valid) {
    return true;
  }
  pocketjs_net_http_client_transport_result_t result =
      core->config.transport_ops->release_read_lease(
          core->config.transport_context, core->orphan_read_lease);
  if (result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_READ_LEASE_RELEASE,
                (int32_t)result);
    return false;
  }
  core->orphan_read_lease_valid = false;
  return true;
}

static bool cleanup_completion_read_lease(
    pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_transport_read_lease_t lease) {
  if ((core->transport_read_lease_valid &&
       read_lease_equal(core->transport_read_lease, lease)) ||
      (core->orphan_read_lease_valid &&
       read_lease_equal(core->orphan_read_lease, lease))) {
    poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_STALE_COMPLETION, 0);
    return false;
  }
  pocketjs_net_http_client_transport_result_t result =
      core->config.transport_ops->release_read_lease(
          core->config.transport_context, lease);
  if (result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    return true;
  }
  poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_READ_LEASE_RELEASE,
              (int32_t)result);
  if (!core->transport_read_lease_valid) {
    core->transport_read_lease = lease;
    core->transport_read_lease_valid = true;
    core->transport_read_bytes = NULL;
    core->transport_read_length = 0U;
    core->transport_read_offset = 0U;
    core->transport_read_eof_pending = false;
  } else if (!core->orphan_read_lease_valid) {
    core->orphan_read_lease = lease;
    core->orphan_read_lease_valid = true;
  } else {
    poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_STALE_COMPLETION, 0);
  }
  return false;
}

static bool start_close(pocketjs_net_http_client_core_t *core) {
  if (core->event_state != EVENT_EMPTY || core->transport_active) {
    return true;
  }
  if (!core->connection_valid) {
    return publish_terminal(core);
  }
  uint64_t token = 0U;
  if (!next_transport_token(core, &token)) {
    poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_CLOSE_ADMISSION, 0);
    (void)publish_terminal(core);
    return false;
  }
  uint64_t close_deadline =
      deadline_after(core->now_us, CORE_CLOSE_TIMEOUT_US);
  pocketjs_net_http_client_transport_result_t result =
      core->config.transport_ops->start_close(core->config.transport_context,
                                              token, core->connection,
                                              close_deadline);
  if (result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_CLOSE_ADMISSION,
                (int32_t)result);
    (void)publish_terminal(core);
    return false;
  }
  core->transport_active = true;
  core->transport_cancel_requested = false;
  core->transport_operation_kind = TRANSPORT_OPERATION_CLOSE;
  core->transport_operation_token = token;
  core->close_deadline_us = close_deadline;
  core->close_cancel_requested = false;
  core->state = CORE_CLOSING;
  return true;
}

static void progress_terminal(pocketjs_net_http_client_core_t *core) {
  if (!core->terminal_selected) {
    return;
  }
  revoke_request_body_credit(core);
  if (core->event_state != EVENT_EMPTY) {
    return;
  }
  (void)release_transport_read_lease(core);
  (void)release_orphan_read_lease(core);
  core->body_credit = 0U;
  core->body_byte_count = 0U;
  if (core->completion_retire_pending) {
    return;
  }
  if (core->transport_active) {
    if (core->transport_operation_kind == TRANSPORT_OPERATION_CLOSE) {
      return;
    }
    if (core->transport_cancel_requested) {
      return;
    }
    pocketjs_net_http_client_transport_result_t result =
        core->config.transport_ops->cancel(core->config.transport_context,
                                           core->transport_operation_token);
    if (result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
      poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_CANCEL,
                  (int32_t)result);
      (void)publish_terminal(core);
    } else {
      core->transport_cancel_requested = true;
    }
    return;
  }
  (void)start_close(core);
}

static pocketjs_net_http_client_transport_result_t
start_resolve(pocketjs_net_http_client_core_t *core) {
  uint64_t token = 0U;
  if (!next_transport_token(core, &token)) {
    return POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT;
  }
  pocketjs_net_http_client_transport_result_t result =
      core->config.transport_ops->start_resolve(core->config.transport_context,
                                                token, core->hostname,
                                                core->connect_deadline_us);
  if (result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    core->transport_active = true;
    core->transport_cancel_requested = false;
    core->transport_operation_kind = TRANSPORT_OPERATION_RESOLVE;
    core->transport_operation_token = token;
    core->state = CORE_RESOLVING;
  }
  return result;
}

static pocketjs_net_http_client_transport_result_t
start_connect(pocketjs_net_http_client_core_t *core, uint32_t ipv4_be) {
  uint64_t token = 0U;
  if (!next_transport_token(core, &token)) {
    return POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT;
  }
  pocketjs_net_http_client_transport_result_t result =
      core->config.transport_ops->start_connect(
          core->config.transport_context, token, ipv4_be, core->port, false,
          core->hostname, core->connect_deadline_us);
  if (result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    core->selected_ipv4_be = ipv4_be;
    core->transport_active = true;
    core->transport_cancel_requested = false;
    core->transport_operation_kind = TRANSPORT_OPERATION_CONNECT;
    core->transport_operation_token = token;
    core->state = CORE_CONNECTING;
  }
  return result;
}

static pocketjs_net_http_client_transport_result_t
start_write(pocketjs_net_http_client_core_t *core, const uint8_t *bytes,
            size_t length, core_state_t state) {
  uint64_t token = 0U;
  if (!next_transport_token(core, &token)) {
    return POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT;
  }
  pocketjs_net_http_client_transport_result_t result =
      core->config.transport_ops->start_write(
          core->config.transport_context, token, core->connection, bytes,
          length, core->total_deadline_us);
  if (result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    core->write_length = length;
    core->transport_active = true;
    core->transport_cancel_requested = false;
    core->transport_operation_kind = TRANSPORT_OPERATION_WRITE;
    core->transport_operation_token = token;
    core->state = state;
  }
  return result;
}

static bool emit_next_request_head(pocketjs_net_http_client_core_t *core) {
  size_t length = 0U;
  pocketjs_net_http1_encoder_result_t encoder_result =
      pocketjs_net_http1_request_encoder_write(
          &core->encoder, core->write_bytes, sizeof(core->write_bytes),
          &length);
  if (encoder_result == POCKETJS_NET_HTTP1_ENCODER_ERROR || length == 0U) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_PROTOCOL,
                   (int32_t)core->encoder.phase);
    return false;
  }
  core->encoder_done = encoder_result == POCKETJS_NET_HTTP1_ENCODER_DONE;
  core->write_length = length;
  pocketjs_net_http_client_transport_result_t result =
      start_write(core, core->write_bytes, length, CORE_WRITING_HEAD);
  if (result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    select_failure(core,
                   result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT
                       ? POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT
                       : POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT,
                   (int32_t)result);
    return false;
  }
  return true;
}

static bool initialize_parser(pocketjs_net_http_client_core_t *core) {
  pocketjs_net_http1_response_callbacks_t callbacks = {
      .on_status = response_on_status,
      .on_header = response_on_header,
      .on_headers_complete = response_on_headers_complete,
      .on_body = response_on_body,
      .on_complete = response_on_complete,
  };
  bool response_to_head =
      ascii_equal_case(core->method, core->method_length, "HEAD");
  pocketjs_net_http1_wire_error_t error =
      pocketjs_net_http1_response_parser_init(
          &core->parser, &pocketjs_net_http1_default_limits, &callbacks, core,
          response_to_head);
  return error == POCKETJS_NET_HTTP1_WIRE_ERROR_NONE;
}

static bool start_read(pocketjs_net_http_client_core_t *core) {
  if (core->transport_active || core->transport_read_lease_valid ||
      core->event_state != EVENT_EMPTY || core->terminal_selected) {
    return true;
  }
  if (core->final_headers_seen && core->body_credit == 0U) {
    return true;
  }
  uint64_t deadline =
      core->final_headers_seen
          ? earlier_deadline(core->idle_deadline_us, core->total_deadline_us)
          : earlier_deadline(core->headers_deadline_us,
                             core->total_deadline_us);
  if (core->now_us >= deadline) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TIMED_OUT, 0);
    return false;
  }
  size_t maximum_bytes = core->final_headers_seen
                             ? core->body_credit
                             : POCKETJS_NET_HTTP_CLIENT_CORE_BODY_LEASE_BYTES;
  if (maximum_bytes > POCKETJS_NET_HTTP_CLIENT_CORE_BODY_LEASE_BYTES) {
    maximum_bytes = POCKETJS_NET_HTTP_CLIENT_CORE_BODY_LEASE_BYTES;
  }
  uint64_t token = 0U;
  if (!next_transport_token(core, &token)) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT, 0);
    return false;
  }
  pocketjs_net_http_client_transport_result_t result =
      core->config.transport_ops->start_read(core->config.transport_context,
                                             token, core->connection,
                                             maximum_bytes, deadline);
  if (result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    select_failure(core,
                   result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT
                       ? POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT
                       : POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT,
                   (int32_t)result);
    return false;
  }
  core->transport_read_maximum = maximum_bytes;
  core->transport_active = true;
  core->transport_cancel_requested = false;
  core->transport_operation_kind = TRANSPORT_OPERATION_READ;
  core->transport_operation_token = token;
  core->state = CORE_READING;
  return true;
}

static bool begin_response_read(pocketjs_net_http_client_core_t *core) {
  core->headers_deadline_us = earlier_deadline(
      deadline_after(core->now_us, core->config.headers_timeout_us),
      core->total_deadline_us);
  core->state = CORE_READING;
  return start_read(core);
}

static void publish_body_if_any(pocketjs_net_http_client_core_t *core) {
  if (core->body_byte_count == 0U || core->event_state != EVENT_EMPTY) {
    return;
  }
  if (core->body_lease_generation == UINT64_MAX) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT, 0);
    return;
  }
  ++core->body_lease_generation;
  core->body_lease = (pocketjs_net_http_client_body_lease_t){
      .slot = 0U,
      .generation = core->body_lease_generation,
  };
  core->body_lease_active = true;
  core->body_lease_released = false;
  if (!publish_event(core, POCKETJS_NET_HTTP_CLIENT_EVENT_BODY)) {
    core->body_lease_active = false;
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT, 0);
    return;
  }
  core->event.detail.body.lease = core->body_lease;
  core->event.detail.body.byte_count = core->body_byte_count;
}

static void consume_retained_read(pocketjs_net_http_client_core_t *core) {
  if (!core->transport_read_lease_valid || core->event_state != EVENT_EMPTY ||
      core->terminal_selected ||
      (core->final_headers_seen && core->body_credit == 0U)) {
    return;
  }
  if (core->transport_read_bytes == NULL ||
      core->transport_read_offset > core->transport_read_length) {
    (void)release_transport_read_lease(core);
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
    return;
  }
  size_t remaining = core->transport_read_length - core->transport_read_offset;
  size_t consumed = 0U;
  size_t credit = core->headers_delivered ? core->body_credit : 0U;
  pocketjs_net_http1_parse_result_t result =
      pocketjs_net_http1_response_parser_feed(&core->parser,
                                              core->transport_read_bytes +
                                                  core->transport_read_offset,
                                              remaining, credit, &consumed);
  core->transport_read_offset += consumed;

  if (result == POCKETJS_NET_HTTP1_PARSE_ERROR) {
    int32_t cause = (int32_t)core->parser.error;
    (void)release_transport_read_lease(core);
    select_failure(core,
                   core->callback_error != 0
                       ? core->callback_error
                       : POCKETJS_NET_HTTP_CLIENT_ERROR_PROTOCOL,
                   cause);
    return;
  }
  if (core->force_no_body && core->final_headers_seen) {
    core->transport_read_offset = core->transport_read_length;
    core->parser_complete = true;
    result = POCKETJS_NET_HTTP1_PARSE_COMPLETE;
  }
  if (result == POCKETJS_NET_HTTP1_PARSE_COMPLETE) {
    if (core->transport_read_offset != core->transport_read_length) {
      (void)release_transport_read_lease(core);
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_PROTOCOL, 0);
      return;
    }
    (void)release_transport_read_lease(core);
    core->parser_complete = true;
  } else if (core->transport_read_offset == core->transport_read_length) {
    bool eof = core->transport_read_eof_pending;
    bool released = release_transport_read_lease(core);
    if (eof && !core->terminal_selected && !core->parser_complete) {
      pocketjs_net_http1_parse_result_t finish_result =
          pocketjs_net_http1_response_parser_finish(&core->parser);
      if (finish_result == POCKETJS_NET_HTTP1_PARSE_COMPLETE) {
        core->parser_complete = true;
      } else {
        select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_PROTOCOL,
                       (int32_t)core->parser.error);
      }
    } else if (!released && !core->terminal_selected &&
               !core->parser_complete) {
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
    }
  }

  publish_body_if_any(core);
  if (core->parser_complete && core->event_state == EVENT_EMPTY) {
    select_success(core);
  }
}

static void handle_resolved(
    pocketjs_net_http_client_core_t *core,
    const pocketjs_net_http_client_transport_completion_t *completion) {
  size_t count = completion->detail.resolved.candidate_count;
  if (count == 0U || count > POCKETJS_NET_HTTP_CLIENT_CORE_MAX_DNS_CANDIDATES) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_DNS, 0);
    return;
  }
  bool allowed[POCKETJS_NET_HTTP_CLIENT_CORE_MAX_DNS_CANDIDATES] = {false};
  for (size_t index = 0U; index < count; ++index) {
    pocketjs_net_http_client_endpoint_t endpoint = {
        .phase = POCKETJS_NET_HTTP_CLIENT_PERMISSION_NUMERIC_CANDIDATE,
        .scheme = core->scheme,
        .hostname = core->hostname,
        .port = core->port,
        .ipv4_be = completion->detail.resolved.ipv4_be[index],
    };
    if (!invoke_permission(core, &endpoint, CORE_RESOLVING,
                           &allowed[index])) {
      return;
    }
  }
  size_t selected = count;
  for (size_t index = 0U; index < count; ++index) {
    if (allowed[index]) {
      selected = index;
      break;
    }
  }
  if (selected == count) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_PERMISSION_DENIED, 0);
    return;
  }
  pocketjs_net_http_client_transport_result_t result =
      start_connect(core, completion->detail.resolved.ipv4_be[selected]);
  if (result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    select_failure(core,
                   result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT
                       ? POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT
                       : POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT,
                   (int32_t)result);
  }
}

static void discard_completion_payload(
    pocketjs_net_http_client_core_t *core,
    const pocketjs_net_http_client_transport_completion_t *completion) {
  if (completion->type == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_READ &&
      completion->detail.read.byte_count != 0U) {
    (void)cleanup_completion_read_lease(core, completion->detail.read.lease);
  }
}

static void handle_transport_completion(
    pocketjs_net_http_client_core_t *core,
    const pocketjs_net_http_client_transport_completion_t *completion) {
  transport_operation_kind_t completed_kind = core->transport_operation_kind;
  core->transport_active = false;
  core->transport_cancel_requested = false;
  core->transport_operation_kind = TRANSPORT_OPERATION_NONE;

  /* A terminal event may have been retired while poisoned native cleanup was
   * still outstanding. Late completion is cleanup-only: it must never restart
   * the state machine or publish an operation-token-zero event. */
  if (core->operation_token == 0U) {
    if (completion->type == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_READ) {
      discard_completion_payload(core, completion);
    }
    if (completed_kind == TRANSPORT_OPERATION_CLOSE) {
      if (completion->type == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_CLOSED &&
          completion->detail.closed.connection.slot == core->connection.slot &&
          completion->detail.closed.connection.generation ==
              core->connection.generation) {
        core->connection_valid = false;
      } else {
        poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_CLOSE_COMPLETION,
                    completion->type ==
                            POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR
                        ? completion->detail.error.cause_code
                        : 0);
      }
    } else if (completion->type ==
                   POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR &&
               (completed_kind == TRANSPORT_OPERATION_CONNECT ||
                completed_kind == TRANSPORT_OPERATION_READ ||
                completed_kind == TRANSPORT_OPERATION_WRITE)) {
      core->connection_valid = false;
    }
    return;
  }

  if (completion->type == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_READ &&
      completed_kind != TRANSPORT_OPERATION_READ) {
    discard_completion_payload(core, completion);
  }

  if (completion->type == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_ERROR) {
    if (completed_kind == TRANSPORT_OPERATION_CONNECT ||
        completed_kind == TRANSPORT_OPERATION_READ ||
        completed_kind == TRANSPORT_OPERATION_WRITE) {
      core->connection_valid = false;
    }
    if (!core->terminal_selected) {
      select_failure(core, map_transport_error(completion->detail.error.code),
                     completion->detail.error.cause_code);
    }
    if (completed_kind == TRANSPORT_OPERATION_CLOSE) {
      poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_CLOSE_COMPLETION,
                  completion->detail.error.cause_code);
      (void)publish_terminal(core);
    }
    return;
  }

  if (core->terminal_selected && completed_kind != TRANSPORT_OPERATION_CLOSE) {
    if (completed_kind == TRANSPORT_OPERATION_READ) {
      discard_completion_payload(core, completion);
    }
    return;
  }

  switch (completed_kind) {
  case TRANSPORT_OPERATION_RESOLVE:
    if (completion->type != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOLVED) {
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
      return;
    }
    handle_resolved(core, completion);
    break;
  case TRANSPORT_OPERATION_CONNECT:
    if (completion->type != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_CONNECTED ||
        completion->detail.connected.tls ||
        completion->detail.connected.ipv4_be != core->selected_ipv4_be ||
        completion->detail.connected.connection.generation == 0U) {
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
      return;
    }
    core->connection = completion->detail.connected.connection;
    core->connection_valid = true;
    core->headers_deadline_us = earlier_deadline(
        deadline_after(core->now_us, core->config.headers_timeout_us),
        core->total_deadline_us);
    if (!initialize_parser(core)) {
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_PROTOCOL, 0);
      return;
    }
    (void)emit_next_request_head(core);
    break;
  case TRANSPORT_OPERATION_WRITE:
    if (completion->type != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_WRITTEN ||
        completion->detail.written.byte_count != core->write_length ||
        completion->detail.written.connection.slot != core->connection.slot ||
        completion->detail.written.connection.generation !=
            core->connection.generation) {
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
      return;
    }
    if (core->state == CORE_WRITING_HEAD) {
      if (!core->encoder_done) {
        (void)emit_next_request_head(core);
      } else if (core->request_body_kind ==
                     POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_FIXED &&
                 core->request_body_length != 0U) {
        pocketjs_net_http_client_transport_result_t result =
            start_write(core, core->request_body, core->request_body_length,
                        CORE_WRITING_BODY);
        if (result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
          select_failure(core,
                         result ==
                                 POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT
                             ? POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT
                             : POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT,
                         (int32_t)result);
        }
      } else if (core->request_body_kind ==
                     POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_STREAMING &&
                 (!core->request_body_length_known ||
                  core->request_body_expected_length != 0U)) {
        core->state = CORE_WAITING_REQUEST_BODY;
        (void)publish_request_body_pull(core);
      } else {
        (void)begin_response_read(core);
      }
    } else if (core->state == CORE_WRITING_BODY) {
      (void)begin_response_read(core);
    } else if (core->state == CORE_WRITING_REQUEST_BODY_CHUNK) {
      core->request_body_pending_payload_length = 0U;
      if (core->request_body_length_known &&
          core->request_body_submitted_length ==
              core->request_body_expected_length) {
        (void)begin_response_read(core);
      } else {
        core->state = CORE_WAITING_REQUEST_BODY;
        (void)publish_request_body_pull(core);
      }
    } else if (core->state == CORE_WRITING_REQUEST_BODY_END) {
      (void)begin_response_read(core);
    } else {
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
    }
    break;
  case TRANSPORT_OPERATION_READ:
    if (completion->type != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_READ ||
        completion->detail.read.connection.slot != core->connection.slot ||
        completion->detail.read.connection.generation !=
            core->connection.generation ||
        completion->detail.read.byte_count > core->transport_read_maximum ||
        (completion->detail.read.byte_count != 0U &&
         completion->detail.read.lease.generation == 0U) ||
        (completion->detail.read.byte_count == 0U &&
         !completion->detail.read.eof)) {
      discard_completion_payload(core, completion);
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
      return;
    }
    if (completion->detail.read.byte_count != 0U) {
      const uint8_t *bytes = NULL;
      size_t capacity = 0U;
      core->transport_read_lease = completion->detail.read.lease;
      core->transport_read_lease_valid = true;
      core->transport_read_bytes = NULL;
      core->transport_read_length = completion->detail.read.byte_count;
      core->transport_read_offset = 0U;
      core->transport_read_eof_pending = completion->detail.read.eof;
      if (core->config.transport_ops->read_lease_view(
              core->config.transport_context, completion->detail.read.lease,
              &bytes, &capacity) != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK ||
          bytes == NULL || capacity < completion->detail.read.byte_count) {
        (void)release_transport_read_lease(core);
        select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
        return;
      }
      core->transport_read_bytes = bytes;
      core->idle_deadline_us = earlier_deadline(
          deadline_after(core->now_us, core->config.idle_timeout_us),
          core->total_deadline_us);
      consume_retained_read(core);
    } else if (completion->detail.read.eof && !core->terminal_selected &&
        !core->parser_complete) {
      pocketjs_net_http1_parse_result_t result =
          pocketjs_net_http1_response_parser_finish(&core->parser);
      if (result == POCKETJS_NET_HTTP1_PARSE_COMPLETE) {
        core->parser_complete = true;
        if (core->event_state == EVENT_EMPTY && core->body_byte_count == 0U) {
          select_success(core);
        }
      } else {
        select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_PROTOCOL,
                       (int32_t)core->parser.error);
      }
    }
    break;
  case TRANSPORT_OPERATION_CLOSE:
    if (completion->type == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_CLOSED &&
        completion->detail.closed.connection.slot == core->connection.slot &&
        completion->detail.closed.connection.generation ==
            core->connection.generation) {
      core->connection_valid = false;
    } else {
      poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_CLOSE_COMPLETION, 0);
    }
    if (completion->type != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_CLOSED ||
        completion->detail.closed.connection.slot != core->connection.slot ||
        completion->detail.closed.connection.generation !=
            core->connection.generation) {
      core->connection_valid = true;
    }
    (void)publish_terminal(core);
    break;
  case TRANSPORT_OPERATION_NONE:
  default:
    poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_STALE_COMPLETION, 0);
    if (core->state != CORE_IDLE) {
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
    }
    break;
  }
}

pocketjs_net_http_client_start_result_t pocketjs_net_http_client_core_init(
    pocketjs_net_http_client_core_storage_t *storage,
    const pocketjs_net_http_client_core_config_t *config,
    pocketjs_net_http_client_core_t **out_core) {
  if (out_core != NULL) {
    *out_core = NULL;
  }
  if (storage == NULL || out_core == NULL || !valid_config(config)) {
    return POCKETJS_NET_HTTP_CLIENT_START_INVALID_ARGUMENT;
  }
  uint64_t existing_magic = 0U;
  memcpy(&existing_magic, storage->bytes, sizeof(existing_magic));
  if (existing_magic == CORE_MAGIC) {
    return POCKETJS_NET_HTTP_CLIENT_START_BUSY;
  }
  memset(storage->bytes, 0, sizeof(storage->bytes));
  pocketjs_net_http_client_core_t *core = (void *)storage->bytes;
  core->config = *config;
  if (core->config.response_header_bytes_limit == 0U) {
    core->config.response_header_bytes_limit =
        POCKETJS_NET_HTTP_CLIENT_CORE_MAX_RESPONSE_HEADER_BYTES;
  }
  core->lifecycle_generation = 1U;
  core->magic = CORE_MAGIC;
  *out_core = core;
  return POCKETJS_NET_HTTP_CLIENT_START_OK;
}

pocketjs_net_http_client_start_result_t pocketjs_net_http_client_core_start(
    pocketjs_net_http_client_core_t *core,
    const pocketjs_net_http_client_request_t *request, uint64_t now_us) {
  if (!core_is_live(core) || request == NULL || now_us == 0U) {
    return POCKETJS_NET_HTTP_CLIENT_START_INVALID_ARGUMENT;
  }
  if (core->permission_callback_active) {
    return POCKETJS_NET_HTTP_CLIENT_START_REENTRANT;
  }
  if (core->shutdown_requested) {
    return POCKETJS_NET_HTTP_CLIENT_START_SHUTTING_DOWN;
  }
  if (core->poison_flags != 0U) {
    return POCKETJS_NET_HTTP_CLIENT_START_POISONED;
  }
  if (core->state != CORE_IDLE || core->event_state != EVENT_EMPTY) {
    return POCKETJS_NET_HTTP_CLIENT_START_BUSY;
  }
  if (core->lifecycle_generation == UINT64_MAX ||
      core->event_sequence == UINT64_MAX) {
    return POCKETJS_NET_HTTP_CLIENT_START_TOKEN_EXHAUSTED;
  }

  pocketjs_net_http_client_start_result_t result =
      snapshot_request(core, request);
  if (result != POCKETJS_NET_HTTP_CLIENT_START_OK) {
    return result;
  }
  if (core->request_body_kind ==
      POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_STREAMING) {
    ++core->last_request_body_generation;
    core->request_body_generation = core->last_request_body_generation;
  } else {
    core->request_body_generation = 0U;
  }
  core->request_body_pull_generation = 0U;
  core->last_operation_token = request->operation_token;
  core->operation_token = request->operation_token;
  ++core->lifecycle_generation;
  core->now_us = now_us;
  core->total_deadline_us =
      deadline_after(now_us, core->config.total_timeout_us);
  core->connect_deadline_us =
      earlier_deadline(deadline_after(now_us, core->config.connect_timeout_us),
                       core->total_deadline_us);
  core->headers_deadline_us = core->total_deadline_us;
  core->idle_deadline_us = core->total_deadline_us;
  core->state = CORE_RESOLVING;

  if (core->numeric_host) {
    pocketjs_net_http_client_endpoint_t endpoint = {
        .phase = POCKETJS_NET_HTTP_CLIENT_PERMISSION_NUMERIC_CANDIDATE,
        .scheme = core->scheme,
        .hostname = core->hostname,
        .port = core->port,
        .ipv4_be = core->numeric_ipv4_be,
    };
    bool allowed = false;
    if (!invoke_permission(core, &endpoint, CORE_RESOLVING, &allowed)) {
      (void)publish_terminal(core);
      return POCKETJS_NET_HTTP_CLIENT_START_OK;
    }
    if (!allowed) {
      select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_PERMISSION_DENIED, 0);
      (void)publish_terminal(core);
      return POCKETJS_NET_HTTP_CLIENT_START_OK;
    }
    pocketjs_net_http_client_transport_result_t transport_result =
        start_connect(core, core->numeric_ipv4_be);
    if (transport_result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
      select_failure(core,
                     transport_result ==
                             POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT
                         ? POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT
                         : POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT,
                     (int32_t)transport_result);
      (void)publish_terminal(core);
    }
    return POCKETJS_NET_HTTP_CLIENT_START_OK;
  }

  pocketjs_net_http_client_endpoint_t endpoint = {
      .phase = POCKETJS_NET_HTTP_CLIENT_PERMISSION_HOSTNAME,
      .scheme = core->scheme,
      .hostname = core->hostname,
      .port = core->port,
      .ipv4_be = 0U,
  };
  bool allowed = false;
  if (!invoke_permission(core, &endpoint, CORE_RESOLVING, &allowed)) {
    (void)publish_terminal(core);
    return POCKETJS_NET_HTTP_CLIENT_START_OK;
  }
  if (!allowed) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_PERMISSION_DENIED, 0);
    (void)publish_terminal(core);
    return POCKETJS_NET_HTTP_CLIENT_START_OK;
  }
  pocketjs_net_http_client_transport_result_t transport_result =
      start_resolve(core);
  if (transport_result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    select_failure(core,
                   transport_result ==
                           POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT
                       ? POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT
                       : POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT,
                   (int32_t)transport_result);
    (void)publish_terminal(core);
  }
  return POCKETJS_NET_HTTP_CLIENT_START_OK;
}

bool pocketjs_net_http_client_core_abort(
    pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_operation_token_t operation_token) {
  if (!core_public_entry_allowed(core) || core->state == CORE_IDLE ||
      operation_token != core->operation_token || core->terminal_selected) {
    return false;
  }
  select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_ABORTED, 0);
  progress_terminal(core);
  return true;
}

bool pocketjs_net_http_client_core_pump(pocketjs_net_http_client_core_t *core,
                                        uint64_t now_us,
                                        size_t max_native_steps,
                                        size_t max_transport_completions) {
  if (!core_public_entry_allowed(core) || now_us == 0U ||
      (max_native_steps == 0U && max_transport_completions == 0U)) {
    return false;
  }
  core->now_us = now_us;

  if (core->completion_retire_pending) {
    pocketjs_net_http_client_transport_result_t retry_result =
        core->config.transport_ops->retire_completion(
            core->config.transport_context, core->completion_retire_token);
    if (retry_result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
      core->completion_retire_pending = false;
      core->completion_retire_token = 0U;
    } else {
      poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_COMPLETION_RETIRE,
                  (int32_t)retry_result);
    }
  }

  if (core->transport_read_lease_valid &&
      (core->terminal_selected || core->shutdown_requested ||
       core->transport_read_offset == core->transport_read_length)) {
    (void)release_transport_read_lease(core);
  }
  (void)release_orphan_read_lease(core);

  if (core->state != CORE_IDLE && !core->terminal_selected &&
      now_us >= core->total_deadline_us) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TIMED_OUT, 0);
  }

  if (core->transport_active &&
      core->transport_operation_kind == TRANSPORT_OPERATION_CLOSE &&
      now_us >= core->close_deadline_us && !core->close_cancel_requested) {
    poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_CLOSE_TIMEOUT, 0);
    pocketjs_net_http_client_transport_result_t cancel_result =
        core->config.transport_ops->cancel(core->config.transport_context,
                                           core->transport_operation_token);
    if (cancel_result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
      core->close_cancel_requested = true;
    } else {
      poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_CANCEL,
                  (int32_t)cancel_result);
      if (core->terminal_selected) {
        (void)publish_terminal(core);
      }
    }
  }
  if (core->terminal_selected) {
    progress_terminal(core);
  }

  if (max_native_steps != 0U) {
    pocketjs_net_http_client_transport_result_t pump_result =
        core->config.transport_ops->pump(core->config.transport_context, now_us,
                                         max_native_steps);
    if (pump_result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
      poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_TRANSPORT_PUMP,
                  (int32_t)pump_result);
      if (core->state != CORE_IDLE && !core->terminal_selected) {
        select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT,
                       (int32_t)pump_result);
      }
    }
  }

  for (size_t index = 0U;
       index < max_transport_completions &&
       !core->completion_retire_pending &&
       !core->transport_read_lease_valid &&
       !core->orphan_read_lease_valid;
       ++index) {
    pocketjs_net_http_client_transport_completion_t completion;
    pocketjs_net_http_client_transport_result_t take_result =
        core->config.transport_ops->take_completion(
            core->config.transport_context, &completion);
    if (take_result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_EMPTY) {
      break;
    }
    if (take_result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
      poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_COMPLETION_TAKE,
                  (int32_t)take_result);
      if (core->state != CORE_IDLE && !core->terminal_selected) {
        select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT,
                       (int32_t)take_result);
      }
      break;
    }
    if (!core->transport_active ||
        completion.operation_token != core->transport_operation_token) {
      discard_completion_payload(core, &completion);
      poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_STALE_COMPLETION, 0);
      if (core->state != CORE_IDLE && !core->terminal_selected) {
        select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
      }
    } else {
      handle_transport_completion(core, &completion);
    }
    pocketjs_net_http_client_transport_result_t retire_result =
        core->config.transport_ops->retire_completion(
            core->config.transport_context, completion.operation_token);
    if (retire_result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
      core->completion_retire_pending = true;
      core->completion_retire_token = completion.operation_token;
      poison_core(core, POCKETJS_NET_HTTP_CLIENT_POISON_COMPLETION_RETIRE,
                  (int32_t)retire_result);
      if (core->state != CORE_IDLE && !core->terminal_selected) {
        select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT, 0);
      }
      break;
    }
  }

  if (!core->terminal_selected && core->state == CORE_READING &&
      core->event_state == EVENT_EMPTY) {
    consume_retained_read(core);
    if (!core->terminal_selected && core->event_state == EVENT_EMPTY &&
        !core->transport_read_lease_valid && core->poison_flags == 0U) {
      (void)start_read(core);
    }
  }
  if (core->parser_complete && !core->terminal_selected &&
      core->event_state == EVENT_EMPTY && !core->body_lease_active) {
    select_success(core);
  }
  progress_terminal(core);
  return true;
}

bool pocketjs_net_http_client_core_grant_body_credit(
    pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_operation_token_t operation_token,
    size_t maximum_bytes) {
  if (!core_public_entry_allowed(core) ||
      operation_token != core->operation_token ||
      !core->headers_delivered || core->terminal_selected ||
      core->event_state != EVENT_EMPTY || core->body_lease_active ||
      core->body_credit != 0U || maximum_bytes == 0U ||
      maximum_bytes > POCKETJS_NET_HTTP_CLIENT_CORE_BODY_LEASE_BYTES) {
    return false;
  }
  core->body_credit = maximum_bytes;
  core->body_byte_count = 0U;
  return true;
}

static bool request_body_credit_matches(
    const pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_operation_token_t operation_token,
    uint64_t body_generation, uint64_t pull_generation) {
  return core->request_body_kind ==
             POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_STREAMING &&
         core->state == CORE_WAITING_REQUEST_BODY &&
         !core->terminal_selected && core->event_state == EVENT_EMPTY &&
         core->request_body_pull_active &&
         core->request_body_pull_event_retired &&
         operation_token == core->operation_token &&
         body_generation != 0U &&
         body_generation == core->request_body_generation &&
         pull_generation != 0U &&
         pull_generation == core->request_body_pull_generation;
}

static void consume_request_body_credit(
    pocketjs_net_http_client_core_t *core) {
  core->request_body_pull_active = false;
  core->request_body_pull_event_retired = false;
  core->request_body_pull_maximum = 0U;
}

static size_t encode_chunked_request_body(
    pocketjs_net_http_client_core_t *core, size_t payload_length) {
  static const uint8_t hex[] = "0123456789abcdef";
  uint8_t reversed[sizeof(size_t) * 2U];
  size_t reversed_length = 0U;
  size_t value = payload_length;
  do {
    reversed[reversed_length++] = hex[value & 0xfU];
    value >>= 4U;
  } while (value != 0U);
  size_t offset = 0U;
  while (reversed_length != 0U) {
    core->write_bytes[offset++] = reversed[--reversed_length];
  }
  core->write_bytes[offset++] = '\r';
  core->write_bytes[offset++] = '\n';
  memmove(core->write_bytes + offset, core->request_body, payload_length);
  offset += payload_length;
  core->write_bytes[offset++] = '\r';
  core->write_bytes[offset++] = '\n';
  return offset;
}

bool pocketjs_net_http_client_core_submit_request_body_chunk(
    pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_operation_token_t operation_token,
    uint64_t body_generation, uint64_t pull_generation,
    const uint8_t *bytes, size_t length) {
  if (!core_public_entry_allowed(core) || bytes == NULL || length == 0U ||
      length > POCKETJS_NET_HTTP_CLIENT_CORE_REQUEST_BODY_CHUNK_BYTES ||
      !request_body_credit_matches(core, operation_token, body_generation,
                                   pull_generation) ||
      length > core->request_body_pull_maximum) {
    return false;
  }
  if (core->request_body_length_known &&
      (core->request_body_submitted_length >
           core->request_body_expected_length ||
       (uint64_t)length > core->request_body_expected_length -
                              core->request_body_submitted_length)) {
    return false;
  }
  memmove(core->request_body, bytes, length);
  consume_request_body_credit(core);
  if ((uint64_t)length >
      UINT64_MAX - core->request_body_submitted_length) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT, 0);
    progress_terminal(core);
    return true;
  }
  core->request_body_submitted_length += (uint64_t)length;
  core->request_body_pending_payload_length = length;
  const uint8_t *wire_bytes = core->request_body;
  size_t wire_length = length;
  if (!core->request_body_length_known) {
    wire_length = encode_chunked_request_body(core, length);
    wire_bytes = core->write_bytes;
  }
  pocketjs_net_http_client_transport_result_t result = start_write(
      core, wire_bytes, wire_length, CORE_WRITING_REQUEST_BODY_CHUNK);
  if (result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    select_failure(core,
                   result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT
                       ? POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT
                       : POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT,
                   (int32_t)result);
    progress_terminal(core);
  }
  return true;
}

bool pocketjs_net_http_client_core_submit_request_body_end(
    pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_operation_token_t operation_token,
    uint64_t body_generation, uint64_t pull_generation) {
  if (!core_public_entry_allowed(core) ||
      !request_body_credit_matches(core, operation_token, body_generation,
                                   pull_generation)) {
    return false;
  }
  consume_request_body_credit(core);
  if (core->request_body_length_known) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_REQUEST_BODY,
                   POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_CAUSE_LENGTH_UNDERFLOW);
    progress_terminal(core);
    return true;
  }
  static const uint8_t terminal_chunk[] = "0\r\n\r\n";
  memcpy(core->write_bytes, terminal_chunk, sizeof(terminal_chunk) - 1U);
  pocketjs_net_http_client_transport_result_t result = start_write(
      core, core->write_bytes, sizeof(terminal_chunk) - 1U,
      CORE_WRITING_REQUEST_BODY_END);
  if (result != POCKETJS_NET_HTTP_CLIENT_TRANSPORT_OK) {
    select_failure(core,
                   result == POCKETJS_NET_HTTP_CLIENT_TRANSPORT_RESOURCE_LIMIT
                       ? POCKETJS_NET_HTTP_CLIENT_ERROR_RESOURCE_LIMIT
                       : POCKETJS_NET_HTTP_CLIENT_ERROR_TRANSPORT,
                   (int32_t)result);
    progress_terminal(core);
  }
  return true;
}

bool pocketjs_net_http_client_core_submit_request_body_error(
    pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_operation_token_t operation_token,
    uint64_t body_generation, uint64_t pull_generation,
    int32_t cause_code) {
  if (!core_public_entry_allowed(core) ||
      !request_body_credit_matches(core, operation_token, body_generation,
                                   pull_generation)) {
    return false;
  }
  consume_request_body_credit(core);
  select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_REQUEST_BODY,
                 cause_code);
  progress_terminal(core);
  return true;
}

bool pocketjs_net_http_client_core_take_event(
    pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_event_t *out_event) {
  if (!core_public_entry_allowed(core) || out_event == NULL ||
      core->event_state != EVENT_PENDING) {
    return false;
  }
  *out_event = core->event;
  core->event_state = EVENT_DELIVERING;
  return true;
}

static void reset_after_terminal(pocketjs_net_http_client_core_t *core) {
  core->state = CORE_IDLE;
  core->operation_token = 0U;
  core->request_body_kind = POCKETJS_NET_HTTP_CLIENT_REQUEST_BODY_NONE;
  core->request_body_length = 0U;
  core->request_body_length_known = false;
  core->request_body_expected_length = 0U;
  core->request_body_submitted_length = 0U;
  core->request_body_generation = 0U;
  core->request_body_pull_generation = 0U;
  core->request_body_pull_maximum = 0U;
  core->request_body_pending_payload_length = 0U;
  core->request_body_pull_active = false;
  core->request_body_pull_event_retired = false;
  core->body_credit = 0U;
  core->body_byte_count = 0U;
  core->body_lease_active = false;
  core->body_lease_released = false;
  core->terminal_selected = false;
  core->terminal_success = false;
  core->terminal_error = 0;
  core->terminal_cause = 0;
  core->final_headers_seen = false;
  core->headers_delivered = false;
  core->parser_complete = false;
  core->force_no_body = false;
  core->callback_error = 0;
}

bool pocketjs_net_http_client_core_retire_event(
    pocketjs_net_http_client_core_t *core, uint64_t sequence) {
  if (!core_public_entry_allowed(core) ||
      core->event_state != EVENT_DELIVERING ||
      sequence != core->event.sequence ||
      (core->event.type == POCKETJS_NET_HTTP_CLIENT_EVENT_BODY &&
       !core->body_lease_released) ||
      (core->event.type ==
           POCKETJS_NET_HTTP_CLIENT_EVENT_REQUEST_BODY_PULL &&
       (!core->request_body_pull_active ||
        core->event.detail.request_body_pull.body_generation !=
            core->request_body_generation ||
        core->event.detail.request_body_pull.pull_generation !=
            core->request_body_pull_generation))) {
    return false;
  }
  pocketjs_net_http_client_event_type_t type = core->event.type;
  core->event_state = EVENT_EMPTY;
  if (type == POCKETJS_NET_HTTP_CLIENT_EVENT_RESPONSE_HEADERS) {
    core->headers_delivered = true;
  } else if (type == POCKETJS_NET_HTTP_CLIENT_EVENT_BODY) {
    core->body_lease_active = false;
    core->body_lease_released = false;
    core->body_byte_count = 0U;
    core->body_credit = 0U;
  } else if (type ==
             POCKETJS_NET_HTTP_CLIENT_EVENT_REQUEST_BODY_PULL) {
    core->request_body_pull_event_retired = true;
    return true;
  } else {
    reset_after_terminal(core);
    return true;
  }
  if (core->parser_complete && !core->body_lease_active) {
    select_success(core);
  }
  progress_terminal(core);
  return true;
}

bool pocketjs_net_http_client_core_body_lease_view(
    pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_body_lease_t lease, const uint8_t **out_bytes,
    size_t *out_length) {
  if (out_bytes != NULL) {
    *out_bytes = NULL;
  }
  if (out_length != NULL) {
    *out_length = 0U;
  }
  if (!core_public_entry_allowed(core) || out_bytes == NULL ||
      out_length == NULL ||
      !core->body_lease_active || core->body_lease_released ||
      lease.slot != core->body_lease.slot ||
      lease.generation != core->body_lease.generation) {
    return false;
  }
  *out_bytes = core->body_bytes;
  *out_length = core->body_byte_count;
  return true;
}

bool pocketjs_net_http_client_core_release_body_lease(
    pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_body_lease_t lease) {
  if (!core_public_entry_allowed(core) || !core->body_lease_active ||
      core->body_lease_released ||
      lease.slot != core->body_lease.slot ||
      lease.generation != core->body_lease.generation) {
    return false;
  }
  core->body_lease_released = true;
  return true;
}

bool pocketjs_net_http_client_core_get_status(
    const pocketjs_net_http_client_core_t *core,
    pocketjs_net_http_client_core_status_t *out_status) {
  if (!core_public_entry_allowed(core) || out_status == NULL) {
    return false;
  }
  *out_status = (pocketjs_net_http_client_core_status_t){
      .initialized = true,
      .shutdown_requested = core->shutdown_requested,
      .poisoned = core->poison_flags != 0U,
      .quiescent = core_is_quiescent_internal(core),
      .request_active = core->operation_token != 0U,
      .transport_operation_active = core->transport_active,
      .connection_owned = core->connection_valid,
      .completion_retire_pending = core->completion_retire_pending,
      .event_outstanding = core->event_state != EVENT_EMPTY,
      .request_body_credit_outstanding = core->request_body_pull_active,
      .transport_read_leases_owned =
          owned_transport_read_lease_count(core),
      .poison_flags = core->poison_flags,
      .first_poison_cause_code = core->first_poison_cause_code,
      .lifecycle_generation = core->lifecycle_generation,
      .operation_token = core->operation_token,
      .request_body_generation = core->request_body_generation,
      .request_body_pull_generation =
          core->request_body_pull_generation,
  };
  return true;
}

bool pocketjs_net_http_client_core_begin_shutdown(
    pocketjs_net_http_client_core_t *core, uint64_t now_us) {
  if (!core_public_entry_allowed(core) || now_us == 0U) {
    return false;
  }
  core->shutdown_requested = true;
  core->now_us = now_us;
  if (core->operation_token != 0U && !core->terminal_selected) {
    select_failure(core, POCKETJS_NET_HTTP_CLIENT_ERROR_ABORTED, 0);
  }
  progress_terminal(core);
  return true;
}

bool pocketjs_net_http_client_core_is_quiescent(
    const pocketjs_net_http_client_core_t *core) {
  return core_public_entry_allowed(core) && core_is_quiescent_internal(core);
}

bool pocketjs_net_http_client_core_confirm_transport_shutdown(
    pocketjs_net_http_client_core_t *core) {
  if (!core_public_entry_allowed(core) || !core->shutdown_requested) {
    return false;
  }
  core->transport_active = false;
  core->transport_operation_kind = TRANSPORT_OPERATION_NONE;
  core->transport_operation_token = 0U;
  core->transport_cancel_requested = false;
  core->close_cancel_requested = false;
  core->connection_valid = false;
  core->transport_read_lease_valid = false;
  core->transport_read_bytes = NULL;
  core->transport_read_length = 0U;
  core->transport_read_offset = 0U;
  core->transport_read_eof_pending = false;
  core->orphan_read_lease_valid = false;
  core->completion_retire_pending = false;
  core->completion_retire_token = 0U;
  progress_terminal(core);
  return true;
}

bool pocketjs_net_http_client_core_deinit(
    pocketjs_net_http_client_core_t *core) {
  if (!core_public_entry_allowed(core) || !core->shutdown_requested ||
      !core_is_quiescent_internal(core)) {
    return false;
  }
  memset(core, 0, sizeof(*core));
  return true;
}
