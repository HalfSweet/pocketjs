//! Turn one frame's plan into executor commands for one damage region.
//!
//! Every item is clipped to the region, checked against the executor's
//! capabilities and thresholds, and either submitted as a [`Cmd`] or
//! appended to a CPU batch. Consecutive CPU items share one rasterizer
//! dispatch; a fence separates hardware writes from CPU writes so painter
//! order holds on both sides.

use alloc::vec::Vec;

use pocketjs_core::raster::{render_scaled_rgb565_over, render_scaled_rgb565_window_over};
use pocketjs_core::{spec, TexView, Ui};

use crate::caps::Capabilities;
use crate::cmd::{Cmd, Corners, Filter, MaskId, MaskRef, Mirror, PixelFormat, TexSrc, MODULATE_NONE};
use crate::geom::{
    channels, fill_rgb565_rect, local_physical_rect, pack_wh, pack_xy, physical_rect, Clip,
    Point, Rect,
};
use crate::mask::{alpha_quad_into_mask, composite_glyph_run, fill_mask_rect};
use crate::plan::PlanItem;
use crate::quad::{axis_aligned_texture_rect, texture_source_rect};
use crate::renderer::RenderStats;
use crate::submit::Frame;

/// Number of A8 planes the emitter alternates between so CPU mask
/// construction can overlap an in-flight blend of the other plane.
pub(crate) const MASK_PLANES: u8 = 2;

/// Per-frame state shared by every region.
pub(crate) struct Context<'r> {
    pub caps: Capabilities,
    pub scale: u32,
    /// Logical rectangle stored in the bound target (the viewport, or a
    /// strip's band).
    pub surface: Clip,
    /// True when `surface` is the whole viewport (direct full-target replay).
    pub full_screen: bool,
    pub width: u32,
    pub stats: &'r mut RenderStats,
    pub fallback: &'r mut Vec<u32>,
    pub mask_index: &'r mut u8,
}

