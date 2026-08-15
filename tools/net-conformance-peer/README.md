# Independent Mac HTTP peer

This tool provides a Python socket implementation for PocketJS HTTP/1.1 LAN
tests. It imports no PocketJS runtime or network backend code. Its output is
NDJSON, and request events contain header names and byte counts but no header
values or bodies.

The peer is a development test fixture. It has no authentication or TLS and
must not be exposed to an untrusted network.

## ESP client to Mac server

Find the Mac address on the Wi-Fi interface and start the peer on the LAN:

```sh
route -n get default | rg interface
ipconfig getifaddr en1  # replace en1 with the interface printed above
python3 tools/net-conformance-peer/http_peer.py serve \
  --host 0.0.0.0 \
  --port 8088 \
  --events /tmp/pocketjs-http-peer.ndjson
```

Use the printed Mac IPv4 address and port as the ESP application's HTTP peer.
macOS may request permission for Python to accept incoming connections. The
default bind address is `127.0.0.1`; LAN exposure therefore requires the
explicit `--host 0.0.0.0` option.

The success paths are:

| Request | Response contract |
|---|---|
| `GET /health` | HTTP 200, bounded JSON body, explicit `Content-Length` |
| `POST /echo` | HTTP 200, exact binary request body, explicit `Content-Length` |
| `GET /chunked?fragment_ms=10` | Three chunks followed by a valid non-framing trailer |
| `GET /status/404` or `/status/500` | Exact 4xx/5xx status without transport failure |
| `GET /redirect?status=302&to=/health` | One visible redirect response |

The controlled failure paths are:

| Request | Wire behavior |
|---|---|
| `GET /delay?phase=headers&ms=2000` | Delays before response headers |
| `GET /delay?phase=body&ms=2000` | Sends headers, then delays the fixed body |
| `GET /delay?phase=chunk&ms=2000` | Sends one chunk, then delays the final chunk |
| `GET /disconnect?phase=before_headers` | Closes without a response |
| `GET /disconnect?phase=mid_headers` | Closes in the header block |
| `GET /disconnect?phase=mid_body` | Closes before the declared body length |
| `GET /malformed/te-cl` | Sends both `Transfer-Encoding` and `Content-Length` |
| `GET /malformed/duplicate-content-length` | Sends two `Content-Length` fields |
| `GET /malformed/obs-fold` | Sends an obsolete folded header |
| `GET /malformed/te-duplicate` | Sends two `Transfer-Encoding: chunked` fields |
| `GET /malformed/te-combined` | Sends `gzip, chunked` transfer codings |
| `GET /malformed/te-unknown` | Sends an unsupported transfer coding |
| `GET /malformed/trailer-forbidden` | Puts `Content-Length` in a chunked trailer |
| `GET /malformed/chunk-size` | Sends a non-hexadecimal chunk size |

`GET /retry-once?token=<unique-safe-token>` closes the first attempt and returns
the attempt number on later attempts. `GET /attempts?token=<same-token>` reports
the count. Together they detect hidden transport retries. Token storage is
bounded to 1024 unique values for one process lifetime.

The expected PocketJS result for every malformed path is one
`http_protocol_error` terminal and a closed connection. Delay paths should be
run with a client timeout lower than `ms` and must produce one timeout terminal.
The retry-once counter must remain one after a single PocketJS request when
hidden retries are disabled. A redirect result depends on the resolved endpoint
permission and redirect policy; the peer itself never follows a redirect.

## Mac client to ESP server

The probe independently checks an ESP HTTP server's health endpoint, exact
binary echo, and persistent-connection behavior:

```sh
python3 tools/net-conformance-peer/http_peer.py probe \
  --base-url http://ESP_IPV4:8080 \
  --rounds 20 \
  --interval-ms 250 \
  --health-contains '"status":"ok"'
```

Each round emits `probe_round_pass`; the process exits with status 1 on the
first status, body, timeout, or connection failure. The probe intentionally
does not follow redirects and does not log response bodies.

## Local verification

```sh
python3 -m unittest tools/net-conformance-peer/test_http_peer.py
```

These routes cover a minimum independent plaintext wire fixture. They do not
by themselves satisfy PocketJS capability admission: HTTPS PKI, DNS candidates,
permission decisions, abort races, resource limits, long soak, and target
hardware/runtime assertions remain separate gates.
