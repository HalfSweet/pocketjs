/* Internal shape of the ESP-IDF host. */
#ifndef POCKETJS_ESP_HOST_INTERNAL_H
#define POCKETJS_ESP_HOST_INTERNAL_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pnet_posix_driver.h"
#include "pocketjs/esp_host.h"
#include "pocketjs/net/esp_tls_provider.h"

struct pocketjs_esp_host {
  pocketjs_esp_host_config cfg;
  const char *bundle;
  size_t bundle_len;
  /* guest */
  JSRuntime *rt;
  JSContext *ctx;
  JSValue frame_fn;
  size_t guest_heap;
  size_t guest_heap_high_water;
  TaskHandle_t guest_task;
  /* network */
  pnet_runtime *net;
  pnet_posix_driver *driver;
  pnet_esp_tls *tls_provider;
  SemaphoreHandle_t net_lock;
  TaskHandle_t net_task;
  volatile bool net_dirty; /* an op ran during this frame: wake the network task */
  /* lifecycle */
  volatile bool stopping;
  volatile bool guest_done;
  volatile bool net_done;
  pocketjs_esp_host_stats_t stats;
};

/* net_binding.c */
void pocketjs_host_mount_network(pocketjs_esp_host_t *host);

/* host.c helpers used by the binding */
static inline void pocketjs_host_net_lock(pocketjs_esp_host_t *host) {
  xSemaphoreTake(host->net_lock, portMAX_DELAY);
}
static inline void pocketjs_host_net_unlock(pocketjs_esp_host_t *host) {
  xSemaphoreGive(host->net_lock);
}

#endif
