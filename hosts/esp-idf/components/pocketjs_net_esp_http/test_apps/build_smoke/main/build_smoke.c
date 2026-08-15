// SPDX-License-Identifier: MIT

#include <assert.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "pocketjs/net/esp_http_client_backend.h"

void app_main(void) {
  const pocketjs_net_esp_http_backend_descriptor_t *descriptor =
      pocketjs_net_esp_http_backend_descriptor();
  assert(descriptor != NULL);
  assert(descriptor->response_streaming);
  assert(descriptor->duplicate_response_headers);
  assert(descriptor->manual_connection_close);
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
}
