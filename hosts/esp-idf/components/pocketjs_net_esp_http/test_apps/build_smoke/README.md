# ESP HTTP Client backend build smoke

This app links the ESP-IDF v6.0.2 experimental candidate and checks its honest
descriptor flags without creating a network interface. The component dependency
is locked to exactly ESP-IDF v6.0.2 because no other release has been reviewed.
Build it for both supported targets with that release:

```sh
idf.py -B build-esp32s3 set-target esp32s3 build
idf.py -B build-esp32p4 set-target esp32p4 build
```

Product integrations must also set
`CONFIG_ESP_HTTP_CLIENT_EVENT_POST_TIMEOUT=0`. The backend does not consume the
default event loop, so a nonzero post timeout would add hidden blocking to its
fixed worker. Keep Basic authentication, Digest authentication and saved
response headers disabled; redirects and authentication retries are controlled
by PocketJS rather than ESP HTTP Client. The component rejects an integration
at compile time when a required security or ownership Kconfig value is
violated.
Even with a zero timeout, native event-loop payload copies and the response
parser may allocate outside PocketJS's fixed queues; the candidate therefore
reports that its response parser is not bounded.

The smoke profile enables ESP-TLS/Mbed TLS, the ESP certificate bundle and TLS
1.2. It disables ESP-TLS insecure options, weak certificate verification,
deprecated roots, DES/3DES, TLS 1.3 and TLS 1.2 renegotiation.
The app checks that the descriptor reports Host bundle and single Host-pinned
CA profiles while keeping application custom-CA append, client authentication,
custom ALPN, revocation and insecure verification disabled. It also creates an
HTTPS client with an untrusted wall clock and verifies that the operation ends
as `tls_certificate_invalid` before network I/O. Native-error assertions cover
TLS version, TLS alert, hostname mismatch, unknown CA and connection refusal.

Cancellation is cooperative. Native DNS, connect, TLS, read and write calls are
not interrupted, and synchronous DNS may not honor `io_timeout_ms`. Therefore
client destruction has no uniform time bound while one of those calls is in
progress.

The native request TX buffer is a fixed 4096-byte budget. Compile-time checks
ensure it holds the maximum 2048-byte URL after ESP HTTP Client splits it into
request-target and Host, plus the maximum method and HTTP framing. Each custom
request header is independently bounded to 580 framed bytes.

Chunked bodies without observed trailers remain supported. The backend rejects
a trailer when ESP HTTP Client exposes it through the header callback, but the
native parser may consume a final trailer without a callback. Complete trailer
validation is therefore an admission blocker, not a capability of this
candidate.
