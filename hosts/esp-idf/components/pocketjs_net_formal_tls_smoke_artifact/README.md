# ESP formal TLS network smoke artifact

This component is an isolated **test-only** PocketJS network factory for
ESP32-S3 and ESP32-P4 hardware validation. It does not change the public
network capability gate.

The verified Build Plan admits exactly `network.http.client` and
`network.http.client.tls`, the ESP HTTP Core, the ESP transport, and the ESP-TLS
provider. The only endpoint is **`https://pocketjs.test:8443`**. Each of 20
rounds performs one health request and one streamed binary echo request with
the SDK's default redirect-follow mode and this exact Guest TLS policy:

- DNS A-label and SNI: `pocketjs.test`
- TLS minimum and maximum: 1.2
- verification: full
- revocation: host-default
- ALPN, Guest custom CA, client credentials, and client certificates: absent

`fixtures/ca.cert.pem` is the public test CA only. Its DER SHA-256 is recorded
in generated metadata. **No CA or server private key belongs in this
component.**

Generate and verify the checked-in artifacts with:

```sh
bun hosts/esp-idf/components/pocketjs_net_formal_tls_smoke_artifact/generate.ts
bun hosts/esp-idf/components/pocketjs_net_formal_tls_smoke_artifact/generate.ts --check
bun test hosts/esp-idf/components/pocketjs_net_formal_tls_smoke_artifact/artifact.test.ts
```

The board harness must configure stock lwIP DNS so `pocketjs.test` resolves to
the selected Mac peer, provision a trusted wall clock, and pass both facts to
`pocketjs_net_formal_tls_smoke_run()`. The runner snapshots the public CA into
the runtime as `POCKETJS_NET_ESP_TLS_TRUST_HOST_PINNED_CA` and rejects every
other Host profile.

The exact runtime descriptor must report `distinct_tls_errors=false`. Stock
ESP-IDF v6.0.2 can expose only the generic X.509 verification error after some
failed handshakes, so the artifact accepts `tls_certificate_invalid` for
hostname, trust-chain, validity, and usage failures. Negative wire evidence
must still show the expected SNI, a failed TLS handshake, zero HTTP requests,
and no plaintext fallback. This limitation keeps the public TLS capability gate
closed.
