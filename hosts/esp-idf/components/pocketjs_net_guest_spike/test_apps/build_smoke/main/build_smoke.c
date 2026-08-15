// SPDX-License-Identifier: MIT

#include <assert.h>

#include "esp_err.h"
#include "pocketjs/net/guest_spike.h"

void app_main(void) {
  pocketjs_net_guest_spike_result_t result;
  const pocketjs_net_guest_spike_config_t missing_origin = {
      .device_id = "build-smoke",
      .worker_core = tskNO_AFFINITY,
  };
  assert(pocketjs_net_guest_spike_run(&missing_origin, &result) ==
         ESP_ERR_INVALID_ARG);

  const pocketjs_net_guest_spike_config_t too_many_rounds = {
      .base_url = "http://127.0.0.1:8088",
      .device_id = "build-smoke",
      .rounds = POCKETJS_NET_GUEST_SPIKE_MAX_ROUNDS + 1U,
      .worker_core = tskNO_AFFINITY,
  };
  assert(pocketjs_net_guest_spike_run(&too_many_rounds, &result) ==
         ESP_ERR_INVALID_ARG);
}
