/* net-smoke firmware: bring Wi-Fi up, start the PocketJS host with the
 * network modules, evaluate the embedded smoke bundle, report stats. The
 * peer/workstation addresses and credentials come from Kconfig
 * (main/Kconfig.projbuild). */
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pocketjs/board.h"
#include "pocketjs/esp_host.h"
#include "sdkconfig.h"

static const char *TAG = "smoke";

extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");

static char s_self_ip[16] = "0.0.0.0";
static char s_policy[1024];

/* Every endpoint the smoke touches must be an explicit connect/listen rule:
 * the workstation peer's HTTP and
 * WebSocket ports, the peer board's HTTP port, plus the same host on the
 * port after the WebSocket one so the "connection refused" case is a real
 * refusal and not a permission denial. Everything here is plaintext on the LAN, so
 * insecureTransport and localNetwork are on. */
static void build_policy(void) {
  char rules[768] = "";
  size_t used = 0;
  if (CONFIG_SMOKE_MAC_HOST[0]) {
    used += snprintf(rules + used, sizeof rules - used,
                     "{\"protocol\":\"http\",\"host\":\"%s\",\"port\":{\"min\":%d,\"max\":%d}},"
                     "{\"protocol\":\"ws\",\"host\":\"%s\",\"port\":%d},",
                     CONFIG_SMOKE_MAC_HOST, CONFIG_SMOKE_MAC_HTTP_PORT, CONFIG_SMOKE_MAC_HTTP_PORT + 2, CONFIG_SMOKE_MAC_HOST,
                     CONFIG_SMOKE_MAC_WS_PORT);
  }
  if (CONFIG_SMOKE_PEER_HOST[0]) {
    used += snprintf(rules + used, sizeof rules - used, "{\"protocol\":\"http\",\"host\":\"%s\",\"port\":%d},",
                     CONFIG_SMOKE_PEER_HOST, CONFIG_SMOKE_PEER_PORT);
  }
#if CONFIG_SMOKE_ENABLE_TLS
  /* Public HTTPS endpoints for the base-TLS gate: one positive control plus
   * badssl.com's negative certificates (expired, wrong host, self-signed,
   * untrusted root). host trust comes from the IDF certificate bundle. */
  used += snprintf(rules + used, sizeof rules - used,
                   "{\"protocol\":\"https\",\"host\":\"%s\",\"port\":443},"
                   "{\"protocol\":\"https\",\"host\":\"expired.badssl.com\",\"port\":443},"
                   "{\"protocol\":\"https\",\"host\":\"wrong.host.badssl.com\",\"port\":443},"
                   "{\"protocol\":\"https\",\"host\":\"self-signed.badssl.com\",\"port\":443},"
                   "{\"protocol\":\"https\",\"host\":\"untrusted-root.badssl.com\",\"port\":443},",
                   CONFIG_SMOKE_TLS_HOST);
#endif
  if (used > 0 && rules[used - 1] == ',') rules[used - 1] = 0;
  snprintf(s_policy, sizeof s_policy,
           "{\"connect\":[%s],\"listen\":[{\"protocol\":\"http\",\"address\":\"0.0.0.0\",\"port\":%d}],"
           "\"credentials\":[],\"insecureTransport\":true,\"localNetwork\":true,\"allowInvalidTlsForDevelopment\":false}",
           rules, CONFIG_SMOKE_SERVE_PORT);
}

static void install_smoke_config(JSContext *ctx, void *user) {
  (void)user;
  char json[512];
  snprintf(json, sizeof json,
           "({\"board\":\"%s\",\"selfIp\":\"%s\",\"peerHost\":\"%s\",\"peerPort\":%d,\"macHost\":\"%s\",\"macPort\":%d,"
           "\"macWsPort\":%d,\"ping\":%s,\"tls\":%s,\"tlsHost\":\"%s\"})",
           CONFIG_SMOKE_BOARD_NAME, s_self_ip, CONFIG_SMOKE_PEER_HOST, CONFIG_SMOKE_PEER_PORT, CONFIG_SMOKE_MAC_HOST,
           CONFIG_SMOKE_MAC_HTTP_PORT, CONFIG_SMOKE_MAC_WS_PORT, CONFIG_SMOKE_PEER_PING ? "true" : "false",
#if CONFIG_SMOKE_ENABLE_TLS
           "true", CONFIG_SMOKE_TLS_HOST);
#else
           "false", "");
