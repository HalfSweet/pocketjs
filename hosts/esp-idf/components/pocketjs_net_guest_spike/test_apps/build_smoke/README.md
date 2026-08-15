# Guest network spike build smoke

This app links the QuickJS Guest factory, private HTTP binding, and experimental
ESP HTTP backend without creating a network interface. It validates bounded
configuration rejection at boot. Build it with the pinned ESP-IDF v6.0.2
worktree for both Phase 1 hardware targets:

```sh
idf.py -B build-esp32s3 -DIDF_TARGET=esp32s3 build
idf.py -B build-esp32p4 -DIDF_TARGET=esp32p4 build
```

Build success checks component integration only. A board application must
connect its product-specific network interface and run the Guest against an
independent peer before this spike demonstrates network communication.

The smoke app uses a 1.5 MiB factory partition because the P4 QuickJS/backend
image is larger than ESP-IDF's default 1 MiB app partition.
