# PocketJS ESP HTTP Client candidate

This component is an **experimental ESP-IDF v6.0.2 backend candidate**. It is
not selectable by a PocketJS host and does not publish a PocketJS network
capability.

All `esp_http_client` handle operations run on one fixed worker task. The owner
API synchronously copies one bounded request snapshot, then returns after a
non-blocking command enqueue. PocketJS owns a fixed event queue with reserved
terminal credit, eight 2048-byte payload leases, and a 256 KiB response-body
limit. The native request TX buffer is fixed at 4096 bytes and checked at build
time against the maximum request line, Host field, and individual request
header. These limits do not bound the native response parser.

The component manifest accepts **exactly ESP-IDF 6.0.2**. Product integration
must preserve these Kconfig values:

```text
CONFIG_ESP_HTTP_CLIENT_EVENT_POST_TIMEOUT=0
# CONFIG_ESP_HTTP_CLIENT_ENABLE_BASIC_AUTH is not set
# CONFIG_ESP_HTTP_CLIENT_ENABLE_DIGEST_AUTH is not set
# CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS is not set
```

The build rejects other values. Redirects and authentication retries are also
disabled in the client configuration. Requests use IPv4, remove the default
User-Agent field, and send `Connection: close`.

Known admission blockers are:

- ESP HTTP Client owns a response parser with hidden reallocations, so response
  parser memory is not bounded by PocketJS. It also posts native events to the
  default ESP event loop, whose payload copies may allocate outside PocketJS's
  queues. A zero post timeout removes hidden waiting, not hidden allocation.
- Arbitrary HTTP methods and the wire status reason phrase are unavailable.
- PocketJS DNS policy is not implemented. Native synchronous DNS may ignore the
  configured I/O timeout, and DNS/connect calls cannot be interrupted.
- ESP HTTP Client may consume a final chunk trailer without exposing it to the
  event callback. The backend rejects only callback-visible trailers and cannot
  claim complete trailer validation.
- Strict HTTP framing behavior has not been proven for host admission. The
  emitted chunked flag records only a callback-visible `Transfer-Encoding`
  value whose SP-trimmed value is exactly `chunked` ignoring case. HTAB is not
  trimmed because the ESP-IDF v6.0.2 parser does not dechunk that form. Other or
  duplicate transfer codings fail the operation. The backend does not use ESP
  HTTP Client's derived chunked flag, which also covers some non-chunked bodies.
- Request-body streaming is not implemented; request bodies are copied into the
  bounded snapshot.

Build the descriptor smoke app for both reviewed targets:

```sh
cd test_apps/build_smoke
idf.py -B build-esp32s3 set-target esp32s3 build
idf.py -B build-esp32p4 set-target esp32p4 build
```
