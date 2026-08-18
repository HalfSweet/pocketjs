/* HTTP Server core — placeholder to be replaced. */
#include "pnet_internal.h"
void pnet_httpd_init(pnet_runtime *rt) { (void)rt; }
void pnet_httpd_shutdown(pnet_runtime *rt) { (void)rt; }
void pnet_httpd_service(pnet_runtime *rt) { (void)rt; }
void pnet_httpd_freeze(pnet_runtime *rt) { (void)rt; }
uint64_t pnet_httpd_next_deadline(pnet_runtime *rt) { (void)rt; return 0; }
bool pnet_httpd_has_output(pnet_runtime *rt) { (void)rt; return false; }
void pnet_httpd_quiesce(pnet_runtime *rt) { (void)rt; }
int pnet_httpd_listen(pnet_runtime *rt, const char *meta_json) { (void)meta_json; pnet_set_last_error(rt, &rt->httpd_last_error, PNET_ERROR_UNAVAILABLE, "httpd not built"); return -1; }
int pnet_httpd_stop(pnet_runtime *rt, int handle, bool graceful, uint32_t timeout_ms) { (void)rt;(void)handle;(void)graceful;(void)timeout_ms; return -1; }
int pnet_httpd_respond(pnet_runtime *rt, int req, const char *meta_json, const uint8_t *body, size_t body_len) { (void)rt;(void)req;(void)meta_json;(void)body;(void)body_len; return -1; }
int pnet_httpd_write(pnet_runtime *rt, int req, const uint8_t *chunk, size_t len) { (void)rt;(void)req;(void)chunk;(void)len; return -1; }
int pnet_httpd_end_body(pnet_runtime *rt, int req) { (void)rt;(void)req; return -1; }
int pnet_httpd_read_into(pnet_runtime *rt, int req, uint8_t *dst, size_t len) { (void)rt;(void)req;(void)dst;(void)len; return -1; }
void pnet_httpd_abort(pnet_runtime *rt, int req) { (void)rt;(void)req; }
const char *pnet_httpd_poll(pnet_runtime *rt, size_t *len) { return pnet_queue_poll(rt, &rt->httpd_queue, len); }
const char *pnet_httpd_last_error(pnet_runtime *rt) { return pnet_sb_cstr(&rt->httpd_last_error); }
const char *pnet_httpd_limits(pnet_runtime *rt) { (void)rt; return "{}"; }
