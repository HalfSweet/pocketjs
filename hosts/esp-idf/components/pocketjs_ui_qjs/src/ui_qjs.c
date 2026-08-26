#include "pocketjs/ui_qjs.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "pocketjs/guest_quickjs.h"

#define PAK_MAGIC 0x4b504344U
#define PAK_VERSION 1U
#define PAK_ENTRY_SIZE 20U
#define MAX_REGISTRATIONS 128U
#define TARGET_ID_BYTES 16U

static const char *TAG = "pocketjs_ui_qjs";

typedef struct {
  char *name;
  int32_t handle;
} texture_registration_t;

typedef struct {
  char *name;
  int32_t handle;
  uint32_t frames;
  uint32_t columns;
  uint32_t step;
} sprite_registration_t;

struct pocketjs_ui_qjs {
  pocketjs_guest_t *guest;
  pocketjs_ui_core_t *core;
  char *target_id;
  uint32_t host_abi;
  uint32_t tick_hz;
  uint32_t logical_width;
  uint32_t logical_height;
  const uint8_t *pak;
  size_t pak_size;
  texture_registration_t *textures;
  size_t texture_count;
  sprite_registration_t *sprites;
  size_t sprite_count;
  bool mounted;
};

static bool range_valid(size_t offset, size_t length, size_t total) {
  return offset <= total && length <= total - offset;
}

static bool read_u16(const uint8_t *bytes, size_t size, size_t offset,
                     uint16_t *out) {
  if (out == NULL || !range_valid(offset, 2U, size)) {
    return false;
  }
  *out = (uint16_t)bytes[offset] | ((uint16_t)bytes[offset + 1U] << 8U);
  return true;
}

static bool read_u32(const uint8_t *bytes, size_t size, size_t offset,
                     uint32_t *out) {
  if (out == NULL || !range_valid(offset, 4U, size)) {
    return false;
  }
  *out = (uint32_t)bytes[offset] | ((uint32_t)bytes[offset + 1U] << 8U) |
         ((uint32_t)bytes[offset + 2U] << 16U) |
         ((uint32_t)bytes[offset + 3U] << 24U);
  return true;
}

static char *copy_name(const uint8_t *name, size_t size) {
  char *copy = malloc(size + 1U);
  if (copy != NULL) {
    memcpy(copy, name, size);
    copy[size] = '\0';
  }
  return copy;
}

static esp_err_t add_texture(pocketjs_ui_qjs_t *binding, const uint8_t *name,
                             size_t name_size, int32_t handle) {
  if (binding->texture_count >= MAX_REGISTRATIONS) {
    return ESP_ERR_NO_MEM;
  }
  texture_registration_t *next =
      realloc(binding->textures, (binding->texture_count + 1U) * sizeof(*next));
  if (next == NULL) {
    return ESP_ERR_NO_MEM;
  }
  binding->textures = next;
  char *copy = copy_name(name, name_size);
  if (copy == NULL) {
    return ESP_ERR_NO_MEM;
  }
  binding->textures[binding->texture_count++] = (texture_registration_t){
      .name = copy,
      .handle = handle,
  };
  return ESP_OK;
}

static esp_err_t add_sprite(pocketjs_ui_qjs_t *binding, const uint8_t *name,
                            size_t name_size, int32_t handle, uint32_t frames,
                            uint32_t columns, uint32_t step) {
  if (binding->sprite_count >= MAX_REGISTRATIONS) {
    return ESP_ERR_NO_MEM;
  }
  sprite_registration_t *next =
      realloc(binding->sprites, (binding->sprite_count + 1U) * sizeof(*next));
  if (next == NULL) {
    return ESP_ERR_NO_MEM;
  }
  binding->sprites = next;
  char *copy = copy_name(name, name_size);
  if (copy == NULL) {
    return ESP_ERR_NO_MEM;
  }
  binding->sprites[binding->sprite_count++] = (sprite_registration_t){
      .name = copy,
      .handle = handle,
      .frames = frames,
      .columns = columns,
      .step = step,
  };
  return ESP_OK;
}

