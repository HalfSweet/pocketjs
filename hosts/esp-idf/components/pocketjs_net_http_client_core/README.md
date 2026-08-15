# PocketJS ESP-IDF HTTP Client Core candidate

This component is an experimental, private plaintext HTTP/1.1 protocol Core.
It combines `pocketjs_net_http1` with the public operation/completion API of
`pocketjs_net_esp_transport`; it does not use `esp_http_client` and does not
register or advertise a PocketJS capability.

## Implemented boundary

One statically supplied 36 KiB instance storage admits one request at a time.
The Core snapshots the method, URL, fields, and an optional fixed request body
before native I/O. A request selects exactly one body mode: none, a fixed
snapshot, or a credit-driven stream. The fixed snapshot is at most 4096 bytes.
A streaming producer may send more than that total, but each submitted chunk
is at most 2048 bytes and is copied into fixed Core storage before the owner API
returns. Request and response field storage are each 8192 bytes, and each
downstream response-body lease is at most 2048 bytes. The Core and wire parser
do not allocate.

Only canonical ASCII DNS names and canonical dotted-decimal IPv4 literals are
accepted. User information, fragments, IPv6, non-canonical numeric hosts,
invalid percent escapes, and non-canonical ports are rejected before I/O.
Requests use origin-form, add `Connection: close` and
`Accept-Encoding: identity`, and reject caller-controlled connection,
framing, proxy, upgrade, and content-coding fields. GET and HEAD request bodies
are rejected. Every accepted request opens one connection and closes it after
the response; there is no pooling or reuse.

Unknown-length request streams use strict HTTP/1.1 chunked coding: one
lowercase hexadecimal size, CRLF, the credited non-empty payload, and CRLF per
chunk, followed by exactly `0\r\n\r\n`. Known-length request streams emit
`Content-Length` and must submit exactly that many raw bytes. Their final byte
ends the upload without another producer pull; an early producer end selects a
request-body error. The fixed and known-length paths never use chunk markers.

For each streaming chunk, the Core publishes one `REQUEST_BODY_PULL` carrying
the operation token, a non-reusable body generation, a non-reusable pull
generation, and `maximum_bytes`. The native adapter takes and immediately
retires that event. Retirement frees the event slot but leaves exactly one
credit active, so an asynchronous Guest producer can submit a later
`BODY_CHUNK`, `BODY_END`, or `BODY_ERROR` command that echoes the token and both
generations. A command before retirement, without credit, with stale identity,
with an empty chunk, or above the advertised bound is rejected without
consuming credit. Abort, timeout, producer failure, and shutdown revoke the
credit before connection cleanup. Event, body, and pull generations never
wrap or reuse during a live Core instance.

The permission callback runs on the owner task. For a DNS name it receives the
canonical `(scheme, hostname, port)` tuple before resolve starts. After resolve,
the callback is invoked for every returned numeric candidate before one allowed
candidate is selected. A literal IPv4 address receives the numeric permission
check directly. A connection failure is terminal; the Core does not try another
candidate. **The callback is non-reentrant:** every Core entry point rejects a
call made from the callback, and the Core verifies its lifecycle generation,
operation token, state, and transport-idle state after each callback returns.

Response headers are copied into the fixed response store and emitted before
body bytes. **`response_header_bytes_limit` bounds the final response fields as
`name + value + ": " + CRLF`; zero selects the 8192-byte compile-time maximum,
and initialization rejects a larger value.** Crossing the selected limit is a
`resource_limit` terminal selected by the parser callback before any response
headers event is published. The Host must retire that event, grant one bounded
body-credit window, view and release the resulting body lease, and retire the
body event before more input is consumed. The HTTP/1.1 substrate validates
framing, chunking, chunk extensions, trailer declarations, and trailer fields.
Statuses from 200 through 599, including 4xx and 5xx, are protocol success.
Content coding other than `identity` is rejected because this Core does no
decoding. When one read completion contains bytes and EOF, **EOF is applied
only after every byte in that transport lease has been consumed under body
credit**.

Connect, response-header, idle-read, and total deadlines use Host-supplied
monotonic microseconds. Abort and all deadline paths select one terminal result.
When a response boundary is unknown, the Core cancels active read/write work or
starts an explicit close. When the transport admits cleanup, the Core waits for
its fail-close or close terminal. If cleanup cannot be admitted or times out,
the Core emits the already selected terminal with poison and retained ownership
so the product can tear down the dedicated transport. The ESP transport instance
must be dedicated to this Core because completion dequeue is single-consumer.

