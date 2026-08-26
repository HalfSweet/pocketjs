#![cfg_attr(not(feature = "std"), no_std)]
#![allow(clippy::missing_safety_doc)]

extern crate alloc;

use alloc::boxed::Box;
#[cfg(not(feature = "std"))]
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;
use core::{ptr, slice, str};

use pocketjs_core::Ui;
use pocketjs_esp32p4_ppa::{
    PpaOps, QuarterTurn, Rect, RenderTargetState, Renderer, RendererConfig, SrmTransform,
};

#[cfg(not(feature = "std"))]
unsafe extern "C" {
    fn pocketjs_idf_rust_alloc(size: usize, alignment: usize) -> *mut u8;
    fn pocketjs_idf_rust_dealloc(pointer: *mut u8, size: usize, alignment: usize);
    fn pocketjs_idf_rust_panic();
}

#[cfg(not(feature = "std"))]
struct IdfAllocator;

#[cfg(not(feature = "std"))]
unsafe impl GlobalAlloc for IdfAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        pocketjs_idf_rust_alloc(layout.size(), layout.align())
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        pocketjs_idf_rust_dealloc(pointer, layout.size(), layout.align())
    }
}

#[cfg(not(feature = "std"))]
#[global_allocator]
static ALLOCATOR: IdfAllocator = IdfAllocator;

#[cfg(not(feature = "std"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo<'_>) -> ! {
    unsafe { pocketjs_idf_rust_panic() };
    loop {
        core::hint::spin_loop();
    }
}

#[repr(C)]
pub struct NativeUiCore {
    ui: Ui,
    epoch: u64,
}

#[repr(C)]
pub struct NativeUiConfig {
    struct_size: usize,
    logical_width: u32,
    logical_height: u32,
    raster_density: u32,
    tick_hz: u32,
}

#[repr(C)]
pub struct NativeFrameView {
    struct_size: usize,
    epoch: u64,
    raster_revision: u64,
    logical_width: u32,
    logical_height: u32,
    raster_density: u32,
    draw_words: *const u32,
    draw_word_count: usize,
    private_core: *mut c_void,
}

#[repr(C)]
pub struct NativeTextureView {
    struct_size: usize,
    pixels: *const u8,
    pixel_bytes: usize,
    width: u32,
    height: u32,
    psm: u32,
    palette: *const u8,
    palette_bytes: usize,
    revision: u64,
    linear: bool,
}

#[repr(C)]
pub struct NativeFontView {
    struct_size: usize,
    bitmap: *const u8,
    bitmap_bytes: usize,
    cell_width: u32,
    cell_height: u32,
    raster_density: u32,
    glyph_count: u32,
}

#[inline]
unsafe fn core_mut<'a>(core: *mut NativeUiCore) -> Option<&'a mut NativeUiCore> {
    core.as_mut()
}

#[inline]
unsafe fn bytes<'a>(data: *const u8, size: usize) -> Option<&'a [u8]> {
    if size == 0 {
        Some(&[])
    } else if data.is_null() {
        None
    } else {
        Some(slice::from_raw_parts(data, size))
    }
}

#[inline]
unsafe fn text<'a>(data: *const u8, size: usize) -> Option<&'a str> {
    str::from_utf8(bytes(data, size)?).ok()
}