impl Context<'_> {
    fn next_mask(&mut self) -> MaskId {
        let id = MaskId(*self.mask_index);
        *self.mask_index = (*self.mask_index + 1) % MASK_PLANES;
        id
    }

    fn local(&self, clip: Clip) -> Rect {
        local_physical_rect(clip, self.surface, self.scale)
    }

    /// Append one DrawList op to the pending CPU batch under `clip`.
    fn push_cpu(&mut self, words: &[u32], at: usize, len: usize, clip: Clip) {
        if clip.is_empty() {
            return;
        }
        self.fallback.push(spec::draw_op::SCISSOR);
        self.fallback.push(pack_xy(clip.x0, clip.y0));
        self.fallback
            .push(pack_wh(clip.x1 - clip.x0, clip.y1 - clip.y0));
        self.fallback.extend_from_slice(&words[at..at + len]);
        self.fallback.push(spec::draw_op::SCISSOR_POP);
        self.stats.software_ops += 1;
        self.stats.software_words += len as u32;
    }

    /// Render the pending CPU batch into the target after a fence.
    fn flush_cpu<F: Frame>(&mut self, ui: &Ui, frame: &mut F) -> Option<()> {
        if self.fallback.is_empty() {
            return Some(());
        }
        frame.fence().ok()?;
        let target = frame.target_mut()?;
        if self.full_screen {
            render_scaled_rgb565_over(ui, self.fallback, target, self.scale);
        } else {
            render_scaled_rgb565_window_over(ui, self.fallback, target, self.scale, self.surface);
        }
        self.fallback.clear();
        Some(())
    }

    fn submit<F: Frame>(&mut self, ui: &Ui, frame: &mut F, cmd: Cmd<'_>) -> Option<()> {
        self.flush_cpu(ui, frame)?;
        frame.submit(&[cmd]).ok()
    }

    /// Clear `region` to black before replaying the DrawList into it.
    pub(crate) fn clear_region<F: Frame>(
        &mut self,
        ui: &Ui,
        frame: &mut F,
        region: Clip,
    ) -> Option<()> {
        let physical = self.local(region);
        if physical.is_empty() {
            return Some(());
        }
        if self.caps.fill_opaque && physical.area() >= self.caps.thresholds.min_fill {
            self.submit(
                ui,
                frame,
                Cmd::Fill {
                    dst: physical,
                    color: [0, 0, 0],
                },
            )?;
            self.stats.epic_fills += 1;
        } else {
            self.flush_cpu(ui, frame)?;
            frame.fence().ok()?;
            fill_rgb565_rect(frame.target_mut()?, self.width, physical, 0);
        }
        Some(())
    }

    /// Emit every plan item that touches `region`.
    pub(crate) fn emit_region<F: Frame>(
        &mut self,
        ui: &Ui,
        words: &[u32],
        plan: &[PlanItem],
        frame: &mut F,
        region: Clip,
    ) -> Option<()> {
        for item in plan {
            match *item {
                PlanItem::Rect { logical, color } => {
                    let logical = logical.intersect(region);
                    if logical.is_empty() {
                        continue;
                    }
                    self.rect(ui, frame, logical, color)?;
                }
                PlanItem::Gradient {
                    at,
                    original,
                    logical,
                    from,
                    to,
                    direction,
                    clip,
                } => {
                    let logical = logical.intersect(region);
                    if logical.is_empty() {
                        continue;
                    }
                    if !self.gradient(ui, frame, original, logical, from, to, direction)? {
                        self.push_cpu(words, at, 6, clip.intersect(region));
                    }
                }
                PlanItem::Glyphs {
                    at,
                    len,
                    bounds,
                    slot,
                    color,
                    clip,
                } => {
                    let bounds = bounds.intersect(region);
                    if bounds.is_empty() {
                        continue;
                    }
                    let op = &words[at..at + len];
                    if !self.glyphs(ui, frame, op, bounds, slot, color)? {
                        self.push_cpu(words, at, len, clip.intersect(region));
                    }
                }
                PlanItem::AlphaQuads {
                    at,
                    count,
                    handle,
                    modulate,
                    bounds,
                    clip,
                } => {
                    let bounds = bounds.intersect(region);
                    if bounds.is_empty() {
                        continue;
                    }
                    let clip = clip.intersect(region);
                    if !self.alpha_quads(ui, words, frame, at, count, handle, modulate, bounds, clip)? {
                        for index in 0..count {
                            self.push_cpu(words, at + index * 9, 9, clip);
                        }
                    }
                }
                PlanItem::TexQuad {
                    at,
                    handle,
                    logical,
                    clip,
                } => {
                    let logical = logical.intersect(region);
                    if logical.is_empty() {
                        continue;
                    }
                    let op = &words[at..at + 9];
                    if !self.tex_quad(ui, frame, op, handle, logical)? {
                        self.push_cpu(words, at, 9, clip.intersect(region));
                    }
                }
                PlanItem::TriPair {
                    at,
                    quad,
                    color,
                    bounds,
                    axis_aligned,
                    clip,
                } => {
                    let bounds = bounds.intersect(region);
                    if bounds.is_empty() {
                        continue;
                    }
                    if !self.tri_pair(ui, frame, quad, color, bounds, axis_aligned)? {
                        let clip = clip.intersect(region);
                        self.push_cpu(words, at, 7, clip);
                        self.push_cpu(words, at + 7, 7, clip);
                    }
                }
                PlanItem::TexTriPair {
                    at,
                    handle,
                    modulate,
                    source_rect,
                    quad,
                    bounds,
                    clip,
                } => {
                    let bounds = bounds.intersect(region);
                    if bounds.is_empty() {
                        continue;
                    }
                    if !self.tex_tri_pair(ui, frame, handle, modulate, source_rect, quad, bounds)? {
                        let clip = clip.intersect(region);
                        self.push_cpu(words, at, 12, clip);
                        self.push_cpu(words, at + 12, 12, clip);
                    }
                }
                PlanItem::Cpu { at, len, clip } => {
                    self.push_cpu(words, at, len, clip.intersect(region));
                }
            }
        }
        self.flush_cpu(ui, frame)
    }

    fn rect<F: Frame>(
        &mut self,
        ui: &Ui,
        frame: &mut F,
        logical: Clip,
        color: u32,
    ) -> Option<()> {
        let (r, g, b, a) = channels(color);
        if a == 0 {
            return Some(());
        }
        let rect = self.local(logical);
        let rgb = [r as u8, g as u8, b as u8];
        let opaque = a == 255;
        let threshold = if opaque {
            self.caps.thresholds.min_fill
        } else {
            self.caps.thresholds.min_blend
        };
        let hardware_fill = if opaque {
            self.caps.fill_opaque
        } else {
            self.caps.fill_alpha
        };
        if hardware_fill && rect.area() >= threshold {
            let cmd = if opaque {
                Cmd::Fill { dst: rect, color: rgb }
            } else {
                Cmd::FillAlpha {
                    dst: rect,
                    color: rgb,
                    alpha: a as u8,
                }
            };
            self.submit(ui, frame, cmd)?;
            self.stats.epic_fills += 1;
            return Some(());
        }
        if !opaque && self.caps.a8_blend && rect.area() >= self.caps.thresholds.min_blend {
            let mask = self.next_mask();
            let stride = self.width;
            fill_mask_rect(frame.mask_mut(mask), stride, rect, a as u8);
            self.submit(
                ui,
                frame,
                Cmd::BlendA8 {
                    dst: rect,
                    mask: MaskRef {
                        mask,
                        offset: rect.y * stride + rect.x,
                        stride,
                    },
                    color: rgb,
                    alpha: 255,
                },
            )?;
            self.stats.epic_blends += 1;
            return Some(());
        }
        // Small or unsupported: replay the rectangle on the CPU with the
        // exact integer blend, batched with its neighbours.
        self.fallback.push(spec::draw_op::SCISSOR);
        self.fallback.push(pack_xy(logical.x0, logical.y0));
        self.fallback
            .push(pack_wh(logical.x1 - logical.x0, logical.y1 - logical.y0));
        self.fallback.extend_from_slice(&[
            spec::draw_op::RECT,
            pack_xy(logical.x0, logical.y0),
            pack_wh(logical.x1 - logical.x0, logical.y1 - logical.y0),
            color,
        ]);
        self.fallback.push(spec::draw_op::SCISSOR_POP);
        self.stats.software_ops += 1;
        self.stats.software_words += 4;
        Some(())
    }

    #[allow(clippy::too_many_arguments)]
    fn gradient<F: Frame>(
        &mut self,
        ui: &Ui,
        frame: &mut F,
        original: Clip,
        logical: Clip,
        from: u32,
        to: u32,
        direction: u32,
    ) -> Option<bool> {
        // Restarting a hardware gradient at a damage/scissor edge changes its
        // phase. Keep clipped gradients on the exact software path until the
        // executor exposes a sampling-origin control.
        if !self.caps.gradient
            || logical != original
            || original.intersect(self.surface) != original
        {
            return Some(false);
        }
        let (_, _, _, from_alpha) = channels(from);
        let (_, _, _, to_alpha) = channels(to);
        if from_alpha != 255 || to_alpha != 255 {
            return Some(false);
        }
        let corners = match direction {
            value if value == spec::GradDir::ToBottom as u32 => Corners {
                top_left: from,
                top_right: from,
                bottom_left: to,
                bottom_right: to,
            },
            value if value == spec::GradDir::ToTop as u32 => Corners {
                top_left: to,
                top_right: to,
                bottom_left: from,
                bottom_right: from,
            },
            value if value == spec::GradDir::ToRight as u32 => Corners {
                top_left: from,
                top_right: to,
                bottom_left: from,
                bottom_right: to,
            },
            value if value == spec::GradDir::ToLeft as u32 => Corners {
                top_left: to,
                top_right: from,
                bottom_left: to,
                bottom_right: from,
            },
            _ => return Some(false),
        };
        let rect = self.local(logical);
        if rect.is_empty() || rect.area() < self.caps.thresholds.min_gradient {
            return Some(false);
        }
        self.submit(ui, frame, Cmd::Gradient { dst: rect, corners })?;
        self.stats.epic_gradients += 1;
        Some(true)
    }

    fn glyphs<F: Frame>(
        &mut self,
        ui: &Ui,
        frame: &mut F,
        op: &[u32],
        bounds: Clip,
        slot: u8,
        color: u32,
    ) -> Option<bool> {
        let (r, g, b, a) = channels(color);
        if a == 0 {
            return Some(true);
        }
        let Some(atlas) = ui.font_atlas(slot) else {
            return Some(true);
        };
        let global_rect = physical_rect(bounds, self.scale);
        let rect = self.local(bounds);
        if !self.caps.a8_blend || rect.is_empty() || rect.area() < self.caps.thresholds.min_blend {
            return Some(false);
        }
        let mask = self.next_mask();
        let stride = self.width;
        let plane = frame.mask_mut(mask);
        fill_mask_rect(plane, stride, rect, 0);
        composite_glyph_run(atlas, op, global_rect, self.surface, self.scale, a, plane, stride);
        self.submit(
            ui,
            frame,
            Cmd::BlendA8 {
                dst: rect,
                mask: MaskRef {
                    mask,
                    offset: rect.y * stride + rect.x,
                    stride,
                },
                color: [r as u8, g as u8, b as u8],
                alpha: 255,
            },
        )?;
        self.stats.epic_blends += 1;
        Some(true)
    }

    #[allow(clippy::too_many_arguments)]
    fn alpha_quads<F: Frame>(
        &mut self,
        ui: &Ui,
        words: &[u32],
        frame: &mut F,
        at: usize,
        count: usize,
        handle: i32,
        modulate: u32,
        bounds: Clip,
        clip: Clip,
    ) -> Option<bool> {
        let (r, g, b, a) = channels(modulate);
        if a == 0 {
            return Some(true);
        }
        let Some(view) = ui.texture(handle) else {
            return Some(false);
        };
        let rect = self.local(bounds);
        if !self.caps.a8_blend || rect.is_empty() || rect.area() < self.caps.thresholds.min_blend {
            return Some(false);
        }
        let mask = self.next_mask();
        let stride = self.width;
        let scale = self.scale;
        let surface = self.surface;
        let plane = frame.mask_mut(mask);
        fill_mask_rect(plane, stride, rect, 0);
        for index in 0..count {
            let op = &words[at + index * 9..at + index * 9 + 9];
            alpha_quad_into_mask(&view, op, surface, clip, scale, plane, stride, a as u8);
        }
        self.submit(
            ui,
            frame,
            Cmd::BlendA8 {
                dst: rect,
                mask: MaskRef {
                    mask,
                    offset: rect.y * stride + rect.x,
                    stride,
                },
                color: [r as u8, g as u8, b as u8],
                alpha: 255,
            },
        )?;
        self.stats.epic_blends += 1;
        Some(true)
    }

    fn tex_quad<F: Frame>(
        &mut self,
        ui: &Ui,
        frame: &mut F,
        op: &[u32],
        handle: i32,
        logical: Clip,
    ) -> Option<bool> {
        let Some(view) = ui.texture(handle) else {
            return Some(false);
        };
        let modulate = op[8];
        let destination = self.local(logical);
        if destination.is_empty() {
            return Some(true);
        }

        // Opaque PSM_5650 copies: 1:1 (with mirroring) or hardware-scaled
        // when the texture asks for linear sampling.
        if view.psm == spec::psm::PSM_5650
            && self.caps.copy_psm5650
            && destination.area() >= self.caps.thresholds.min_blit
        {
            // A fractional texel edge on a PSM_5650 quad is a sampling
            // transform the copy engine cannot express; keep the whole op on
            // the CPU rather than re-deriving a different phase.
            let Some((source_rect, mirror_x, mirror_y)) =
                texture_source_rect(&view, op, logical)
            else {
                return Some(false);
            };
            let one_to_one =
                source_rect.w == destination.w && source_rect.h == destination.h;
            if (one_to_one || view.linear) && modulate == MODULATE_NONE {
                self.submit(
                    ui,
                    frame,
                    Cmd::Blit {
                        src: portable(&view, PixelFormat::Psm5650),
                        src_rect: source_rect,
                        dst: destination,
                        clip: destination,
                        mirror: Mirror {
                            x: mirror_x,
                            y: mirror_y,
                        },
                        modulate: MODULATE_NONE,
                        filter: Filter::Nearest,
                    },
                )?;
                self.stats.epic_copies += 1;
                return Some(true);
            }
        }

        let (_, _, _, alpha) = channels(modulate);
        if alpha == 0 {
            return Some(true);
        }
        let Some(format) = pixel_format(&view) else {
            return Some(false);
        };
        if !self.caps.blits(format) || destination.area() < self.caps.thresholds.min_blit {
            return Some(false);
        }
        let Some((source_rect, mirror_x, mirror_y)) = texture_source_rect(&view, op, logical)
        else {
            return Some(false);
        };
        self.submit(
            ui,
            frame,
            Cmd::Blit {
                src: portable(&view, format),
                src_rect: source_rect,
                dst: destination,
                clip: destination,
                mirror: Mirror {
                    x: mirror_x,
                    y: mirror_y,
                },
                modulate,
                filter: filter(&view),
            },
        )?;
        self.stats.epic_copies += 1;
        Some(true)
    }

    fn tri_pair<F: Frame>(
        &mut self,
        ui: &Ui,
        frame: &mut F,
        quad: [Point; 4],
        color: u32,
        bounds: Clip,
        axis_aligned: bool,
    ) -> Option<bool> {
        let destination_clip = self.local(bounds);
        if destination_clip.area() < self.caps.thresholds.min_fill {
            return Some(false);
        }
        let (r, g, b, a) = channels(color);
        if a == 0 {
            return Some(true);
        }
        if axis_aligned {
            // Z-only 2.5D projection and scaleX keep a solid card face axis
            // aligned; the native rectangle fill covers it without a texture.
            let cmd = if a == 255 {
                if !self.caps.fill_opaque {
                    return Some(false);
                }
                Cmd::Fill {
                    dst: destination_clip,
                    color: [r as u8, g as u8, b as u8],
                }
            } else {
                if !self.caps.fill_alpha {
                    return Some(false);
                }
                Cmd::FillAlpha {
                    dst: destination_clip,
                    color: [r as u8, g as u8, b as u8],
                    alpha: a as u8,
                }
            };
            self.submit(ui, frame, cmd)?;
            self.stats.epic_fills += 1;
            return Some(true);
        }
        if !self.caps.blit_quad.rgba8888 {
            return Some(false);
        }
        let physical_quad = self.physical_quad(quad);
        self.submit(
            ui,
            frame,
            Cmd::BlitQuad {
                src: TexSrc::Solid { abgr: color },
                src_rect: Rect {
                    x: 0,
                    y: 0,
                    w: 1,
                    h: 1,
                },
                quad: physical_quad,
                clip: destination_clip,
                modulate: MODULATE_NONE,
                filter: Filter::Nearest,
            },
        )?;
        self.stats.epic_copies += 1;
        Some(true)
    }

    #[allow(clippy::too_many_arguments)]
    fn tex_tri_pair<F: Frame>(
        &mut self,
        ui: &Ui,
        frame: &mut F,
        handle: i32,
        modulate: u32,
        source_rect: Rect,
        quad: [Point; 4],
        bounds: Clip,
    ) -> Option<bool> {
        let destination_clip = self.local(bounds);
        if destination_clip.area() < self.caps.thresholds.min_blit {
            return Some(false);
        }
        let (_, _, _, alpha) = channels(modulate);
        if alpha == 0 {
            return Some(true);
        }
        let Some(view) = ui.texture(handle) else {
            return Some(false);
        };
        let Some(format) = pixel_format(&view) else {
            return Some(false);
        };
        let physical_quad = self.physical_quad(quad);
        if let Some(destination) = axis_aligned_texture_rect(physical_quad) {
            if self.caps.blits(format) {
                self.submit(
                    ui,
                    frame,
                    Cmd::Blit {
                        src: portable(&view, format),
                        src_rect: source_rect,
                        dst: destination,
                        clip: destination_clip,
                        mirror: Mirror::default(),
                        modulate,
                        filter: filter(&view),
                    },
                )?;
                self.stats.epic_copies += 1;
                return Some(true);
            }
        }
        if !self.caps.blits_quad(format) {
            return Some(false);
        }
        self.submit(
            ui,
            frame,
            Cmd::BlitQuad {
                src: portable(&view, format),
                src_rect: source_rect,
                quad: physical_quad,
                clip: destination_clip,
                modulate,
                filter: filter(&view),
            },
        )?;
        self.stats.epic_copies += 1;
        Some(true)
    }

    fn physical_quad(&self, quad: [Point; 4]) -> [Point; 4] {
        let scale = self.scale as i32;
        quad.map(|point| Point {
            x: (point.x - self.surface.x0) * scale,
            y: (point.y - self.surface.y0) * scale,
        })
    }
}

fn pixel_format(view: &TexView<'_>) -> Option<PixelFormat> {
    match view.psm {
        spec::psm::PSM_5650 => Some(PixelFormat::Psm5650),
        spec::psm::PSM_8888 => Some(PixelFormat::Rgba8888),
        spec::psm::PSM_T8 if view.palette.is_some() => Some(PixelFormat::T8Clut),
        _ => None,
    }
}

fn portable<'a>(view: &TexView<'a>, format: PixelFormat) -> TexSrc<'a> {
    TexSrc::Portable {
        pixels: view.pixels,
        palette: view.palette,
        width: view.w,
        height: view.h,
        format,
    }
}

fn filter(view: &TexView<'_>) -> Filter {
    if view.linear {
        Filter::Linear
    } else {
        Filter::Nearest
    }
}
