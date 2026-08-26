# PocketJS on ESP-IDF

PocketJS ships as caller-composed ESP-IDF components. **The product firmware
owns tasks, input drivers, display buffers, presentation, and package storage.**
Only the optional `pocketjs_runner` component creates a task.

## Components

| Component | Responsibility | Targets |
| --- | --- | --- |
| `pocketjs_package` | Zero-copy `.pocket` parsing and host admission | P4, S3 |
| `pocketjs_guest` | Caller-driven QuickJS realm and guest turns | P4, S3 |
| `pocketjs_ui_core` | Retained UI core and versioned frame view | P4, S3 |
| `pocketjs_ui_qjs` | `globalThis.ui` binding between guest and core | P4, S3 |
| `pocketjs_render_rgb565` | Transactional RGB565 damage and software rendering | P4, S3 |
| `pocketjs_esp32p4_ppa` | Blocking P4 FILL, A8 BLEND, and SRM acceleration | P4 |
| `pocketjs_runner` | Optional exact-cadence owner task | P4, S3 |

The full dependency graph and integration sequence are documented in the
[ESP-IDF guide](https://pocketjs.dev/docs/esp-idf/). Component headers live
under `include/pocketjs/`; their comments are the API authority.

## Build the examples

The caller-driven smoke example builds a package from source when the PocketJS
CLI is in `PATH`:

```sh
cd hosts/esp-idf/examples/smoke
idf.py set-target esp32p4 # or esp32s3
idf.py build
```

Pass `-DPOCKETJS_SMOKE_PREBUILT_PACKAGE=/absolute/app.pocket` to build without
Bun. Registry releases provide the matching target-native Rust archive; a
PocketJS source checkout can set `POCKETJS_RUST_FROM_SOURCE=ON` and uses the
Rust environment already supplied by the developer.

`examples/runner` executes the same application through the optional task
runner. Neither example initializes a panel or reads a device input driver.

## Maintainer workflow

Archive receipts, Registry packaging, CI, and hardware gates are described in
[`docs/ESP_IDF.md`](../../docs/ESP_IDF.md).
