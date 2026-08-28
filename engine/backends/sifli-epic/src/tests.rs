use alloc::vec;
use alloc::vec::Vec;

use pocketjs_core::raster::pack_rgb565;
use pocketjs_core::{spec, Ui};

use crate::caps::{Capabilities, Formats, Thresholds};
use crate::cmd::{Filter, Mirror, PixelFormat};
use crate::geom::{Point, Rect};
use crate::mock::MockGpu;
use crate::renderer::{RenderTargetState, Renderer, RendererConfig};

/// The default executor of these tests: opaque fills, A8 blends, gradients,
/// and 1:1 PSM_5650 copies; no translucent hardware fills and no texture
/// blits (those tests enable them explicitly).
fn caps() -> Capabilities {
    Capabilities {
        fill_opaque: true,
        fill_alpha: false,
        a8_blend: true,
        gradient: true,
        copy_psm5650: true,
        blit: Formats::NONE,
        blit_quad: Formats::NONE,
        coordinate_limit: u32::MAX,
        direct_cpu_writes: true,
        thresholds: Thresholds::ALWAYS,
    }
}

fn caps_with_texture_blits() -> Capabilities {
    Capabilities {
        blit: Formats::ALL,
        blit_quad: Formats::ALL,
        ..caps()
    }
}

fn gpu() -> MockGpu {
    MockGpu::new(caps())
}

fn xy_word(x: i16, y: i16) -> u32 {
    x as u16 as u32 | ((y as u16 as u32) << 16)
}

fn wh_word(w: u16, h: u16) -> u32 {
    w as u32 | ((h as u32) << 16)
}

fn renderer() -> Renderer {
    Renderer::new(RendererConfig { scale: 1 }).unwrap()
}

fn full_reference(ui: &Ui, words: &[u32], width: usize, height: usize) -> Vec<u16> {
    let mut output = vec![0u16; width * height];
    pocketjs_core::raster::render_scaled_rgb565(ui, words, &mut output, 1);
    output
}

#[test]
fn in_place_t8_updates_follow_the_core_classification() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let mut data = vec![0u8; 1024 + 16];
    data[0..4].copy_from_slice(&[255, 255, 255, 255]);
    let handle = ui.upload_texture(&data, 4, 4, spec::psm::PSM_T8);
    let words = [
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(1, 1),
        wh_word(4, 4),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        0xff00_80ff,
    ];
    let mut output = vec![0u16; 64];
    let mut renderer = renderer();
    let stats = renderer
        .render(&ui, &words, &mut output, 8, 8, &mut gpu())
        .unwrap();
    assert_eq!(stats.epic_blends, 1);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(output, full_reference(&ui, &words, 8, 8));

    let mut palette = vec![0u8; 1024];
    palette[0..4].copy_from_slice(&[0, 255, 0, 255]);
    assert!(ui.update_texture_t8(handle, &palette, &[0u8; 16]));
    let stats = renderer
        .render(&ui, &words, &mut output, 8, 8, &mut gpu())
        .unwrap();
    assert_eq!(stats.epic_blends, 0);
    assert_eq!(stats.software_ops, 1);
    assert_eq!(output, full_reference(&ui, &words, 8, 8));
}

#[test]
fn compact_strip_matches_the_same_window_of_a_full_render() {
    let mut ui = Ui::new();
    ui.set_viewport(16.0, 10.0);
    let words = [
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(16, 10),
        0xff20_1008,
        spec::draw_op::GRAD_RECT,
        xy_word(2, 2),
        wh_word(12, 6),
        0xff00_0040,
        0xffc0_8000,
        spec::GradDir::ToRight as u32,
        spec::draw_op::RECT,
        xy_word(5, 3),
        wh_word(5, 5),
        0x8000_ff00,
    ];
    let region = Rect {
        x: 3,
        y: 4,
        w: 9,
        h: 3,
    };
    let untouched = 0x5aa5;
    let mut strip = vec![untouched; 16 * region.h as usize];
    let mut epic = gpu();
    let stats = renderer()
        .render_strip(&ui, &words, &mut strip, region, &mut epic)
        .unwrap();
    let full = full_reference(&ui, &words, 16, 10);

    assert_eq!(stats.damage_bounds, region);
    assert!(
        stats.software_ops > 0,
        "gradient must exercise window fallback"
    );
    for local_y in 0..region.h as usize {
        let global_y = region.y as usize + local_y;
        for x in 0..16usize {
            let actual = strip[local_y * 16 + x];
            if x >= region.x as usize && x < (region.x + region.w) as usize {
                assert_eq!(actual, full[global_y * 16 + x], "pixel ({x},{global_y})");
            } else {
                assert_eq!(actual, untouched, "strip write escaped dirty x range");
            }
        }
    }
}

