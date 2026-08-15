# Formal ESP network hardware-smoke artifact

This test-only component embeds one PocketJS network factory for ESP32-S3 and
ESP32-P4 hardware checks. **Both boards link the same NUL-terminated factory,
32-byte Build Plan hash, sorted feature list, endpoint tuple, and factory
SHA-256.** The component does not advertise a public capability and does not
change the stock target registry.

`app.ts` imports `fetch` from `@pocketjs/framework/net/http`. The generator
resolves a format-3 manifest and invokes the normal private network-factory
pipeline with a narrow test-only compiler permit. The permit requires the
exact plan hash, app entry, target, host ABI, feature set, HTTP backend, and ESP
transport. Normal builds, changed plans, `Headers`, `serve`, and namespace
imports remain staged.

The Guest runs 20 rounds against `http://172.16.10.126:8088`. Each round checks
`GET /health` and a binary, unknown-length streamed `POST /echo`. Every request
sets `redirect: "manual"` because redirect follow is not admitted by the ESP
runtime candidate. The factory installs the legacy `frame` slot required by
the Guest host, but **the headless runner never calls it and requires
`frameCalls === 0`.**

Generate or check the committed artifact from the repository root:

```sh
bun hosts/esp-idf/components/pocketjs_net_formal_smoke_artifact/generate.ts
bun hosts/esp-idf/components/pocketjs_net_formal_smoke_artifact/generate.ts --check
bun test hosts/esp-idf/components/pocketjs_net_formal_smoke_artifact/artifact.test.ts
```

`pocketjs_net_formal_smoke_run()` is called from a dedicated non-lwIP owner
task. It creates a PSRAM-only QuickJS Guest, performs the exact formal ABI 1.1
handshake, services native work and Promise jobs with fixed budgets, snapshots
the report without accessors or eval, and completes the runtime's three-stage
shutdown. **The minimum Guest heap is 2 MiB, the recommended board setting is
4 MiB, and the minimum QuickJS stack limit is 24 KiB.** The runner exclusively
uses FreeRTOS task notification index zero while active.

Once native runtime creation succeeds, the runner does not return until the
runtime and Guest can be destroyed safely. `shutdown_warning_ms` emits a
fail-stop diagnostic but does not abandon callback-owned memory. A broken
native subsystem therefore keeps the dedicated owner task in cleanup instead
of returning with a dangling wake or permission context.

This artifact validates the formal plaintext request path and board scheduler
integration. It does not satisfy public HTTP admission: redirect follow,
connection reuse, descriptor aggregation, complete resource accounting, DNS
candidate completeness, and the full hardware/conformance matrix remain
required before the compiler or stock target registry can expose
`network.http.client`.