static esp_err_t feed_entry(pocketjs_ui_qjs_t *binding, const uint8_t *name,
                            size_t name_size, const uint8_t *blob,
                            size_t blob_size) {
  static const char styles[] = "ui:styles";
  static const char font_prefix[] = "ui:font.";
  static const char image_prefix[] = "ui:img.";
  static const char sprite_prefix[] = "ui:sprite.";
  if (name_size == sizeof(styles) - 1U &&
      memcmp(name, styles, name_size) == 0) {
    return pocketjs_ui_core_load_styles(binding->core, blob, blob_size);
  }
  if (name_size > sizeof(font_prefix) - 1U &&
      memcmp(name, font_prefix, sizeof(font_prefix) - 1U) == 0) {
    return pocketjs_ui_core_load_font_atlas(binding->core, blob, blob_size);
  }
  if (name_size > sizeof(image_prefix) - 1U &&
      memcmp(name, image_prefix, sizeof(image_prefix) - 1U) == 0) {
    const int32_t handle =
        pocketjs_ui_core_upload_img_entry(binding->core, blob, blob_size);
    return handle >= 0
               ? add_texture(binding, name + sizeof(image_prefix) - 1U,
                             name_size - (sizeof(image_prefix) - 1U), handle)
               : ESP_ERR_INVALID_RESPONSE;
  }
  if (name_size > sizeof(sprite_prefix) - 1U &&
      memcmp(name, sprite_prefix, sizeof(sprite_prefix) - 1U) == 0) {
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t frames = 0;
    uint16_t columns = 0;
    uint16_t step = 0;
    if (blob_size < 16U || !read_u16(blob, blob_size, 0, &width) ||
        !read_u16(blob, blob_size, 2, &height) ||
        !read_u16(blob, blob_size, 6, &frames) ||
        !read_u16(blob, blob_size, 8, &columns) ||
        !read_u16(blob, blob_size, 10, &step)) {
      return ESP_ERR_INVALID_RESPONSE;
    }
    const int32_t handle = pocketjs_ui_core_upload_texture(
        binding->core, blob + 16U, blob_size - 16U, width, height, blob[4]);
    return handle >= 0 ? add_sprite(binding, name + sizeof(sprite_prefix) - 1U,
                                    name_size - (sizeof(sprite_prefix) - 1U),
                                    handle, frames, columns, step)
                       : ESP_ERR_INVALID_RESPONSE;
  }
  return ESP_OK;
}

esp_err_t pocketjs_ui_qjs_create(pocketjs_guest_t *guest,
                                 pocketjs_ui_core_t *core,
                                 const pocketjs_ui_qjs_config_t *config,
                                 pocketjs_ui_qjs_t **out_binding) {
  if (guest == NULL || core == NULL || config == NULL || out_binding == NULL ||
      config->struct_size < sizeof(*config) || config->target_id == NULL ||
      config->host_abi == 0U ||
      strnlen(config->target_id, TARGET_ID_BYTES) == 0U ||
      strnlen(config->target_id, TARGET_ID_BYTES) >= TARGET_ID_BYTES) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_binding = NULL;
  pocketjs_ui_core_config_t core_config = {.struct_size = sizeof(core_config)};
  if (pocketjs_ui_core_get_config(core, &core_config) != ESP_OK) {
    return ESP_ERR_INVALID_ARG;
  }
  pocketjs_ui_qjs_t *binding = calloc(1, sizeof(*binding));
  if (binding == NULL) {
    return ESP_ERR_NO_MEM;
  }
  binding->target_id = strdup(config->target_id);
  if (binding->target_id == NULL) {
    free(binding);
    return ESP_ERR_NO_MEM;
  }
  binding->guest = guest;
  binding->core = core;
  binding->host_abi = config->host_abi;
  binding->tick_hz = core_config.tick_hz;
  binding->logical_width = core_config.logical_width;
  binding->logical_height = core_config.logical_height;
  *out_binding = binding;
  return ESP_OK;
}

