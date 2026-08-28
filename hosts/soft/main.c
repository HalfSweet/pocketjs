/*
 * hosts/soft/main.c — the software framebuffer host.
 *
 * Boots one PocketJS guest bundle on QuickJS through the shared C runtime
 * (pocket_runtime.c), drives virtual frames from a scripted button tape,
 * rasterizes every frame into memory through the core software rasterizer
 * (engine/symbian built with `software-only`), and reports one FNV-1a hash
 * per frame over the RGBA bytes — the same bytes and the same hash the wasm
 * oracle produces (hosts/sim/sim.ts, tools/tape.ts), so the two hosts can be
 * compared frame by frame. There is no window, no presentation and no wall
 * clock: a frame is a function of the frame index and the tape only.
 *
 *   pocket-soft --js app.js --pak app.pak [--frames N] [--input "f:mask,..."]
 *               [--capture f,f,...] [--dump DIR] [--hashes out.json]
 *               [--width 480] [--height 272]
 *
 * `--input` is the latched tape format shared with tools/tape.ts record and
 * the PSP capture build: `frame:mask` pairs, each mask held from its frame
 * until the next pair; frames before the first pair read an empty mask.
 * `--capture` names frames whose raw RGBA bytes are written to
 * `DIR/frame-NNNN.rgba` (tools/soft.ts turns them into PNGs).
 */

#include "pocket_runtime.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef POCKETJS_TARGET_ID
#define POCKETJS_TARGET_ID "soft"
#endif
#define SOFT_SIMULATION_HZ 60
#define SOFT_MAX_TAPE 4096
#define SOFT_MAX_CAPTURE 256

typedef struct {
  unsigned long frame;
  uint32_t mask;
} TapeEntry;

typedef struct {
  const char *java_script;
  const char *pack;
  const char *hashes;
  const char *dump;
  unsigned long frames;
  int width;
  int height;
  TapeEntry tape[SOFT_MAX_TAPE];
  size_t tape_length;
  unsigned long capture[SOFT_MAX_CAPTURE];
  size_t capture_length;
} Options;

static void usage(FILE *out) {
  fputs(
    "usage: pocket-soft --js app.js --pak app.pak [--frames N] [--input \"f:mask,...\"]\n"
    "                   [--capture f,f,...] [--dump DIR] [--hashes out.json]\n"
    "                   [--width 480] [--height 272]\n",
    out
  );
}

static int read_file(const char *path, uint8_t **bytes, size_t *length) {
  FILE *file = fopen(path, "rb");
  long size;
  uint8_t *buffer;
  if (file == NULL) return 0;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return 0;
  }
  size = ftell(file);
  if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return 0;
  }
  buffer = (uint8_t *)malloc((size_t)size + 1);
  if (buffer == NULL) {
    fclose(file);
    return 0;
  }
  if (size > 0 && fread(buffer, 1, (size_t)size, file) != (size_t)size) {
    free(buffer);
    fclose(file);
    return 0;
  }
  fclose(file);
  buffer[size] = 0;
  *bytes = buffer;
  *length = (size_t)size;
  return 1;
}

static int parse_tape(Options *options, const char *script) {
  const char *cursor = script;
  while (*cursor != '\0') {
    char *end = NULL;
    unsigned long frame;
    unsigned long mask;
    if (options->tape_length >= SOFT_MAX_TAPE) {
      fputs("pocket-soft: --input has too many entries\n", stderr);
      return 0;
    }
    frame = strtoul(cursor, &end, 10);
    if (end == cursor || *end != ':') {
      fprintf(stderr, "pocket-soft: bad --input entry near '%s'\n", cursor);
      return 0;
    }
    cursor = end + 1;
    mask = strtoul(cursor, &end, 0);
    if (end == cursor || (*end != ',' && *end != '\0')) {
      fprintf(stderr, "pocket-soft: bad --input mask near '%s'\n", cursor);
      return 0;
    }
    if (options->tape_length > 0 &&
        frame < options->tape[options->tape_length - 1].frame) {
      fputs("pocket-soft: --input frames must not decrease\n", stderr);
      return 0;
    }
    options->tape[options->tape_length].frame = frame;
    options->tape[options->tape_length].mask = (uint32_t)mask;
    options->tape_length += 1;
    cursor = *end == ',' ? end + 1 : end;
  }
  return 1;
}

