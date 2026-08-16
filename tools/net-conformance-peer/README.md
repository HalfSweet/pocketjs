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

Create a fresh private evidence directory as the normal desktop user. Do not
run either peer with `sudo`:

```sh
export POCKETJS_PEER_RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pocketjs-peer.XXXXXX")"
chmod 700 "$POCKETJS_PEER_RUN_DIR"
echo "$POCKETJS_PEER_RUN_DIR" # copy this path into the other peer terminals
```

Start the valid TLS peer and lock both ends of the version range to TLS 1.2:

```sh
python3 "$PEER_DIR/http_peer.py" serve \
  --host 0.0.0.0 \
  --port 8443 \
  --tls-cert "$PEER_DIR/.pki/server.cert.pem" \
  --tls-key "$PEER_DIR/.pki/server.key.pem" \
  --tls-min-version 1.2 \
  --tls-max-version 1.2 \
  --observe-tls-close-notify \
  --events "$POCKETJS_PEER_RUN_DIR/tls.ndjson"
```

**The command above accepts only TLS 1.2.** Without `--tls-max-version`, the
server accepts TLS 1.2 and newer by default. Use `--tls-min-version 1.3` for a
version-rejection test. Configure the ESP client with `https://MAC_IPV4:8443`
and the generated `ca.cert.pem` trust anchor. The certificate must contain the
exact DNS name or IP address used by the URL.

With `--observe-tls-close-notify`, each TLS `connection_close` event records
`tls_close_state` as `close_notify`, `ragged_eof`, or `not_observed` and includes
the corresponding `tls_close_notify_observed` boolean. **A Phase 1B graceful
shutdown pass requires `close_notify`; a TCP EOF alone is not sufficient.**

`--tls-handshake-delay-ms N` waits for the bounded interval before the server
processes ClientHello. It requires TLS and cannot exceed
`--delay-ceiling-ms`. Use it with a client-side connect deadline or
AbortSignal to prove that a stalled handshake terminates without opening HTTP
or falling back to plaintext.

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

## Controlled DNS for TLS hostname and SNI

`dns_peer.py` is a bounded IPv4 authoritative fixture for the TLS test name.
It serves UDP and TCP on the same port and has no upstream resolver. The LAN
configuration used by the ESP test boards is:

| Setting | Value |
|---|---|
| DNS server | `172.16.10.126:53` |
| Exact A record | `pocketjs.test. 30 IN A 172.16.10.126` |
| HTTPS URL and SNI | `https://pocketjs.test:8443` |
| Trust anchor | Host-fixed `.pki/ca.cert.pem` |

Use the fresh `POCKETJS_PEER_RUN_DIR` created above in every peer terminal.
Start the fixture on the Mac Wi-Fi interface:

```sh
PEER_DIR=tools/net-conformance-peer
python3 "$PEER_DIR/dns_peer.py" serve \
  --host 0.0.0.0 \
  --port 53 \
  --interface en1 \
  --allow-cidr 172.16.10.0/24 \
  --name pocketjs.test \
  --address 172.16.10.126 \
  --events "$POCKETJS_PEER_RUN_DIR/dns.ndjson"
```

On Darwin, `--interface en1` applies `IP_BOUND_IF` to both sockets before the
wildcard bind. On platforms that cannot enforce the requested interface, the
fixture fails startup. The source CIDR is checked again for every UDP datagram
and TCP connection. Only RFC1918 and loopback subnets are accepted; public
CIDRs and ranges assembled from broad prefixes are rejected by both the CLI
and the server constructor.

On the current Darwin test host, the exact wildcard-plus-`IP_BOUND_IF=en1`
command was verified at UID 501 on UDP and TCP port 53 without `sudo`. Direct
non-root binds to `127.0.0.1:53` and `172.16.10.126:53` returned `EACCES`; this
is why the local parser smoke below uses loopback port 1053. Do not remove the
interface constraint or add privilege based on the loopback result.

**This fixture is not an open recursive resolver.** It sets `RA=0`, never sends
an upstream query, and silently drops sources outside the allowlist. The exact
`A/IN` question receives the one configured answer. Descendants of
`pocketjs.test` receive authoritative `NXDOMAIN`; unrelated names, other types,
and other classes receive `REFUSED`. A client may set `RD`, as normal stub
resolvers do, but it does not change those decisions. Messages without a usable
DNS header, responses sent to the server, and messages above 1232 bytes are
dropped. Other malformed requests receive `FORMERR` when a bounded reply is
possible.

The fixed non-EDNS positive exchange is a 31-byte query and a 47-byte response,
so it is not described as zero amplification. Interface binding, private-source
allowlisting, the single fixed answer, and the absence of recursion prevent it
from serving as a public reflection endpoint.

