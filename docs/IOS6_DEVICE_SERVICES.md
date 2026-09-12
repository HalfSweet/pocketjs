# iOS 6 device services

The `ipodtouch4-dev` host provides `camera.qr`, `host.clipboard`,
`host.activity`, and the existing `data.fs` contract. Applications declare
the capabilities they require in `pocket.json`. Other legacy hosts do not
link the new services unless explicitly configured to do so.

## SDK and delivery

`@pocketjs/framework/device` exports `scanQr(onResult)`,
`clipboard.readText()`, `clipboard.writeText(text)`, and
`onAppActivity(listener)`. The scanner returns a cancellation function.
Results are `decoded` with text, `cancelled`, or `error` with a stable code.
Clipboard methods return an `ok` result; writes are immediately read back
for confirmation. None of the methods logs payloads.

The host mounts `globalThis.device` before guest evaluation. Scanner results
are polled through the shared service pump, at a guest frame boundary.
Native callbacks never enter QuickJS from the camera queue. Only one scan
is pending at a time. Cancellation supersedes a decoded result that has not
yet reached the guest, including when the app becomes inactive.

Activity is a transition sequence, initially zero: even values mean active,
odd values mean inactive. The SDK reports an inactive interval even when
suspension kept the guest from observing its first transition. A synchronous
UIKit curtain covers the app before the system snapshot; it is removed after
the first resumed guest update and presentation. Applications remain
responsible for clearing their drafts in the activity callback.

## Camera and storage

The camera uses AVFoundation's video data output and a native preview layer.
It starts at 640×480 BGRA. A serial queue converts accepted frames to luma
and passes them to quirc at most eight times per second. Late frames are
discarded. Decoder buffers are reused at a fixed resolution. Stop, cancel,
failure and deactivation detach the delegate, stop capture, drain the queue
and release camera resources. The frame pixels never cross into JS.

The original quirc source and ISC license live in `hosts/ios-legacy/quirc`.
`SOURCE.json` records its exact upstream revision and per-file hashes, checked
at build time. AVFoundation/CoreMedia/CoreVideo/QuartzCore link stubs are
generated locally from hash-pinned images in the already validated iOS 6
sysroot. No Apple binary is shipped.

The C filesystem adapter implements all nine `data.fs` operations, including
UTF-8/text and Base64/binary payloads, 64 KiB I/O limits and paged listings.
Paths are relative, segment-validated and checked for symlinks. Its data root
is `Library/PocketJS/data` inside the User app container. Atomic truncate
writes use fsynced temporary files beside the data root and rename them into
place. Applications needing atomic snapshots larger than one I/O operation
write a staging file and rename it after all chunks succeed.

This storage is **not encrypted** and does not implement a credential vault,
authentication or backups. Clipboard cleanup is not scheduled.

## Terminal touch facts

The packed contact word retains its previous coordinates and eight-bit ID.
Bit 31 still selects wide coordinates. Bit 30 marks cancellation; bit 29
marks an ordinary release with its final coordinates. Unmarked legacy tapes
retain their meaning. Both facts travel through the existing recorded touch
track, without introducing a timing side channel.

`touches()` excludes terminal facts. The gesture pump consumes cancellation
before any release handler, delivering `onCancel` without tap or pan commit.
An ordinary release updates the final position, then ends an existing pan.
Final movement beyond tap slop cannot turn into button activation. The
native slot table preserves these facts for one frame and still latches very
short, previously unseen taps.

The matching host and guest revisions should be built together. Tests cover
retreat at release, system cancellation, short cancellations and immediate
contact-ID reuse.

## Verification

```sh
bun test --conditions=browser ./tests/gesture.test.ts ./tests/touch.test.ts \
  ./tests/touch-terminal.test.ts ./tests/device-services.test.ts \
  ./tests/ipodtouch4-profile.test.ts ./tests/ipodtouch4-installation.test.ts
bun tests/contract.ts
```

`tests/native-ios-fs-runner.c` runs a bundled guest against the actual C
adapter and pinned QuickJS with the same JS stack limit as the device.
`tests/native-qr-runner.c` verifies decoded fixture payloads without printing
them. Pocket Flickey supplies the executable test drivers and public test
fixtures for both. Physical camera/interaction acceptance is separate from
these host-side checks.