#[test]
fn strip_rects_and_masks_are_local_to_each_strip() {
    let mut ui = Ui::new();
    ui.set_viewport(16.0, 10.0);
    let words = [
        spec::draw_op::RECT,
        xy_word(4, 5),
        wh_word(3, 2),
        0x8000_00ff,
    ];
    let mut renderer = renderer();

    let mut large = vec![0u16; 16 * 4];
    let mut large_epic = gpu();
    renderer
        .render_strip(
            &ui,
            &words,
            &mut large,
            Rect {
                x: 4,
                y: 3,
                w: 3,
                h: 4,
            },
            &mut large_epic,
        )
        .unwrap();
    assert_eq!(large_epic.last_surface_height, 4);
    assert_eq!(
        large_epic.last_blend_rect,
        Rect {
            x: 4,
            y: 2,
            w: 3,
            h: 2,
        }
    );

    let mut small = vec![0u16; 16 * 2];
    let mut small_epic = gpu();
    renderer
        .render_strip(
            &ui,
            &words,
            &mut small,
            Rect {
                x: 4,
                y: 5,
                w: 3,
                h: 2,
            },
            &mut small_epic,
        )
        .unwrap();
    assert_eq!(small_epic.last_surface_width, 16);
    assert_eq!(small_epic.last_surface_height, 2);
    assert_eq!(
        small_epic.last_blend_rect,
        Rect {
            x: 4,
            y: 0,
            w: 3,
            h: 2,
        }
    );
    assert_eq!(small_epic.last_mask.len(), 3 * 2);
    assert_eq!(small, full_reference(&ui, &words, 16, 10)[16 * 5..16 * 7]);
}

#[test]
fn split_damage_transactions_track_two_native_targets_independently() {
    let mut ui = Ui::new();
    ui.set_viewport(24.0, 8.0);
    let frame = |x: i16, color: u32| {
        vec![
            spec::draw_op::RECT,
            xy_word(0, 0),
            wh_word(24, 8),
            0xff10_0804,
            spec::draw_op::RECT,
            xy_word(x, 2),
            wh_word(3, 3),
            color,
        ]
    };
    let first = frame(1, 0xff00_00ff);
    let second = frame(5, 0xff00_ff00);
    let third = frame(9, 0xffff_0000);
    let mut states = [RenderTargetState::new(), RenderTargetState::new()];
    let mut renderer = renderer();

    assert!(renderer
        .prepare_damage(&states[0], &ui, &first)
        .unwrap()
        .is_full_redraw());
    assert!(renderer.commit_damage(&mut states[0], &ui, &first));
    assert!(renderer
        .prepare_damage(&states[1], &ui, &second)
        .unwrap()
        .is_full_redraw());
    assert!(renderer.commit_damage(&mut states[1], &ui, &second));

    let target_zero = renderer.prepare_damage(&states[0], &ui, &third).unwrap();
    assert!(!target_zero.is_full_redraw());
    assert!(!target_zero.is_empty());
    let target_one_unchanged = renderer.prepare_damage(&states[1], &ui, &second).unwrap();
    assert!(target_one_unchanged.is_empty());
}

#[test]
fn incremental_render_skips_an_unchanged_target() {
    let mut ui = Ui::new();
    ui.set_viewport(16.0, 8.0);
    let words = [
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(16, 8),
        0xff20_1008,
        spec::draw_op::RECT,
        xy_word(2, 2),
        wh_word(4, 3),
        0xff00_00ff,
    ];
    let mut output = vec![0u16; 16 * 8];
    let mut state = RenderTargetState::new();
    let mut renderer = renderer();
    let first = renderer
        .render_incremental(&mut state, &ui, &words, &mut output, 16, 8, &mut gpu())
        .unwrap();
    assert!(first.full_redraw);
    assert_eq!(first.damage_regions, 1);
    assert_eq!(first.damage_pixels, 16 * 8);

    let before = output.clone();
    let mut epic = gpu();
    let second = renderer
        .render_incremental(&mut state, &ui, &words, &mut output, 16, 8, &mut epic)
        .unwrap();
    assert!(!second.full_redraw);
    assert_eq!(second.damage_regions, 0);
    assert_eq!(second.damage_pixels, 0);
    assert_eq!(epic.fills + epic.gradients + epic.blends + epic.copies, 0);
    assert_eq!(output, before);
}

#[test]
fn incremental_render_reports_policy_promoted_full_redraws() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let frame = |color| [spec::draw_op::RECT, xy_word(0, 0), wh_word(8, 8), color];
    let mut output = vec![0u16; 64];
    let mut state = RenderTargetState::new();
    let mut renderer = renderer();
    let mut epic = gpu();

    let initial = renderer
        .render_incremental(
            &mut state,
            &ui,
            &frame(0xff00_00ff),
            &mut output,
            8,
            8,
            &mut epic,
        )
        .unwrap();
    assert!(initial.full_redraw);
    assert!(!initial.full_redraw_promoted);

    let changed = renderer
        .render_incremental(
            &mut state,
            &ui,
            &frame(0xff00_ff00),
            &mut output,
            8,
            8,
            &mut epic,
        )
        .unwrap();
    assert!(changed.full_redraw);
    assert!(changed.full_redraw_promoted);
}

