# PocketJS ESP-IDF network runtime candidate

This experimental component adapts the frozen private network ABI 1.1 table to
a bounded pool of HTTP Client Cores and dedicated ESP transports. **The table
is owner-task-only, frozen, accessor-free, and bound to the exact Build Plan
hash and either the `network.http.client` projection or the exact
`network.http.client` plus `network.http.client.tls` projection.** The
handshake and `getLimits()` return the same frozen feature list. lwIP callbacks
only call the configured scheduler wake hook; Guest code runs only through
`pocketjs_esp_guest_call_function()`.

Each operation slot owns one Core and one transport because Core-local
transport tokens cannot safely share a completion consumer. The pool has a
compile-time ceiling of eight slots and a smaller admitted runtime size.
Response body events retain the Core lease until the formal
`leaseTake`/`leaseReadInto`/`leaseRelease` sequence completes. Request upload
pull events are retired immediately while their single credit and Core body
and pull generations remain recorded for the later producer command.
`leaseReadInto` accepts only an exact borrowed `Uint8Array` window whose byte
length equals `maxBytes`, and rejects any range beyond the retained lease.

Plaintext `http:` is available in both projections. HTTPS requires the exact
TLS feature projection and an immutable Host-selected certificate bundle or a
single Host-pinned CA. The runtime copies a pinned CA during `create()`; the
trusted-clock callback and its context remain borrowed until successful
destroy. **The native boundary accepts only TLS 1.2, full certificate and
hostname verification, no client certificate, no custom CA input, no ALPN,
and the canonical DNS A-label as `serverName`.** Numeric HTTPS hosts and every
other TLS policy are rejected before permission checks, DNS, or socket I/O.

Redirect mode `manual` is the only admitted mode. `follow` remains disabled
because the current Core does not expose relative Location canonicalization
and the formal one-shot streaming producer cannot replay a retained request
body for 307/308. The selected TLS provider and complete Host descriptor are
not yet verified against the Build Plan by this component, TLS close-notify is
not bounded, and native TLS allocation limits are not proven. **The descriptor
therefore keeps public capability advertisement off.**

Shutdown has three explicit stages: stop admission and request cancellation,
run guarded shutdown service turns while pumping native cleanup, then destroy
quiescent transports and deinitialize every Core before releasing the binding.
If a poisoned Core retains native ownership, phase 2 destroys only that Core's
dedicated transport and synchronously confirms the invalid transport context
before any other Core call. Persistent Host event retirement then uses the
Core's poison-only exact-sequence abandon path; healthy event retirement is
never discarded. A late stock lwIP DNS callback delays this recovery without
freeing its callback context.
Both zero-budget probes and nonzero-budget empty polls read Core status before
reporting `Drained`, so Core poison cannot be hidden by an empty Host queue.
Binding construction clears and logs any QuickJS exception on failure, including
the closure constructor's returned-value/pending-exception boundary.
