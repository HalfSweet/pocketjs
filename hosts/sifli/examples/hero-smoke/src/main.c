/*
 * hero-smoke: links the PocketJS core staticlib and the GPU component into a
 * SiFli firmware and renders one frame into a PSRAM framebuffer, proving the
 * integration compiles and the queue accepts commands. The full host
 * (QuickJS, LCD ring, input) lives in components/pocketjs_host.
 */
#include <rtthread.h>
#include <stdint.h>
#include <string.h>

#include "mem_section.h"

#include "pocket_core.h"
#include "pocketjs_gpu_host.h"

#define LOGICAL_WIDTH 512u
#define LOGICAL_HEIGHT 300u
#define RENDER_SCALE 2u
#define FRAME_PIXELS (LOGICAL_WIDTH * RENDER_SCALE * LOGICAL_HEIGHT * RENDER_SCALE)

L2_NON_RET_BSS_SECT(pocketjs_smoke_framebuffer,
                    ALIGN(64) static uint16_t g_framebuffer[FRAME_PIXELS]);

/* ---- allocator and panic hooks the staticlib links against ---------------- */

void *pocket_heap_alloc(size_t size, size_t align)
{
    if (size == 0)
    {
        size = 1;
    }
    if (align < sizeof(void *))
    {
        align = sizeof(void *);
    }
    return rt_malloc_align(size, align);
}

void pocket_heap_free(void *ptr)
{
    if (ptr != NULL)
    {
        rt_free_align(ptr);
    }
}

void pocket_rust_panic(void)
{
    rt_kprintf("[PocketJS] fatal: panic in pocketjs-core\n");
    while (1)
    {
        rt_thread_mdelay(1000);
    }
}

/* ---- smoke ---------------------------------------------------------------- */

int main(void)
{
    PocketCore *core;
    PocketRenderStats stats;
    PocketjsGpuProfile profile;
    int32_t root;
    int32_t frame;

    rt_kprintf("[PocketJS] hero-smoke: gpu open\n");
    if (!pocketjs_gpu_open())
    {
        rt_kprintf("[PocketJS] fatal: pocketjs_gpu_open failed\n");
        return -1;
    }
    core = pocket_core_create(LOGICAL_WIDTH, LOGICAL_HEIGHT, RENDER_SCALE, RENDER_SCALE, 1);
    if (core == NULL)
    {
        rt_kprintf("[PocketJS] fatal: pocket_core_create failed\n");
        return -1;
    }

    /* One box under the root so the frame has a fill and a damage region. */
    root = 1;
    frame = pocket_core_create_node(core, 0);
    pocket_core_insert_before(core, root, frame, 0);
    pocket_core_tick(core);

    memset(&stats, 0, sizeof(stats));
    if (pocket_core_render_rgb565(core, g_framebuffer, FRAME_PIXELS, 0, &stats) != 0)
    {
        rt_kprintf("[PocketJS] fatal: render failed\n");
        return -1;
    }
    pocketjs_gpu_profile_take(&profile);
    rt_kprintf("[PocketJS] hero-smoke: words=%u regions=%u pixels=%u fills=%u blends=%u "
               "sw=%u/%u tiles=%u fences=%u tx=%u hash=%08x%08x\n",
               (unsigned)stats.draw_words, (unsigned)stats.damage_regions,
               (unsigned)stats.damage_pixels, (unsigned)stats.epic_fills,
               (unsigned)stats.epic_blends, (unsigned)stats.software_ops,
               (unsigned)stats.software_words, (unsigned)stats.cpu_tiles,
               (unsigned)stats.fences, (unsigned)profile.transactions,
               (unsigned)(pocket_core_draw_hash(core) >> 32),
               (unsigned)pocket_core_draw_hash(core));

    pocket_core_destroy(core);
    pocketjs_gpu_close();
    rt_kprintf("[PocketJS] hero-smoke: done\n");
    while (1)
    {
        rt_thread_mdelay(1000);
    }
}