#endif
  JSValue value = JS_Eval(ctx, json, strlen(json), "smoke-config", JS_EVAL_TYPE_GLOBAL);
  JSValue global = JS_GetGlobalObject(ctx);
  JS_SetPropertyStr(ctx, global, "__pocketSmoke", value);
  JS_FreeValue(ctx, global);
}

static void report(uint32_t frame, void *user) {
  pocketjs_esp_host_t **host = user;
  if (frame % (60 * 30) != 0 || !*host) return; /* every 30 s at 60 Hz */
  pocketjs_esp_host_stats_t st;
  pocketjs_esp_host_stats(*host, &st);
  ESP_LOGI(TAG, "frames=%u jobs=%u frameErrors=%u frameMax=%uus guestHeap=%u/%u netHeap=%u sockets=%d freeInternal=%u freePsram=%u",
           (unsigned)st.frames, (unsigned)st.jobs, (unsigned)st.frame_errors, (unsigned)st.frame_max_us,
           (unsigned)st.guest_heap_bytes, (unsigned)st.guest_heap_high_water, (unsigned)st.net_heap_bytes, st.net_sockets,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static pocketjs_esp_host_t *s_host;

void app_main(void) {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  ESP_LOGI(TAG, "%s (%s, rev v%d.%d) free internal %u, psram %u", CONFIG_SMOKE_BOARD_NAME, CONFIG_IDF_TARGET,
           chip.revision / 100, chip.revision % 100, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  pocketjs_board_wifi_config wifi = {.ssid = CONFIG_SMOKE_WIFI_SSID, .password = CONFIG_SMOKE_WIFI_PASSWORD, .timeout_ms = 60000};
  esp_ip4_addr_t ip;
  while (pocketjs_board_wifi_connect(&wifi, &ip) != ESP_OK) {
    ESP_LOGW(TAG, "retrying Wi-Fi");
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  pocketjs_board_ip_text(s_self_ip, sizeof s_self_ip);
  ESP_LOGI(TAG, "station ip %s, serving http://%s:%d/", s_self_ip, s_self_ip, CONFIG_SMOKE_SERVE_PORT);
#if CONFIG_SMOKE_ENABLE_TLS
  if (pocketjs_board_sync_time(20000) != ESP_OK) ESP_LOGW(TAG, "TLS certificate validity may fail without a synced clock");
#endif
  build_policy();
  ESP_LOGI(TAG, "policy %s", s_policy);

  pocketjs_esp_host_config cfg;
  pocketjs_esp_host_config_defaults(&cfg);
  cfg.tick_hz = CONFIG_SMOKE_TICK_HZ;
  cfg.network_policy_json = s_policy;
#if CONFIG_SMOKE_ENABLE_TLS
  cfg.network_tls = true;
#endif
  cfg.guest_in_psram = true;
  cfg.guest_memory_limit = CONFIG_SMOKE_GUEST_MEMORY_KB * 1024;
  cfg.guest_task_stack = CONFIG_SMOKE_GUEST_STACK_KB * 1024;
  cfg.before_eval = install_smoke_config;
  cfg.after_frame = report;
  cfg.user = &s_host;
  size_t bundle_len = (size_t)(app_js_end - app_js_start);
  if (bundle_len > 0 && app_js_start[bundle_len - 1] == 0) bundle_len--; /* EMBED_TXTFILES adds a NUL */
  ESP_LOGI(TAG, "starting the guest with a %u byte bundle", (unsigned)bundle_len);
  ESP_ERROR_CHECK(pocketjs_esp_host_start(&cfg, app_js_start, bundle_len, &s_host));
}