#[test]
fn incremental_render_keeps_disjoint_damage_regions() {
    let mut ui = Ui::new();
    ui.set_viewport(32.0, 16.0);
    let frame = |left: u32, right: u32| {
        vec![
            spec::draw_op::RECT,
            xy_word(0, 0),
            wh_word(32, 16),
            0xff20_1008,
            spec::draw_op::RECT,
            xy_word(2, 4),
            wh_word(4, 4),
            left,
            spec::draw_op::RECT,
            xy_word(26, 4),
            wh_word(4, 4),
            right,
            spec::draw_op::GRAD_RECT,
            xy_word(13, 10),
            wh_word(6, 3),
            0xff00_0000,
            0xffff_ffff,
            spec::GradDir::ToRight as u32,
        ]
    };
    let previous = frame(0xff00_00ff, 0xff00_ff00);
    let current = frame(0xffff_0000, 0xffff_ffff);
    let mut output = vec![0u16; 32 * 16];
    let mut state = RenderTargetState::new();
    let mut renderer = renderer();
    renderer
        .render_incremental(&mut state, &ui, &previous, &mut output, 32, 16, &mut gpu())
        .unwrap();

    let stats = renderer
        .render_incremental(&mut state, &ui, &current, &mut output, 32, 16, &mut gpu())
        .unwrap();
    assert!(!stats.full_redraw);
    assert_eq!(stats.damage_regions, 2);
    assert_eq!(stats.damage_pixels, 32);
    assert_eq!(stats.software_ops, 0, "unchanged off-damage gradient");
    assert_eq!(output, full_reference(&ui, &current, 32, 16));
}

#[test]
fn incremental_render_merges_a_ninth_region_at_default_capacity() {
    let mut ui = Ui::new();
    ui.set_viewport(96.0, 8.0);
    let frame = |color: u32| {
        let mut words = vec![
            spec::draw_op::RECT,
            xy_word(0, 0),
            wh_word(96, 8),
            0xff10_0804,
        ];
        for index in 0..9 {
            words.extend_from_slice(&[
                spec::draw_op::RECT,
                xy_word((index * 10 + 1) as i16, 2),
                wh_word(2, 2),
                color,
            ]);
        }
        words
    };
    let previous = frame(0xff00_00ff);
    let current = frame(0xff00_ff00);
    let mut output = vec![0u16; 96 * 8];
    let mut state = RenderTargetState::new();
    let mut renderer = renderer();
    renderer
        .render_incremental(&mut state, &ui, &previous, &mut output, 96, 8, &mut gpu())
        .unwrap();

    let stats = renderer
        .render_incremental(&mut state, &ui, &current, &mut output, 96, 8, &mut gpu())
        .unwrap();
    assert!(!stats.full_redraw);
    assert_eq!(stats.damage_regions, 8);
    assert_eq!(output, full_reference(&ui, &current, 96, 8));
}

#[test]
fn incremental_render_replays_unchanged_overlays_in_painter_order() {
    let mut ui = Ui::new();
    ui.set_viewport(24.0, 12.0);
    let frame = |moving_x: i16, moving_color: u32| {
        vec![
            spec::draw_op::RECT,
            xy_word(0, 0),
            wh_word(24, 12),
            0xff20_1008,
            spec::draw_op::RECT,
            xy_word(moving_x, 2),
            wh_word(8, 8),
            moving_color,
            spec::draw_op::RECT,
            xy_word(8, 4),
            wh_word(8, 6),
            0x8000_ff00,
        ]
    };
    let previous = frame(2, 0x8000_00ff);
    let current = frame(6, 0x80ff_0000);
    let mut output = vec![0u16; 24 * 12];
    let mut state = RenderTargetState::new();
    let mut renderer = renderer();
    renderer
        .render_incremental(&mut state, &ui, &previous, &mut output, 24, 12, &mut gpu())
        .unwrap();

    let stats = renderer
        .render_incremental(&mut state, &ui, &current, &mut output, 24, 12, &mut gpu())
        .unwrap();
    assert!(!stats.full_redraw);
    assert_eq!(output, full_reference(&ui, &current, 24, 12));
}

#[test]
fn incremental_render_tracks_each_double_buffer_independently() {
    let mut ui = Ui::new();
    ui.set_viewport(24.0, 8.0);
    let frame = |x: i16, color: u32| {
        vec![
            spec::draw_op::RECT,
            xy_word(0, 0),
            wh_word(24, 8),
            0xff10_0804,
            spec::draw_op::RECT,
            xy_word(x, 2),
            wh_word(3, 3),
            color,
        ]
    };
    let frames = [
        frame(1, 0xff00_00ff),
        frame(5, 0xff00_ff00),
        frame(9, 0xffff_0000),
        frame(13, 0xffff_ffff),
    ];
    let mut renderer = renderer();
    let mut states = [RenderTargetState::new(), RenderTargetState::new()];
    let mut outputs = [vec![0u16; 24 * 8], vec![0u16; 24 * 8]];

    for (index, words) in frames.iter().enumerate() {
        let target = index & 1;
        let stats = renderer
            .render_incremental(
                &mut states[target],
                &ui,
                words,
                &mut outputs[target],
                24,
                8,
                &mut gpu(),
            )
            .unwrap();
        assert_eq!(stats.full_redraw, index < 2);
        assert_eq!(outputs[target], full_reference(&ui, words, 24, 8));
    }
}

