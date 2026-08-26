# ESP-IDF component maintenance

PocketJS publishes six caller-driven components and one optional task runner
from `hosts/esp-idf/components`. **ESP32-P4 and ESP32-S3 consume the same C ABI
and package contract.** P4 can add the PPA component; S3 always uses the
software RGB565 path.

## Dependency gates

```text
pocketjs_package
pocketjs_guest -> quickjs-ng
pocketjs_ui_core
pocketjs_ui_qjs -> pocketjs_guest + pocketjs_ui_core
pocketjs_render_rgb565 -> pocketjs_ui_core
pocketjs_esp32p4_ppa -> pocketjs_render_rgb565 + esp_driver_ppa
pocketjs_runner -> pocketjs_ui_qjs
```

Only `pocketjs_runner` calls `xTaskCreatePinnedToCore`. Only
`pocketjs_esp32p4_ppa` includes PPA driver headers. Package, core, and renderer
must not acquire a QuickJS dependency.

## Native archives

Registry archives for `pocketjs_ui_core` contain:

```text
lib/esp32p4/libpocketjs_idf_native.a
lib/esp32p4/build-receipt.json
lib/esp32s3/libpocketjs_idf_native.a
lib/esp32s3/build-receipt.json
```

Build P4 with a Rust installation that provides
`riscv32imafc-unknown-none-elf`:

```sh
bun tools/esp-idf-native.ts --target esp32p4
```

Build S3 with a Rust installation whose `cargo` and `rustc` support
`xtensa-esp32s3-none-elf`:

```sh
bun tools/esp-idf-native.ts \
  --target esp32s3 \
  --cargo /path/to/xtensa-rust/bin/cargo
```

The script does not download or select a toolchain. The receipt records the
compiler commit, Rust target, source digest, archive digest, and byte size.
Release automation builds both archives before uploading the component.
If the host's default `ar` cannot resolve GNU archive long names, pass LLVM or
GNU ar with `--archiver /path/to/llvm-ar`.

The component build accepts `POCKETJS_RUST_FROM_SOURCE=ON` for development.
P4 runs a locked `cargo build --no-default-features`; S3 additionally uses
`-Zbuild-std=core,alloc`. Missing Cargo, target support, or linker state is an
error with no installation side effect.

**The P4 preparation step removes Rust-bundled soft-float compiler-rt C
members. ESP-IDF provides the equivalent runtime symbols through ROM or
libgcc under its `ilp32f` ABI.**

## Package contract

`pocket.host.json` is validated against
`contracts/schema/pocket-idf-host-1.json`. Its canonical SHA-256 travels in the
resolved plan, generated C contract, and `.pocket` host-input section.

Section kind 7 is a 104-byte little-endian record:

| Offset | Field |
| ---: | --- |
| 0 | `PHST` magic |
| 4 | format version 1 |
| 8 | HostOps ABI |
| 12 | tick rate |
| 16–28 | logical and physical width/height |
| 32 | raster density |
| 36 | presentation enum |
| 40 | 32-byte host-profile SHA-256 |
| 72 | 32-byte resolved-plan SHA-256 |

The variant table still carries target id and HostOps ABI. Device admission
checks both copies, every numeric host field, and the profile hash before
returning JS or PAK bytes. The JSON plan remains in section kind 2 for artifact
inspection and is not parsed during device boot.

## CI

`.github/workflows/esp-idf.yml` is manually dispatched for release validation
and runs:

- host contract/package tests and the native Rust test;
- native archive builds for both targets;
- ESP-IDF `release/v6.0` and `release/v6.1` firmware builds for both targets;
- the same generated `.pocket` in each firmware build.

**The committed package fixture and this workflow use Bun 1.3.14.** Bundle
bytes are compiler-versioned; update the pin and fixture in the same change.

The smoke application renders a 320×240 logical UI. The P4 build includes the
PPA component. The S3 build uses `MINIMAL_BUILD` and excludes PPA from component
discovery and the link map.

## Hardware release gates

Before a component release:

1. Flash `examples/smoke` to the Waveshare ESP32-P4 test board. Require the
   fixed framebuffer hash and non-zero FILL, BLEND, and SRM counters.
2. Flash the S3 build to AtomS3R. Require the same software-reference pixel
   hash and no PPA symbols in the map.
3. Run 10,000 caller-driven turns on both boards and compare heap before and
   after the steady-state interval.
4. Run `examples/runner` for 30 seconds. Require the configured cadence, zero
   ordinary skipped frames, and a successful stop/join.
5. Archive UART receipts, map files, component versions, native-archive
   receipts, and firmware SHA-256 values with the release record.

## Registry release

All PocketJS components use the same release version. Internal manifest
dependencies require that exact version. Upload leaf components only after
the corresponding dependency versions exist in the Registry. The release
archive for `pocketjs_ui_core` is assembled after both native archives and
receipts have been generated.

Every component's `documentation` URL must resolve to the ESP-IDF guide. Build
the site and the two example projects before publishing. A release is rejected
if the prebuilt package path needs Bun or if the default native-archive path
invokes Cargo.
