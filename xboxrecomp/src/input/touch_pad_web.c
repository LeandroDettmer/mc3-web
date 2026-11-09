/*
 * touch_pad_web.c -- the page's on-screen controller (web/touch.js) as pad 1,
 * browser only.
 *
 * The touch controls are a virtual pad: the page writes its state into the
 * box below (shared wasm memory, Module.mc3TouchPad in web/pre.js; the same
 * pattern as nv2a_wgpu_aspect_box), and xbox_InputGetState merges it into
 * port 0 like the keyboard: buttons OR'ed, analog buttons at the larger
 * pressure, a stick that is off its centre replaces the pad's. While the page
 * says the controls are on (word 0), port 0 counts as connected even with no
 * real pad and the keyboard off. After nfsu2-web's touch controls (th0mk,
 * MIT; web/src/app/touch.ts, a virtual controller merged with a real one).
 *
 * Words (int32, one writer: the page's main thread; a torn read across words
 * is one frame of mixed input, harmless):
 *   0       1 while the touch controls are on
 *   1       wButtons (XBOX_GAMEPAD_* bits)
 *   2..9    bAnalogButtons A B X Y Black White LT RT, 0..255
 *   10..13  sThumbLX LY RX RY, -32767..32767 (Y up)
 */
#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xinput_xbox.h"

extern int (*xbox_keyboard_driving)(void);   /* sdl_keyboard.c */

#define TP_WORDS 14
static volatile int32_t s_touch[TP_WORDS];

EMSCRIPTEN_KEEPALIVE uintptr_t xbox_touch_pad_box(void) { return (uintptr_t)s_touch; }

static int32_t tp(int i) { return __atomic_load_n(&s_touch[i], __ATOMIC_RELAXED); }

int xbox_touch_pad_on(void) { return tp(0) != 0; }

void xbox_touch_pad_merge(XBOX_INPUT_STATE *s)
{
    int i;
    SHORT *sticks[4];

    static int trace = -1;
    static int32_t last_w = -1, last_an = -1;

    if (!tp(0))
        return;
    s->Gamepad.wButtons |= (WORD)tp(1);
    if (trace < 0) {
        const char *v = getenv("MC3_TOUCH_TRACE");
        trace = v && *v && *v != '0';
    }
    if (trace) {
        /* every press/release of a touch digital or analog button, with the merged wButtons */
        int32_t an = 0;
        for (i = 0; i < 8; i++)
            an |= (tp(2 + i) > 0) << i;
        if (tp(1) != last_w || an != last_an) {
            fprintf(stderr, "[TOUCH] page wButtons=0x%04X (was 0x%04X) analog=0x%02X (was 0x%02X) merged wButtons=0x%04X\n",
                    (unsigned)tp(1) & 0xFFFF, (unsigned)last_w & 0xFFFF, (unsigned)an, (unsigned)last_an & 0xFF,
                    (unsigned)s->Gamepad.wButtons);
            last_w = tp(1); last_an = an;
        }
    }
    for (i = 0; i < 8; i++) {
        int32_t v = tp(2 + i);
        v = v < 0 ? 0 : v > 255 ? 255 : v;
        if (v > s->Gamepad.bAnalogButtons[i])
            s->Gamepad.bAnalogButtons[i] = (BYTE)v;
    }
    sticks[0] = &s->Gamepad.sThumbLX; sticks[1] = &s->Gamepad.sThumbLY;
    sticks[2] = &s->Gamepad.sThumbRX; sticks[3] = &s->Gamepad.sThumbRY;
    for (i = 0; i < 4; i++) {
        int32_t v = tp(10 + i);
        if (v)
            *sticks[i] = (SHORT)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
    /* The menus navigate with the left stick, not the d-pad (as the keyboard's
     * arrows do): out of a race the touch d-pad pushes the stick too. */
    if (!(xbox_keyboard_driving && xbox_keyboard_driving())) {
        WORD b = (WORD)tp(1);
        if (!tp(10)) {
            if (b & XBOX_GAMEPAD_DPAD_LEFT)  s->Gamepad.sThumbLX = -32767;
            if (b & XBOX_GAMEPAD_DPAD_RIGHT) s->Gamepad.sThumbLX = 32767;
        }
        if (!tp(11)) {
            if (b & XBOX_GAMEPAD_DPAD_UP)   s->Gamepad.sThumbLY = 32767;
            if (b & XBOX_GAMEPAD_DPAD_DOWN) s->Gamepad.sThumbLY = -32767;
        }
    }
}

#endif /* __EMSCRIPTEN__ */
