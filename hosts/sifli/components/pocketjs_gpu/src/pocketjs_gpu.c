/*
 * PocketJS GPU command queue: the executor behind pocketjs_gpu.h.
 *
 * Commands arrive in painter order from the Rust renderer and run on EPIC
 * (pocketjs_gpu_epic.c) one transaction at a time; a fence waits for the
 * transaction in flight. A8 planes and RGB565 tiles live in on-chip SRAM so
 * the CPU builds coverage and renders fallbacks without touching the
 * framebuffer or maintaining caches.
 */
#include <string.h>

#include "mem_section.h"

#include "pocketjs_gpu_internal.h"

#define MASK_BYTES ((size_t)POCKETJS_GPU_MASK_TILE_KB * 1024u)
#define TILE_PIXELS ((size_t)POCKETJS_GPU_CPU_TILE_KB * 1024u / sizeof(uint16_t))
#define PLANE_SLOTS 2u

L1_NON_RET_BSS_SECT(pocketjs_gpu_masks,
                    ALIGN(64) static uint8_t g_masks[PLANE_SLOTS][MASK_BYTES]);
L1_NON_RET_BSS_SECT(pocketjs_gpu_tiles,
                    ALIGN(64) static uint16_t g_tiles[PLANE_SLOTS][TILE_PIXELS]);

static bool g_open;
static PocketjsGpuTarget g_target;
static PocketjsGpuTexture g_textures[POCKETJS_GPU_MAX_TEXTURES];
static uint32_t g_texture_count;
static PocketjsGpuProfile g_profile;

const PocketjsGpuTarget *pocketjs_gpu_target(void)
{
    return &g_target;
}

const PocketjsGpuTexture *pocketjs_gpu_texture_by_id(uint32_t id)
{
    if (id == 0 || id > g_texture_count)
    {
        return NULL;
    }
    return &g_textures[id - 1u];
}

uint8_t *pocketjs_gpu_mask_base(uint32_t id, size_t *len)
{
    if (id >= PLANE_SLOTS)
    {
        return NULL;
    }
    if (len != NULL)
    {
        *len = MASK_BYTES;
    }
    return g_masks[id];
}

uint16_t *pocketjs_gpu_tile_base(uint32_t id, size_t *len)
{
    if (id >= PLANE_SLOTS)
    {
        return NULL;
    }
    if (len != NULL)
    {
        *len = TILE_PIXELS;
    }
    return g_tiles[id];
}

void pocketjs_gpu_profile_submit(uint32_t started)
{
    g_profile.submit_cycles += (uint32_t)(HAL_DBG_DWT_GetCycles() - started);
    ++g_profile.transactions;
}

void pocketjs_gpu_profile_wait(uint32_t started)
{
    g_profile.wait_cycles += (uint32_t)(HAL_DBG_DWT_GetCycles() - started);
}

bool pocketjs_gpu_rect_in_target(const PocketjsGpuRect *rect)
{
    return rect->w > 0 && rect->h > 0 && rect->x < g_target.width &&
           rect->y < g_target.height && rect->w <= g_target.width - rect->x &&
           rect->h <= g_target.height - rect->y;
}

/* ---- host API ------------------------------------------------------------------- */

int32_t pocketjs_gpu_open(void)
{
    if (g_open)
    {
        return 1;
    }
    if (!pocketjs_gpu_epic_open())
    {
        return 0;
    }
    memset(&g_profile, 0, sizeof(g_profile));
    g_open = true;
    return 1;
}

void pocketjs_gpu_close(void)
{
    if (!g_open)
    {
        return;
    }
    pocketjs_gpu_epic_wait();
    pocketjs_gpu_epic_close();
    g_open = false;
}

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

int32_t pocketjs_gpu_texture_register(int32_t handle, uint64_t revision,
                                      const uint8_t *blob, size_t blob_len)
{
    PocketjsGpuTexture *texture;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t pixels;
    size_t required;
    size_t palette_len = 0;
    uint32_t index;

    if (handle < 0 || blob == NULL || blob_len < POCKETJS_GPU_NATIVE_HEADER)
    {
        return 0;
    }
    for (index = 0; index < g_texture_count; ++index)
    {
        if (g_textures[index].handle == handle)
        {
            return 0; /* one native copy per handle */
        }
    }
    if (g_texture_count >= POCKETJS_GPU_MAX_TEXTURES)
    {
        return 0;
    }
    width = read_u16_le(blob);
    height = read_u16_le(blob + 2);
    format = blob[4];
    pixels = (uint64_t)width * height;
    if (width == 0 || height == 0)
    {
        return 0;
    }
    switch (format)
    {
    case POCKETJS_GPU_NATIVE_RGB565:
        required = (size_t)pixels * 2u;
        break;
    case POCKETJS_GPU_NATIVE_BGRA8888:
        required = (size_t)pixels * 4u;
        break;
    case POCKETJS_GPU_NATIVE_L8:
#if !POCKETJS_GPU_HAS_L8
        return 0;
#else
        palette_len = 1024u;
        required = (size_t)pixels;
        break;
#endif
    default:
        return 0;
    }
    if (pixels > SIZE_MAX / 4u ||
        required > blob_len - POCKETJS_GPU_NATIVE_HEADER - palette_len)
    {
        return 0;
    }

    /* EPIC reads memory, not the CPU cache; a blob staged in cached PSRAM
     * is cleaned once here (a no-op for XIP flash and SRAM). */
    mpu_dcache_clean((void *)blob, (uint32_t)blob_len);

    texture = &g_textures[g_texture_count++];
    texture->handle = handle;
    texture->revision = revision;
    texture->width = width;
    texture->height = height;
    texture->format = format;
    texture->palette = palette_len != 0 ? blob + POCKETJS_GPU_NATIVE_HEADER : NULL;
    texture->pixels = blob + POCKETJS_GPU_NATIVE_HEADER + palette_len;
    texture->pixel_len = required;
    return 1;
}