#[test]
fn incremental_render_keeps_structural_removal_partial_and_invalidation_full() {
    let mut ui = Ui::new();
    ui.set_viewport(16.0, 8.0);
    let previous = vec![
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(16, 8),
        0xff10_0804,
        spec::draw_op::RECT,
        xy_word(2, 2),
        wh_word(3, 3),
        0xff00_00ff,
    ];
    let current = vec![
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(16, 8),
        0xff10_0804,
    ];
    let mut renderer = renderer();
    let mut state = RenderTargetState::new();
    let mut output = vec![0u16; 16 * 8];
    renderer
        .render_incremental(&mut state, &ui, &previous, &mut output, 16, 8, &mut gpu())
        .unwrap();

    let structural = renderer
        .render_incremental(&mut state, &ui, &current, &mut output, 16, 8, &mut gpu())
        .unwrap();
    assert!(!structural.full_redraw);
    assert_eq!(structural.damage_pixels, 3 * 3);
    assert_eq!(output, full_reference(&ui, &current, 16, 8));

    state.invalidate();
    let invalidated = renderer
        .render_incremental(&mut state, &ui, &current, &mut output, 16, 8, &mut gpu())
        .unwrap();
    assert!(invalidated.full_redraw);
    assert_eq!(invalidated.damage_pixels, 16 * 8);
}

#[test]
fn accelerates_fills_and_opaque_gradients_in_order() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let words = vec![
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(8, 8),
        0xff00_00ff,
        spec::draw_op::GRAD_RECT,
        xy_word(2, 2),
        wh_word(4, 4),
        0xff00_0000,
        0xffff_ffff,
        spec::GradDir::ToRight as u32,
    ];
    let mut output = vec![0u16; 64];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_fills, 2, "clear plus the red rectangle");
    assert_eq!(stats.epic_gradients, 1);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(epic.fills, 2);
    assert_eq!(epic.gradients, 1);
    assert_eq!(output[0], pack_rgb565(255, 0, 0));
    assert_ne!(output[2 * 8 + 2], output[2 * 8 + 5]);
    assert_eq!(output, full_reference(&ui, &words, 8, 8));
}

#[test]
fn every_gradient_direction_matches_the_core() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    for direction in [
        spec::GradDir::ToTop,
        spec::GradDir::ToBottom,
        spec::GradDir::ToLeft,
        spec::GradDir::ToRight,
    ] {
        let words = vec![
            spec::draw_op::GRAD_RECT,
            xy_word(1, 2),
            wh_word(6, 5),
            0xff10_2030,
            0xfff0_c080,
            direction as u32,
        ];
        let mut output = vec![0u16; 64];
        let mut epic = gpu();
        let stats = renderer()
            .render(&ui, &words, &mut output, 8, 8, &mut epic)
            .unwrap();
        assert_eq!(stats.epic_gradients, 1);
        assert_eq!(stats.software_ops, 0);
        assert_eq!(output, full_reference(&ui, &words, 8, 8), "{direction:?}");
    }
}

#[test]
fn batches_small_rect_fallbacks_into_one_cpu_dispatch() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let words = vec![
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(8, 8),
        0xff20_1008,
        spec::draw_op::RECT,
        xy_word(1, 1),
        wh_word(2, 2),
        0xff00_00ff,
        spec::draw_op::RECT,
        xy_word(2, 2),
        wh_word(2, 2),
        0x8000_ff00,
    ];
    let mut output = vec![0u16; 64];
    let mut epic = MockGpu::new(Capabilities {
        thresholds: Thresholds {
            min_fill: 64,
            min_gradient: 64,
            min_blend: 64,
            min_blit: 64,
        },
        ..caps()
    });
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_fills, 2, "clear plus background");
    assert_eq!(stats.software_ops, 2);
    assert_eq!(stats.software_words, 8);
    assert_eq!(epic.fences, 1, "one fence for the batched CPU rectangles");
    assert_eq!(output, full_reference(&ui, &words, 8, 8));
}

#[test]
fn translucent_gradients_keep_ordered_software_blending() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let words = vec![
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(8, 8),
        0xff20_1008,
        spec::draw_op::GRAD_RECT,
        xy_word(1, 1),
        wh_word(6, 6),
        0x8000_00ff,
        0xc0ff_8000,
        spec::GradDir::ToBottom as u32,
    ];
    let mut output = vec![0u16; 64];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_gradients, 0);
    assert_eq!(stats.software_ops, 1);
    assert_eq!(epic.gradients, 0);
    assert_eq!(output, full_reference(&ui, &words, 8, 8));
}

#[test]
fn routes_translucent_solid_rects_to_hardware_fills() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let words = [
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(8, 8),
        0xff20_1008,
        spec::draw_op::RECT,
        xy_word(2, 2),
        wh_word(4, 4),
        0x8000_00ff,
    ];
    let expected = full_reference(&ui, &words, 8, 8);
    let mut output = vec![0u16; 64];
    let mut epic = MockGpu::new(Capabilities {
        fill_alpha: true,
        ..caps()
    });
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_fills, 3, "clear, background, alpha fill");
    assert_eq!(stats.epic_blends, 0);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(output, expected);
}

#[test]
fn translucent_rects_without_alpha_fills_use_a8_blends() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let words = [
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(8, 8),
        0xff20_1008,
        spec::draw_op::RECT,
        xy_word(2, 2),
        wh_word(4, 4),
        0x8000_00ff,
    ];
    let mut output = vec![0u16; 64];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_fills, 2);
    assert_eq!(stats.epic_blends, 1);
    assert_eq!(epic.last_mask_max, 0x80);
    assert_eq!(output, full_reference(&ui, &words, 8, 8));
}

