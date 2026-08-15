# PocketJS ESP-IDF Guest network spike

This experimental component runs a real QuickJS Guest against the ESP HTTP
Client backend. The bundle is evaluated as a factory and receives one frozen
private binding argument. **The binding is never installed on `globalThis`, and
the component does not advertise `network.http.client` or any other public
capability.**

The factory validates private ABI version 1 and constructs a frozen lexical
`net.http` namespace around the native probe function. The name is never added
to the global object. The Guest performs bounded, sequential `GET /health` and
`POST /echo` requests. It verifies the health marker, HTTP status, and exact
echo body for every round. Every echo body includes a deterministic xorshift
nonce generated inside the Guest. The result records the stable FNV-1a 64-bit
hash of the embedded bundle.
The C Guest Binding accepts only these two method/path pairs, copies each input
through the backend's bounded admission call, accumulates at most 4096 response
bytes, and permits only one in-flight Promise. Rounds, request timeout, total
timeout, QuickJS memory, QuickJS stack, response bytes, and worker stack are all
bounded by the runner configuration or constants in `guest_spike.h`.
The runner also exposes the Guest allocator snapshot and can require every
QuickJS allocation to come from external RAM on PSRAM-equipped product boards.

The HTTP worker never calls QuickJS. Its wake callback only sets a FreeRTOS
EventGroup bit. The task that calls `pocketjs_net_guest_spike_run()` owns the
realm, drains native events in sequence, releases every received payload lease,
settles Promises, and executes a fixed maximum of 64 jobs per service turn.
Exceeding that job budget fails the spike instead of carrying an unbounded
microtask chain.

The owner scheduler uses a fixed 16.667 ms frame deadline. When a frame and a
network event are ready together, it calls the Host-cached frame function before
the service turn. The Guest reports each increment of its private frame counter
to native diagnostics, and a successful run requires at least two frames.

The product BSP must connect an `esp_netif` before calling the runner. The spike
accepts only a plaintext `http://` origin. It deliberately omits public policy,
redirect, TLS, streaming body, Build Plan, descriptor admission, and formal
private-ABI command generation. Passing this probe is evidence for Guest/worker
ownership and basic interoperability; it is not Phase 1A capability admission.

Cancellation inherits the ESP HTTP backend limitation: synchronous native DNS
or connect work cannot be interrupted. Timeout requests cancellation and then
waits for the worker's terminal event so queued payload leases are reclaimed;
therefore teardown has no uniform wall-time bound.

## Board integration

Run the probe from a dedicated owner task after Wi-Fi obtains an IP address:

```c
pocketjs_net_guest_spike_result_t result;
const pocketjs_net_guest_spike_config_t config = {
    .base_url = "http://192.0.2.10:8088",
    .device_id = "atom-s3r",
    .rounds = 20,
    .allocate_guest_in_external_memory = true,
    .worker_core = tskNO_AFFINITY,
};
ESP_ERROR_CHECK(pocketjs_net_guest_spike_run(&config, &result));
assert(result.passed);
```

Do not commit Wi-Fi credentials or a workstation-specific LAN address. Supply
those through the product app's ignored local configuration.

## Build smoke

The nested build-smoke app links the runner for both Phase 1 ESP-IDF targets;
it does not initialize a network interface. See `test_apps/build_smoke/README.md`.
