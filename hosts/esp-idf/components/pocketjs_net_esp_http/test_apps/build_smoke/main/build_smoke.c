// SPDX-License-Identifier: MIT

#include <assert.h>
#include <errno.h>

#include "esp_err.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"
#include "pocketjs/net/esp_http_client_backend.h"

static bool untrusted_wall_clock(void *context) {
  (void)context;
  return false;
}

static const uint8_t valid_host_ca[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBrDCCAVKgAwIBAgIUNC8Vf/q+01GRjtS6kpda47jALKkwCgYIKoZIzj0EAwIw\n"
    "IjEgMB4GA1UEAwwXUG9ja2V0SlMgQnVpbGQgU21va2UgQ0EwHhcNMjYwODE1MDYy\n"
    "NDIwWhcNMzYwODEyMDYyNDIwWjAiMSAwHgYDVQQDDBdQb2NrZXRKUyBCdWlsZCBT\n"
    "bW9rZSBDQTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABAklmonztskTI5CpNMcz\n"
    "WvIq4o/E8QgxkY0LSTiQHAyiKCFquopgzkpqt0MZrW4/9RazvAI413ANwACRpGcN\n"
    "kgajZjBkMB0GA1UdDgQWBBQ7MuQ9BVr1fntpdHRs1/8/P8NuOTAfBgNVHSMEGDAW\n"
    "gBQ7MuQ9BVr1fntpdHRs1/8/P8NuOTASBgNVHRMBAf8ECDAGAQH/AgEAMA4GA1Ud\n"
    "DwEB/wQEAwIBBjAKBggqhkjOPQQDAgNIADBFAiA6bsEVTBNLBAy2WEkVt+dNgL2p\n"
    "+syNtTq4tQYH+qU9QQIhAJlie22e/RJwY84Ts3a+mpTXmM/0h7zxY6jxx8AiHQ3K\n"
    "-----END CERTIFICATE-----\n";

void app_main(void) {
  const pocketjs_net_esp_http_backend_descriptor_t *descriptor =
      pocketjs_net_esp_http_backend_descriptor();
  assert(descriptor != NULL);
  assert(descriptor->response_streaming);
  assert(descriptor->duplicate_response_headers);
  assert(descriptor->manual_connection_close);
  assert(descriptor->internal_tls_compiled);
  assert(descriptor->internal_tls.compiled);
  assert(descriptor->internal_tls.tls_1_2);
  assert(!descriptor->internal_tls.tls_1_3);
  assert(descriptor->internal_tls.host_trust);
  assert(descriptor->internal_tls.hostname_verification);
  assert(descriptor->internal_tls.sni);
  assert(descriptor->internal_tls.certificate_bundle);
  assert(descriptor->internal_tls.host_pinned_ca);
  assert(!descriptor->internal_tls.custom_ca_append);
  assert(!descriptor->internal_tls.client_auth);
  assert(!descriptor->internal_tls.custom_alpn);
  assert(!descriptor->internal_tls.revocation);
  assert(!descriptor->internal_tls.insecure_verification);
  assert(!descriptor->internal_tls.plaintext_fallback);
  assert(!descriptor->internal_tls.bounded_handshake_memory);
  assert(descriptor->internal_tls.max_host_pinned_ca_bytes ==
         POCKETJS_NET_ESP_HTTP_MAX_HOST_PINNED_CA_PEM_BYTES);
  assert(descriptor->internal_tls.max_host_pinned_ca_certificates == 1U);
  assert(descriptor->internal_tls.max_custom_ca_bytes == 0);
  assert(descriptor->internal_tls.max_custom_ca_certificates == 0);
  assert(descriptor->hard_limits.native_tx_buffer_bytes ==
         POCKETJS_NET_ESP_HTTP_TX_BUFFER_BYTES);
  assert(!descriptor->bounded_response_parser);
  assert(!descriptor->arbitrary_method);
  assert(!descriptor->wire_status_text);
  assert(!descriptor->cancel_during_connect);
  assert(!descriptor->response_trailers);

  pocketjs_net_esp_http_client_config_t config = {
      .worker_core = tskNO_AFFINITY,
  };
  pocketjs_net_esp_http_client_t *client = NULL;

  config.worker_priority = (UBaseType_t)configMAX_PRIORITIES;
  assert(pocketjs_net_esp_http_client_create(&config, &client) ==
         ESP_ERR_INVALID_ARG);
  assert(client == NULL);

  /* Zero retains its documented default-priority meaning. */
  config.worker_priority = 0;
  ESP_ERROR_CHECK(pocketjs_net_esp_http_client_create(&config, &client));
  assert(client != NULL);

  pocketjs_net_esp_http_request_t zero_token_request = {
      .operation_id = 0,
      .url = "http://127.0.0.1/",
      .method = "GET",
  };
  assert(pocketjs_net_esp_http_client_start(client, &zero_token_request) ==
         ESP_ERR_INVALID_ARG);

  pocketjs_net_esp_http_client_destroy(client);

  config.allow_https = true;
  assert(pocketjs_net_esp_http_client_create(&config, &client) ==
         ESP_ERR_INVALID_ARG);
  assert(client == NULL);

  config.tls_trust_source = POCKETJS_NET_ESP_HTTP_TLS_TRUST_CERTIFICATE_BUNDLE;
  assert(pocketjs_net_esp_http_client_create(&config, &client) ==
         ESP_ERR_INVALID_ARG);
  assert(client == NULL);

  static const uint8_t invalid_ca[] = "not a CA certificate";
  config.tls_trust_source = POCKETJS_NET_ESP_HTTP_TLS_TRUST_HOST_PINNED_CA_PEM;
  config.host_pinned_ca_pem = invalid_ca;
  config.host_pinned_ca_pem_bytes = sizeof(invalid_ca) - 1U;
  config.wall_clock_trusted = untrusted_wall_clock;
  assert(pocketjs_net_esp_http_client_create(&config, &client) ==
         ESP_ERR_INVALID_ARG);
  assert(client == NULL);

  config.host_pinned_ca_pem = valid_host_ca;
  config.host_pinned_ca_pem_bytes = sizeof(valid_host_ca) - 1U;
  ESP_ERROR_CHECK(pocketjs_net_esp_http_client_create(&config, &client));
  assert(client != NULL);
  pocketjs_net_esp_http_client_destroy(client);
  client = NULL;

  config.tls_trust_source = POCKETJS_NET_ESP_HTTP_TLS_TRUST_CERTIFICATE_BUNDLE;
  config.host_pinned_ca_pem = NULL;
  config.host_pinned_ca_pem_bytes = 0;
  ESP_ERROR_CHECK(pocketjs_net_esp_http_client_create(&config, &client));

  pocketjs_net_esp_http_request_t untrusted_clock_request = {
      .operation_id = 1,
      .url = "https://example.com/",
      .method = "GET",
  };
  ESP_ERROR_CHECK(
      pocketjs_net_esp_http_client_start(client, &untrusted_clock_request));
  pocketjs_net_esp_http_event_t event;
  ESP_ERROR_CHECK(
      pocketjs_net_esp_http_client_receive(client, &event, portMAX_DELAY));
  assert(event.type == POCKETJS_NET_ESP_HTTP_EVENT_ERROR);
  assert(event.operation_id == 1);
  assert(event.detail.error.code == POCKETJS_NET_ERROR_TLS_CERTIFICATE_INVALID);
  assert(event.detail.error.tls_code == 0);
  assert(event.detail.error.tls_certificate_flags == 0);
  assert(!event.detail.error.temporary);
  pocketjs_net_esp_http_client_destroy(client);

  assert(pocketjs_net_error_code_name(
             POCKETJS_NET_ERROR_TLS_VERSION_UNSUPPORTED) != NULL);
  assert(pocketjs_net_error_code_name(POCKETJS_NET_ERROR_TLS_ALERT) != NULL);

  assert(pocketjs_net_esp_http_map_native_error(
             ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED, 0,
             -MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION, 0,
             true) == POCKETJS_NET_ERROR_TLS_VERSION_UNSUPPORTED);
  assert(pocketjs_net_esp_http_map_native_error(
             ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED, 0,
             -MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE, 0,
             true) == POCKETJS_NET_ERROR_TLS_ALERT);
  assert(pocketjs_net_esp_http_map_native_error(
             ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED, 0,
             -MBEDTLS_ERR_SSL_ALLOC_FAILED, 0,
             true) == POCKETJS_NET_ERROR_RESOURCE_LIMIT);
  assert(pocketjs_net_esp_http_map_native_error(
             ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED, 0, -MBEDTLS_ERR_SSL_TIMEOUT,
             0, true) == POCKETJS_NET_ERROR_TIMED_OUT);
  assert(pocketjs_net_esp_http_map_native_error(
             ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED, 0, 0,
             MBEDTLS_X509_BADCERT_CN_MISMATCH,
             true) == POCKETJS_NET_ERROR_TLS_HOSTNAME_MISMATCH);
  assert(pocketjs_net_esp_http_map_native_error(
             ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED, 0, 0,
             MBEDTLS_X509_BADCERT_NOT_TRUSTED,
             true) == POCKETJS_NET_ERROR_TLS_CERTIFICATE_INVALID);
  assert(pocketjs_net_esp_http_map_native_error(
             ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST, ECONNREFUSED, 0, 0,
             true) == POCKETJS_NET_ERROR_CONNECTION_REFUSED);
}
