/* WebSocket Client core — placeholder to be replaced. */
#include "pnet_internal.h"
void pnet_ws_init(pnet_runtime *rt) { (void)rt; }
void pnet_ws_shutdown(pnet_runtime *rt) { (void)rt; }
void pnet_ws_service(pnet_runtime *rt) { (void)rt; }
void pnet_ws_freeze(pnet_runtime *rt) { (void)rt; }
uint64_t pnet_ws_next_deadline(pnet_runtime *rt) { (void)rt; return 0; }
bool pnet_ws_has_output(pnet_runtime *rt) { (void)rt; return false; }
void pnet_ws_quiesce(pnet_runtime *rt) { (void)rt; }
int pnet_ws_connect(pnet_runtime *rt, const char *meta_json) { (void)meta_json; pnet_set_last_error(rt, &rt->ws_last_error, PNET_ERROR_UNAVAILABLE, "ws not built"); return -1; }
int pnet_ws_send(pnet_runtime *rt, int handle, int opcode, const uint8_t *payload, size_t len) { (void)rt;(void)handle;(void)opcode;(void)payload;(void)len; return PWS_SEND_CLOSED; }
int pnet_ws_receive_into(pnet_runtime *rt, int handle, uint8_t *dst, size_t len) { (void)rt;(void)handle;(void)dst;(void)len; return -1; }
int pnet_ws_close(pnet_runtime *rt, int handle, int code, const char *reason, size_t reason_len) { (void)rt;(void)handle;(void)code;(void)reason;(void)reason_len; return -1; }
void pnet_ws_terminate(pnet_runtime *rt, int handle) { (void)rt;(void)handle; }
int pnet_ws_buffered_amount(pnet_runtime *rt, int handle) { (void)rt;(void)handle; return -1; }
const char *pnet_ws_poll(pnet_runtime *rt, size_t *len) { return pnet_queue_poll(rt, &rt->ws_queue, len); }
const char *pnet_ws_last_error(pnet_runtime *rt) { return pnet_sb_cstr(&rt->ws_last_error); }
const char *pnet_ws_limits(pnet_runtime *rt) { (void)rt; return "{}"; }
