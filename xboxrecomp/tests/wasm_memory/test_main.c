/*
 * WebAssembly guest memory (src/platform/wasm_memory.c): the RAM mirrors and
 * the tiled aperture must be the same bytes as what they alias, through the
 * lifted code's XBOX_PTR/MEM32 and the runtime's xbox_ptr alike.
 *
 * Emscripten only (the table is wasm32 arithmetic). From the repo root:
 *   emcc -pthread -O1 -Ixboxrecomp/src/kernel -Ixboxrecomp/templates/runtime \
 *        xboxrecomp/tests/wasm_memory/test_main.c \
 *        xboxrecomp/src/platform/wasm_memory.c \
 *        -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=4GB -o /tmp/wasm_memory.js
 *   node /tmp/wasm_memory.js
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "xbox_guest_ptr.h"
#include "recomp_types.h"

#define MB (1024u * 1024u)

ptrdiff_t g_xbox_mem_offset;   /* declared by recomp_types.h; unused on wasm */

int main(void)
{
    uint8_t *ram, *contig;
    int m;

    /* The layout xbox_memory_layout.c builds: RAM, mirrors, contiguous
     * window, tiled aperture onto it. */
    ram = xbox_wasm_back(0, 64 * MB);
    assert(ram && ram == xbox_ptr(0));
    for (m = 1; m <= 28; m++)
        assert(xbox_wasm_alias((uint32_t)m * 64 * MB, 64 * MB, 0));
    contig = xbox_wasm_back(0x80000000u, 64 * MB);
    assert(contig);
    assert(xbox_wasm_alias(0xF0000000u, 64 * MB, 0x80000000u));
    assert(xbox_wasm_back(0xFD000000u, 16 * MB));

    /* Occupied ranges are refused, as an exact-address view is. */
    assert(!xbox_wasm_back(0x80000000u, MB));
    assert(!xbox_wasm_alias(0x80000000u, 64 * MB, 0));

    /* A mirror write lands in RAM, and back. */
    MEM32(0x04000448u) = 0xA5C30F17u;
    assert(MEM32(0x00000448u) == 0xA5C30F17u);
    assert(*(uint32_t *)(ram + 0x448) == 0xA5C30F17u);
    MEM32(0x70000448u) = 0x01020304u;                 /* last mirror */
    assert(*(volatile uint32_t *)xbox_ptr(0x448) == 0x01020304u);

    /* Tiled writes, contiguous reads: a surface and its texture. */
    MEM32(0xF1954000u) = 0xDEADBEEFu;
    assert(MEM32(0x81954000u) == 0xDEADBEEFu);
    assert(*(uint32_t *)(contig + 0x1954000) == 0xDEADBEEFu);

    /* Both translations agree, and a region is contiguous across granules. */
    assert(XBOX_PTR(0xF1954000u) == XBOX_GUEST_ADDR(0xF1954000u));
    assert((uint8_t *)xbox_ptr(0x000FFFFCu) + 4 == xbox_ptr(0x00100000u));
    assert((uint8_t *)xbox_ptr(0x040FFFFCu) + 4 == xbox_ptr(0x04100000u));

    /* Host back to guest: the storage's own region. */
    assert(xbox_native_to_va(xbox_ptr(0x04000448u)) == 0x448u);
    assert(xbox_native_to_va(xbox_ptr(0xF1954000u)) == 0x81954000u);
    assert(xbox_native_to_va(&m) == 0);

    /* Extended VMA: a guest commit backs what was unbacked. */
    assert(xbox_wasm_commit(0x74100000u, 0x3000));
    MEM32(0x74100010u) = 7;
    assert(MEM32(0x74100010u) == 7);
    assert(xbox_native_to_va(xbox_ptr(0x74100010u)) == 0x74100010u);
    assert(xbox_wasm_commit(0x00000000u, 0x1000));   /* already RAM: no-op */
    assert(MEM32(0x00000448u) == 0x01020304u);

    /* Unbacked granules read the shared sink, not wasm memory at the VA. */
    assert(xbox_native_to_va(xbox_ptr(0x78000000u)) == 0);

    printf("wasm_memory: OK\n");
    return 0;
}
