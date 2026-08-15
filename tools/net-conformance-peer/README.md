# Independent Mac HTTP and TLS peer

This tool provides a Python socket implementation for PocketJS HTTP/1.1 LAN
tests over plaintext or TLS. It imports no PocketJS runtime or network backend
code. Its output is NDJSON. Request events contain header names and byte counts
but no header values or bodies. TLS events contain the negotiated version,
cipher name, received SNI hostname, and handshake error class.

The peer is a development test fixture. It has no client or application
authentication and must not be exposed to an untrusted network.

## Plaintext ESP client to Mac server

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

## Generate local TLS profiles

The generator uses OpenSSL to create a disposable CA and six server profiles.
Add the Mac LAN address to the valid certificate's IP subjectAltName when an ESP
connects by address:

```sh
PEER_DIR=tools/net-conformance-peer
MAC_IPV4="$(ipconfig getifaddr en1)" # use the active interface
python3 "$PEER_DIR/generate_test_pki.py" --san-ip "$MAC_IPV4"
```

The default SANs are `pocketjs.test`, `localhost`, `127.0.0.1`, and `::1`.
Repeated `--san-dns` and `--san-ip` arguments add names and addresses. Run the
generator again with `--force` after the Mac address changes.

`pocketjs.test` uses the reserved `.test` suffix. A hardware SNI test must map
that name through its controlled DNS fixture; PocketJS v1 does not treat
`.local` as ordinary DNS or enable mDNS implicitly.

**All private keys are generated under the ignored `.pki/` directory with mode
`0600`. They are test-only assets and must not be committed or installed as
production trust anchors.** The generator replaces only its known files when
`--force` is present.

Start the valid TLS peer:

```sh
python3 "$PEER_DIR/http_peer.py" serve \
  --host 0.0.0.0 \
  --port 8443 \
  --tls-cert "$PEER_DIR/.pki/server.cert.pem" \
  --tls-key "$PEER_DIR/.pki/server.key.pem" \
  --events /tmp/pocketjs-tls-peer.ndjson
```

**The server accepts TLS 1.2 and newer by default.** Use
`--tls-max-version 1.2` to lock a TLS 1.2 positive test, or
`--tls-min-version 1.3` for a version-rejection test. Configure the ESP client
with `https://MAC_IPV4:8443` and the generated `ca.cert.pem` trust anchor. The
certificate must contain the exact DNS name or IP address used by the URL.

The HTTPS probe exercises the same verified path from a Mac client. It has no
insecure mode:

```sh
python3 "$PEER_DIR/http_peer.py" probe \
  --base-url https://127.0.0.1:8443 \
  --ca-cert "$PEER_DIR/.pki/ca.cert.pem" \
  --rounds 2 \
  --health-contains '"status":"ok"'
```

The generated negative profiles are:

| Profile | Certificate and key | Expected verification result when `ca.cert.pem` is trusted |
|---|---|---|
| Valid | `server.cert.pem`, `server.key.pem` | TLS succeeds for an exact configured SAN |
| Wrong hostname | `wrong-host.cert.pem`, `wrong-host.key.pem` | Reject because its only SAN is `wrong-host.invalid` |
| Unknown CA | `untrusted-server.cert.pem`, `untrusted-server.key.pem` | Reject because a separate untrusted CA signed it |
| Bad signature | `bad-signature-server.cert.pem`, `server.key.pem` | Reject the deliberately corrupted certificate signature |
| Expired | `expired-server.cert.pem`, `expired-server.key.pem` | Reject the fixed 2020 validity interval |
| Not yet valid | `not-yet-valid-server.cert.pem`, `not-yet-valid-server.key.pem` | Reject the fixed 2050 validity interval |

Stop the valid peer, then substitute one profile's certificate and key in the
same `serve` command. For every negative profile, a verifying client must fail
the TLS handshake before an HTTP `request` event appears. The peer emits
`tls_client_hello` with the received SNI hostname or `null` when the client sends
no SNI, and emits `tls_handshake_error` when the peer observes the TLS alert.
The wrong-host, expired, not-yet-valid, and bad-signature profiles are signed by
the trusted test CA, so each isolates its stated verification decision. The
unknown-CA profile has matching SANs and isolates trust-chain rejection.

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
binary echo, and persistent-connection behavior. An `https://` URL requires
normal system trust or an explicit `--ca-cert`; certificate and hostname
verification are always enabled:

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
openssl version
python3 -m unittest tools/net-conformance-peer/test_http_peer.py
```

The tests generate PKI in a temporary directory and cover plaintext behavior,
a TLS 1.2 health/echo connection, SNI capture, unknown CA, bad signature,
hostname mismatch, expired certificate, and not-yet-valid certificate. The
fixture does not by itself cover DNS candidates, permission decisions, abort
races, target resource limits, long soak, or hardware/runtime assertions.
