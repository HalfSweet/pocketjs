/*
 * Host-facing side of the PocketJS GPU command queue: lifecycle, the native
 * texture registry, and profiling. The renderer-facing side is
 * hosts/sifli/include/pocketjs_gpu.h.
 */
#ifndef POCKETJS_GPU_HOST_H
#define POCKETJS_GPU_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Initialize the EPIC HAL and the SRAM planes. Returns 1 on success, 0 on
 * failure; calling it again is a no-op that returns 1. */
int32_t pocketjs_gpu_open(void);

/* Wait for the hardware and release it. */
void pocketjs_gpu_close(void);

/* Native texture blob layout (the `.epic` pak payload): u16 width, u16
 * height, u8 format, u8 flags, u16 reserved, then the pixels; L8 blobs carry
 * a 1024-byte EPIC-order palette before the indices. */
#define POCKETJS_GPU_NATIVE_RGB565   0u
#define POCKETJS_GPU_NATIVE_BGRA8888 1u
#define POCKETJS_GPU_NATIVE_L8       2u
#define POCKETJS_GPU_NATIVE_HEADER   8u

/* Register a native blob for core texture `handle` at content `revision`.
 * The blob must stay valid and unchanged until pocketjs_gpu_texture_reset().
 * Returns 1 on success, 0 when the blob is malformed or the registry is
 * full. */
int32_t pocketjs_gpu_texture_register(int32_t handle, uint64_t revision,
                                      const uint8_t *blob, size_t blob_len);

/* Forget every registered texture (guest switch). */
void pocketjs_gpu_texture_reset(void);

typedef struct
{
    uint64_t submit_cycles; /* DWT cycles spent programming the HAL */
    uint64_t wait_cycles;   /* DWT cycles spent waiting for completion */
    uint32_t transactions;  /* hardware transactions started */
    uint32_t rejected;      /* commands the queue refused */
} PocketjsGpuProfile;

/* Read and reset the counters (NULL just resets). */
void pocketjs_gpu_profile_take(PocketjsGpuProfile *out);

#ifdef __cplusplus
}
#endif

#endif