The parser accepts one structurally valid EDNS record, caps the advertised UDP
payload at 1232 bytes, does not echo options, and returns `BADVERS` for a
nonzero EDNS version. Compression in a query name, additional records other
than the single EDNS record, and incomplete UDP or TCP framing are rejected.

The en1-constrained listener is intended to receive queries from the two ESP
boards. macOS routes its own `172.16.10.126` destination through `lo0`, so test
the parser locally with a separate loopback instance instead of removing the
interface constraint from the LAN listener. Start that instance in one terminal:

```sh
python3 "$PEER_DIR/dns_peer.py" serve \
  --host 127.0.0.1 \
  --port 1053 \
  --allow-cidr 127.0.0.0/8
```

Query both transports from another terminal:

```sh
dig @127.0.0.1 -p 1053 pocketjs.test A +noall +comments +answer
dig @127.0.0.1 -p 1053 pocketjs.test A +tcp +noall +comments +answer
```

After DHCP and trusted-clock establishment, both ESP projects must set their
DNS server to `172.16.10.126`, retain `pocketjs.test` as the URL hostname, and
load `.pki/ca.cert.pem` as a Host-owned test trust anchor. The DNS answer must
not replace the URL hostname before ESP-TLS performs hostname verification and
constructs SNI. Use separate terminals for the two board projects:

```sh
# AtomS3R; select the port whose USB serial is 98:88:E0:0F:34:A0.
idf.py -C "$ATOM_S3_PROJECT" -p "$ATOM_S3_PORT" flash monitor

# Tab5; select the port whose USB serial is E8:F6:0A:E2:F6:46.
idf.py -C "$TAB5_P4_PROJECT" -p "$TAB5_P4_PORT" flash monitor
```

The fixture emits bounded metadata in `dns_ready`, `dns_query`, `dns_drop`, and
error records as NDJSON. Metadata includes the allowed source address and a
validated query name, but no raw packet or EDNS option data. It creates the
event file with mode `0600`, or appends only when an existing file is regular,
owned by the current user, and inaccessible to group and other users. Symbolic
links are refused. The private run directory also protects the HTTP peer's
event path. Do not substitute a predictable shared `/tmp/*.ndjson` path.

Run one board at a time and create a new private run directory before each
acceptance run. Treat the `dns_ready` and `peer_ready` records as the beginning
of that run; do not append evidence to a directory from an earlier run. Let
`BOARD_IPV4` be the DHCP address of the board under test. A positive result
requires this ordered evidence within that run's monotonic time window:

1. `dns_query` has `outcome=answer`, `query_name=pocketjs.test`, and
   `peer_ipv4=BOARD_IPV4`.
2. A later `tls_client_hello` has `server_name=pocketjs.test` and the same
   `peer_ipv4`.
3. The next matching `connection_open` has the same `peer_ipv4`,
   `tls_server_name=pocketjs.test`, and `tls_version=TLSv1.2`.
4. The expected `/health` and `/echo` request records use that
   `connection_id`.

Separate DNS and TLS records without the same board address and ordering are
not acceptance evidence. Do not use `.local` for this test: it invokes mDNS
rather than the ordinary DNS path. Do not install a packet-filter redirect or
expose this fixture beyond the controlled LAN.

The success paths are:

| Request | Response contract |
|---|---|
| `GET /health` | HTTP 200, bounded JSON body, explicit `Content-Length` |
| `POST /echo` | HTTP 200, exact binary request body, explicit response `Content-Length` |
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

`POST /echo` accepts either one valid `Content-Length` or one HTTP/1.1
`Transfer-Encoding: chunked`. **Chunked request decoding is cumulative and
bounded by `--body-limit`; transfer-coding lists, chunk extensions, declared or
actual trailers, `Content-Length` conflicts, malformed framing, and oversized
bodies fail before a `request` event is emitted.** This strict subset is the
wire peer for the formal PocketJS streaming request-body path.

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
python3 -m unittest tools/net-conformance-peer/test_dns_peer.py
```

The tests generate PKI in a temporary directory and cover plaintext behavior,
bounded chunked uploads and connection reuse, a TLS 1.2 health/echo connection,
SNI capture, unknown CA, bad signature, hostname mismatch, expired certificate,
and not-yet-valid certificate. The DNS tests cover UDP and TCP answers, source
allowlisting, no-recursion behavior, `NXDOMAIN`, `REFUSED`, malformed and
oversized messages, and EDNS bounds. The fixtures do not by themselves cover
permission decisions, abort races, target resource limits, long soak, or
hardware/runtime assertions.
