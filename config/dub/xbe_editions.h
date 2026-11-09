/* Every retail default.xbe this lift runs, checked at boot (src/main.c).
 *
 * The editions' code is byte-identical: the European disc's XBE differs from
 * the American one in 531 header bytes only (signature, certificate region
 * and version, section digests), and its data in three files (the pause
 * screen, two videos) -- compared 2026-10-24. So one lift and one build
 * serve them all; this list only tells the boot check which discs to accept
 * and what to call them. A new entry needs that comparison first: a disc
 * whose code differs needs a lift of its own (make lift).
 *
 *   X(name, FNV-1a 64 of the whole default.xbe, certificate region, version)
 *
 * The name is the one used everywhere else: game-<name>/ folders, EDITION=,
 * the log's "disc: edition <name>". */
#pragma once

#define MC3_XBE_EDITIONS(X) \
    X("usa", 0x45be4d110be69ba7ull, 0x1u, 0x1u)   /* NTSC-U, TT-121 (Redump: USA) */ \
    X("eur", 0x6b9edf2e0233549full, 0x4u, 0x2u)   /* PAL, En/Fr/De/Es/It (Redump: Europe) */