#[test]
fn routes_rgba_texture_scaling_and_modulate_to_hardware_blits() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let texture = [
        255, 0, 0, 255, 0, 255, 0, 192, 0, 0, 255, 128, 255, 255, 0, 64,
    ];
    let handle = ui.upload_texture(&texture, 2, 2, spec::psm::PSM_8888);
    let modulate = 0xc080_ffff;
    let words = [
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(2, 1),
        wh_word(4, 6),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        modulate,
    ];
    let mut output = vec![0u16; 64];
    let mut epic = MockGpu::new(caps_with_texture_blits());
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_copies, 1);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(epic.blits, 1);
    assert_eq!(epic.last_format, Some(PixelFormat::Rgba8888));
    assert_eq!(
        epic.last_blit_src,
        Rect {
            x: 0,
            y: 0,
            w: 2,
            h: 2
        }
    );
    assert_eq!(
        epic.last_blit_dst,
        Rect {
            x: 2,
            y: 1,
            w: 4,
            h: 6
        }
    );
    assert_eq!(epic.last_blit_clip, epic.last_blit_dst);
    assert_eq!(epic.last_modulate, modulate);
    assert_eq!(epic.last_filter, Filter::Nearest);
    let expected = full_reference(&ui, &words, 8, 8);
    assert_eq!(output, expected, "nearest hardware scaling matches the core");

    let mut fallback = vec![0u16; 64];
    let fallback_stats = renderer()
        .render(&ui, &words, &mut fallback, 8, 8, &mut gpu())
        .unwrap();
    assert_eq!(fallback_stats.epic_copies, 0);
    assert_eq!(fallback_stats.software_ops, 1);
    assert_eq!(fallback, expected);
}

#[test]
fn routes_indexed_texture_and_palette_conversion_to_hardware_blits() {
    let mut ui = Ui::new();
    ui.set_viewport(4.0, 4.0);
    let mut texture = vec![0u8; 1024];
    texture[0..4].copy_from_slice(&[255, 0, 0, 255]);
    texture[4..8].copy_from_slice(&[0, 0, 255, 128]);
    texture.extend_from_slice(&[0, 1, 1, 0]);
    let handle = ui.upload_texture(&texture, 2, 2, spec::psm::PSM_T8);
    let words = [
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(0, 0),
        wh_word(4, 4),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        0xffff_ffff,
    ];
    let mut output = vec![0u16; 16];
    let mut epic = MockGpu::new(caps_with_texture_blits());
    let stats = renderer()
        .render(&ui, &words, &mut output, 4, 4, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_copies, 1);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(epic.last_format, Some(PixelFormat::T8Clut));
    assert_eq!(output, full_reference(&ui, &words, 4, 4));
}

#[test]
fn combines_textured_triangles_into_one_projective_quad() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let texture = [
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255,
    ];
    let handle = ui.upload_texture(&texture, 2, 2, spec::psm::PSM_8888);
    let vertex = |x: i16, y: i16, u: f32, v: f32| [xy_word(x, y), u.to_bits(), v.to_bits()];
    let tl = vertex(2, 0, 0.0, 0.0);
    let bl = vertex(0, 4, 0.0, 1.0);
    let br = vertex(4, 6, 1.0, 1.0);
    let tr = vertex(6, 2, 1.0, 0.0);
    let mut words = vec![spec::draw_op::TEX_TRI, handle as u32];
    words.extend_from_slice(&tl);
    words.extend_from_slice(&bl);
    words.extend_from_slice(&br);
    words.push(0xffff_ffff);
    words.extend_from_slice(&[spec::draw_op::TEX_TRI, handle as u32]);
    words.extend_from_slice(&tl);
    words.extend_from_slice(&br);
    words.extend_from_slice(&tr);
    words.push(0xffff_ffff);
    let mut output = vec![0u16; 64];
    let mut epic = MockGpu::new(caps_with_texture_blits());
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_copies, 1);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(epic.quads, 1);
    assert_eq!(
        epic.last_quad,
        [
            Point { x: 2, y: 0 },
            Point { x: 0, y: 4 },
            Point { x: 4, y: 6 },
            Point { x: 6, y: 2 },
        ]
    );
    assert_eq!(
        epic.last_quad_clip,
        Rect {
            x: 0,
            y: 0,
            w: 6,
            h: 6
        }
    );
    let expected = full_reference(&ui, &words, 8, 8);
    // The quad covers the same pixels as the two software triangles.
    for (index, (&actual, &reference)) in output.iter().zip(&expected).enumerate() {
        assert_eq!(actual != 0, reference != 0, "coverage at pixel {index}");
    }

    let mut fallback = vec![0u16; 64];
    let fallback_stats = renderer()
        .render(&ui, &words, &mut fallback, 8, 8, &mut gpu())
        .unwrap();
    assert_eq!(fallback_stats.software_ops, 2);
    assert_eq!(fallback, expected);
}