**The first selected HTTP terminal result is immutable.** A later close
admission failure, close error, close timeout, read-lease release failure, or
completion-retirement failure cannot replace it. Those native cleanup failures
set machine-readable poison flags exposed by
`pocketjs_net_http_client_core_get_status`; a poisoned Core rejects every new
request. Failed lease release retains the exact handle as owned state, and a
failed completion retirement retains its token for bounded retries. An exact
Host event-retirement invariant failure is reported separately and retains the
delivering event and any body lease until shutdown cleanup.

The lifecycle is explicit. `begin_shutdown` permanently stops admission and
aborts a current request. The owner continues pumping and retiring events until
`is_quiescent` succeeds, then calls `deinit`. If poisoned native ownership cannot
be discharged through the transport API, the product must synchronously destroy
the dedicated transport and call `confirm_transport_shutdown` before the Core
can become quiescent. A poisoned Core whose exact Host event still cannot be
retired after that confirmation exposes a separate abandon call; it is rejected
before shutdown, on a healthy Core, before transport confirmation, for a pending
event, or for a mismatched sequence. `init` detects and rejects reuse of live
storage.

The Core performs no automatic redirect, retry, authentication, cookie,
proxy, compression, or decompression behavior. Redirect responses are exposed
as ordinary responses. A streamed request body is consumed once and is never
buffered in full or implicitly replayed.

## HTTPS and admission blockers

The parser recognizes `https://`, but every HTTPS start returns
`POCKETJS_NET_HTTP_CLIENT_START_UNSUPPORTED_TLS` before permission callbacks,
DNS, or socket I/O. This is deliberate: the current ESP transport reports a
bounded one millisecond internal TLS wait, native DNS/socket/TLS allocations
without caller-owned byte bounds, and incomplete DNS candidate sets. Those
transport blockers also apply to this plaintext candidate's eventual public
admission where relevant.

The current Core additionally has these gaps:

- it supports one in-flight operation and one connection, not the architecture
  concurrency target;
- it supports IPv4 only and does not implement IDNA or IPv6;
- it does not follow redirects or implement redirect method rewriting and
  cross-origin sensitive-field stripping;
- a native failure to start or complete the final close has no lower-level
  force-close primitive, so the Core preserves the HTTP terminal result,
  exposes the cleanup poison and retained ownership, and requires the product
  to tear down the dedicated transport;
- fixed PocketJS-owned storage is proven, but the composed lwIP pools and socket
  allocations still need target-specific Kconfig bounds, peak measurements,
  and soak evidence;
- it is not connected to the QuickJS Guest Binding or NetworkServiceTurn and
  has not passed the AtomS3R/Tab5 hardware admission matrix.

Until those gaps are resolved, this component must not enter a formal Build
Plan and must not advertise `network.http.client` or
`network.http.client.tls`.

## Verification

`test_host/core_test.c` uses a deterministic fake transport to cover permission
ordering, headers-first delivery, bounded body credit and leases, 4xx success,
strict trailer failure, close-before-error ordering, pre-I/O HTTPS rejection,
abort, total timeout, selected response-header configuration and parse
boundaries, and exact-one terminal delivery. Hostile cases cover all
public API calls attempted from a permission callback, bytes-plus-EOF reads,
stale completions while idle, malformed read cleanup, lease and completion
retirement failures, close admission/error/timeout, terminal immutability,
explicit teardown, poison-only Host event/lease abandonment, HEAD, 1xx followed
by 304, numeric-host denial, and the exact
4096-byte fixed request-body boundary. Streaming hostile cases cover strict
chunk coding over more than 64 KiB, one-byte chunks, asynchronous credit after
event retirement, empty/oversized/no-credit/stale submissions, known-length
underflow and overflow, abort, timeout, producer error, shutdown, and
generation non-reuse. It is compiled with `-Wall -Wextra -Werror` plus
AddressSanitizer and UndefinedBehaviorSanitizer and is also checked with Clang
Static Analyzer.

`test_apps/build_smoke` links the Core, wire codec, and real ESP transport under
the pinned ESP-IDF v6.0.2 tree. It builds for both `esp32s3` and `esp32p4` with
the transport TLS policy Kconfig, while asserting that the Core itself keeps
HTTPS fail-closed.