#[inline]
fn mutated(core: &mut NativeUiCore) {
    core.epoch = core.epoch.wrapping_add(1);
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_create(
    config: *const NativeUiConfig,
    out_core: *mut *mut NativeUiCore,
) -> i32 {
    if config.is_null() || out_core.is_null() {
        return -1;
    }
    *out_core = ptr::null_mut();
    let config = &*config;
    if config.struct_size < core::mem::size_of::<NativeUiConfig>()
        || config.logical_width == 0
        || config.logical_height == 0
        || config.raster_density == 0
        || config.tick_hz == 0
        || config.tick_hz > pocketjs_core::MAX_TICK_HZ
    {
        return -1;
    }
    let mut ui = Ui::new_with_raster_density(config.raster_density);
    ui.set_viewport(config.logical_width as f32, config.logical_height as f32);
    if !ui.set_tick_rate(config.tick_hz) {
        return -1;
    }
    *out_core = Box::into_raw(Box::new(NativeUiCore { ui, epoch: 1 }));
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_get_config(
    core: *const NativeUiCore,
    output: *mut NativeUiConfig,
) -> i32 {
    let (Some(core), Some(output)) = (core.as_ref(), output.as_mut()) else {
        return -1;
    };
    if output.struct_size < core::mem::size_of::<NativeUiConfig>() {
        return -1;
    }
    let output_size = output.struct_size;
    let (logical_width, logical_height) = core.ui.viewport();
    *output = NativeUiConfig {
        struct_size: output_size,
        logical_width: logical_width as u32,
        logical_height: logical_height as u32,
        raster_density: core.ui.raster_density(),
        tick_hz: core.ui.tick_rate(),
    };
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_destroy(core: *mut NativeUiCore) {
    if !core.is_null() {
        drop(Box::from_raw(core));
    }
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_create_node(core: *mut NativeUiCore, kind: u32) -> i32 {
    let Some(core) = core_mut(core) else {
        return -1;
    };
    let id = core.ui.create_node(kind as u8);
    mutated(core);
    id
}

macro_rules! core_void {
    ($name:ident($($arg:ident: $ty:ty),*) $body:expr) => {
        #[no_mangle]
        pub unsafe extern "C" fn $name(core: *mut NativeUiCore, $($arg: $ty),*) {
            let Some(core) = core_mut(core) else { return };
            $body(core, $($arg),*);
            mutated(core);
        }
    };
}

core_void!(pocketjs_native_ui_destroy_node(id: i32) |core: &mut NativeUiCore, id| core.ui.destroy_node(id));
core_void!(pocketjs_native_ui_insert_before(parent: i32, child: i32, anchor: i32)
    |core: &mut NativeUiCore, parent, child, anchor| core.ui.insert_before(parent, child, anchor));
core_void!(pocketjs_native_ui_remove_child(parent: i32, child: i32)
    |core: &mut NativeUiCore, parent, child| core.ui.remove_child(parent, child));
core_void!(pocketjs_native_ui_set_style(id: i32, style: i32)
    |core: &mut NativeUiCore, id, style| core.ui.set_style(id, style));
core_void!(pocketjs_native_ui_set_prop(id: i32, prop: u32, value: f64)
    |core: &mut NativeUiCore, id, prop, value| core.ui.set_prop(id, prop as u8, value));
core_void!(pocketjs_native_ui_cancel_animation(animation: i32)
    |core: &mut NativeUiCore, animation| core.ui.cancel_anim(animation));
core_void!(pocketjs_native_ui_set_focus(id: i32)
    |core: &mut NativeUiCore, id| core.ui.set_focus(id));
core_void!(pocketjs_native_ui_set_active(id: i32, active: i32)
    |core: &mut NativeUiCore, id, active| core.ui.set_active(id, active != 0));
core_void!(pocketjs_native_ui_set_cursor(texture: i32, hot_x: f32, hot_y: f32, width: f32, height: f32)
    |core: &mut NativeUiCore, texture, hot_x, hot_y, width, height|
        core.ui.set_cursor(texture, hot_x, hot_y, width, height));
core_void!(pocketjs_native_ui_set_cursor_position(x: f32, y: f32)
    |core: &mut NativeUiCore, x, y| core.ui.set_cursor_pos(x, y));
core_void!(pocketjs_native_ui_free_texture(texture: i32)
    |core: &mut NativeUiCore, texture| core.ui.free_texture(texture));
core_void!(pocketjs_native_ui_set_image(id: i32, texture: i32)
    |core: &mut NativeUiCore, id, texture| core.ui.set_image(id, texture));
core_void!(pocketjs_native_ui_set_sprite(id: i32, atlas: i32, frames: u32, columns: u32, step: u32)
    |core: &mut NativeUiCore, id, atlas, frames, columns, step|
        core.ui.set_sprite(id, atlas, frames, columns, step));

unsafe fn set_text_common(
    core: *mut NativeUiCore,
    id: i32,
    data: *const u8,
    size: usize,
    replace: bool,
) -> i32 {
    let Some(core) = core_mut(core) else {
        return -1;
    };
    let Some(value) = text(data, size) else {
        return -1;
    };
    if replace {
        core.ui.replace_text(id, value);
    } else {
        core.ui.set_text(id, value);
    }
    mutated(core);
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_set_text(
    core: *mut NativeUiCore,
    id: i32,
    data: *const u8,
    size: usize,
) -> i32 {
    set_text_common(core, id, data, size, false)
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_replace_text(
    core: *mut NativeUiCore,
    id: i32,
    data: *const u8,
    size: usize,
) -> i32 {
    set_text_common(core, id, data, size, true)
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_animate(
    core: *mut NativeUiCore,
    id: i32,
    prop: u32,
    to: f64,
    duration: u32,
    easing: u32,
    delay: u32,
) -> i32 {
    let Some(core) = core_mut(core) else {
        return -1;
    };
    let animation = core
        .ui
        .animate(id, prop as u8, to, duration, easing as u8, delay);
    mutated(core);
    animation
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_hit_test(
    core: *mut NativeUiCore,
    x: f32,
    y: f32,
) -> i32 {
    core_mut(core).map_or(0, |core| core.ui.hit_test(x, y))
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_hit_test_bounds(
    core: *mut NativeUiCore,
    x: f32,
    y: f32,
) -> i32 {
    core_mut(core).map_or(0, |core| core.ui.hit_test_bounds(x, y))
}

unsafe fn load_bytes(core: *mut NativeUiCore, data: *const u8, size: usize, font: bool) -> i32 {
    let Some(core) = core_mut(core) else {
        return -1;
    };
    let Some(data) = bytes(data, size) else {
        return -1;
    };
    let loaded = if font {
        core.ui.load_font_atlas(data)
    } else {
        core.ui.load_styles(data)
    };
    if loaded {
        mutated(core);
        0
    } else {
        -1
    }
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_load_styles(
    core: *mut NativeUiCore,
    data: *const u8,
    size: usize,
) -> i32 {
    load_bytes(core, data, size, false)
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_load_font(
    core: *mut NativeUiCore,
    data: *const u8,
    size: usize,
) -> i32 {
    load_bytes(core, data, size, true)
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_upload_texture(
    core: *mut NativeUiCore,
    data: *const u8,
    size: usize,
    width: u32,
    height: u32,
    psm: u32,
) -> i32 {
    let Some(core) = core_mut(core) else {
        return -1;
    };
    let Some(data) = bytes(data, size) else {
        return -1;
    };
    let handle = core.ui.upload_texture(data, width, height, psm);
    mutated(core);
    handle
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_upload_img_entry(
    core: *mut NativeUiCore,
    data: *const u8,
    size: usize,
) -> i32 {
    let Some(core) = core_mut(core) else {
        return -1;
    };
    let Some(data) = bytes(data, size) else {
        return -1;
    };
    let handle = core.ui.upload_img_entry(data);
    mutated(core);
    handle
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_measure_text(
    core: *mut NativeUiCore,
    data: *const u8,
    size: usize,
    slot: u32,
) -> f32 {
    let Some(core) = core_mut(core) else {
        return 0.0;
    };
    let Some(value) = text(data, size) else {
        return 0.0;
    };
    core.ui.measure_text(value, slot as u8)
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_wrap_text(
    core: *mut NativeUiCore,
    data: *const u8,
    size: usize,
    slot: u32,
    max_width: f32,
    output: *mut u32,
    capacity: usize,
) -> usize {
    let Some(core) = core_mut(core) else { return 0 };
    let Some(value) = text(data, size) else {
        return 0;
    };
    let breaks = core.ui.wrap_text(value, slot as u8, max_width);
    if !output.is_null() {
        ptr::copy_nonoverlapping(breaks.as_ptr(), output, breaks.len().min(capacity));
    }
    breaks.len()
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_tick(core: *mut NativeUiCore) {
    let Some(core) = core_mut(core) else { return };
    core.ui.tick();
    mutated(core);
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_draw(
    core: *mut NativeUiCore,
    out: *mut NativeFrameView,
) -> i32 {
    let Some(core) = core_mut(core) else {
        return -1;
    };
    if out.is_null() || (*out).struct_size < core::mem::size_of::<NativeFrameView>() {
        return -1;
    }
    let output_size = (*out).struct_size;
    let raster_revision = core.ui.raster_revision();
    let raster_density = core.ui.raster_density();
    let (width, height) = core.ui.viewport();
    let words = &core.ui.draw().words;
    *out = NativeFrameView {
        struct_size: output_size,
        epoch: core.epoch,
        raster_revision,
        logical_width: width as u32,
        logical_height: height as u32,
        raster_density,
        draw_words: words.as_ptr(),
        draw_word_count: words.len(),
        private_core: core as *mut NativeUiCore as *mut c_void,
    };
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_touch_hits(
    core: *mut NativeUiCore,
    touches: *const u32,
    touch_count: usize,
    output: *mut i32,
    output_capacity: usize,
) -> usize {
    if touch_count > 8
        || touch_count > output_capacity
        || (touch_count != 0 && (touches.is_null() || output.is_null()))
    {
        return 0;
    }
    if touch_count == 0 {
        return 0;
    }
    let Some(core) = core_mut(core) else { return 0 };
    let touches = slice::from_raw_parts(touches, touch_count);
    let mut hits = [0i32; 8];
    let count = core.ui.touch_hits(touches, &mut hits);
    ptr::copy_nonoverlapping(hits.as_ptr(), output, count);
    count
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_texture(
    core: *mut NativeUiCore,
    handle: i32,
    out: *mut NativeTextureView,
) -> i32 {
    let Some(core) = core_mut(core) else {
        return -1;
    };
    if out.is_null() || (*out).struct_size < core::mem::size_of::<NativeTextureView>() {
        return -1;
    }
    let Some(texture) = core.ui.texture(handle) else {
        return -1;
    };
    let output_size = (*out).struct_size;
    let palette = texture.palette.unwrap_or(&[]);
    *out = NativeTextureView {
        struct_size: output_size,
        pixels: texture.pixels.as_ptr(),
        pixel_bytes: texture.pixels.len(),
        width: texture.w,
        height: texture.h,
        psm: texture.psm,
        palette: palette.as_ptr(),
        palette_bytes: palette.len(),
        revision: core.ui.texture_revision(handle).unwrap_or(0),
        linear: texture.linear,
    };
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_ui_font(
    core: *mut NativeUiCore,
    slot: u32,
    out: *mut NativeFontView,
) -> i32 {
    let Some(core) = core_mut(core) else {
        return -1;
    };
    if out.is_null() || (*out).struct_size < core::mem::size_of::<NativeFontView>() {
        return -1;
    }
    let Some(font) = core.ui.font_atlas(slot as u8) else {
        return -1;
    };
    let output_size = (*out).struct_size;
    *out = NativeFontView {
        struct_size: output_size,
        bitmap: font.bitmap.as_ptr(),
        bitmap_bytes: font.bitmap.len(),
        cell_width: font.cell_w,
        cell_height: font.cell_h,
        raster_density: font.raster_density as u32,
        glyph_count: font.glyph_count as u32,
    };
    0
}

const MAX_DAMAGE_REGIONS: usize = 8;

#[repr(C)]
pub struct NativeRendererConfig {
    struct_size: usize,
    scale: u32,
    min_fill_pixels: u32,
    min_blend_pixels: u32,
    min_srm_pixels: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NativeRect {
    x: u32,
    y: u32,
    width: u32,
    height: u32,
}

#[repr(C)]
pub struct NativeDamagePlan {
    struct_size: usize,
    region_count: u32,
    full_redraw: bool,
    regions: [NativeRect; MAX_DAMAGE_REGIONS],
}

#[repr(C)]
pub struct NativeRenderStats {
    struct_size: usize,
    ppa_fills: u32,
    ppa_blends: u32,
    ppa_srm: u32,
    software_ops: u32,
    software_words: u32,
    damage_regions: u32,
    damage_pixels: u32,
    damage_bounds: NativeRect,
    full_redraw: bool,
}

type FillFn = unsafe extern "C" fn(*mut c_void, *mut u16, usize, u32, u32, NativeRect, u16) -> bool;
type BlendFn = unsafe extern "C" fn(
    *mut c_void,
    *mut u16,
    usize,
    u32,
    u32,
    *const u8,
    usize,
    NativeRect,
    u8,
    u8,
    u8,
    u8,
) -> bool;
type SrmFn = unsafe extern "C" fn(
    *mut c_void,
    *mut u16,
    usize,
    u32,
    u32,
    *const u8,
    usize,
    u32,
    u32,
    NativeRect,
    NativeRect,
    u32,
    bool,
    bool,
) -> bool;

#[repr(C)]
pub struct NativeAccelerator {
    struct_size: usize,
    user_data: *mut c_void,
    fill_rgb565: Option<FillFn>,
    blend_a8_rgb565: Option<BlendFn>,
    srm_psm5650_rgb565: Option<SrmFn>,
}

pub struct NativeRenderer {
    renderer: Renderer,
}

pub struct NativeRenderTarget {
    state: RenderTargetState,
    prepared_epoch: u64,
    prepared: bool,
}

struct CAccelerator<'a> {
    value: Option<&'a NativeAccelerator>,
}

impl PpaOps for CAccelerator<'_> {
    fn fill_rgb565(
        &mut self,
        destination: &mut [u16],
        width: u32,
        height: u32,
        rect: Rect,
        color: u16,
    ) -> bool {
        let Some(value) = self.value else {
            return false;
        };
        let Some(callback) = value.fill_rgb565 else {
            return false;
        };
        unsafe {
            callback(
                value.user_data,
                destination.as_mut_ptr(),
                destination.len(),
                width,
                height,
                rect.into(),
                color,
            )
        }
    }

    fn blend_a8_rgb565(
        &mut self,
        destination: &mut [u16],
        width: u32,
        height: u32,
        mask: &[u8],
        rect: Rect,
        color: [u8; 3],
        global_alpha: u8,
    ) -> bool {
        let Some(value) = self.value else {
            return false;
        };
        let Some(callback) = value.blend_a8_rgb565 else {
            return false;
        };
        unsafe {
            callback(
                value.user_data,
                destination.as_mut_ptr(),
                destination.len(),
                width,
                height,
                mask.as_ptr(),
                mask.len(),
                rect.into(),
                color[0],
                color[1],
                color[2],
                global_alpha,
            )
        }
    }

    fn srm_psm5650_to_rgb565(
        &mut self,
        destination: &mut [u16],
        width: u32,
        height: u32,
        source: &[u8],
        source_width: u32,
        source_height: u32,
        source_rect: Rect,
        destination_rect: Rect,
        transform: SrmTransform,
    ) -> bool {
        let Some(value) = self.value else {
            return false;
        };
        let Some(callback) = value.srm_psm5650_rgb565 else {
            return false;
        };
        let rotation = match transform.rotation {
            QuarterTurn::None => 0,
            QuarterTurn::Ccw90 => 1,
            QuarterTurn::Ccw180 => 2,
            QuarterTurn::Ccw270 => 3,
        };
        unsafe {
            callback(
                value.user_data,
                destination.as_mut_ptr(),
                destination.len(),
                width,
                height,
                source.as_ptr(),
                source.len(),
                source_width,
                source_height,
                source_rect.into(),
                destination_rect.into(),
                rotation,
                transform.mirror_x,
                transform.mirror_y,
            )
        }
    }
}

impl From<Rect> for NativeRect {
    fn from(value: Rect) -> Self {
        Self {
            x: value.x,
            y: value.y,
            width: value.w,
            height: value.h,
        }
    }
}

impl From<NativeRect> for Rect {
    fn from(value: NativeRect) -> Self {
        Self {
            x: value.x,
            y: value.y,
            w: value.width,
            h: value.height,
        }
    }
}

unsafe fn frame_parts<'a>(frame: *const NativeFrameView) -> Option<(&'a NativeUiCore, &'a [u32])> {
    let frame = frame.as_ref()?;
    if frame.struct_size < core::mem::size_of::<NativeFrameView>()
        || frame.private_core.is_null()
        || (frame.draw_word_count != 0 && frame.draw_words.is_null())
    {
        return None;
    }
    let core = (frame.private_core as *const NativeUiCore).as_ref()?;
    if frame.epoch != core.epoch {
        return None;
    }
    let words = slice::from_raw_parts(frame.draw_words, frame.draw_word_count);
    Some((core, words))
}

fn copy_stats(output: &mut NativeRenderStats, stats: pocketjs_esp32p4_ppa::RenderStats) {
    let output_size = output.struct_size;
    *output = NativeRenderStats {
        struct_size: output_size,
        ppa_fills: stats.ppa_fills,
        ppa_blends: stats.ppa_blends,
        ppa_srm: stats.ppa_srm,
        software_ops: stats.software_ops,
        software_words: stats.software_words,
        damage_regions: stats.damage_regions,
        damage_pixels: stats.damage_pixels,
        damage_bounds: stats.damage_bounds.into(),
        full_redraw: stats.full_redraw,
    };
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_create(
    config: *const NativeRendererConfig,
    output: *mut *mut NativeRenderer,
) -> i32 {
    if config.is_null()
        || output.is_null()
        || (*config).struct_size < core::mem::size_of::<NativeRendererConfig>()
    {
        return -1;
    }
    *output = ptr::null_mut();
    let config = &*config;
    let Some(renderer) = Renderer::new(RendererConfig {
        scale: config.scale,
        min_fill_pixels: config.min_fill_pixels,
        min_blend_pixels: config.min_blend_pixels,
        min_srm_pixels: config.min_srm_pixels,
    }) else {
        return -1;
    };
    *output = Box::into_raw(Box::new(NativeRenderer { renderer }));
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_destroy(renderer: *mut NativeRenderer) {
    if !renderer.is_null() {
        drop(Box::from_raw(renderer));
    }
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_render_target_create(
    output: *mut *mut NativeRenderTarget,
) -> i32 {
    if output.is_null() {
        return -1;
    }
    *output = ptr::null_mut();
    *output = Box::into_raw(Box::new(NativeRenderTarget {
        state: RenderTargetState::new(),
        prepared_epoch: 0,
        prepared: false,
    }));
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_render_target_destroy(target: *mut NativeRenderTarget) {
    if !target.is_null() {
        drop(Box::from_raw(target));
    }
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_render_target_invalidate(target: *mut NativeRenderTarget) {
    if let Some(target) = target.as_mut() {
        target.state.invalidate();
        target.prepared = false;
    }
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_prepare(
    renderer: *mut NativeRenderer,
    target: *mut NativeRenderTarget,
    frame: *const NativeFrameView,
    output: *mut NativeDamagePlan,
) -> i32 {
    let (Some(renderer), Some(target), Some(output)) =
        (renderer.as_mut(), target.as_mut(), output.as_mut())
    else {
        return -1;
    };
    target.prepared = false;
    if output.struct_size < core::mem::size_of::<NativeDamagePlan>() {
        return -1;
    }
    let Some((core, words)) = frame_parts(frame) else {
        return -1;
    };
    if renderer.renderer.config().scale != (*frame).raster_density {
        return -1;
    }
    let Some(plan) = renderer
        .renderer
        .prepare_damage(&target.state, &core.ui, words)
    else {
        return -1;
    };
    let output_size = output.struct_size;
    *output = NativeDamagePlan {
        struct_size: output_size,
        region_count: plan.region_count() as u32,
        full_redraw: plan.is_full_redraw(),
        regions: [NativeRect::default(); MAX_DAMAGE_REGIONS],
    };
    for (index, region) in plan.regions().iter().enumerate() {
        output.regions[index] = NativeRect {
            x: region.x0.max(0) as u32,
            y: region.y0.max(0) as u32,
            width: region.x1.saturating_sub(region.x0) as u32,
            height: region.y1.saturating_sub(region.y0) as u32,
        };
    }
    target.prepared_epoch = (*frame).epoch;
    target.prepared = true;
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_render_strip(
    renderer: *mut NativeRenderer,
    frame: *const NativeFrameView,
    destination: *mut u16,
    destination_pixels: usize,
    region: NativeRect,
    accelerator: *const NativeAccelerator,
    output: *mut NativeRenderStats,
) -> i32 {
    let (Some(renderer), Some(output)) = (renderer.as_mut(), output.as_mut()) else {
        return -1;
    };
    if output.struct_size < core::mem::size_of::<NativeRenderStats>() || destination.is_null() {
        return -1;
    }
    let Some((core, words)) = frame_parts(frame) else {
        return -1;
    };
    if renderer.renderer.config().scale != (*frame).raster_density {
        return -1;
    }
    let destination = slice::from_raw_parts_mut(destination, destination_pixels);
    let accelerator = accelerator
        .as_ref()
        .filter(|value| value.struct_size >= core::mem::size_of::<NativeAccelerator>());
    let mut accelerator = CAccelerator { value: accelerator };
    let Some(stats) = renderer.renderer.render_strip(
        &core.ui,
        words,
        destination,
        region.into(),
        &mut accelerator,
    ) else {
        return -1;
    };
    copy_stats(output, stats);
    0
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_commit(
    renderer: *mut NativeRenderer,
    target: *mut NativeRenderTarget,
    frame: *const NativeFrameView,
) -> i32 {
    let (Some(renderer), Some(target)) = (renderer.as_mut(), target.as_mut()) else {
        return -1;
    };
    if !target.prepared {
        target.state.invalidate();
        return -1;
    }
    let Some((core, words)) = frame_parts(frame) else {
        target.state.invalidate();
        target.prepared = false;
        return -1;
    };
    if target.prepared_epoch != (*frame).epoch {
        target.state.invalidate();
        target.prepared = false;
        return -1;
    }
    let committed = renderer
        .renderer
        .commit_damage(&mut target.state, &core.ui, words);
    target.prepared = false;
    committed as i32
}

#[no_mangle]
pub unsafe extern "C" fn pocketjs_native_renderer_abort(
    renderer: *mut NativeRenderer,
    target: *mut NativeRenderTarget,
) {
    if let (Some(renderer), Some(target)) = (renderer.as_mut(), target.as_mut()) {
        renderer.renderer.abort_damage(&mut target.state);
        target.prepared = false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn instance_core_and_transactional_software_renderer_roundtrip() {
        unsafe {
            let config = NativeUiConfig {
                struct_size: core::mem::size_of::<NativeUiConfig>(),
                logical_width: 32,
                logical_height: 16,
                raster_density: 1,
                tick_hz: 60,
            };
            let mut core = ptr::null_mut();
            assert_eq!(pocketjs_native_ui_create(&config, &mut core), 0);
            let mut actual_config = NativeUiConfig {
                struct_size: core::mem::size_of::<NativeUiConfig>(),
                logical_width: 0,
                logical_height: 0,
                raster_density: 0,
                tick_hz: 0,
            };
            assert_eq!(pocketjs_native_ui_get_config(core, &mut actual_config), 0);
            assert_eq!(actual_config.logical_width, 32);
            assert_eq!(actual_config.logical_height, 16);
            assert_eq!(actual_config.raster_density, 1);
            assert_eq!(actual_config.tick_hz, 60);
            pocketjs_native_ui_tick(core);
            let mut frame = NativeFrameView {
                struct_size: core::mem::size_of::<NativeFrameView>(),
                epoch: 0,
                raster_revision: 0,
                logical_width: 0,
                logical_height: 0,
                raster_density: 0,
                draw_words: ptr::null(),
                draw_word_count: 0,
                private_core: ptr::null_mut(),
            };
            assert_eq!(pocketjs_native_ui_draw(core, &mut frame), 0);
            assert_eq!((frame.logical_width, frame.logical_height), (32, 16));

            let renderer_config = NativeRendererConfig {
                struct_size: core::mem::size_of::<NativeRendererConfig>(),
                scale: 1,
                min_fill_pixels: 1,
                min_blend_pixels: 1,
                min_srm_pixels: 1,
            };
            let mut renderer = ptr::null_mut();
            let mut target = ptr::null_mut();
            assert_eq!(
                pocketjs_native_renderer_create(&renderer_config, &mut renderer),
                0
            );
            assert_eq!(pocketjs_native_render_target_create(&mut target), 0);
            let mut plan = NativeDamagePlan {
                struct_size: core::mem::size_of::<NativeDamagePlan>(),
                region_count: 0,
                full_redraw: false,
                regions: [NativeRect::default(); MAX_DAMAGE_REGIONS],
            };
            assert_eq!(
                pocketjs_native_renderer_prepare(renderer, target, &frame, &mut plan),
                0
            );
            assert!(plan.full_redraw);
            assert_eq!(plan.region_count, 1);
            let region = plan.regions[0];
            let mut pixels = vec![0xffffu16; 32 * 16];
            let mut stats = NativeRenderStats {
                struct_size: core::mem::size_of::<NativeRenderStats>(),
                ppa_fills: 0,
                ppa_blends: 0,
                ppa_srm: 0,
                software_ops: 0,
                software_words: 0,
                damage_regions: 0,
                damage_pixels: 0,
                damage_bounds: NativeRect::default(),
                full_redraw: false,
            };
            assert_eq!(
                pocketjs_native_renderer_render_strip(
                    renderer,
                    &frame,
                    pixels.as_mut_ptr(),
                    pixels.len(),
                    region,
                    ptr::null(),
                    &mut stats,
                ),
                0
            );
            assert!(pixels.iter().all(|pixel| *pixel == 0));
            assert_eq!(pocketjs_native_renderer_commit(renderer, target, &frame), 1);
            assert_eq!(
                pocketjs_native_renderer_commit(renderer, target, &frame),
                -1
            );
            assert_eq!(
                pocketjs_native_ui_touch_hits(core, ptr::null(), 0, ptr::null_mut(), 0),
                0
            );
            pocketjs_native_ui_tick(core);
            assert_eq!(
                pocketjs_native_renderer_prepare(renderer, target, &frame, &mut plan),
                -1
            );
            assert_eq!(pocketjs_native_ui_draw(core, &mut frame), 0);
            let mismatched_config = NativeRendererConfig {
                struct_size: core::mem::size_of::<NativeRendererConfig>(),
                scale: 2,
                min_fill_pixels: 1,
                min_blend_pixels: 1,
                min_srm_pixels: 1,
            };
            let mut mismatched_renderer = ptr::null_mut();
            let mut mismatched_target = ptr::null_mut();
            assert_eq!(
                pocketjs_native_renderer_create(&mismatched_config, &mut mismatched_renderer),
                0
            );
            assert_eq!(
                pocketjs_native_render_target_create(&mut mismatched_target),
                0
            );
            assert_eq!(
                pocketjs_native_renderer_prepare(
                    mismatched_renderer,
                    mismatched_target,
                    &frame,
                    &mut plan,
                ),
                -1
            );
            pocketjs_native_render_target_destroy(mismatched_target);
            pocketjs_native_renderer_destroy(mismatched_renderer);

            pocketjs_native_render_target_destroy(target);
            pocketjs_native_renderer_destroy(renderer);
            pocketjs_native_ui_destroy(core);
        }
    }
}