void pocketjs_gpu_texture_reset(void)
{
    pocketjs_gpu_epic_wait();
    g_texture_count = 0;
    memset(g_textures, 0, sizeof(g_textures));
}

void pocketjs_gpu_profile_take(PocketjsGpuProfile *out)
{
    if (out != NULL)
    {
        *out = g_profile;
    }
    memset(&g_profile, 0, sizeof(g_profile));
}

/* ---- renderer API --------------------------------------------------------------- */

int pocketjs_gpu_caps(PocketjsGpuCaps *out)
{
    if (out == NULL || !g_open)
    {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->abi_version = POCKETJS_GPU_ABI_VERSION;
    out->flags = POCKETJS_GPU_CAP_FILL_OPAQUE | POCKETJS_GPU_CAP_FILL_ALPHA |
                 POCKETJS_GPU_CAP_GRADIENT | POCKETJS_GPU_CAP_BLIT_NATIVE;
#if POCKETJS_GPU_HAS_A8
    out->flags |= POCKETJS_GPU_CAP_A8_BLEND;
#endif
#ifdef POCKETJS_GPU_DIRECT_CPU_WRITES
    out->flags |= POCKETJS_GPU_CAP_DIRECT_CPU_WRITES;
#endif
    out->blit_formats = 0; /* portable layouts need a color matrix EPIC lacks here */
    out->blit_quad_formats = 0;
    out->coordinate_limit = POCKETJS_GPU_COORD_MAX;
    out->mask_tile_bytes = (uint32_t)MASK_BYTES;
    out->cpu_tile_pixels = (uint32_t)TILE_PIXELS;
    out->min_fill = POCKETJS_GPU_MIN_PIXELS;
    out->min_gradient = POCKETJS_GPU_MIN_PIXELS;
    out->min_blend = POCKETJS_GPU_MIN_PIXELS;
    out->min_blit = POCKETJS_GPU_MIN_PIXELS;
    return 0;
}

int pocketjs_gpu_begin(uint16_t *target, size_t pixels, uint32_t width,
                       uint32_t height, uint32_t kind)
{
    if (!g_open || target == NULL || width == 0 || height == 0 ||
        width > UINT16_MAX || height > UINT16_MAX ||
        pixels != (size_t)width * height)
    {
        return -1;
    }
    pocketjs_gpu_epic_wait();
    g_target.pixels = target;
    g_target.width = width;
    g_target.height = height;
    g_target.kind = kind;
    return 0;
}

int pocketjs_gpu_submit(const PocketjsGpuCmd *cmds, size_t count)
{
    size_t index;

    if (!g_open || g_target.pixels == NULL || (cmds == NULL && count != 0))
    {
        return POCKETJS_GPU_FAILED;
    }
    for (index = 0; index < count; ++index)
    {
        const PocketjsGpuCmd *cmd = &cmds[index];
        int result;

        switch (cmd->op)
        {
        case POCKETJS_GPU_OP_FILL:
        case POCKETJS_GPU_OP_FILL_ALPHA:
            result = pocketjs_gpu_epic_fill(cmd);
            break;
        case POCKETJS_GPU_OP_BLEND_A8:
            result = pocketjs_gpu_epic_blend_a8(cmd);
            break;
        case POCKETJS_GPU_OP_GRADIENT:
            result = pocketjs_gpu_epic_gradient(cmd);
            break;
        case POCKETJS_GPU_OP_BLIT:
            result = pocketjs_gpu_epic_blit(cmd);
            break;
        case POCKETJS_GPU_OP_TILE_OUT:
            result = pocketjs_gpu_epic_tile_out(cmd);
            break;
        case POCKETJS_GPU_OP_TILE_IN:
            result = pocketjs_gpu_epic_tile_in(cmd);
            break;
        case POCKETJS_GPU_OP_FENCE:
            pocketjs_gpu_epic_wait();
            result = POCKETJS_GPU_EXEC_OK;
            break;
        case POCKETJS_GPU_OP_BLIT_QUAD:
        default:
            result = POCKETJS_GPU_EXEC_REJECT;
            break;
        }
        if (result == POCKETJS_GPU_EXEC_REJECT)
        {
            ++g_profile.rejected;
            return -(int)(index + 1u);
        }
        if (result != POCKETJS_GPU_EXEC_OK)
        {
            return POCKETJS_GPU_FAILED;
        }
    }
    return 0;
}

int pocketjs_gpu_fence(void)
{
    if (!g_open)
    {
        return POCKETJS_GPU_FAILED;
    }
    pocketjs_gpu_epic_wait();
    return 0;
}

int pocketjs_gpu_end(void)
{
    if (!g_open)
    {
        return POCKETJS_GPU_FAILED;
    }
    pocketjs_gpu_epic_wait();
    g_target.pixels = NULL;
    return 0;
}

uint8_t *pocketjs_gpu_mask(uint32_t id, size_t *len)
{
    return pocketjs_gpu_mask_base(id, len);
}

uint16_t *pocketjs_gpu_tile(uint32_t id, size_t *len)
{
    return pocketjs_gpu_tile_base(id, len);
}

uint32_t pocketjs_gpu_native_texture(int32_t handle, uint64_t revision)
{
    uint32_t index;
    for (index = 0; index < g_texture_count; ++index)
    {
        if (g_textures[index].handle == handle &&
            g_textures[index].revision == revision)
        {
            return index + 1u;
        }
    }
    return 0;
}