#[test]
fn routes_axis_aligned_textured_triangles_to_hardware_blits() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let texture = [
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255,
    ];
    let handle = ui.upload_texture(&texture, 2, 2, spec::psm::PSM_8888);
    let vertex = |x: i16, y: i16, u: f32, v: f32| [xy_word(x, y), u.to_bits(), v.to_bits()];
    let top_left = vertex(1, 1, 0.0, 0.0);
    let bottom_left = vertex(1, 7, 0.0, 1.0);
    let bottom_right = vertex(7, 7, 1.0, 1.0);
    let top_right = vertex(7, 1, 1.0, 0.0);
    let mut words = vec![spec::draw_op::TEX_TRI, handle as u32];
    words.extend_from_slice(&top_left);
    words.extend_from_slice(&bottom_left);
    words.extend_from_slice(&bottom_right);
    words.push(0xffff_ffff);
    words.extend_from_slice(&[spec::draw_op::TEX_TRI, handle as u32]);
    words.extend_from_slice(&top_left);
    words.extend_from_slice(&bottom_right);
    words.extend_from_slice(&top_right);
    words.push(0xffff_ffff);
    let mut output = vec![0u16; 64];
    let mut epic = MockGpu::new(caps_with_texture_blits());
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_copies, 1);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(epic.blits, 1);
    assert_eq!(epic.quads, 0);
    assert_eq!(
        epic.last_blit_dst,
        Rect {
            x: 1,
            y: 1,
            w: 6,
            h: 6,
        }
    );
    assert_eq!(output, full_reference(&ui, &words, 8, 8));
}

#[test]
fn combines_flat_triangles_into_one_solid_quad() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let color = 0x8000_80ff;
    let words = [
        spec::draw_op::TRI,
        xy_word(2, 0),
        xy_word(0, 4),
        xy_word(4, 6),
        color,
        color,
        color,
        spec::draw_op::TRI,
        xy_word(2, 0),
        xy_word(4, 6),
        xy_word(6, 2),
        color,
        color,
        color,
    ];
    let mut output = vec![0u16; 64];
    let mut epic = MockGpu::new(caps_with_texture_blits());
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_fills, 1, "the region clear");
    assert_eq!(stats.epic_copies, 1, "the solid quad");
    assert_eq!(stats.software_ops, 0);
    assert_eq!(epic.quads, 1);
    assert_eq!(epic.last_format, Some(PixelFormat::Rgba8888));
    assert_eq!(
        epic.last_quad_clip,
        Rect {
            x: 0,
            y: 0,
            w: 6,
            h: 6
        }
    );

    let expected = full_reference(&ui, &words, 8, 8);
    let mut fallback = vec![0u16; 64];
    let fallback_stats = renderer()
        .render(&ui, &words, &mut fallback, 8, 8, &mut gpu())
        .unwrap();
    assert_eq!(fallback_stats.software_ops, 2);
    assert_eq!(fallback, expected);
}

#[test]
fn routes_axis_aligned_flat_triangles_to_hardware_fills() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let color = 0x8000_80ff;
    let words = [
        spec::draw_op::TRI,
        xy_word(1, 1),
        xy_word(1, 5),
        xy_word(6, 5),
        color,
        color,
        color,
        spec::draw_op::TRI,
        xy_word(1, 1),
        xy_word(6, 5),
        xy_word(6, 1),
        color,
        color,
        color,
    ];
    let mut output = vec![0u16; 64];
    let mut epic = MockGpu::new(Capabilities {
        fill_alpha: true,
        ..caps_with_texture_blits()
    });
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_fills, 2, "clear plus axis-aligned fill");
    assert_eq!(stats.software_ops, 0);
    assert_eq!(epic.quads, 0);
    assert_eq!(
        epic.last_fill_rect,
        Rect {
            x: 1,
            y: 1,
            w: 5,
            h: 4,
        }
    );
    assert_eq!(output, full_reference(&ui, &words, 8, 8));
}

#[test]
fn batches_white_alpha_quads_into_one_a8_blend() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let mut texture = vec![0u8; 4 * 4 * 4];
    for (i, pixel) in texture.chunks_exact_mut(4).enumerate() {
        pixel.copy_from_slice(&[255, 255, 255, (i * 17) as u8]);
    }
    let handle = ui.upload_texture(&texture, 4, 4, spec::psm::PSM_8888);
    let quad = |x: i16| {
        [
            spec::draw_op::TEX_QUAD,
            handle as u32,
            xy_word(x, 0),
            wh_word(4, 4),
            0.0f32.to_bits(),
            0.0f32.to_bits(),
            1.0f32.to_bits(),
            1.0f32.to_bits(),
            0xff00_ffff,
        ]
    };
    let words = [quad(0), quad(4)].concat();
    let mut output = vec![0u16; 64];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_blends, 1);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(epic.blends, 1);
    assert_eq!(output, full_reference(&ui, &words, 8, 8));
}

#[test]
fn linear_alpha_mask_matches_core_edge_sampling() {
    let mut ui = Ui::new();
    ui.set_viewport(4.0, 1.0);
    let handle = ui.upload_texture_flags(
        &[
            255, 255, 255, 0, //
            255, 255, 255, 255,
        ],
        2,
        1,
        spec::psm::PSM_8888,
        spec::img::FLAG_LINEAR,
    );
    let words = [
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(0, 0),
        wh_word(4, 1),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        0xff00_ffff,
    ];
    let mut expected = vec![0u16; 4];
    pocketjs_core::raster::render_scaled_rgb565(&ui, &words, &mut expected, 1);
    let mut output = vec![0u16; 4];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 4, 1, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_blends, 1);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(&epic.last_mask[..4], &[191, 63, 191, 255]);
    assert_eq!(output, expected);
}