esp_err_t pocketjs_ui_qjs_feed_pak(pocketjs_ui_qjs_t *binding, const void *pak,
                                   size_t pak_size) {
  if (binding == NULL || pak == NULL || pak_size < 20U || binding->mounted ||
      binding->pak != NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  const uint8_t *bytes = pak;
  uint32_t magic = 0;
  uint16_t version = 0;
  uint32_t count = 0;
  uint32_t directory = 0;
  uint32_t names = 0;
  if (!read_u32(bytes, pak_size, 0, &magic) || magic != PAK_MAGIC ||
      !read_u16(bytes, pak_size, 4, &version) || version != PAK_VERSION ||
      !read_u32(bytes, pak_size, 8, &count) ||
      !read_u32(bytes, pak_size, 12, &directory) ||
      !read_u32(bytes, pak_size, 16, &names) ||
      count > SIZE_MAX / PAK_ENTRY_SIZE ||
      !range_valid(directory, (size_t)count * PAK_ENTRY_SIZE, pak_size)) {
    return ESP_ERR_INVALID_RESPONSE;
  }
  for (uint32_t index = 0; index < count; ++index) {
    const size_t entry = directory + (size_t)index * PAK_ENTRY_SIZE;
    uint32_t blob_offset = 0;
    uint32_t blob_size = 0;
    uint32_t name_offset = 0;
    uint16_t name_size = 0;
    if (!read_u32(bytes, pak_size, entry + 4U, &blob_offset) ||
        !read_u32(bytes, pak_size, entry + 8U, &blob_size) ||
        !read_u32(bytes, pak_size, entry + 12U, &name_offset) ||
        !read_u16(bytes, pak_size, entry + 16U, &name_size) ||
        (size_t)names + name_offset < names ||
        !range_valid((size_t)names + name_offset, name_size, pak_size) ||
        !range_valid(blob_offset, blob_size, pak_size)) {
      return ESP_ERR_INVALID_RESPONSE;
    }
    const esp_err_t result =
        feed_entry(binding, bytes + names + name_offset, name_size,
                   bytes + blob_offset, blob_size);
    if (result != ESP_OK) {
      ESP_LOGE(TAG, "Rejected PAK entry %u: %s", (unsigned)index,
               esp_err_to_name(result));
      return result;
    }
  }
  binding->pak = bytes;
  binding->pak_size = pak_size;
  return ESP_OK;
}

static pocketjs_ui_qjs_t *from_context(JSContext *context) {
  return JS_GetContextOpaque(context);
}

static int32_t arg_i32(JSContext *context, int count, JSValueConst *values,
                       int index) {
  int32_t value = 0;
  if (index < count)
    JS_ToInt32(context, &value, values[index]);
  return value;
}

static uint32_t arg_u32(JSContext *context, int count, JSValueConst *values,
                        int index) {
  uint32_t value = 0;
  if (index < count)
    JS_ToUint32(context, &value, values[index]);
  return value;
}

static double arg_f64(JSContext *context, int count, JSValueConst *values,
                      int index) {
  double value = 0.0;
  if (index < count)
    JS_ToFloat64(context, &value, values[index]);
  return value;
}

#define UI_CORE(ctx) (from_context((ctx))->core)

static JSValue js_create_node(JSContext *ctx, JSValueConst this_value, int argc,
                              JSValueConst *argv) {
  (void)this_value;
  return JS_NewInt32(ctx, pocketjs_ui_core_create_node(
                              UI_CORE(ctx), arg_u32(ctx, argc, argv, 0)));
}

static JSValue js_destroy_node(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_destroy_node(UI_CORE(ctx), arg_i32(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

static JSValue js_insert_before(JSContext *ctx, JSValueConst this_value,
                                int argc, JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_insert_before(UI_CORE(ctx), arg_i32(ctx, argc, argv, 0),
                                 arg_i32(ctx, argc, argv, 1),
                                 arg_i32(ctx, argc, argv, 2));
  return JS_UNDEFINED;
}

static JSValue js_remove_child(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_remove_child(UI_CORE(ctx), arg_i32(ctx, argc, argv, 0),
                                arg_i32(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

static JSValue js_set_style(JSContext *ctx, JSValueConst this_value, int argc,
                            JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_set_style(UI_CORE(ctx), arg_i32(ctx, argc, argv, 0),
                             arg_i32(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

static JSValue js_set_prop(JSContext *ctx, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_set_prop(UI_CORE(ctx), arg_i32(ctx, argc, argv, 0),
                            arg_u32(ctx, argc, argv, 1),
                            arg_f64(ctx, argc, argv, 2));
  return JS_UNDEFINED;
}

static JSValue set_text(JSContext *ctx, int argc, JSValueConst *argv,
                        bool replace) {
  if (argc < 2)
    return JS_ThrowTypeError(ctx, "text op requires id and value");
  size_t size = 0;
  const char *text = JS_ToCStringLen(ctx, &size, argv[1]);
  if (text == NULL)
    return JS_EXCEPTION;
  const esp_err_t result =
      replace ? pocketjs_ui_core_replace_text(
                    UI_CORE(ctx), arg_i32(ctx, argc, argv, 0), text, size)
              : pocketjs_ui_core_set_text(
                    UI_CORE(ctx), arg_i32(ctx, argc, argv, 0), text, size);
  JS_FreeCString(ctx, text);
  return result == ESP_OK ? JS_UNDEFINED
                          : JS_ThrowInternalError(ctx, "invalid UTF-8 text");
}

static JSValue js_set_text(JSContext *ctx, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
  (void)this_value;
  return set_text(ctx, argc, argv, false);
}

static JSValue js_replace_text(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv) {
  (void)this_value;
  return set_text(ctx, argc, argv, true);
}

static JSValue js_upload_texture(JSContext *ctx, JSValueConst this_value,
                                 int argc, JSValueConst *argv) {
  (void)this_value;
  if (argc < 4)
    return JS_ThrowTypeError(
        ctx, "uploadTexture requires pixels, width, height, psm");
  size_t size = 0;
  const uint8_t *bytes = JS_GetUint8Array(ctx, &size, argv[0]);
  if (bytes == NULL)
    return JS_EXCEPTION;
  return JS_NewInt32(
      ctx, pocketjs_ui_core_upload_texture(
               UI_CORE(ctx), bytes, size, arg_u32(ctx, argc, argv, 1),
               arg_u32(ctx, argc, argv, 2), arg_u32(ctx, argc, argv, 3)));
}

static JSValue js_set_image(JSContext *ctx, JSValueConst this_value, int argc,
                            JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_set_image(UI_CORE(ctx), arg_i32(ctx, argc, argv, 0),
                             arg_i32(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

static JSValue js_set_sprite(JSContext *ctx, JSValueConst this_value, int argc,
                             JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_set_sprite(
      UI_CORE(ctx), arg_i32(ctx, argc, argv, 0), arg_i32(ctx, argc, argv, 1),
      arg_u32(ctx, argc, argv, 2), arg_u32(ctx, argc, argv, 3),
      arg_u32(ctx, argc, argv, 4));
  return JS_UNDEFINED;
}

static JSValue js_animate(JSContext *ctx, JSValueConst this_value, int argc,
                          JSValueConst *argv) {
  (void)this_value;
  return JS_NewInt32(
      ctx, pocketjs_ui_core_animate(
               UI_CORE(ctx), arg_i32(ctx, argc, argv, 0),
               arg_u32(ctx, argc, argv, 1), arg_f64(ctx, argc, argv, 2),
               arg_u32(ctx, argc, argv, 3), arg_u32(ctx, argc, argv, 4),
               arg_u32(ctx, argc, argv, 5)));
}

static JSValue js_cancel_anim(JSContext *ctx, JSValueConst this_value, int argc,
                              JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_cancel_animation(UI_CORE(ctx), arg_i32(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

static JSValue js_set_focus(JSContext *ctx, JSValueConst this_value, int argc,
                            JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_set_focus(UI_CORE(ctx), arg_i32(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

static JSValue js_set_active(JSContext *ctx, JSValueConst this_value, int argc,
                             JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_set_active(UI_CORE(ctx), arg_i32(ctx, argc, argv, 0),
                              arg_i32(ctx, argc, argv, 1) != 0);
  return JS_UNDEFINED;
}

static JSValue js_hit_test(JSContext *ctx, JSValueConst this_value, int argc,
                           JSValueConst *argv) {
  (void)this_value;
  return JS_NewInt32(ctx, pocketjs_ui_core_hit_test(
                              UI_CORE(ctx), (float)arg_f64(ctx, argc, argv, 0),
                              (float)arg_f64(ctx, argc, argv, 1)));
}

static JSValue js_hit_test_bounds(JSContext *ctx, JSValueConst this_value,
                                  int argc, JSValueConst *argv) {
  (void)this_value;
  return JS_NewInt32(ctx, pocketjs_ui_core_hit_test_bounds(
                              UI_CORE(ctx), (float)arg_f64(ctx, argc, argv, 0),
                              (float)arg_f64(ctx, argc, argv, 1)));
}

static JSValue js_set_cursor(JSContext *ctx, JSValueConst this_value, int argc,
                             JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_set_cursor(
      UI_CORE(ctx), arg_i32(ctx, argc, argv, 0),
      (float)arg_f64(ctx, argc, argv, 1), (float)arg_f64(ctx, argc, argv, 2),
      (float)arg_f64(ctx, argc, argv, 3), (float)arg_f64(ctx, argc, argv, 4));
  return JS_UNDEFINED;
}

static JSValue js_set_cursor_pos(JSContext *ctx, JSValueConst this_value,
                                 int argc, JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_set_cursor_position(UI_CORE(ctx),
                                       (float)arg_f64(ctx, argc, argv, 0),
                                       (float)arg_f64(ctx, argc, argv, 1));
  return JS_UNDEFINED;
}

static JSValue js_load_styles(JSContext *ctx, JSValueConst this_value, int argc,
                              JSValueConst *argv) {
  (void)this_value;
  size_t size = 0;
  const uint8_t *bytes =
      argc > 0 ? JS_GetUint8Array(ctx, &size, argv[0]) : NULL;
  return JS_NewBool(ctx, bytes != NULL &&
                             pocketjs_ui_core_load_styles(UI_CORE(ctx), bytes,
                                                          size) == ESP_OK);
}

static JSValue js_load_font(JSContext *ctx, JSValueConst this_value, int argc,
                            JSValueConst *argv) {
  (void)this_value;
  size_t size = 0;
  const uint8_t *bytes =
      argc > 0 ? JS_GetUint8Array(ctx, &size, argv[0]) : NULL;
  return JS_NewBool(ctx,
                    bytes != NULL && pocketjs_ui_core_load_font_atlas(
                                         UI_CORE(ctx), bytes, size) == ESP_OK);
}

static JSValue js_measure_text(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv) {
  (void)this_value;
  if (argc < 1)
    return JS_NewFloat64(ctx, 0.0);
  size_t size = 0;
  const char *text = JS_ToCStringLen(ctx, &size, argv[0]);
  if (text == NULL)
    return JS_EXCEPTION;
  const float width = pocketjs_ui_core_measure_text(
      UI_CORE(ctx), text, size, arg_u32(ctx, argc, argv, 1));
  JS_FreeCString(ctx, text);
  return JS_NewFloat64(ctx, width);
}

static JSValue js_wrap_text(JSContext *ctx, JSValueConst this_value, int argc,
                            JSValueConst *argv) {
  (void)this_value;
  if (argc < 3)
    return JS_NewArray(ctx);
  size_t size = 0;
  const char *text = JS_ToCStringLen(ctx, &size, argv[0]);
  if (text == NULL)
    return JS_EXCEPTION;
  const size_t count = pocketjs_ui_core_wrap_text(
      UI_CORE(ctx), text, size, arg_u32(ctx, argc, argv, 1),
      (float)arg_f64(ctx, argc, argv, 2), NULL, 0);
  uint32_t *breaks = count == 0 ? NULL : malloc(count * sizeof(*breaks));
  if (count != 0 && breaks == NULL) {
    JS_FreeCString(ctx, text);
    return JS_EXCEPTION;
  }
  pocketjs_ui_core_wrap_text(UI_CORE(ctx), text, size,
                             arg_u32(ctx, argc, argv, 1),
                             (float)arg_f64(ctx, argc, argv, 2), breaks, count);
  JS_FreeCString(ctx, text);
  JSValue array = JS_NewArray(ctx);
  for (size_t index = 0; index < count; ++index) {
    JS_SetPropertyUint32(ctx, array, (uint32_t)index,
                         JS_NewUint32(ctx, breaks[index]));
  }
  free(breaks);
  return array;
}

static JSValue js_free_texture(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv) {
  (void)this_value;
  pocketjs_ui_core_free_texture(UI_CORE(ctx), arg_i32(ctx, argc, argv, 0));
  return JS_UNDEFINED;
}

static JSValue js_upload_img_entry(JSContext *ctx, JSValueConst this_value,
                                   int argc, JSValueConst *argv) {
  (void)this_value;
  size_t size = 0;
  const uint8_t *bytes =
      argc > 0 ? JS_GetUint8Array(ctx, &size, argv[0]) : NULL;
  return bytes != NULL ? JS_NewInt32(ctx, pocketjs_ui_core_upload_img_entry(
                                              UI_CORE(ctx), bytes, size))
                       : JS_EXCEPTION;
}

static void set_function(JSContext *ctx, JSValue object, const char *name,
                         JSCFunction *fn, int arity) {
  JS_SetPropertyStr(ctx, object, name, JS_NewCFunction(ctx, fn, name, arity));
}

static esp_err_t install_ui(JSContext *ctx, void *user_data) {
  pocketjs_ui_qjs_t *binding = user_data;
  JS_SetContextOpaque(ctx, binding);
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ui = JS_NewObject(ctx);
  if (JS_IsException(global) || JS_IsException(ui)) {
    JS_FreeValue(ctx, ui);
    JS_FreeValue(ctx, global);
    return ESP_ERR_NO_MEM;
  }
  set_function(ctx, ui, "createNode", js_create_node, 1);
  set_function(ctx, ui, "destroyNode", js_destroy_node, 1);
  set_function(ctx, ui, "insertBefore", js_insert_before, 3);
  set_function(ctx, ui, "removeChild", js_remove_child, 2);
  set_function(ctx, ui, "setStyle", js_set_style, 2);
  set_function(ctx, ui, "setProp", js_set_prop, 3);
  set_function(ctx, ui, "setText", js_set_text, 2);
  set_function(ctx, ui, "replaceText", js_replace_text, 2);
  set_function(ctx, ui, "uploadTexture", js_upload_texture, 4);
  set_function(ctx, ui, "setImage", js_set_image, 2);
  set_function(ctx, ui, "setSprite", js_set_sprite, 5);
  set_function(ctx, ui, "animate", js_animate, 6);
  set_function(ctx, ui, "cancelAnim", js_cancel_anim, 1);
  set_function(ctx, ui, "setFocus", js_set_focus, 1);
  set_function(ctx, ui, "setActive", js_set_active, 2);
  set_function(ctx, ui, "hitTest", js_hit_test, 2);
  set_function(ctx, ui, "hitTestBounds", js_hit_test_bounds, 2);
  set_function(ctx, ui, "setCursor", js_set_cursor, 5);
  set_function(ctx, ui, "setCursorPos", js_set_cursor_pos, 2);
  set_function(ctx, ui, "loadStyles", js_load_styles, 1);
  set_function(ctx, ui, "loadFontAtlas", js_load_font, 1);
  set_function(ctx, ui, "measureText", js_measure_text, 2);
  set_function(ctx, ui, "wrapText", js_wrap_text, 3);
  set_function(ctx, ui, "freeTexture", js_free_texture, 1);
  set_function(ctx, ui, "uploadImgEntry", js_upload_img_entry, 1);

  JSValue viewport = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, viewport, "w",
                    JS_NewUint32(ctx, binding->logical_width));
  JS_SetPropertyStr(ctx, viewport, "h",
                    JS_NewUint32(ctx, binding->logical_height));
  JS_SetPropertyStr(ctx, ui, "__viewport", viewport);
  JS_SetPropertyStr(ctx, ui, "__host", JS_NewString(ctx, binding->target_id));
  JS_SetPropertyStr(ctx, ui, "__hostAbi", JS_NewUint32(ctx, binding->host_abi));
  JS_SetPropertyStr(ctx, ui, "__tickHz", JS_NewUint32(ctx, binding->tick_hz));

  JSValue textures = JS_NewObject(ctx);
  for (size_t index = 0; index < binding->texture_count; ++index) {
    JS_SetPropertyStr(ctx, textures, binding->textures[index].name,
                      JS_NewInt32(ctx, binding->textures[index].handle));
  }
  JS_SetPropertyStr(ctx, ui, "__textures", textures);
  JSValue sprites = JS_NewObject(ctx);
  for (size_t index = 0; index < binding->sprite_count; ++index) {
    const sprite_registration_t *sprite = &binding->sprites[index];
    JSValue record = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, record, "handle", JS_NewInt32(ctx, sprite->handle));
    JS_SetPropertyStr(ctx, record, "frames", JS_NewUint32(ctx, sprite->frames));
    JS_SetPropertyStr(ctx, record, "cols", JS_NewUint32(ctx, sprite->columns));
    JS_SetPropertyStr(ctx, record, "step", JS_NewUint32(ctx, sprite->step));
    JS_SetPropertyStr(ctx, sprites, sprite->name, record);
  }
  JS_SetPropertyStr(ctx, ui, "__sprites", sprites);
  JSValue surfaces = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, ui, "__surfaces", surfaces);

  if (binding->pak != NULL) {
    JSValue pak = JS_NewArrayBuffer(ctx, (uint8_t *)binding->pak,
                                    binding->pak_size, NULL, NULL, false);
    JS_SetPropertyStr(ctx, global, "__pak", pak);
  }
  const int result = JS_SetPropertyStr(ctx, global, "ui", ui);
  JS_FreeValue(ctx, global);
  return result >= 0 && !JS_HasException(ctx) ? ESP_OK : ESP_FAIL;
}

esp_err_t pocketjs_ui_qjs_mount(pocketjs_ui_qjs_t *binding) {
  if (binding == NULL || binding->mounted) {
    return ESP_ERR_INVALID_STATE;
  }
  const esp_err_t result =
      pocketjs_guest_quickjs_install(binding->guest, install_ui, binding);
  if (result == ESP_OK)
    binding->mounted = true;
  return result;
}

static uint32_t pack_analog(int16_t value) {
  return (uint32_t)((int32_t)value + 32896) / 257U;
}

esp_err_t pocketjs_ui_turn(pocketjs_ui_qjs_t *binding,
                           const pocketjs_ui_input_t *input,
                           pocketjs_ui_frame_view_t *out_frame) {
  if (binding == NULL || !binding->mounted || out_frame == NULL ||
      out_frame->struct_size < sizeof(*out_frame) ||
      (input != NULL &&
       (input->struct_size < sizeof(*input) ||
        input->touch_count > POCKETJS_UI_MAX_TOUCHES ||
        (input->touch_count != 0U && input->touches == NULL)))) {
    return ESP_ERR_INVALID_ARG;
  }
  const pocketjs_ui_input_t empty = {.struct_size = sizeof(empty)};
  if (input == NULL)
    input = &empty;
  uint32_t touches[POCKETJS_UI_MAX_TOUCHES] = {0};
  int32_t hits[POCKETJS_UI_MAX_TOUCHES] = {0};
  for (size_t index = 0; index < input->touch_count; ++index) {
    if (input->touches[index].x > 511U || input->touches[index].y > 511U) {
      return ESP_ERR_INVALID_ARG;
    }
    touches[index] = ((uint32_t)input->touches[index].id << 18U) |
                     ((uint32_t)input->touches[index].y << 9U) |
                     input->touches[index].x;
  }
  if (input->touch_count != 0U) {
    const size_t hit_count =
        pocketjs_ui_core_touch_hits(binding->core, touches, input->touch_count,
                                    hits, POCKETJS_UI_MAX_TOUCHES);
    if (hit_count != input->touch_count)
      return ESP_FAIL;
  }
  const pocketjs_guest_frame_t frame = {
      .struct_size = sizeof(frame),
      .buttons = input->buttons,
      .analog =
          (pack_analog(input->analog_x) << 8U) | pack_analog(input->analog_y),
      .touches = touches,
      .touch_hits = hits,
      .touch_count = input->touch_count,
  };
  esp_err_t result = pocketjs_guest_frame(binding->guest, &frame);
  if (result != ESP_OK)
    return result;
  pocketjs_ui_core_tick(binding->core);
  return pocketjs_ui_core_draw(binding->core, out_frame);
}

uint32_t pocketjs_ui_qjs_tick_hz(const pocketjs_ui_qjs_t *binding) {
  return binding != NULL && binding->mounted ? binding->tick_hz : 0U;
}

void pocketjs_ui_qjs_interrupt(pocketjs_ui_qjs_t *binding) {
  if (binding != NULL)
    pocketjs_guest_interrupt(binding->guest);
}

void pocketjs_ui_qjs_destroy(pocketjs_ui_qjs_t *binding) {
  if (binding == NULL)
    return;
  for (size_t index = 0; index < binding->texture_count; ++index)
    free(binding->textures[index].name);
  for (size_t index = 0; index < binding->sprite_count; ++index)
    free(binding->sprites[index].name);
  free(binding->textures);
  free(binding->sprites);
  free(binding->target_id);
  free(binding);
}
