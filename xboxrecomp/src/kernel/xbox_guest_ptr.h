/*
 * Guest VA -> host pointer, for runtime code.
 *
 * The lifted code translates with XBOX_PTR (templates/runtime/recomp_types.h);
 * this is the same translation for the runtime, which does not include that
 * header everywhere. The two must agree.
 *
 * Native hosts map the whole 4 GB guest space at one offset, with the RAM
 * mirrors and the tiled aperture aliased by the OS, so a guest address is one
 * add away. WebAssembly has neither mmap aliasing nor a free 4 GB of address
 * space: there guest memory is a table of 1 MB granules, each with its own
 * offset (platform/wasm_memory.c), and aliases are granules sharing storage.
 * An address computed as base + guest VA is only right in RAM there, so
 * runtime code translates every guest address through xbox_ptr.
 *
 * A host pointer from xbox_ptr(va) stays valid for a range only while the
 * range sits inside one guest region (RAM, a mirror, the contiguous window,
 * an aperture): those are contiguous storage on every host.
 */
#ifndef XBOX_GUEST_PTR_H
#define XBOX_GUEST_PTR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__EMSCRIPTEN__)

#define XBOX_GRANULE_SHIFT 20
#define XBOX_GRANULE_COUNT 4096u

/* Host offset per 1 MB guest granule. wasm32 pointers are 32 bits, so the add
 * wraps mod 2^32 and every offset fits a uint32_t. */
extern uint32_t g_xbox_region_off[XBOX_GRANULE_COUNT];

#define XBOX_GUEST_ADDR(va) \
    ((uintptr_t)((uint32_t)(va) + g_xbox_region_off[(uint32_t)(va) >> XBOX_GRANULE_SHIFT]))

/* Host pointer back to the guest VA of the storage's own region (RAM for a
 * mirror, the contiguous window for the tiled aperture); 0 if the pointer is
 * not guest memory. */
uint32_t xbox_native_to_va(const void *p);

/* Fresh zeroed storage for [va, va + size), rounded out to granules. NULL if
 * any granule is already backed or aliased (as MapViewOfFileEx refuses an
 * occupied address) or the allocation fails. Returns the host address of va. */
void *xbox_wasm_back(uint32_t va, uint32_t size);
/* Point [va, va + size) at the storage behind [target_va, ...): a mirror.
 * NULL under the same conditions, or if the target is not backed. */
void *xbox_wasm_alias(uint32_t va, uint32_t size, uint32_t target_va);
/* Back every still-unbacked granule of [va, va + size): a guest commit. 1 on
 * success. */
int xbox_wasm_commit(uint32_t va, uint32_t size);

#else

extern ptrdiff_t g_xbox_mem_offset;

#define XBOX_GUEST_ADDR(va) ((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset)
#define xbox_native_to_va(p) \
    ((uint32_t)((uintptr_t)(p) - (uintptr_t)g_xbox_mem_offset))

#endif

#define xbox_ptr(va) ((void *)XBOX_GUEST_ADDR(va))

#ifdef __cplusplus
}
#endif

#endif /* XBOX_GUEST_PTR_H */