#[test]
fn linear_transparent_color_uses_software_fallback() {
    let mut ui = Ui::new();
    ui.set_viewport(4.0, 1.0);
    let handle = ui.upload_texture_flags(
        &[
            0, 0, 0, 0, //
            255, 255, 255, 255,
        ],
        2,
        1,
        spec::psm::PSM_8888,
        spec::img::FLAG_LINEAR,
    );
    let words = [
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(0, 0),
        wh_word(4, 1),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        0xffff_ffff,
    ];
    let mut expected = vec![0u16; 4];
    pocketjs_core::raster::render_scaled_rgb565(&ui, &words, &mut expected, 1);
    let mut output = vec![0u16; 4];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 4, 1, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_blends, 0);
    assert_eq!(stats.software_ops, 1);
    assert_eq!(epic.blends, 0);
    assert_eq!(output, expected);
}

#[test]
fn nearest_transparent_color_remains_alpha_only() {
    let mut ui = Ui::new();
    ui.set_viewport(2.0, 1.0);
    let handle = ui.upload_texture(
        &[
            0, 0, 0, 0, //
            255, 255, 255, 255,
        ],
        2,
        1,
        spec::psm::PSM_8888,
    );
    let words = [
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(0, 0),
        wh_word(2, 1),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        0xffff_ffff,
    ];
    let mut expected = vec![0u16; 2];
    pocketjs_core::raster::render_scaled_rgb565(&ui, &words, &mut expected, 1);
    let mut output = vec![0u16; 2];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 2, 1, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_blends, 1);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(output, expected);
}

#[test]
fn folds_global_alpha_into_a8_before_batching_overlaps() {
    let mut ui = Ui::new();
    ui.set_viewport(2.0, 2.0);
    let handle = ui.upload_texture(&[255, 255, 255, 255], 1, 1, spec::psm::PSM_8888);
    let quad = [
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(0, 0),
        wh_word(2, 2),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        0x80ff_ffff,
    ];
    let words = [quad, quad].concat();
    let mut output = vec![0u16; 4];
    let mut epic = gpu();
    renderer()
        .render(&ui, &words, &mut output, 2, 2, &mut epic)
        .unwrap();

    assert_eq!(epic.blends, 1);
    assert_eq!(epic.last_global_alpha, 255);
    assert_eq!(epic.last_mask_max, 192);
}

#[test]
fn routes_opaque_psm5650_texture_to_hardware_copy() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let pixels = vec![0x1f, 0x00].repeat(64);
    let handle = ui.upload_texture(&pixels, 8, 8, spec::psm::PSM_5650);
    assert!(handle >= 0);
    let words = [
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(0, 0),
        wh_word(8, 8),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        0xffff_ffff,
    ];
    let mut output = vec![0u16; 64];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_copies, 1);
    assert_eq!(stats.software_ops, 0);
    assert_eq!(epic.copies, 1);
    assert_eq!(epic.last_format, Some(PixelFormat::Psm5650));
    assert!(output.iter().all(|&pixel| pixel == pack_rgb565(255, 0, 0)));
}

#[test]
fn clips_psm5650_source_and_destination_without_rescaling() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let pixels = vec![0x1f, 0x00].repeat(64);
    let handle = ui.upload_texture(&pixels, 8, 8, spec::psm::PSM_5650);
    let words = [
        spec::draw_op::SCISSOR,
        xy_word(2, 1),
        wh_word(4, 6),
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(0, 0),
        wh_word(8, 8),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        0xffff_ffff,
        spec::draw_op::SCISSOR_POP,
    ];
    let mut output = vec![0u16; 64];
    let mut epic = gpu();
    renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(
        epic.last_blit_src,
        Rect {
            x: 2,
            y: 1,
            w: 4,
            h: 6,
        }
    );
    assert_eq!(epic.last_blit_dst, epic.last_blit_src);
    assert_eq!(epic.last_mirror, Mirror::default());
    assert_eq!(output, full_reference(&ui, &words, 8, 8));
}

#[test]
fn mirrored_psm5650_copies_reverse_the_source_axis() {
    let mut ui = Ui::new();
    ui.set_viewport(4.0, 1.0);
    let pixels = [0x1f, 0x00, 0xe0, 0x07, 0x00, 0xf8, 0xff, 0xff];
    let handle = ui.upload_texture(&pixels, 4, 1, spec::psm::PSM_5650);
    let words = [
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(0, 0),
        wh_word(4, 1),
        1.0f32.to_bits(),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        0xffff_ffff,
    ];
    let mut output = vec![0u16; 4];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 4, 1, &mut epic)
        .unwrap();
    assert_eq!(stats.epic_copies, 1);
    assert_eq!(epic.last_mirror, Mirror { x: true, y: false });
    assert_eq!(output, full_reference(&ui, &words, 4, 1));
}

#[test]
fn fractional_psm5650_uv_edges_use_ordered_software_fallback() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let pixels = vec![0x1f, 0x00].repeat(64);
    let handle = ui.upload_texture(&pixels, 8, 8, spec::psm::PSM_5650);
    let words = [
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(0, 0),
        wh_word(8, 8),
        0.1f32.to_bits(),
        0.0f32.to_bits(),
        0.9f32.to_bits(),
        1.0f32.to_bits(),
        0xffff_ffff,
    ];
    let mut output = vec![0u16; 64];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.epic_copies, 0);
    assert_eq!(stats.software_ops, 1);
    assert_eq!(output, full_reference(&ui, &words, 8, 8));
}

