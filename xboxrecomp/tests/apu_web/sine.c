/*
 * The browser's audio output (src/apu/apu_web.c + web/audio.js) on its own:
 * a 440 Hz sine fed through xa2_* as the APU frame thread feeds it (256-sample
 * blocks, paced by xa2_queued), played by the AudioWorklet from the ring.
 *
 * From the repo root (emsdk sourced):
 *   emcc -O2 -pthread -sPROXY_TO_PTHREAD -Ixboxrecomp/src/apu \
 *        xboxrecomp/tests/apu_web/sine.c xboxrecomp/src/apu/apu_web.c \
 *        xboxrecomp/src/apu/apu_xaudio2.c -sEXIT_RUNTIME=0 \
 *        --pre-js web/pre.js --pre-js web/audio.js -o <dir>/sine.js
 *   cp xboxrecomp/tests/apu_web/index.html <dir>/
 *   python3 tools/web_serve.py --root <dir> --port 3001
 * then open http://127.0.0.1:3001/ (click once if the browser wants a gesture).
 * The page shows the worklet's counters; check.mjs reads them headless.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include "apu_xaudio2.h"

int main(void)
{
    int16_t blk[256 * 2];
    double ph = 0, inc = 2 * M_PI * 440.0 / 48000.0;
    unsigned long blocks = 0;

    if (!xa2_init()) { printf("sine: no output\n"); return 1; }
    for (;;) {
        if (xa2_queued() >= xa2_queue_target()) { usleep(1000); continue; }
        for (int i = 0; i < 256; i++) {
            int16_t s = (int16_t)(sin(ph) * 16000.0);
            blk[2 * i] = blk[2 * i + 1] = s;
            ph += inc;
            if (ph >= 2 * M_PI) ph -= 2 * M_PI;
        }
        if (xa2_submit_samples(blk, 256) && ++blocks % 1875 == 0)   /* every 10 s */
            printf("sine: %lu blocks\n", blocks);
    }
}
