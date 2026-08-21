# PocketJS SiFli EPIC backend

This `no_std` crate interprets PocketJS DrawLists into a persistent RGB565
surface and submits compatible operations through the narrow `EpicOps` trait.
The trait deliberately contains no RT-Thread, LVGL, or SiFli driver types. A
board host can implement it directly with the SiFli HAL.

Accelerated paths are:

- opaque and translucent RGB565 rectangle fills;
- opaque horizontal and vertical two-stop gradients when the operation is not
  clipped;
- A8 coverage blended with a fixed color, used for translucent rectangles,
  glyph runs, and PocketJS rounded-corner masks;
- PSM 5650, PSM 8888, and CLUT8 texture blending with format conversion,
  color/alpha modulation, scaling, mirroring, and destination clipping;
- pairs of textured triangles reconstructed as affine or projective quads for
  rotated, scaled, and 2.5D/3D image transforms;
- pairs of flat-color triangles reconstructed as transformed solid quads.

Gouraud triangles, clipped or translucent gradients, PSM 4444 textures,
linear-filtered textures on hosts without matching sampling, unmatched or
clipped triangle fans, and unsupported copies are replayed in order through
`pocketjs_core::raster::render_scaled_rgb565_over`. Hardware and software
operations therefore share one RGB565 target without a 32-bit intermediate.

Texture transforms use four physical edge coordinates ordered TL, BL, BR,
TR. A HAL adapter may map them to EPIC's 3×3 transform matrix. PocketJS stores
PSM 8888 and CLUT entries as RGBA bytes and PSM 5650 with red in the low bits;
the SF32LB58 adapter uses the EPIC color matrix to swap red/blue while applying
DrawList modulation in the same transaction.

## Synchronous contract

Every `EpicOps` method is synchronous. Returning `true` means destination
pixels are visible to the next CPU or EPIC operation. Returning `false` means
the destination was not changed and requests software fallback.

For a write-back cached framebuffer, a HAL implementation must:

1. clean source, mask, and destination ranges before EPIC reads them;
2. wait for the EPIC transaction to complete;
3. invalidate the destination range before returning to Rust.

The SF32LB58 EPIC coordinate limit is 1010 pixels per transaction. Opaque
fills may be split because a later software retry overwrites them. Blend and
gradient implementations should reject oversized operations before writing
unless they can guarantee an all-or-nothing tiled transaction.

## Damage tracking

Keep one `RenderTargetState` for every persistent framebuffer. This is
required for RAM-less double-buffered displays because alternating targets
contain different older frames.

## Test

```bash
cargo test --locked --manifest-path engine/backends/sifli-epic/Cargo.toml \
  --features std
```

The tests compare the hybrid output against the core RGB565 software
rasterizer, including incremental damage, A8 batching, fallback ordering, and
texture sampling.