#[test]
fn empty_scissors_skip_software_fallbacks() {
    let mut ui = Ui::new();
    ui.set_viewport(8.0, 8.0);
    let words = [
        spec::draw_op::SCISSOR,
        xy_word(20, 20),
        wh_word(4, 4),
        spec::draw_op::GRAD_RECT,
        xy_word(0, 0),
        wh_word(8, 8),
        0xff00_0000,
        0xffff_ffff,
        spec::GradDir::ToRight as u32,
        spec::draw_op::SCISSOR_POP,
    ];
    let mut output = vec![pack_rgb565(255, 0, 0); 64];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 8, 8, &mut epic)
        .unwrap();

    assert_eq!(stats.software_ops, 0);
    assert!(output.iter().all(|&pixel| pixel == 0));
}

#[test]
fn skips_native_text_runs_and_surface_quads_like_the_core() {
    let mut ui = Ui::new();
    ui.set_viewport(16.0, 8.0);
    let words = vec![
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(16, 8),
        0xff20_4060,
        // TEXT_RUN: 8 header words + ceil(5 bytes / 4) payload words.
        spec::draw_op::TEXT_RUN,
        0,
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        8.0f32.to_bits(),
        f32::NAN.to_bits(),
        0xffff_ffff,
        5,
        u32::from_le_bytes(*b"Hell"),
        u32::from_le_bytes([b'o', 0, 0, 0]),
        // SURFACE_QUAD: nine words with the destination rect at words 6-7.
        spec::draw_op::SURFACE_QUAD,
        0,
        0,
        0,
        0,
        0,
        xy_word(2, 2),
        wh_word(4, 4),
        0,
        spec::draw_op::RECT,
        xy_word(10, 1),
        wh_word(4, 4),
        0xffff_0000,
    ];
    let mut output = vec![0u16; 16 * 8];
    let mut epic = gpu();
    let stats = renderer()
        .render(&ui, &words, &mut output, 16, 8, &mut epic)
        .unwrap();
    assert_eq!(stats.software_ops, 0);
    assert_eq!(output, full_reference(&ui, &words, 16, 8));

    let mut state = RenderTargetState::new();
    let mut incremental = vec![0u16; 16 * 8];
    let mut renderer = renderer();
    renderer
        .render_incremental(&mut state, &ui, &words, &mut incremental, 16, 8, &mut epic)
        .unwrap();
    let mut moved = words.clone();
    moved[words.len() - 3] = xy_word(6, 1);
    let second = renderer
        .render_incremental(&mut state, &ui, &moved, &mut incremental, 16, 8, &mut epic)
        .unwrap();
    assert!(!second.full_redraw);
    assert_eq!(incremental, full_reference(&ui, &moved, 16, 8));
}

#[test]
fn malformed_drawlists_fail_the_frame() {
    let mut ui = Ui::new();
    ui.set_viewport(4.0, 4.0);
    let mut output = vec![0u16; 16];
    assert!(renderer()
        .render(&ui, &[99], &mut output, 4, 4, &mut gpu())
        .is_none());
    assert!(renderer()
        .render(
            &ui,
            &[spec::draw_op::SCISSOR, xy_word(0, 0), wh_word(2, 2)],
            &mut output,
            4,
            4,
            &mut gpu()
        )
        .is_none());
}

#[test]
fn software_only_capabilities_reproduce_the_core_exactly() {
    let mut ui = Ui::new();
    ui.set_viewport(12.0, 8.0);
    let mut texture = vec![0u8; 4 * 4 * 4];
    for (i, pixel) in texture.chunks_exact_mut(4).enumerate() {
        pixel.copy_from_slice(&[(i * 16) as u8, 255 - (i * 16) as u8, 90, (i * 17) as u8]);
    }
    let handle = ui.upload_texture(&texture, 4, 4, spec::psm::PSM_8888);
    let words = vec![
        spec::draw_op::RECT,
        xy_word(0, 0),
        wh_word(12, 8),
        0xff30_2010,
        spec::draw_op::GRAD_RECT,
        xy_word(1, 1),
        wh_word(10, 3),
        0x8000_00ff,
        0xffff_8000,
        spec::GradDir::ToLeft as u32,
        spec::draw_op::TEX_QUAD,
        handle as u32,
        xy_word(2, 2),
        wh_word(8, 4),
        0.0f32.to_bits(),
        0.0f32.to_bits(),
        1.0f32.to_bits(),
        1.0f32.to_bits(),
        0xc0ff_ffff,
        spec::draw_op::TRI,
        xy_word(1, 7),
        xy_word(6, 3),
        xy_word(11, 7),
        0xff00_ff00,
        0xffff_0000,
        0xff00_00ff,
        spec::draw_op::RECT,
        xy_word(4, 4),
        wh_word(4, 2),
        0x40ff_ffff,
    ];
    let mut output = vec![0u16; 12 * 8];
    let mut epic = MockGpu::new(Capabilities::NONE);
    let stats = renderer()
        .render(&ui, &words, &mut output, 12, 8, &mut epic)
        .unwrap();
    assert_eq!(epic.fills + epic.gradients + epic.blends + epic.copies, 0);
    assert_eq!(stats.software_ops, 5);
    assert_eq!(output, full_reference(&ui, &words, 12, 8));
}
