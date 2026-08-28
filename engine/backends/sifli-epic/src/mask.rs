//! CPU construction of A8 coverage planes for glyph runs and alpha-only
//! textures. The formulas match the core rasterizer's coverage and sampling
//! so a hardware A8 blend reproduces the software result.

use pocketjs_core::raster::{coverage_index, linear_sample_coordinates};
use pocketjs_core::text::Atlas;
use pocketjs_core::{spec, TexView};

use crate::geom::{logical_rect, physical_rect, wh, xy, Clip, Rect};

/// Set every mask byte under `rect` (target coordinates, `stride` bytes per
/// row) to `value`.
#[inline]
pub fn fill_mask_rect(mask: &mut [u8], stride: u32, rect: Rect, value: u8) {
    for y in rect.y..rect.y + rect.h {
        let start = y as usize * stride as usize + rect.x as usize;
        mask[start..start + rect.w as usize].fill(value);
    }
}

/// Union of two coverage values: `s + d * (1 - s)`.
#[inline]
pub fn composite_mask(destination: &mut u8, source: u8) {
    let d = *destination as u32;
    let s = source as u32;
    *destination = (s + (d * (255 - s) + 127) / 255) as u8;
}

/// Composite one GLYPH_RUN's atlas coverage into `mask` over `global_rect`
/// (the run's physical bounds in global target coordinates). `alpha` is the
/// run color's alpha, folded into the coverage; `surface` is the target's
/// logical origin (non-zero for strips).
#[allow(clippy::too_many_arguments)]
pub fn composite_glyph_run(
    atlas: &Atlas,
    op: &[u32],
    global_rect: Rect,
    surface: Clip,
    scale: u32,
    alpha: u32,
    mask: &mut [u8],
    stride: u32,
) {
    let scale = scale as i32;
    let cell_w = atlas.cell_w as i32;
    let cell_h = atlas.cell_h as i32;
    let density = atlas.raster_density as i32;
    let coverage_w = atlas.coverage_width() as i32;
    let coverage_h = atlas.coverage_height() as i32;
    let bpr = atlas.bytes_per_row();
    for glyph in op[3..].chunks_exact(2) {
        let (gx, gy) = xy(glyph[0]);
        let gid = (glyph[1] & 0xffff) as u16;
        if gid >= atlas.glyph_count {
            continue;
        }
        let rows = atlas.glyph_rows(gid);
        let x0 = (gx * scale).max(global_rect.x as i32);
        let y0 = (gy * scale).max(global_rect.y as i32);
        let x1 = ((gx + cell_w) * scale).min((global_rect.x + global_rect.w) as i32);
        let y1 = ((gy + cell_h) * scale).min((global_rect.y + global_rect.h) as i32);
        for py in y0..y1 {
            let sy = coverage_index(py - gy * scale, scale, density, coverage_h);
            let row = &rows[sy * bpr..];
            for px in x0..x1 {
                let sx = coverage_index(px - gx * scale, scale, density, coverage_w);
                let local_x = px - surface.x0 * scale;
                let local_y = py - surface.y0 * scale;
                composite_mask(
                    &mut mask[local_y as usize * stride as usize + local_x as usize],
                    ((row[sx] as u32 * alpha + 127) / 255) as u8,
                );
            }
        }
    }
}

#[inline]
fn texel_alpha(view: &TexView<'_>, x: i32, y: i32) -> u32 {
    let x = x.clamp(0, view.w as i32 - 1) as usize;
    let y = y.clamp(0, view.h as i32 - 1) as usize;
    let index = y * view.w as usize + x;
    match view.psm {
        spec::psm::PSM_8888 => view.pixels[index * 4 + 3] as u32,
        spec::psm::PSM_4444 => {
            let o = index * 2;
            ((u16::from_le_bytes([view.pixels[o], view.pixels[o + 1]]) >> 12) as u32) * 17
        }
        spec::psm::PSM_T8 => {
            let palette = view.palette.unwrap();
            palette[view.pixels[index] as usize * 4 + 3] as u32
        }
        _ => 0,
    }
}

/// Alpha of a texture at normalized `(u, v)`, nearest or bilinear exactly as
/// the core samples it.
pub fn sample_alpha(view: &TexView<'_>, u: f32, v: f32) -> u8 {
    if !view.linear {
        return texel_alpha(view, (u * view.w as f32) as i32, (v * view.h as f32) as i32) as u8;
    }
    let Some(sample) = linear_sample_coordinates(view.w, view.h, u, v) else {
        return 0;
    };
    let lerp = |a: u32, b: u32, f: u32| (a * (256 - f) + b * f) >> 8;
    let top = lerp(
        texel_alpha(view, sample.x0 as i32, sample.y0 as i32),
        texel_alpha(view, sample.x1 as i32, sample.y0 as i32),
        sample.fx,
    );
    let bottom = lerp(
        texel_alpha(view, sample.x0 as i32, sample.y1 as i32),
        texel_alpha(view, sample.x1 as i32, sample.y1 as i32),
        sample.fx,
    );
    lerp(top, bottom, sample.fy) as u8
}

/// Composite one TEX_QUAD's alpha channel (times `global_alpha`) into `mask`.
#[allow(clippy::too_many_arguments)]
pub fn alpha_quad_into_mask(
    view: &TexView<'_>,
    op: &[u32],
    surface: Clip,
    clip: Clip,
    scale: u32,
    mask: &mut [u8],
    stride: u32,
    global_alpha: u8,
) {
    let logical = logical_rect(op[2], op[3]).intersect(clip);
    if logical.is_empty() {
        return;
    }
    let (x, y) = xy(op[2]);
    let (w, h) = wh(op[3]);
    let scale_i = scale as i32;
    let physical = physical_rect(logical, scale);
    let u0 = f32::from_bits(op[4]);
    let v0 = f32::from_bits(op[5]);
    let u1 = f32::from_bits(op[6]);
    let v1 = f32::from_bits(op[7]);
    for py in physical.y..physical.y + physical.h {
        let v = v0 + (v1 - v0) * ((py as i32 - y * scale_i) as f32 + 0.5) / (h * scale_i) as f32;
        for px in physical.x..physical.x + physical.w {
            let u =
                u0 + (u1 - u0) * ((px as i32 - x * scale_i) as f32 + 0.5) / (w * scale_i) as f32;
            let alpha = (sample_alpha(view, u, v) as u32 * global_alpha as u32 + 127) / 255;
            let local_x = px as i32 - surface.x0 * scale_i;
            let local_y = py as i32 - surface.y0 * scale_i;
            composite_mask(
                &mut mask[local_y as usize * stride as usize + local_x as usize],
                alpha as u8,
            );
        }
    }
}