static int parse_capture(Options *options, const char *list) {
  const char *cursor = list;
  while (*cursor != '\0') {
    char *end = NULL;
    unsigned long frame = strtoul(cursor, &end, 10);
    if (end == cursor || (*end != ',' && *end != '\0')) {
      fprintf(stderr, "pocket-soft: bad --capture entry near '%s'\n", cursor);
      return 0;
    }
    if (options->capture_length >= SOFT_MAX_CAPTURE) {
      fputs("pocket-soft: --capture has too many entries\n", stderr);
      return 0;
    }
    options->capture[options->capture_length++] = frame;
    cursor = *end == ',' ? end + 1 : end;
  }
  return 1;
}

static uint32_t mask_at(const Options *options, unsigned long frame) {
  uint32_t mask = 0;
  size_t i;
  for (i = 0; i < options->tape_length; ++i) {
    if (options->tape[i].frame > frame) break;
    mask = options->tape[i].mask;
  }
  return mask;
}

static int wants_capture(const Options *options, unsigned long frame) {
  size_t i;
  for (i = 0; i < options->capture_length; ++i) {
    if (options->capture[i] == frame) return 1;
  }
  return 0;
}

static int parse_options(int argc, char **argv, Options *options) {
  int i;
  memset(options, 0, sizeof(*options));
  options->frames = 60;
  options->width = 480;
  options->height = 272;
  for (i = 1; i < argc; ++i) {
    const char *argument = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (strcmp(argument, "--help") == 0 || strcmp(argument, "-h") == 0) {
      usage(stdout);
      exit(0);
    }
    if (value == NULL) {
      fprintf(stderr, "pocket-soft: %s needs a value\n", argument);
      return 0;
    }
    if (strcmp(argument, "--js") == 0) options->java_script = value;
    else if (strcmp(argument, "--pak") == 0) options->pack = value;
    else if (strcmp(argument, "--hashes") == 0) options->hashes = value;
    else if (strcmp(argument, "--dump") == 0) options->dump = value;
    else if (strcmp(argument, "--frames") == 0) options->frames = strtoul(value, NULL, 10);
    else if (strcmp(argument, "--width") == 0) options->width = atoi(value);
    else if (strcmp(argument, "--height") == 0) options->height = atoi(value);
    else if (strcmp(argument, "--input") == 0) {
      if (!parse_tape(options, value)) return 0;
    } else if (strcmp(argument, "--capture") == 0) {
      if (!parse_capture(options, value)) return 0;
    } else {
      fprintf(stderr, "pocket-soft: unknown option %s\n", argument);
      return 0;
    }
    i += 1;
  }
  if (options->java_script == NULL || options->pack == NULL) {
    fputs("pocket-soft: --js and --pak are required\n", stderr);
    return 0;
  }
  if (options->width <= 0 || options->height <= 0) {
    fputs("pocket-soft: --width and --height must be positive\n", stderr);
    return 0;
  }
  if (options->capture_length > 0 && options->dump == NULL) {
    fputs("pocket-soft: --capture needs --dump DIR\n", stderr);
    return 0;
  }
  return 1;
}

/* FNV-1a 32-bit over RGBA bytes: identical to hosts/sim/sim.ts fnv1a(). */
static uint32_t fnv1a(const uint8_t *bytes, size_t length) {
  uint32_t hash = 0x811c9dc5U;
  size_t i;
  for (i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 0x01000193U;
  }
  return hash;
}

/* The runtime renders opaque BGRA bytes (ARGB32 words); the oracle hashes
 * RGBA bytes. Swap the channels into `rgba` so both hosts hash one layout. */
static void to_rgba(
  const uint8_t *bgra,
  uint32_t width,
  uint32_t height,
  uint32_t stride,
  uint8_t *rgba
) {
  uint32_t x;
  uint32_t y;
  for (y = 0; y < height; ++y) {
    const uint8_t *row = bgra + (size_t)y * stride;
    uint8_t *out = rgba + (size_t)y * width * 4;
    for (x = 0; x < width; ++x) {
      out[x * 4 + 0] = row[x * 4 + 2];
      out[x * 4 + 1] = row[x * 4 + 1];
      out[x * 4 + 2] = row[x * 4 + 0];
      out[x * 4 + 3] = row[x * 4 + 3];
    }
  }
}

static int write_capture(
  const char *directory,
  unsigned long frame,
  const uint8_t *rgba,
  size_t length
) {
  char path[1024];
  FILE *file;
  if (snprintf(path, sizeof(path), "%s/frame-%04lu.rgba", directory, frame) >= (int)sizeof(path)) {
    fputs("pocket-soft: --dump path is too long\n", stderr);
    return 0;
  }
  file = fopen(path, "wb");
  if (file == NULL) {
    fprintf(stderr, "pocket-soft: cannot write %s: %s\n", path, strerror(errno));
    return 0;
  }
  if (fwrite(rgba, 1, length, file) != length) {
    fclose(file);
    fprintf(stderr, "pocket-soft: short write to %s\n", path);
    return 0;
  }
  fclose(file);
  return 1;
}

