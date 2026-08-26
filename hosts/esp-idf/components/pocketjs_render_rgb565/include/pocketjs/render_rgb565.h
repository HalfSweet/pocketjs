#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "pocketjs/ui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POCKETJS_RGB565_MAX_DAMAGE_REGIONS 8U

typedef struct pocketjs_rgb565_renderer pocketjs_rgb565_renderer_t;
typedef struct pocketjs_rgb565_target pocketjs_rgb565_target_t;

typedef struct {
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} pocketjs_rgb565_rect_t;

typedef struct {
  size_t struct_size;
  /** Must equal the UI core raster density used to produce each frame. */
  uint32_t scale;
  uint32_t min_fill_pixels;
  uint32_t min_blend_pixels;
  uint32_t min_srm_pixels;
} pocketjs_rgb565_renderer_config_t;

typedef struct {
  size_t struct_size;
  uint32_t region_count;
  bool full_redraw;
  pocketjs_rgb565_rect_t regions[POCKETJS_RGB565_MAX_DAMAGE_REGIONS];
} pocketjs_rgb565_damage_plan_t;

typedef struct {
  size_t struct_size;
  uint32_t ppa_fills;
  uint32_t ppa_blends;
  uint32_t ppa_srm;
  uint32_t software_ops;
  uint32_t software_words;
  uint32_t damage_regions;
  uint32_t damage_pixels;
  pocketjs_rgb565_rect_t damage_bounds;
  bool full_redraw;
} pocketjs_rgb565_render_stats_t;

typedef bool (*pocketjs_rgb565_fill_fn)(void *user_data, uint16_t *destination,
                                        size_t destination_pixels,
                                        uint32_t width, uint32_t height,
                                        pocketjs_rgb565_rect_t rect,
                                        uint16_t color);

typedef bool (*pocketjs_rgb565_blend_fn)(void *user_data, uint16_t *destination,
                                         size_t destination_pixels,
                                         uint32_t width, uint32_t height,
                                         const uint8_t *mask, size_t mask_size,
                                         pocketjs_rgb565_rect_t rect,
                                         uint8_t red, uint8_t green,
                                         uint8_t blue, uint8_t alpha);

typedef bool (*pocketjs_rgb565_srm_fn)(
    void *user_data, uint16_t *destination, size_t destination_pixels,
    uint32_t width, uint32_t height, const uint8_t *source, size_t source_size,
    uint32_t source_width, uint32_t source_height,
    pocketjs_rgb565_rect_t source_rect, pocketjs_rgb565_rect_t destination_rect,
    uint32_t quarter_turn, bool mirror_x, bool mirror_y);

typedef struct {
  size_t struct_size;
  void *user_data;
  pocketjs_rgb565_fill_fn fill_rgb565;
  pocketjs_rgb565_blend_fn blend_a8_rgb565;
  pocketjs_rgb565_srm_fn srm_psm5650_rgb565;
} pocketjs_rgb565_accelerator_t;

void pocketjs_rgb565_renderer_config_defaults(
    pocketjs_rgb565_renderer_config_t *config);
esp_err_t
pocketjs_rgb565_renderer_create(const pocketjs_rgb565_renderer_config_t *config,
                                pocketjs_rgb565_renderer_t **out_renderer);
void pocketjs_rgb565_renderer_destroy(pocketjs_rgb565_renderer_t *renderer);

esp_err_t pocketjs_rgb565_target_create(pocketjs_rgb565_target_t **out_target);
void pocketjs_rgb565_target_invalidate(pocketjs_rgb565_target_t *target);
void pocketjs_rgb565_target_destroy(pocketjs_rgb565_target_t *target);

esp_err_t pocketjs_rgb565_prepare(pocketjs_rgb565_renderer_t *renderer,
                                  pocketjs_rgb565_target_t *target,
                                  const pocketjs_ui_frame_view_t *frame,
                                  pocketjs_rgb565_damage_plan_t *out_plan);

/** Render one logical damage rectangle into a full-viewport-width strip.
 * Pixels outside the rectangle's horizontal interval are left untouched. */
esp_err_t
pocketjs_rgb565_render_strip(pocketjs_rgb565_renderer_t *renderer,
                             const pocketjs_ui_frame_view_t *frame,
                             uint16_t *destination, size_t destination_pixels,
                             pocketjs_rgb565_rect_t region,
                             const pocketjs_rgb565_accelerator_t *accelerator,
                             pocketjs_rgb565_render_stats_t *out_stats);

esp_err_t pocketjs_rgb565_commit(pocketjs_rgb565_renderer_t *renderer,
                                 pocketjs_rgb565_target_t *target,
                                 const pocketjs_ui_frame_view_t *frame);

void pocketjs_rgb565_abort(pocketjs_rgb565_renderer_t *renderer,
                           pocketjs_rgb565_target_t *target);

#ifdef __cplusplus
}
#endif
