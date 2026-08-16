# PocketJS formal ESP TLS conformance artifact

This test-only component runs a second immutable Guest artifact against the
independent Mac HTTPS peer. It keeps the validated repeated-handshake soak
factory unchanged and exercises **redirect modes, chunked responses and
trailers, 4xx/5xx responses, strict malformed framing, HEAD, fixed and streamed
uploads, connection reuse, and no hidden retry** over TLS 1.2.

The component deliberately reuses the audited owner-only C runner and report
parser from `pocketjs_net_formal_tls_smoke_artifact`, but supplies a distinct
verified Build Plan and factory. The two components export the same runner API
and therefore are mutually exclusive in one firmware image. A board harness
selects exactly one in `EXTRA_COMPONENT_DIRS` and `REQUIRES`.

This is not a public capability target. The compiler permit remains restricted
to the exact test entry and verified plan, all native descriptors continue to
advertise `public_capability=false`, and the production target registry remains
unchanged.

Regenerate or verify the checked-in outputs with:

```sh
bun hosts/esp-idf/components/pocketjs_net_formal_tls_conformance_artifact/generate.ts
bun hosts/esp-idf/components/pocketjs_net_formal_tls_conformance_artifact/generate.ts --check
```