static int write_report(
  const Options *options,
  uint32_t width,
  uint32_t height,
  const uint32_t *hashes
) {
  FILE *out = stdout;
  unsigned long frame;
  if (options->hashes != NULL && strcmp(options->hashes, "-") != 0) {
    out = fopen(options->hashes, "w");
    if (out == NULL) {
      fprintf(stderr, "pocket-soft: cannot write %s: %s\n", options->hashes, strerror(errno));
      return 0;
    }
  }
  fprintf(
    out,
    "{\"host\":\"soft\",\"target\":\"%s\",\"width\":%lu,\"height\":%lu,\"frames\":%lu,\"hz\":%d,\"hashes\":[",
    POCKETJS_TARGET_ID,
    (unsigned long)width,
    (unsigned long)height,
    options->frames,
    SOFT_SIMULATION_HZ
  );
  for (frame = 0; frame < options->frames; ++frame) {
    fprintf(out, "%s\"%08lx\"", frame == 0 ? "" : ",", (unsigned long)hashes[frame]);
  }
  fputs("]}\n", out);
  if (out != stdout) fclose(out);
  return 1;
}

int main(int argc, char **argv) {
  Options options;
  uint8_t *java_script = NULL;
  uint8_t *pack = NULL;
  size_t java_script_length = 0;
  size_t pack_length = 0;
  uint32_t *hashes = NULL;
  uint8_t *rgba = NULL;
  size_t rgba_length = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  unsigned long frame;
  int exit_code = 1;

  if (!parse_options(argc, argv, &options)) {
    usage(stderr);
    return 1;
  }
  if (!read_file(options.java_script, &java_script, &java_script_length)) {
    fprintf(stderr, "pocket-soft: cannot read %s\n", options.java_script);
    return 4;
  }
  if (!read_file(options.pack, &pack, &pack_length)) {
    fprintf(stderr, "pocket-soft: cannot read %s\n", options.pack);
    free(java_script);
    return 4;
  }
  hashes = (uint32_t *)calloc(options.frames == 0 ? 1 : options.frames, sizeof(uint32_t));
  if (hashes == NULL) {
    fputs("pocket-soft: out of memory\n", stderr);
    free(pack);
    free(java_script);
    return 4;
  }

  if (!pocket_runtime_boot(
        (const char *)java_script,
        java_script_length,
        pack,
        pack_length,
        options.width,
        options.height
      )) {
    fprintf(stderr, "pocket-soft: boot failed: %s\n", pocket_runtime_error());
    exit_code = 2;
    goto done;
  }

  for (frame = 0; frame < options.frames; ++frame) {
    PocketRuntimeInput input;
    const uint8_t *pixels;
    uint32_t stride;
    input.buttons = mask_at(&options, frame);
    input.touch_down = 0;
    input.touch_x = 0;
    input.touch_y = 0;
    input.touch_hit = 0;
    if (!pocket_runtime_tick(&input)) {
      fprintf(stderr, "pocket-soft: frame %lu failed: %s\n", frame, pocket_runtime_error());
      exit_code = 3;
      goto done;
    }
    pixels = pocket_runtime_render();
    if (pixels == NULL) {
      fprintf(stderr, "pocket-soft: frame %lu rendered nothing: %s\n", frame, pocket_runtime_error());
      exit_code = 3;
      goto done;
    }
    width = pocket_runtime_width();
    height = pocket_runtime_height();
    stride = pocket_runtime_stride();
    if (rgba == NULL) {
      rgba_length = (size_t)width * (size_t)height * 4;
      rgba = (uint8_t *)malloc(rgba_length == 0 ? 1 : rgba_length);
      if (rgba == NULL) {
        fputs("pocket-soft: out of memory\n", stderr);
        exit_code = 4;
        goto done;
      }
    }
    to_rgba(pixels, width, height, stride, rgba);
    hashes[frame] = fnv1a(rgba, rgba_length);
    if (wants_capture(&options, frame) &&
        !write_capture(options.dump, frame, rgba, rgba_length)) {
      exit_code = 4;
      goto done;
    }
  }

  exit_code = write_report(&options, width, height, hashes) ? 0 : 4;

done:
  pocket_runtime_shutdown();
  free(rgba);
  free(hashes);
  free(pack);
  free(java_script);
  return exit_code;
}
