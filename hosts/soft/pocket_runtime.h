#ifndef POCKETJS_SOFT_RUNTIME_H
#define POCKETJS_SOFT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

/*
 * Stage ids delivered to pocket_bench_stage() when pocket_runtime.c is
 * compiled with POCKET_RUNTIME_BENCH_HOOKS (hosts/soft/README.md).
 */
#define POCKET_BENCH_STAGE_IDLE 0
#define POCKET_BENCH_STAGE_EVAL 1
#define POCKET_BENCH_STAGE_JS 2
#define POCKET_BENCH_STAGE_JOBS 3
#define POCKET_BENCH_STAGE_TICK 4

int pocket_runtime_boot(
  const char *java_script,
  size_t java_script_length,
  const uint8_t *pack,
  size_t pack_length,
  int width,
  int height
);
/* `pack` is borrowed by QuickJS and must remain valid until shutdown. */
/*
 * One guest turn followed by exactly one core tick — the frame contract
 * (docs/RUNTIMES.md, law 3). Hosts call it once per presented frame with the
 * portable button mask (pocket_spec.h) and the sampled touch contact in
 * logical pixels; `touch_hit` is the host-resolved bounds hit for that
 * contact (pocket_runtime_hit_test_bounds) or zero.
 */
typedef struct {
  uint32_t buttons;
  int touch_down;
  int touch_x;
  int touch_y;
  int touch_hit;
} PocketRuntimeInput;
int pocket_runtime_tick(const PocketRuntimeInput *input);

/*
 * Legacy frame entry points for the original iPhone host, which presents at
 * 30 Hz and advances two core ticks per guest turn, and for the Windows CE
 * host's tick-count form. They pass an empty button mask. New hosts call
 * pocket_runtime_tick.
 */
int pocket_runtime_frame(int touch_down, int touch_x, int touch_y, int touch_hit);
int pocket_runtime_frame_ticks(
  int touch_down,
  int touch_x,
  int touch_y,
  int touch_hit,
  unsigned int tick_count
);
int pocket_runtime_hit_test(float x, float y);
int pocket_runtime_hit_test_bounds(float x, float y);
const char *pocket_runtime_action_name(void);
int pocket_runtime_action_value(void);
unsigned long pocket_runtime_action_sequence(void);
const uint8_t *pocket_runtime_render(void);
/*
 * Rendered pixels are opaque top-left BGRA bytes (ARGB32 words). The pointer
 * remains valid only until the next render, viewport change, or shutdown.
 */

/*
 * Damage statistics for the software raster path, and the most recent plan's
 * bounds so the host can scope its composite. `bounds` receives x0,y0,x1,y1 in
 * logical pixels, top-left origin; the return is zero when nothing changed.
 */
unsigned long pocket_runtime_damage_attempts(void);
unsigned long pocket_runtime_damage_failures(void);
unsigned long pocket_runtime_damage_full_redraws(void);
unsigned long pocket_runtime_damage_pixels(void);
int pocket_runtime_damage_bounds(int *bounds);

/*
 * Hardware path. `pocket_runtime_gl_initialize` needs a current OpenGL ES
 * context matching the core backend selected at compile time and returns zero
 * if the GPU pipeline cannot be established.
 * `pocket_runtime_gl_render` draws the current retained tree into the bound
 * framebuffer; the CPU never rasterizes a pixel on this path.
 */
int pocket_runtime_gl_initialize(void);
void pocket_runtime_gl_reset(void);
int pocket_runtime_gl_render(int width, int height);
void pocket_runtime_gl_shutdown(void);

uint32_t pocket_runtime_width(void);
uint32_t pocket_runtime_height(void);
uint32_t pocket_runtime_stride(void);
size_t pocket_runtime_length(void);
const char *pocket_runtime_error(void);
void pocket_runtime_shutdown(void);

#endif
